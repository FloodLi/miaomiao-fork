#include "display_policy.h"

#include <stdatomic.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "scheduler.h"
#include "display_mode.h"
#include "epd.h"

static atomic_bool s_manual_screen;
static atomic_bool s_quick_refresh;
static atomic_uint s_display_epoch;
static portMUX_TYPE s_policy_mux = portMUX_INITIALIZER_UNLOCKED;
static display_request_token_t s_current_request;
static bool s_committed_manual;
static bool s_request_finished;
static esp_err_t s_request_result;
static bool s_content_owner;
#define RESULT_HISTORY_SIZE 32
static struct {
    unsigned epoch;
    display_result_t result;
} s_results[RESULT_HISTORY_SIZE];

static void record_result_locked(unsigned epoch, display_result_state_t state,
                                 esp_err_t error)
{
    unsigned slot = epoch % RESULT_HISTORY_SIZE;
    s_results[slot].epoch = epoch;
    s_results[slot].result = (display_result_t){state, error};
}

static bool background_allowed_locked(int mode)
{
    return mode >= 0 && mode < display_mode_count() &&
           s_request_finished && !s_content_owner && !atomic_load(&s_quick_refresh) &&
           (mode == DISPLAY_MODE_SLIDESHOW
                ? !s_committed_manual && !atomic_load(&s_manual_screen)
                : display_mode_active() == mode);
}

static bool background_allowed(int mode)
{
    portENTER_CRITICAL(&s_policy_mux);
    bool allowed = background_allowed_locked(mode);
    portEXIT_CRITICAL(&s_policy_mux);
    return allowed;
}

static bool weather_page_is_active(void)
{
    return display_mode_active() == DISPLAY_MODE_WEATHER;
}

void display_policy_init(void)
{
    atomic_store(&s_manual_screen, false);
    atomic_store(&s_quick_refresh, false);
    atomic_store(&s_display_epoch, 0);
    s_committed_manual = false;
    s_content_owner = false;
    memset(s_results, 0, sizeof(s_results));
    memset(&s_current_request, 0, sizeof(s_current_request));
    s_current_request.target_mode = DISPLAY_POLICY_KEEP_MODE;
    s_current_request.previous_mode = -1;
    s_request_finished = true;
    s_request_result = ESP_OK;
}

void display_policy_set_quick_refresh_active(bool active)
{
    atomic_store(&s_quick_refresh, active);
}

bool display_policy_quick_refresh_active(void)
{
    return atomic_load(&s_quick_refresh);
}

bool display_policy_slideshow_owns_display(void)
{
    return display_mode_active() == DISPLAY_MODE_SLIDESHOW;
}

bool display_policy_clock_may_auto_refresh(void)
{
    return background_allowed(DISPLAY_MODE_CLOCK);
}

bool display_policy_calendar_may_midnight_refresh(void)
{
    return background_allowed(DISPLAY_MODE_CALENDAR);
}

bool display_policy_weather_use_full_page(bool user_explicit)
{
    if (user_explicit)
        return true;
    if (display_policy_request_pending() || display_policy_manual_content_owns_display())
        return false;
    if (atomic_load(&s_quick_refresh))
        return false;
    if (display_policy_slideshow_owns_display())
        return false;
    if (weather_page_is_active())
        return true;
    if (display_mode_active() == DISPLAY_MODE_CLOCK)
        return false;
    if (atomic_load(&s_manual_screen))
        return false;
    return true;
}

bool display_policy_weather_may_network_fetch(bool user_explicit)
{
    if (user_explicit)
        return true;
    if (display_policy_request_pending() || display_policy_manual_content_owns_display())
        return false;
    if (atomic_load(&s_quick_refresh))
        return false;
    return !display_policy_slideshow_owns_display();
}

bool display_policy_weather_may_render_full_page(void)
{
    return background_allowed(DISPLAY_MODE_WEATHER);
}

bool display_policy_manual_content_owns_display(void)
{
    portENTER_CRITICAL(&s_policy_mux);
    bool owner = s_content_owner;
    portEXIT_CRITICAL(&s_policy_mux);
    return owner;
}

