#include <string.h>
#include "unity.h"
#include "host_support.h"
#include "message_board.h"
#include "canvas_board.h"
#include "display_mode.h"

static void reset_board_state(void)
{
    host_run_tasks();
    host_reset_faults();
    display_policy_init();
    display_mode_set_active(-1);
    display_request_token_t page = display_policy_begin_request(DISPLAY_MODE_CALENDAR,
                                                                EPD_REFRESH_FULL);
    TEST_ASSERT_TRUE(display_policy_finish_request(&page, ESP_OK, true));
    msg_config_t cfg = {.text = "host message", .align = MSG_ALIGN_CENTER};
    TEST_ASSERT_EQUAL(ESP_OK, message_board_set_config(&cfg));
}

static void assert_error(display_result_t result, esp_err_t error)
{
    TEST_ASSERT_EQUAL(DISPLAY_RESULT_ERROR, result.state);
    TEST_ASSERT_EQUAL(error, result.error);
    TEST_ASSERT_EQUAL(DISPLAY_MODE_CALENDAR, display_mode_active());
    TEST_ASSERT_TRUE(display_policy_calendar_may_midnight_refresh());
    TEST_ASSERT_FALSE(display_policy_manual_content_owns_display());
}

static void test_message_framebuffer_failure_is_returned_per_epoch(void)
{
    reset_board_state();
    host_fail_framebuffer = true;
    unsigned epoch;
    TEST_ASSERT_EQUAL(ESP_OK, message_board_show_queued(&epoch));
    assert_error(message_board_wait_result(epoch), ESP_ERR_NO_MEM);
    TEST_ASSERT_EQUAL(0, host_display_calls);
}

static void test_message_export_and_epd_failures_are_not_success_or_cancellation(void)
{
    reset_board_state();
    host_export_result = ESP_FAIL;
    unsigned epoch;
    TEST_ASSERT_EQUAL(ESP_OK, message_board_show_queued(&epoch));
    assert_error(message_board_wait_result(epoch), ESP_FAIL);
    TEST_ASSERT_EQUAL(0, host_display_calls);

    host_export_result = ESP_OK;
    host_display_result = ESP_ERR_INVALID_STATE;
    TEST_ASSERT_EQUAL(ESP_OK, message_board_show_queued(&epoch));
    assert_error(message_board_wait_result(epoch), ESP_ERR_INVALID_STATE);
    TEST_ASSERT_EQUAL(1, host_display_calls);
}

static void test_message_queued_before_worker_start_is_not_lost(void)
{
    reset_board_state();
    unsigned first, second, latest;
    TEST_ASSERT_EQUAL(ESP_OK, message_board_show_queued(&first));
    TEST_ASSERT_EQUAL(ESP_OK, message_board_show_queued(&second));
    TEST_ASSERT_EQUAL(ESP_OK, message_board_show_queued(&latest));
    TEST_ASSERT_EQUAL(DISPLAY_RESULT_CANCELED, message_board_wait_result(first).state);
    TEST_ASSERT_EQUAL(DISPLAY_RESULT_CANCELED, message_board_wait_result(second).state);
    TEST_ASSERT_EQUAL(DISPLAY_RESULT_OK, message_board_wait_result(latest).state);
    TEST_ASSERT_EQUAL(1, host_display_calls);
    TEST_ASSERT_TRUE(display_policy_manual_content_owns_display());
    message_board_wait_idle();
    TEST_ASSERT_EQUAL(DISPLAY_RESULT_CANCELED, message_board_wait_result(first).state);
}

static void test_queued_message_emptied_before_render_returns_error(void)
{
    reset_board_state();
    unsigned first, latest;
    TEST_ASSERT_EQUAL(ESP_OK, message_board_show_queued(&first));
    TEST_ASSERT_EQUAL(ESP_OK, message_board_show_queued(&latest));
    msg_config_t empty = {0};
    TEST_ASSERT_EQUAL(ESP_OK, message_board_set_config(&empty));
    assert_error(message_board_wait_result(latest), ESP_ERR_INVALID_STATE);
    TEST_ASSERT_EQUAL(DISPLAY_RESULT_CANCELED, message_board_wait_result(first).state);
    TEST_ASSERT_EQUAL(0, host_display_calls);
}

static void test_canvas_missing_layout_and_malloc_failures_are_returned(void)
{
    reset_board_state();
    host_canvas_layout = NULL;
    unsigned missing, no_memory;
    TEST_ASSERT_EQUAL(ESP_OK, canvas_board_show_queued(&missing));
    assert_error(canvas_board_wait_result(missing), ESP_ERR_NOT_FOUND);

    host_canvas_layout = "[]";
    host_fail_canvas_malloc = true;
    TEST_ASSERT_EQUAL(ESP_OK, canvas_board_show_queued(&no_memory));
    assert_error(canvas_board_wait_result(no_memory), ESP_ERR_NO_MEM);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, canvas_board_wait_result(missing).error);
    TEST_ASSERT_EQUAL(0, host_display_calls);
}

