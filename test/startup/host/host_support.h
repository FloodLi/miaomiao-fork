#pragma once
#include <stdbool.h>
#include "esp_err.h"
extern bool host_fail_task_create;
extern bool host_fail_framebuffer;
extern bool host_fail_canvas_malloc;
extern bool host_supersede_during_display;
extern bool host_supersede_before_epd_lock;
extern const char *host_canvas_layout;
extern esp_err_t host_export_result;
extern esp_err_t host_display_result;
extern unsigned host_display_calls;
void host_run_tasks(void);
void host_reset_faults(void);
void host_run_board_tests(void);
