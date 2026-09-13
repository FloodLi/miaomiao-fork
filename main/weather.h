#pragma once

#include "esp_err.h"
#include "fb_render.h"
#include "ui_color_theme.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WEATHER_PROVIDER_OPEN_METEO = 0,
    WEATHER_PROVIDER_QWEATHER   = 1,
} weather_provider_t;

typedef struct {
    bool     enabled;
    weather_provider_t provider;
    char     api_key[48];        /* QWeather key; preserved when Open-Meteo is active. */
    char     api_host[64];       /* QWeather host, or Open-Meteo cached timezone. */
    char     location[32];       /* QWeather location ID, or Open-Meteo cached "lat,lon". */
    char     city_name[64];      /* User-entered city/display name. */
    uint32_t refresh_min;        /* 0=manual; nonzero auto mode normalizes to 30 min */
} weather_config_t;

typedef struct {
    int  temp;
    int  feels_like;
    int  humidity;
    int  pressure;
    char text[24];               /* e.g. "多云" */
    char wind_dir[16];
    int  wind_degree;            /* 0..359, or -1 when the provider omits it. */
    int  wind_scale;
    int  icon;
} weather_now_t;

typedef struct {
    char date[12];               /* "2026-03-16" */
    int  temp_max;
    int  temp_min;
    char text_day[24];
    int  icon_day;
} weather_daily_t;

typedef struct {
    char time[6];                 /* "HH:MM" */
    int  temp;
    int  icon;
} weather_hourly_t;

typedef struct {
    weather_now_t   now;
    weather_daily_t daily[3];
    int             daily_count;
    weather_hourly_t hourly[24];
    int             hourly_count;
    char            update_time[24]; /* full strftime buf, then truncated to "HH:MM" */
    bool            valid;
} weather_data_t;

typedef struct {
    weather_now_t   now;
    weather_daily_t daily[3];
    int             daily_count;
    char            update_time[24];
    bool            valid;
} weather_summary_t;

#define WEATHER_ICON_W 24
#define WEATHER_ICON_H 24

esp_err_t weather_init(void);
esp_err_t weather_get_config(weather_config_t *out);
esp_err_t weather_set_config(const weather_config_t *cfg);
const uint8_t *weather_icon_bitmap(int code);
const char *weather_text_localized(int code, const char *provider_text);
const char *weather_wind_dir_localized(int degree, const char *provider_text);
fb_color_t weather_icon_color_for(ui_color_page_t page, int code,
                                  const char *text, fb_color_t fallback);
void weather_draw_icon(fb_t *fb, int x, int y, int code, const char *text,
                       int target_size, ui_color_page_t page,
                       fb_color_t fallback);
/* Direct fetch is synchronous; request_* schedules the large-stack weather task. */
esp_err_t weather_fetch_and_display(bool force_fullscreen);
esp_err_t weather_request_fullscreen_fetch(void);
esp_err_t weather_request_fullscreen_fetch_wait(uint32_t timeout_ms);
esp_err_t weather_request_cache_fetch_wait(uint32_t timeout_ms);

/** Display cached weather data on EPD without network fetch. Returns ESP_ERR_INVALID_STATE if no data cached. */
esp_err_t weather_display_cached(void);

/** 线程安全：在 s_fetch_mutex 保护下拷贝一份天气数据。out 必填。 */
void weather_get_data_copy(weather_data_t *out);
void weather_get_summary_copy(weather_summary_t *out);
int weather_data_signature(void);

/** 供时钟「显示天气摘要」调用：在已配置 API 时触发一次拉取（不抢整页） */
void weather_request_embedded_refresh(void);

/** 轮播从开启变为关闭后调用，便于恢复时钟天气摘要的拉取 */
void weather_notify_slideshow_stopped(void);

/** 在创建 boot_wx 任务之前调用；若随后 xTaskCreate 失败，请调用 weather_skip_initial_task_fetch_cancel */
void weather_skip_initial_task_fetch_once(void);
void weather_skip_initial_task_fetch_cancel(void);
void weather_set_quick_refresh_network_allowed(bool allowed);