bool display_policy_request_pending(void)
{
    portENTER_CRITICAL(&s_policy_mux);
    bool pending = !s_request_finished;
    portEXIT_CRITICAL(&s_policy_mux);
    return pending;
}

void display_policy_set_manual_screen_active(bool active)
{
    portENTER_CRITICAL(&s_policy_mux);
    s_committed_manual = active;
    atomic_store(&s_manual_screen, active);
    portEXIT_CRITICAL(&s_policy_mux);
}

bool display_policy_manual_screen_active(void)
{
    return atomic_load(&s_manual_screen);
}

unsigned display_policy_begin_manual_display(void)
{
    return display_policy_begin_manual_display_mode(EPD_REFRESH_FULL);
}

unsigned display_policy_begin_manual_display_mode(epd_refresh_mode_t mode)
{
    return display_policy_begin_request(DISPLAY_POLICY_KEEP_MODE, mode).epoch;
}

static display_request_token_t begin_request_locked(int target_mode)
{
    display_request_token_t token;
    if (!s_request_finished)
        record_result_locked(s_current_request.epoch, DISPLAY_RESULT_CANCELED,
                              ESP_ERR_INVALID_STATE);
    token.previous_mode = display_mode_active();
    token.previous_manual = s_committed_manual;
    token.previous_content_owner = s_content_owner;
    token.target_mode = target_mode;
    token.epoch = atomic_load(&s_display_epoch) + 1;
    if (!token.epoch)
        token.epoch = 1;
    atomic_store(&s_display_epoch, token.epoch);
    atomic_store(&s_manual_screen, true);
    s_current_request = token;
    s_request_finished = false;
    record_result_locked(token.epoch, DISPLAY_RESULT_PENDING, ESP_OK);
    return token;
}

static void prepare_refresh(epd_refresh_mode_t mode)
{
    bool mode_falls_back_to_full =
        (mode == EPD_REFRESH_FAST &&
         !epd_refresh_mode_supported(EPD_REFRESH_FAST)) ||
        (mode == EPD_REFRESH_PARTIAL &&
         !epd_refresh_mode_supported(EPD_REFRESH_PARTIAL) &&
         !epd_refresh_mode_supported(EPD_REFRESH_FAST));
    if (mode == EPD_REFRESH_FULL || mode_falls_back_to_full)
        epd_request_full_refresh_next();
}

display_request_token_t display_policy_begin_request(int target_mode,
                                                     epd_refresh_mode_t mode)
{
    portENTER_CRITICAL(&s_policy_mux);
    display_request_token_t token = begin_request_locked(target_mode);
    portEXIT_CRITICAL(&s_policy_mux);
    prepare_refresh(mode);
    scheduler_notify_manual_show();
    ESP_LOGI("display_policy", "txn begin epoch=%lu target=%d previous=%d manual=%d",
             (unsigned long)token.epoch, token.target_mode,
             token.previous_mode, token.previous_manual ? 1 : 0);
    return token;
}

bool display_policy_try_begin_request(int target_mode, unsigned expected_epoch,
                                      epd_refresh_mode_t mode,
                                      display_request_token_t *out)
{
    if (!out)
        return false;
    portENTER_CRITICAL(&s_policy_mux);
    bool allowed = atomic_load(&s_display_epoch) == expected_epoch && s_request_finished;
    if (allowed)
        *out = begin_request_locked(target_mode);
    portEXIT_CRITICAL(&s_policy_mux);
    if (allowed) {
        prepare_refresh(mode);
        scheduler_notify_manual_show();
    }
    return allowed;
}

bool display_policy_try_begin_background(int target_mode, unsigned expected_epoch,
                                          epd_refresh_mode_t mode,
                                          display_request_token_t *out)
{
    if (!out)
        return false;
    portENTER_CRITICAL(&s_policy_mux);
    bool allowed = atomic_load(&s_display_epoch) == expected_epoch &&
                   background_allowed_locked(target_mode);
    if (allowed)
        *out = begin_request_locked(target_mode);
    portEXIT_CRITICAL(&s_policy_mux);
    if (allowed)
        prepare_refresh(mode);
    return allowed;
}