static void test_canvas_framebuffer_parse_and_epd_failures_are_returned(void)
{
    reset_board_state();
    unsigned epoch;
    host_fail_framebuffer = true;
    TEST_ASSERT_EQUAL(ESP_OK, canvas_board_show_queued(&epoch));
    assert_error(canvas_board_wait_result(epoch), ESP_ERR_NO_MEM);
    host_fail_framebuffer = false;
    host_canvas_layout = "invalid";
    TEST_ASSERT_EQUAL(ESP_OK, canvas_board_show_queued(&epoch));
    assert_error(canvas_board_wait_result(epoch), ESP_ERR_INVALID_ARG);
    host_canvas_layout = "[]";
    host_display_result = ESP_ERR_TIMEOUT;
    TEST_ASSERT_EQUAL(ESP_OK, canvas_board_show_queued(&epoch));
    assert_error(canvas_board_wait_result(epoch), ESP_ERR_TIMEOUT);
    TEST_ASSERT_EQUAL(1, host_display_calls);
}

static void test_task_creation_failure_records_result_and_releases_worker(void)
{
    reset_board_state();
    host_fail_task_create = true;
    unsigned message, canvas;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, message_board_show_queued(&message));
    assert_error(message_board_wait_result(message), ESP_ERR_NO_MEM);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, canvas_board_show_queued(&canvas));
    assert_error(canvas_board_wait_result(canvas), ESP_ERR_NO_MEM);
    host_fail_task_create = false;
    TEST_ASSERT_EQUAL(ESP_OK, canvas_board_show_queued(&canvas));
    TEST_ASSERT_EQUAL(DISPLAY_RESULT_OK, canvas_board_wait_result(canvas).state);
}

static void test_canvas_busy_rejection_does_not_cancel_accepted_request(void)
{
    reset_board_state();
    unsigned accepted, rejected = 123;
    TEST_ASSERT_EQUAL(ESP_OK, canvas_board_show_queued(&accepted));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, canvas_board_show_queued(&rejected));
    TEST_ASSERT_EQUAL(0, rejected);
    TEST_ASSERT_EQUAL(accepted, display_policy_display_epoch());
    TEST_ASSERT_EQUAL(DISPLAY_RESULT_OK, canvas_board_wait_result(accepted).state);
}

static void test_message_and_canvas_superseded_during_refresh_return_canceled(void)
{
    reset_board_state();
    host_supersede_during_display = true;
    unsigned epoch;
    TEST_ASSERT_EQUAL(ESP_OK, message_board_show_queued(&epoch));
    TEST_ASSERT_EQUAL(DISPLAY_RESULT_CANCELED, message_board_wait_result(epoch).state);
    TEST_ASSERT_TRUE(display_policy_request_pending());

    TEST_ASSERT_EQUAL(ESP_OK, canvas_board_show_queued(&epoch));
    TEST_ASSERT_EQUAL(DISPLAY_RESULT_CANCELED, canvas_board_wait_result(epoch).state);
    TEST_ASSERT_TRUE(display_policy_request_pending());
    TEST_ASSERT_EQUAL(DISPLAY_MODE_CALENDAR, display_mode_active());
    TEST_ASSERT_FALSE(display_policy_manual_content_owns_display());
}

static void test_workers_pass_captured_epoch_across_epd_lock_wait(void)
{
    reset_board_state();
    host_supersede_before_epd_lock = true;
    unsigned epoch;
    TEST_ASSERT_EQUAL(ESP_OK, message_board_show_queued(&epoch));
    TEST_ASSERT_EQUAL(DISPLAY_RESULT_CANCELED, message_board_wait_result(epoch).state);
    TEST_ASSERT_EQUAL(ESP_OK, canvas_board_show_queued(&epoch));
    TEST_ASSERT_EQUAL(DISPLAY_RESULT_CANCELED, canvas_board_wait_result(epoch).state);
    TEST_ASSERT_EQUAL(0, host_display_calls);
}

void host_run_board_tests(void)
{
    host_reset_faults();
    TEST_ASSERT_EQUAL(ESP_OK, message_board_init());
    TEST_ASSERT_EQUAL(ESP_OK, canvas_board_init());
    RUN_TEST(test_message_framebuffer_failure_is_returned_per_epoch);
    RUN_TEST(test_message_export_and_epd_failures_are_not_success_or_cancellation);
    RUN_TEST(test_message_queued_before_worker_start_is_not_lost);
    RUN_TEST(test_queued_message_emptied_before_render_returns_error);
    RUN_TEST(test_canvas_missing_layout_and_malloc_failures_are_returned);
    RUN_TEST(test_canvas_framebuffer_parse_and_epd_failures_are_returned);
    RUN_TEST(test_task_creation_failure_records_result_and_releases_worker);
    RUN_TEST(test_canvas_busy_rejection_does_not_cancel_accepted_request);
    RUN_TEST(test_message_and_canvas_superseded_during_refresh_return_canceled);
    RUN_TEST(test_workers_pass_captured_epoch_across_epd_lock_wait);
}
