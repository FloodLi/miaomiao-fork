#include <stdlib.h>
#include <stdatomic.h>

#include "unity.h"
#include "display_mode.h"
#include "display_policy.h"
#ifdef MSP_HOST_TEST
#include "host_support.h"
#include <pthread.h>
#endif

static atomic_int s_full_refresh_requests;
static atomic_int s_manual_notifications;

void epd_request_full_refresh_next(void)
{
    s_full_refresh_requests++;
}

bool epd_refresh_mode_supported(epd_refresh_mode_t mode)
{
    (void)mode;
    return true;
}

void scheduler_notify_manual_show(void)
{
    s_manual_notifications++;
}

static bool release_manual(void) { return false; }
static bool hold_manual(void) { return true; }

static esp_err_t show_ok(void)
{
    return ESP_OK;
}

static esp_err_t show_fail(void)
{
    return ESP_FAIL;
}

static esp_err_t complete_fail(void)
{
    return ESP_FAIL;
}

static esp_err_t show_finishes_failure(void)
{
    (void)display_policy_finish_epoch(display_mode_render_epoch(), ESP_ERR_NO_MEM, true);
    return ESP_OK;
}

static esp_err_t show_superseded(void)
{
    unsigned invoking_epoch = display_mode_render_epoch();
    unsigned epoch = 0;
    TEST_ASSERT_EQUAL(ESP_OK, display_mode_show_request(1, &epoch));
    TEST_ASSERT_TRUE(display_policy_epoch_is_current(epoch));
    TEST_ASSERT_EQUAL(invoking_epoch, display_mode_render_epoch());
    return ESP_OK;
}

static esp_err_t show_superseded_by_failure(void)
{
    unsigned epoch = 0;
    TEST_ASSERT_EQUAL(ESP_FAIL, display_mode_show_request(2, &epoch));
    TEST_ASSERT_TRUE(display_policy_epoch_is_current(epoch));
    return ESP_OK;
}

static void reset_state(void)
{
    display_policy_init();
    display_mode_set_active(-1);
    s_full_refresh_requests = 0;
    s_manual_notifications = 0;
}

static void test_newer_request_wins_when_old_show_finishes_last(void)
{
    reset_state();
    unsigned old_epoch = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      display_mode_show_request(0, &old_epoch));
    TEST_ASSERT_FALSE(display_policy_epoch_is_current(old_epoch));
    TEST_ASSERT_EQUAL(1, display_mode_active());
    TEST_ASSERT_FALSE(display_policy_manual_screen_active());
}

static void test_failed_request_restores_previous_owner(void)
{
    reset_state();
    unsigned epoch = 0;
    TEST_ASSERT_EQUAL(ESP_OK, display_mode_show_request(1, &epoch));
    TEST_ASSERT_EQUAL(1, display_mode_active());
    TEST_ASSERT_FALSE(display_policy_manual_screen_active());

    TEST_ASSERT_EQUAL(ESP_FAIL, display_mode_show_request(2, &epoch));
    TEST_ASSERT_EQUAL(1, display_mode_active());
    TEST_ASSERT_FALSE(display_policy_manual_screen_active());
}

static void test_failed_newer_request_restores_committed_owner(void)
{
    reset_state();
    unsigned epoch = 0;
    TEST_ASSERT_EQUAL(ESP_OK, display_mode_show_request(1, &epoch));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      display_mode_show_request(5, &epoch));
    TEST_ASSERT_EQUAL(1, display_mode_active());
    TEST_ASSERT_FALSE(display_policy_manual_screen_active());
}

static void test_unavailable_mode_does_not_take_ownership(void)
{
    reset_state();
    unsigned before = display_policy_display_epoch();
    unsigned epoch = UINT32_MAX;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      display_mode_show_request(3, &epoch));
    TEST_ASSERT_EQUAL(before, display_policy_display_epoch());
    TEST_ASSERT_EQUAL(before, epoch);
    TEST_ASSERT_EQUAL(-1, display_mode_active());
}

static void test_manual_policy_is_committed_with_mode(void)
{
    reset_state();
    TEST_ASSERT_EQUAL(ESP_OK, display_mode_show_request(4, NULL));
    TEST_ASSERT_EQUAL(4, display_mode_active());
    TEST_ASSERT_TRUE(display_policy_manual_screen_active());
}

