#pragma once

#include <stdbool.h>

/**
 * 墨水屏显示仲裁：幻灯片 / 时钟 / 天气全页 / 留言与日历等「手动全屏」互斥，
 * 避免多任务同时往 EPD 推画面。
 */
void display_policy_init(void);

/** 幻灯片开启且应由轮播占用屏幕时 */
bool display_policy_slideshow_owns_display(void);

/**
 * 时钟是否允许自动按分钟刷新（幻灯片开、或存在手动全屏内容时为否）。
 */
bool display_policy_clock_may_auto_refresh(void);

/**
 * 天气拉取后是否刷「整页天气」：时钟作为主页且非用户显式请求时只更新数据，由时钟界面展示。
 * @param user_explicit 用户通过 /weather_show 等主动要求全页天气时为 true。
 */
bool display_policy_weather_use_full_page(bool user_explicit);

/**
 * 是否允许发起天气 API（HTTPS）：轮播占屏时禁止后台拉取，仅网页「显示天气」等 user_explicit 请求除外。
 */
bool display_policy_weather_may_network_fetch(bool user_explicit);

/** 留言板、日历、HTTP 看图、上传预览等全屏内容展示中 */
void display_policy_set_manual_screen_active(bool active);
bool display_policy_manual_screen_active(void);
