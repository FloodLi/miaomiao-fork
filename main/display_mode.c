#include "display_mode.h"
#include <stdatomic.h>
#include <string.h>
#include "esp_log.h"
#include "display_policy.h"
#include "epd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dmode";

static display_mode_entry_t s_modes[DISPLAY_MODE_MAX];
static atomic_int s_count = ATOMIC_VAR_INIT(0);
static atomic_int s_active_mode = ATOMIC_VAR_INIT(-1);
typedef struct render_context {
    TaskHandle_t task;
    unsigned epoch;
    struct render_context *next;
} render_context_t;
static render_context_t *s_render_contexts;
static portMUX_TYPE s_context_mux = portMUX_INITIALIZER_UNLOCKED;

unsigned display_mode_render_epoch(void)
{
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    unsigned epoch = display_policy_display_epoch();
    portENTER_CRITICAL(&s_context_mux);
    for (render_context_t *ctx = s_render_contexts; ctx; ctx = ctx->next) {
        if (ctx->task == task) {
            epoch = ctx->epoch;
            break;
        }
    }
    portEXIT_CRITICAL(&s_context_mux);
    return epoch;
}

int display_mode_register(const display_mode_entry_t *entry)
{
    if (!entry || !entry->name || !entry->show) return -1;
    int index = atomic_load(&s_count);
    if (index >= DISPLAY_MODE_MAX) {
        ESP_LOGE(TAG, "mode registry full (%d)", DISPLAY_MODE_MAX);
        return -1;
    }
    s_modes[index] = *entry;
    atomic_store(&s_count, index + 1);
    ESP_LOGI(TAG, "[%d] %s (%s)", index, entry->name, entry->label_cn);
    return index;
}

int display_mode_count(void) { return atomic_load(&s_count); }

const display_mode_entry_t *display_mode_get(int idx)
{
    if (idx < 0 || idx >= atomic_load(&s_count)) return NULL;
    return &s_modes[idx];
}

typedef enum {
    REQUEST_EXPLICIT,
    REQUEST_CONDITIONAL,
    REQUEST_BACKGROUND,
} request_kind_t;

static esp_err_t display_mode_show_internal(int idx, request_kind_t kind,
                                            unsigned expected_epoch, unsigned *epoch_out)
{
    if (epoch_out)
        *epoch_out = kind == REQUEST_EXPLICIT ? display_policy_display_epoch() : expected_epoch;
    const display_mode_entry_t *m = display_mode_get(idx);
    if (!m) return ESP_ERR_INVALID_ARG;
    if (!m->available)
        return ESP_ERR_INVALID_STATE;
    display_request_token_t token;
    if (kind == REQUEST_BACKGROUND) {
        if (!display_policy_try_begin_background(idx, expected_epoch,
                                                  EPD_REFRESH_FULL, &token))
            return ESP_ERR_INVALID_STATE;
    } else if (kind == REQUEST_CONDITIONAL) {
        if (!display_policy_try_begin_request(idx, expected_epoch,
                                               EPD_REFRESH_FULL, &token))
            return ESP_ERR_INVALID_STATE;
    } else {
        token = display_policy_begin_request(idx, EPD_REFRESH_FULL);
    }
    unsigned epoch = token.epoch;
    if (epoch_out)
        *epoch_out = epoch;
    render_context_t context = {xTaskGetCurrentTaskHandle(), epoch, NULL};
    portENTER_CRITICAL(&s_context_mux);
    context.next = s_render_contexts;
    s_render_contexts = &context;
    portEXIT_CRITICAL(&s_context_mux);
    esp_err_t err = display_policy_epoch_is_current(epoch)
                        ? m->show() : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK && m->wait_complete)
        err = m->wait_complete();
    bool final_manual = m->manual_after_show ? m->manual_after_show() : true;
    portENTER_CRITICAL(&s_context_mux);
    render_context_t **link = &s_render_contexts;
    while (*link && *link != &context)
        link = &(*link)->next;
    if (*link)
        *link = context.next;
    portEXIT_CRITICAL(&s_context_mux);
    if (!display_policy_finish_request(&token, err, final_manual))
        return ESP_ERR_INVALID_STATE;
    display_result_t result = display_policy_get_result(epoch);
    return result.state == DISPLAY_RESULT_OK ? ESP_OK : result.error;
}

esp_err_t display_mode_show(int idx)
{
    return display_mode_show_internal(idx, REQUEST_EXPLICIT, 0, NULL);
}

esp_err_t display_mode_show_request(int idx, unsigned *epoch_out)
{
    return display_mode_show_internal(idx, REQUEST_EXPLICIT, 0, epoch_out);
}

esp_err_t display_mode_show_background(int idx, unsigned expected_epoch)
{
    return display_mode_show_internal(idx, REQUEST_BACKGROUND, expected_epoch, NULL);
}

esp_err_t display_mode_show_if_current(int idx, unsigned expected_epoch, unsigned *epoch_out)
{
    return display_mode_show_internal(idx, REQUEST_CONDITIONAL, expected_epoch, epoch_out);
}

const char *display_mode_name(int idx)
{
    const display_mode_entry_t *m = display_mode_get(idx);
    return m ? m->name : "?";
}

const char *display_mode_label(int idx)
{
    const display_mode_entry_t *m = display_mode_get(idx);
    return m ? m->label_cn : "?";
}

bool display_mode_is_available(int idx)
{
    const display_mode_entry_t *m = display_mode_get(idx);
    return m && m->available;
}

void display_mode_set_available(int idx, bool available)
{
    if (idx >= 0 && idx < atomic_load(&s_count))
        s_modes[idx].available = available;
}

int display_mode_active(void)
{
    return atomic_load(&s_active_mode);
}

void display_mode_set_active(int idx)
{
    if (idx < 0 || idx >= atomic_load(&s_count)) {
        atomic_store(&s_active_mode, -1);
        return;
    }
    atomic_store(&s_active_mode, idx);
}