static void test_async_completion_failure_rolls_back(void)
{
    reset_state();
    TEST_ASSERT_EQUAL(ESP_OK, display_mode_show_request(1, NULL));
    TEST_ASSERT_EQUAL(ESP_FAIL, display_mode_show_request(6, NULL));
    TEST_ASSERT_EQUAL(1, display_mode_active());
    TEST_ASSERT_FALSE(display_policy_manual_screen_active());
}

static void commit_page(int mode, bool manual)
{
    display_request_token_t token = display_policy_begin_request(mode, EPD_REFRESH_FULL);
    TEST_ASSERT_TRUE(display_policy_finish_request(&token, ESP_OK, manual));
}

static void test_normal_calendar_and_weather_allow_background_refresh(void)
{
    reset_state();
    commit_page(DISPLAY_MODE_CALENDAR, true);
    TEST_ASSERT_TRUE(display_policy_manual_screen_active());
    TEST_ASSERT_FALSE(display_policy_manual_content_owns_display());
    TEST_ASSERT_TRUE(display_policy_calendar_may_midnight_refresh());
    commit_page(DISPLAY_MODE_WEATHER, true);
    TEST_ASSERT_TRUE(display_policy_manual_screen_active());
    TEST_ASSERT_TRUE(display_policy_weather_may_render_full_page());
}

static void test_manual_content_blocks_background_and_rollback_restores_owner(void)
{
    const int modes[] = {DISPLAY_MODE_CLOCK, DISPLAY_MODE_CALENDAR, DISPLAY_MODE_WEATHER,
                         DISPLAY_MODE_CODEX_QUOTA};
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        reset_state();
        commit_page(modes[i], modes[i] != DISPLAY_MODE_CLOCK);
        unsigned content = display_policy_begin_manual_display();
        TEST_ASSERT_TRUE(display_policy_finish_epoch(content, ESP_OK, true));
        TEST_ASSERT_TRUE(display_policy_manual_content_owns_display());
        display_policy_set_manual_screen_active(false);
        TEST_ASSERT_TRUE(display_policy_manual_content_owns_display());
        TEST_ASSERT_FALSE(display_policy_clock_may_auto_refresh());
        TEST_ASSERT_FALSE(display_policy_calendar_may_midnight_refresh());
        TEST_ASSERT_FALSE(display_policy_weather_may_render_full_page());
        display_request_token_t failed = display_policy_begin_request(
            DISPLAY_MODE_CLOCK, EPD_REFRESH_FULL);
        TEST_ASSERT_TRUE(display_policy_finish_request(&failed, ESP_ERR_NO_MEM, false));
        TEST_ASSERT_EQUAL(modes[i], display_mode_active());
        TEST_ASSERT_TRUE(display_policy_manual_content_owns_display());
        display_request_token_t background;
        TEST_ASSERT_FALSE(display_policy_try_begin_background(modes[i], failed.epoch,
                                                               EPD_REFRESH_FULL, &background));
    }
}

static void test_pending_request_blocks_background_and_failure_releases_it(void)
{
    const int modes[] = {DISPLAY_MODE_CLOCK, DISPLAY_MODE_CALENDAR,
                         DISPLAY_MODE_WEATHER, DISPLAY_MODE_SLIDESHOW,
                         DISPLAY_MODE_CODEX_QUOTA};
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        reset_state();
        commit_page(modes[i], modes[i] == DISPLAY_MODE_CALENDAR || modes[i] == DISPLAY_MODE_WEATHER);
        display_request_token_t request = display_policy_begin_request(
            DISPLAY_POLICY_KEEP_MODE, EPD_REFRESH_FAST);
        TEST_ASSERT_TRUE(display_policy_request_pending());
        TEST_ASSERT_FALSE(display_policy_clock_may_auto_refresh());
        TEST_ASSERT_FALSE(display_policy_calendar_may_midnight_refresh());
        TEST_ASSERT_FALSE(display_policy_weather_may_render_full_page());
        display_request_token_t background;
        TEST_ASSERT_FALSE(display_policy_try_begin_background(modes[i], request.epoch,
                                                               EPD_REFRESH_FULL, &background));
        TEST_ASSERT_TRUE(display_policy_finish_request(&request, ESP_FAIL, true));
        TEST_ASSERT_FALSE(display_policy_request_pending());
        int notifications = s_manual_notifications;
        TEST_ASSERT_TRUE(display_policy_try_begin_background(modes[i], request.epoch,
                                                              EPD_REFRESH_FULL, &background));
        TEST_ASSERT_EQUAL(notifications, s_manual_notifications);
        TEST_ASSERT_TRUE(display_policy_finish_request(&background, ESP_FAIL, true));
    }
}