bool display_policy_finish_request(const display_request_token_t *token,
                                   esp_err_t result,
                                   bool manual_after_success)
{
    if (!token)
        return false;
    bool current = false;
    bool already_finished = false;
    esp_err_t first_result = ESP_OK;
    portENTER_CRITICAL(&s_policy_mux);
    if (token->epoch && atomic_load(&s_display_epoch) == token->epoch &&
        s_current_request.epoch == token->epoch) {
        current = true;
        if (s_request_finished) {
            already_finished = true;
            first_result = s_request_result;
        } else if (result == ESP_OK) {
            if (token->target_mode != DISPLAY_POLICY_KEEP_MODE)
                display_mode_set_active(token->target_mode);
            s_committed_manual = manual_after_success;
            s_content_owner = token->target_mode == DISPLAY_POLICY_KEEP_MODE &&
                              manual_after_success;
            atomic_store(&s_manual_screen, manual_after_success);
        } else {
            display_mode_set_active(token->previous_mode);
            s_committed_manual = token->previous_manual;
            s_content_owner = token->previous_content_owner;
            atomic_store(&s_manual_screen, token->previous_manual);
        }
        if (!already_finished) {
            s_request_finished = true;
            s_request_result = result;
            record_result_locked(token->epoch,
                                  result == ESP_OK ? DISPLAY_RESULT_OK : DISPLAY_RESULT_ERROR,
                                  result);
        }
    }
    portEXIT_CRITICAL(&s_policy_mux);
    if (already_finished) {
        if (result != first_result)
            ESP_LOGW("display_policy",
                     "txn repeated finish epoch=%lu first=%s repeated=%s",
                     (unsigned long)token->epoch,
                     esp_err_to_name(first_result), esp_err_to_name(result));
        return true;
    }
    ESP_LOGI("display_policy", "txn %s epoch=%lu result=%s active=%d manual=%d",
             current ? (result == ESP_OK ? "commit" : "rollback") : "stale",
             (unsigned long)token->epoch, esp_err_to_name(result),
             display_mode_active(), display_policy_manual_screen_active() ? 1 : 0);
    return current;
}

bool display_policy_finish_epoch(unsigned epoch, esp_err_t result,
                                 bool manual_after_success)
{
    display_request_token_t token;
    portENTER_CRITICAL(&s_policy_mux);
    token = s_current_request;
    portEXIT_CRITICAL(&s_policy_mux);
    if (token.epoch != epoch)
        return false;
    return display_policy_finish_request(&token, result, manual_after_success);
}

bool display_policy_epoch_is_current(unsigned epoch)
{
    return atomic_load(&s_display_epoch) == epoch;
}

void display_policy_bump_display_epoch(void)
{
    portENTER_CRITICAL(&s_policy_mux);
    if (!s_request_finished) {
        record_result_locked(s_current_request.epoch, DISPLAY_RESULT_CANCELED,
                              ESP_ERR_INVALID_STATE);
        s_committed_manual = s_current_request.previous_manual;
        s_content_owner = s_current_request.previous_content_owner;
        atomic_store(&s_manual_screen, s_committed_manual);
        s_request_finished = true;
    }
    atomic_fetch_add(&s_display_epoch, 1);
    portEXIT_CRITICAL(&s_policy_mux);
    epd_request_full_refresh_next();
}

unsigned display_policy_display_epoch(void)
{
    return atomic_load(&s_display_epoch);
}

display_result_t display_policy_get_result(unsigned epoch)
{
    display_result_t result = {DISPLAY_RESULT_ERROR, ESP_ERR_NOT_FOUND};
    portENTER_CRITICAL(&s_policy_mux);
    unsigned slot = epoch % RESULT_HISTORY_SIZE;
    if (epoch && s_results[slot].epoch == epoch)
        result = s_results[slot].result;
    portEXIT_CRITICAL(&s_policy_mux);
    return result;
}

display_result_t display_policy_wait_result(unsigned epoch)
{
    display_result_t result;
    do {
        result = display_policy_get_result(epoch);
        if (result.state == DISPLAY_RESULT_PENDING)
            vTaskDelay(1);
    } while (result.state == DISPLAY_RESULT_PENDING);
    return result;
}
