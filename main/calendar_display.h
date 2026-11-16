#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t calendar_display_init(void);
esp_err_t calendar_display_show(int year, int month);
esp_err_t calendar_display_show_current(void);
esp_err_t calendar_display_toggle_style(void);
bool calendar_display_style_uses_weather(void);
bool calendar_display_needs_midnight_refresh(void);

/** 开学时间（月/日）：0=未配置。 */
void calendar_get_school_start(int *month, int *day);
/** 保存开学时间（0,0=清除）；日期非法返回 ESP_ERR_INVALID_ARG，NVS 失败返回底层错误。 */
esp_err_t calendar_set_school_start(int month, int day);
/** 当前“开学第X周”：0=未配置或时间未同步；N=开学第N周。
 * 规则：开学日起为第1周（到当周周日为止），之后每周一到周日为下一周；
 * 开学年份自动取不晚于今天的最近一次（月/日）出现（跨年回溯到去年）。 */
int calendar_school_week_now(void);

/* Block until the async calendar render task has finished. */
void calendar_display_wait_render_idle(void);
esp_err_t calendar_display_wait_render_result(void);