static void test_background_does_not_adopt_epoch_after_newer_request_finishes(void)
{
    reset_state();
    commit_page(DISPLAY_MODE_CALENDAR, true);
    unsigned observed = display_policy_display_epoch();
    commit_page(DISPLAY_MODE_CALENDAR, true);
    unsigned latest = display_policy_display_epoch();
    display_request_token_t background;
    TEST_ASSERT_FALSE(display_policy_try_begin_background(DISPLAY_MODE_CALENDAR, observed,
                                                           EPD_REFRESH_FULL, &background));
    TEST_ASSERT_EQUAL(latest, display_policy_display_epoch());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      display_mode_show_background(DISPLAY_MODE_CALENDAR, observed));
    TEST_ASSERT_EQUAL(latest, display_policy_display_epoch());
}

static void test_per_epoch_results_retain_first_outcome(void)
{
    reset_state();
    unsigned failed = display_policy_begin_manual_display();
    TEST_ASSERT_EQUAL(DISPLAY_RESULT_PENDING, display_policy_get_result(failed).state);
    TEST_ASSERT_TRUE(display_policy_finish_epoch(failed, ESP_ERR_INVALID_STATE, true));
    TEST_ASSERT_TRUE(display_policy_finish_epoch(failed, ESP_OK, true));
    display_result_t result = display_policy_wait_result(failed);
    TEST_ASSERT_EQUAL(DISPLAY_RESULT_ERROR, result.state);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, result.error);
    unsigned canceled = display_policy_begin_manual_display();
    unsigned successful = display_policy_begin_manual_display();
    TEST_ASSERT_FALSE(display_policy_finish_epoch(canceled, ESP_OK, true));
    TEST_ASSERT_TRUE(display_policy_finish_epoch(successful, ESP_OK, true));
    TEST_ASSERT_EQUAL(DISPLAY_RESULT_CANCELED, display_policy_wait_result(canceled).state);
    TEST_ASSERT_EQUAL(DISPLAY_RESULT_ERROR, display_policy_wait_result(failed).state);
    TEST_ASSERT_EQUAL(DISPLAY_RESULT_OK, display_policy_wait_result(successful).state);
    (void)display_policy_begin_manual_display();
    TEST_ASSERT_EQUAL(DISPLAY_RESULT_OK, display_policy_wait_result(successful).state);
}

static void test_expired_result_is_not_another_epoch_success(void)
{
    reset_state();
    unsigned first = display_policy_begin_manual_display();
    TEST_ASSERT_TRUE(display_policy_finish_epoch(first, ESP_FAIL, true));
    for (int i = 0; i < 40; i++)
        commit_page(DISPLAY_MODE_CALENDAR, true);
    display_result_t result = display_policy_wait_result(first);
    TEST_ASSERT_EQUAL(DISPLAY_RESULT_ERROR, result.state);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, result.error);
}

static void test_mode_callback_cannot_overwrite_first_completion_failure(void)
{
    reset_state();
    commit_page(DISPLAY_MODE_CALENDAR, true);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, display_mode_show_request(8, NULL));
    TEST_ASSERT_EQUAL(DISPLAY_MODE_CALENDAR, display_mode_active());
}

