#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     enabled;
    char     api_key[48];
    char     api_host[64];       /* e.g. "xxx.re.qweatherapi.com" (no https://) */
    char     location[32];       /* QWeather location ID or "lon,lat" */
    char     city_name[32];
    uint32_t refresh_min;        /* auto-refresh interval in minutes (0=manual) */
} weather_config_t;

typedef struct {
    int  temp;
    int  feels_like;
    int  humidity;
    int  pressure;
    char text[24];               /* e.g. "多云" */
    char wind_dir[16];
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
    weather_now_t   now;
    weather_daily_t daily[3];
    int             daily_count;
    char            update_time[24]; /* full strftime buf, then truncated to "HH:MM" */
    bool            valid;
} weather_data_t;

esp_err_t weather_init(void);
esp_err_t weather_get_config(weather_config_t *out);
esp_err_t weather_set_config(const weather_config_t *cfg);
/** force_fullscreen: true = 始终刷整页天气（如 /weather_show）；false = 由显示策略决定（时钟主页时只拉数据） */
esp_err_t weather_fetch_and_display(bool force_fullscreen);
const weather_data_t *weather_get_data(void);

/** Display cached weather data on EPD without network fetch. Returns ESP_ERR_INVALID_STATE if no data cached. */
esp_err_t weather_display_cached(void);

/** 线程安全：在 s_fetch_mutex 保护下拷贝一份天气数据 */
void weather_get_data_copy(weather_data_t *out);

/** 供时钟「显示天气摘要」调用：在已配置 API 时触发一次拉取（不抢整页） */
void weather_request_embedded_refresh(void);

/** 轮播从开启变为关闭后调用，便于恢复时钟天气摘要的拉取 */
void weather_notify_slideshow_stopped(void);

/** 在创建 boot_wx 任务之前调用；若随后 xTaskCreate 失败，请调用 weather_skip_initial_task_fetch_cancel */
void weather_skip_initial_task_fetch_once(void);
void weather_skip_initial_task_fetch_cancel(void);
