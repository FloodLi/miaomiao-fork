#pragma once

#include "esp_err.h"

esp_err_t calendar_display_init(void);
esp_err_t calendar_display_show(int year, int month);
esp_err_t calendar_display_show_current(void);
/** 阻塞直到异步月历渲染任务结束（定时唤醒 quick-refresh 进深睡前须调用） */
void calendar_display_wait_render_idle(void);