static void test_quick_refresh_blocks_background_but_allows_explicit_requests(void)
{
    const int modes[] = {DISPLAY_MODE_CLOCK, DISPLAY_MODE_CALENDAR, DISPLAY_MODE_WEATHER,
                         DISPLAY_MODE_SLIDESHOW, DISPLAY_MODE_CODEX_QUOTA};
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        reset_state();
        commit_page(modes[i], modes[i] != DISPLAY_MODE_CLOCK && modes[i] != DISPLAY_MODE_SLIDESHOW);
        display_policy_set_quick_refresh_active(true);
        unsigned observed = display_policy_display_epoch();
        display_request_token_t background;
        TEST_ASSERT_FALSE(display_policy_try_begin_background(modes[i], observed,
                                                               EPD_REFRESH_FULL, &background));
        TEST_ASSERT_EQUAL(observed, display_policy_display_epoch());
        TEST_ASSERT_FALSE(display_policy_weather_may_network_fetch(false));
        TEST_ASSERT_TRUE(display_policy_weather_may_network_fetch(true));
        commit_page(modes[i], true);
    }
}

static void test_superseding_failure_preserves_committed_manual_content(void)
{
    reset_state();
    commit_page(DISPLAY_MODE_WEATHER, true);
    unsigned content = display_policy_begin_manual_display();
    TEST_ASSERT_TRUE(display_policy_finish_epoch(content, ESP_OK, true));
    display_request_token_t old = display_policy_begin_request(DISPLAY_MODE_CLOCK,
                                                               EPD_REFRESH_FULL);
    display_request_token_t newer = display_policy_begin_request(DISPLAY_MODE_CALENDAR,
                                                                 EPD_REFRESH_FULL);
    TEST_ASSERT_TRUE(display_policy_finish_request(&newer, ESP_FAIL, true));
    TEST_ASSERT_FALSE(display_policy_finish_request(&old, ESP_OK, false));
    TEST_ASSERT_EQUAL(DISPLAY_MODE_WEATHER, display_mode_active());
    TEST_ASSERT_TRUE(display_policy_manual_content_owns_display());
    TEST_ASSERT_FALSE(display_policy_weather_may_render_full_page());
    TEST_ASSERT_EQUAL(DISPLAY_RESULT_CANCELED, display_policy_wait_result(old.epoch).state);
    TEST_ASSERT_EQUAL(DISPLAY_RESULT_ERROR, display_policy_wait_result(newer.epoch).state);
}

static void test_conditional_boot_acquisition_chains_only_its_own_epochs(void)
{
    reset_state();
    display_request_token_t welcome;
    TEST_ASSERT_TRUE(display_policy_try_begin_request(DISPLAY_POLICY_KEEP_MODE, 0,
                                                       EPD_REFRESH_FULL, &welcome));
    TEST_ASSERT_TRUE(display_policy_finish_request(&welcome, ESP_OK, true));
    TEST_ASSERT_TRUE(display_policy_manual_content_owns_display());
    unsigned epoch = welcome.epoch;
    TEST_ASSERT_EQUAL(ESP_OK, display_mode_show_if_current(1, epoch, &epoch));
    TEST_ASSERT_TRUE(epoch != welcome.epoch);
    TEST_ASSERT_FALSE(display_policy_manual_content_owns_display());
    int notifications = s_manual_notifications;
    int refreshes = s_full_refresh_requests;
    display_request_token_t rejected = {.epoch = 123};
    TEST_ASSERT_FALSE(display_policy_try_begin_request(DISPLAY_MODE_CALENDAR, welcome.epoch,
                                                        EPD_REFRESH_FULL, &rejected));
    TEST_ASSERT_EQUAL(123, rejected.epoch);
    TEST_ASSERT_EQUAL(epoch, display_policy_display_epoch());
    TEST_ASSERT_EQUAL(notifications, s_manual_notifications);
    TEST_ASSERT_EQUAL(refreshes, s_full_refresh_requests);
}

