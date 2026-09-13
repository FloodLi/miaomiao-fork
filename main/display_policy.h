#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "epd.h"

#define DISPLAY_POLICY_KEEP_MODE (-2)

typedef struct {
    uint32_t epoch;
    int target_mode;
    int previous_mode;
    bool previous_manual;
    bool previous_content_owner;
} display_request_token_t;

typedef enum {
    DISPLAY_RESULT_PENDING,
    DISPLAY_RESULT_OK,
    DISPLAY_RESULT_CANCELED,
    DISPLAY_RESULT_ERROR,
} display_result_state_t;

typedef struct {
    display_result_state_t state;
    esp_err_t error;
} display_result_t;

/* Results retain the last 32 transactions. Expired/unknown epochs return
 * ERROR/ESP_ERR_NOT_FOUND, never another request's outcome. */
display_result_t display_policy_get_result(unsigned epoch);
display_result_t display_policy_wait_result(unsigned epoch);
bool display_policy_manual_content_owns_display(void);
bool display_policy_request_pending(void);

/* Start the next request only if the observed epoch is still current and
 * complete. Unlike background acquisition this may replace committed content. */
bool display_policy_try_begin_request(int target_mode, unsigned expected_epoch,
                                      epd_refresh_mode_t mode,
                                      display_request_token_t *out);

/* Compare the caller's pre-work snapshot and acquire under the policy lock.
 * Background callers must never adopt a newly observed user epoch. */
bool display_policy_try_begin_background(int target_mode, unsigned expected_epoch,
                                          epd_refresh_mode_t mode,
                                          display_request_token_t *out);

/**
 * 墨水屏显示仲裁：幻灯片 / 时钟 / 天气全页 / 留言与日历等「手动全屏」互斥，
 * 避免多任务同时往 EPD 推画面。
 */
void display_policy_init(void);
void display_policy_set_quick_refresh_active(bool active);
bool display_policy_quick_refresh_active(void);

/** 幻灯片开启且应由轮播占用屏幕时 */
bool display_policy_slideshow_owns_display(void);

/**
 * 时钟是否允许自动按分钟刷新（幻灯片开、或存在手动全屏内容时为否）。
 */
bool display_policy_clock_may_auto_refresh(void);
bool display_policy_calendar_may_midnight_refresh(void);

/**
 * 天气拉取后是否刷「整页天气」：时钟作为主页且非用户显式请求时只更新数据，由时钟界面展示。
 * @param user_explicit 用户通过 /weather_show 等主动要求全页天气时为 true。
 */
bool display_policy_weather_use_full_page(bool user_explicit);

/**
 * 是否允许发起天气 API（HTTPS）：轮播占屏时禁止后台拉取，仅网页「显示天气」等 user_explicit 请求除外。
 */
bool display_policy_weather_may_network_fetch(bool user_explicit);

/** 后台天气刷新是否仍可占用整页显示。 */
bool display_policy_weather_may_render_full_page(void);

/** 留言板、日历、HTTP 看图、上传预览等全屏内容展示中 */
void display_policy_set_manual_screen_active(bool active);
bool display_policy_manual_screen_active(void);
display_request_token_t display_policy_begin_request(int target_mode,
                                                     epd_refresh_mode_t mode);
bool display_policy_finish_request(const display_request_token_t *token,
                                   esp_err_t result,
                                   bool manual_after_success);
bool display_policy_finish_epoch(unsigned epoch, esp_err_t result,
                                 bool manual_after_success);
unsigned display_policy_begin_manual_display(void);
unsigned display_policy_begin_manual_display_mode(epd_refresh_mode_t mode);
bool display_policy_epoch_is_current(unsigned epoch);

/** 每次用户主动发起全屏显示时递增；长耗时显示可用它判断自己是否已过期。 */
void display_policy_bump_display_epoch(void);
unsigned display_policy_display_epoch(void);