static void test_slideshow_can_acquire_clock_but_not_startup_or_manual_content(void)
{
    reset_state();
    commit_page(DISPLAY_MODE_CLOCK, false);
    display_request_token_t slide;
    TEST_ASSERT_TRUE(display_policy_try_begin_background(DISPLAY_MODE_SLIDESHOW,
                        display_policy_display_epoch(), EPD_REFRESH_FULL, &slide));
    TEST_ASSERT_EQUAL(DISPLAY_MODE_CLOCK, slide.previous_mode);
    TEST_ASSERT_TRUE(display_policy_finish_request(&slide, ESP_OK, false));
    TEST_ASSERT_EQUAL(DISPLAY_MODE_SLIDESHOW, display_mode_active());

    commit_page(DISPLAY_MODE_CLOCK, false);
    display_policy_set_manual_screen_active(true);
    TEST_ASSERT_FALSE(display_policy_try_begin_background(DISPLAY_MODE_SLIDESHOW,
                        display_policy_display_epoch(), EPD_REFRESH_FULL, &slide));
    display_policy_set_manual_screen_active(false);
    unsigned content = display_policy_begin_manual_display();
    TEST_ASSERT_TRUE(display_policy_finish_epoch(content, ESP_OK, true));
    display_policy_set_manual_screen_active(false);
    TEST_ASSERT_TRUE(display_policy_manual_content_owns_display());
    TEST_ASSERT_FALSE(display_policy_try_begin_background(DISPLAY_MODE_SLIDESHOW,
                        display_policy_display_epoch(), EPD_REFRESH_FULL, &slide));
    TEST_ASSERT_EQUAL(DISPLAY_MODE_CLOCK, display_mode_active());
}

static void test_conditional_begin_rejects_current_pending_request(void)
{
    reset_state();
    unsigned user = display_policy_begin_manual_display();
    display_request_token_t token;
    TEST_ASSERT_FALSE(display_policy_try_begin_request(DISPLAY_POLICY_KEEP_MODE, user,
                                                        EPD_REFRESH_FULL, &token));
    unsigned out = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, display_mode_show_if_current(1, user, &out));
    TEST_ASSERT_EQUAL(user, out);
    TEST_ASSERT_EQUAL(user, display_policy_display_epoch());
    TEST_ASSERT_EQUAL(DISPLAY_RESULT_PENDING, display_policy_get_result(user).state);
}

static void test_unavailable_boot_mode_cannot_adopt_user_epoch_for_fallback(void)
{
    reset_state();
    unsigned guard = display_policy_display_epoch();
    unsigned user = display_policy_begin_manual_display();
    TEST_ASSERT_TRUE(display_policy_finish_epoch(user, ESP_OK, true));
    unsigned out = 123;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, display_mode_show_if_current(3, guard, &out));
    TEST_ASSERT_EQUAL(guard, out);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, display_mode_show_if_current(1, out, &out));
    TEST_ASSERT_EQUAL(user, display_policy_display_epoch());
    TEST_ASSERT_TRUE(display_policy_manual_content_owns_display());
}

static void test_conditional_fallback_after_failure_requires_failed_request_epoch(void)
{
    reset_state();
    unsigned guard = 0;
    TEST_ASSERT_EQUAL(ESP_FAIL, display_mode_show_if_current(2, guard, &guard));
    TEST_ASSERT_EQUAL(ESP_OK, display_mode_show_if_current(1, guard, &guard));
    TEST_ASSERT_EQUAL(1, display_mode_active());

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, display_mode_show_if_current(5, guard, &guard));
    unsigned newer = display_policy_display_epoch();
    TEST_ASSERT_TRUE(newer != guard);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, display_mode_show_if_current(1, guard, &guard));
    TEST_ASSERT_EQUAL(newer, display_policy_display_epoch());
}

#ifdef MSP_HOST_TEST
static atomic_bool s_race_start;
static bool s_race_conditional;
static unsigned s_race_observed;
static unsigned s_race_user_epoch;
static void *race_user(void *arg)
{
    while (!atomic_load(&s_race_start)) { }
    s_race_user_epoch = display_policy_begin_manual_display();
    return NULL;
}
static void *race_background(void *arg)
{
    while (!atomic_load(&s_race_start)) { }
    display_request_token_t token;
    bool acquired = s_race_conditional
        ? display_policy_try_begin_request(DISPLAY_MODE_CALENDAR, s_race_observed,
                                            EPD_REFRESH_PARTIAL, &token)
        : display_policy_try_begin_background(DISPLAY_MODE_CALENDAR, s_race_observed,
                                               EPD_REFRESH_PARTIAL, &token);
    if (acquired)
        (void)display_policy_finish_request(&token, ESP_OK, true);
    return NULL;
}
static void run_user_acquisition_race(bool conditional)
{
    s_race_conditional = conditional;
    for (int i = 0; i < 100; i++) {
        reset_state();
        commit_page(DISPLAY_MODE_CALENDAR, true);
        s_race_observed = display_policy_display_epoch();
        atomic_store(&s_race_start, false);
        pthread_t user, background;
        TEST_ASSERT_EQUAL(0, pthread_create(&user, NULL, race_user, NULL));
        TEST_ASSERT_EQUAL(0, pthread_create(&background, NULL, race_background, NULL));
        atomic_store(&s_race_start, true);
        pthread_join(user, NULL);
        pthread_join(background, NULL);
        TEST_ASSERT_EQUAL(s_race_user_epoch, display_policy_display_epoch());
        TEST_ASSERT_TRUE(display_policy_request_pending());
    }
}

static void test_user_wins_concurrent_background_acquisition(void)
{
    run_user_acquisition_race(false);
}

static void test_user_wins_concurrent_conditional_boot_acquisition(void)
{
    run_user_acquisition_race(true);
}
#endif

void app_main(void)
{
    display_mode_register(&(display_mode_entry_t){
        "old", "旧请求", show_superseded, hold_manual, true, NULL});
    display_mode_register(&(display_mode_entry_t){
        "clock", "时钟", show_ok, release_manual, true, NULL});
    display_mode_register(&(display_mode_entry_t){
        "fail", "失败", show_fail, hold_manual, true, NULL});
    display_mode_register(&(display_mode_entry_t){
        "disabled", "不可用", show_ok, hold_manual, false, NULL});
    display_mode_register(&(display_mode_entry_t){
        "manual", "手动画面", show_ok, hold_manual, true, NULL});
    display_mode_register(&(display_mode_entry_t){
        "old_fail", "被失败请求抢占", show_superseded_by_failure,
        hold_manual, true, NULL});
    display_mode_register(&(display_mode_entry_t){
        "async_fail", "异步完成失败", show_ok, hold_manual, true,
        complete_fail});
    display_mode_register(&(display_mode_entry_t){
        "codex", "quota", show_ok, hold_manual, true, NULL});
    display_mode_register(&(display_mode_entry_t){
        "early_failure", "early failure", show_finishes_failure, hold_manual, true, NULL});

    UNITY_BEGIN();
    RUN_TEST(test_newer_request_wins_when_old_show_finishes_last);
    RUN_TEST(test_failed_request_restores_previous_owner);
    RUN_TEST(test_failed_newer_request_restores_committed_owner);
    RUN_TEST(test_unavailable_mode_does_not_take_ownership);
    RUN_TEST(test_manual_policy_is_committed_with_mode);
    RUN_TEST(test_async_completion_failure_rolls_back);
    RUN_TEST(test_normal_calendar_and_weather_allow_background_refresh);
    RUN_TEST(test_manual_content_blocks_background_and_rollback_restores_owner);
    RUN_TEST(test_pending_request_blocks_background_and_failure_releases_it);
    RUN_TEST(test_background_does_not_adopt_epoch_after_newer_request_finishes);
    RUN_TEST(test_per_epoch_results_retain_first_outcome);
    RUN_TEST(test_expired_result_is_not_another_epoch_success);
    RUN_TEST(test_mode_callback_cannot_overwrite_first_completion_failure);
    RUN_TEST(test_quick_refresh_blocks_background_but_allows_explicit_requests);
    RUN_TEST(test_superseding_failure_preserves_committed_manual_content);
    RUN_TEST(test_conditional_boot_acquisition_chains_only_its_own_epochs);
    RUN_TEST(test_slideshow_can_acquire_clock_but_not_startup_or_manual_content);
    RUN_TEST(test_conditional_begin_rejects_current_pending_request);
    RUN_TEST(test_unavailable_boot_mode_cannot_adopt_user_epoch_for_fallback);
    RUN_TEST(test_conditional_fallback_after_failure_requires_failed_request_epoch);
#ifdef MSP_HOST_TEST
    RUN_TEST(test_user_wins_concurrent_background_acquisition);
    RUN_TEST(test_user_wins_concurrent_conditional_boot_acquisition);
    host_run_board_tests();
#endif
    int failures = UNITY_END();

#if CONFIG_IDF_TARGET_LINUX || defined(MSP_HOST_TEST)
    exit(failures == 0 ? 0 : 1);
#else
    (void)failures;
#endif
}
