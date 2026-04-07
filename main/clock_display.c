#include "clock_display.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include "fb_render.h"
#include "epd.h"
#include "scheduler.h"
#include "time_sync.h"
#include "weather.h"
#include "display_policy.h"

/* 摘要区占位（无有效天气数据时） */
static const char k_wx_placeholder_l1[] =
    "\xe5\xa4\xa9\xe6\xb0\x94\xe6\x95\xb0\xe6\x8d\xae\xe6\x9c\xaa\xe5\xb0\xb1\xe7\xbb\xaa"; /* 天气数据未就绪 */
static const char k_wx_placeholder_l2[] =
    "\xe8\xaf\xb7\xe5\x9c\xa8\xe3\x80\x8c\xe5\xa4\xa9\xe6\xb0\x94\xe3\x80\x8d\xe9\xa1\xb5\xe5\xa1\xab API \xe5\x90\x8e\xe7\x82\xb9\xe5\x88\xb7\xe6\x96\xb0"; /* 请在「天气」页填 API 后点刷新 */

static const char *TAG    = "clock";
static const char *NVS_NS = "clock";

#define BIT_CFG_CHANGED   BIT0
#define BIT_WEATHER_DATA  BIT1

static clock_config_t s_cfg = {
    .enabled      = false,
    .style        = 0,
    .show_weather = true,
};

static EventGroupHandle_t s_event;
static SemaphoreHandle_t  s_render_mutex;

/* ── 7-segment digit rendering ────────────────────────────────────── */

/*
 *    aaaa
 *   f    b
 *   f    b
 *    gggg
 *   e    c
 *   e    c
 *    dddd
 */

#define SEG_A 0x01
#define SEG_B 0x02
#define SEG_C 0x04
#define SEG_D 0x08
#define SEG_E 0x10
#define SEG_F 0x20
#define SEG_G 0x40

static const uint8_t digit_segs[10] = {
    SEG_A|SEG_B|SEG_C|SEG_D|SEG_E|SEG_F,        /* 0 */
    SEG_B|SEG_C,                                   /* 1 */
    SEG_A|SEG_B|SEG_D|SEG_E|SEG_G,                /* 2 */
    SEG_A|SEG_B|SEG_C|SEG_D|SEG_G,                /* 3 */
    SEG_B|SEG_C|SEG_F|SEG_G,                       /* 4 */
    SEG_A|SEG_C|SEG_D|SEG_F|SEG_G,                /* 5 */
    SEG_A|SEG_C|SEG_D|SEG_E|SEG_F|SEG_G,          /* 6 */
    SEG_A|SEG_B|SEG_C,                             /* 7 */
    SEG_A|SEG_B|SEG_C|SEG_D|SEG_E|SEG_F|SEG_G,   /* 8 */
    SEG_A|SEG_B|SEG_C|SEG_D|SEG_F|SEG_G,          /* 9 */
};

/* base geometry at 1x (56x88, seg_th=8) — scaled at runtime */
static const int seg_base[7][4] = {
    { 10,  0, 36,  8 },   /* A - top */
    { 48, 10,  8, 31 },   /* B - top right */
    { 48, 49,  8, 31 },   /* C - bottom right */
    { 10, 80, 36,  8 },   /* D - bottom */
    {  0, 49,  8, 31 },   /* E - bottom left */
    {  0, 10,  8, 31 },   /* F - top left */
    { 10, 40, 36,  8 },   /* G - middle */
};

#define BASE_DW   56
#define BASE_DH   88
#define BASE_COL  18

static void draw_digit_s(fb_t *fb, int x, int y, int digit,
                          fb_color_t color, int num, int den)
{
    if (digit < 0 || digit > 9) return;
    uint8_t segs = digit_segs[digit];
    for (int s = 0; s < 7; s++) {
        if (segs & (1 << s)) {
            fb_fill_rect(fb,
                x + seg_base[s][0] * num / den,
                y + seg_base[s][1] * num / den,
                seg_base[s][2] * num / den,
                seg_base[s][3] * num / den,
                color);
        }
    }
}

static void draw_colon_s(fb_t *fb, int x, int y, fb_color_t color,
                          int num, int den)
{
    int dh = BASE_DH * num / den;
    int dot = 8 * num / den; if (dot < 4) dot = 4;
    int cx = x + 4 * num / den;
    fb_fill_rect(fb, cx, y + dh * 3 / 10, dot, dot, color);
    fb_fill_rect(fb, cx, y + dh * 7 / 10, dot, dot, color);
}

/* ── weekday strings ──────────────────────────────────────────────── */

static const char *weekday_zh[] = {
    "\xe6\x97\xa5",  /* 日 */
    "\xe4\xb8\x80",  /* 一 */
    "\xe4\xba\x8c",  /* 二 */
    "\xe4\xb8\x89",  /* 三 */
    "\xe5\x9b\x9b",  /* 四 */
    "\xe4\xba\x94",  /* 五 */
    "\xe5\x85\xad",  /* 六 */
};

/* ── NVS ──────────────────────────────────────────────────────────── */

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    uint8_t v8;
    if (nvs_get_u8(h, "enabled", &v8) == ESP_OK) s_cfg.enabled = (v8 != 0);
    if (nvs_get_u8(h, "style", &v8) == ESP_OK) s_cfg.style = v8;
    if (nvs_get_u8(h, "show_w", &v8) == ESP_OK) s_cfg.show_weather = (v8 != 0);

    nvs_close(h);
}

static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "enabled", s_cfg.enabled ? 1 : 0);
    nvs_set_u8(h, "style",   s_cfg.style);
    nvs_set_u8(h, "show_w",  s_cfg.show_weather ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

/* ── render clock ─────────────────────────────────────────────────── */

static void render_clock(void)
{
    if (s_render_mutex && xSemaphoreTake(s_render_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "render_clock: another render in progress, skip");
        return;
    }

    fb_t *fb = fb_create();
    if (!fb) {
        ESP_LOGE(TAG, "fb_create failed");
        if (s_render_mutex)
            xSemaphoreGive(s_render_mutex);
        return;
    }

    struct tm tm;
    if (!time_sync_get_local(&tm)) {
        ESP_LOGW(TAG, "Time not synced yet");
        fb_destroy(fb);
        return;
    }

    int hour = tm.tm_hour;
    int min  = tm.tm_min;

    const int W = fb->width;
    const int H = fb->height;
    const int MX = 20;
    const bool big = (W >= 600);
    const int sc = big ? 2 : 1;

    /* digit scale: 648px -> num=3,den=2 (1.5x); 400px -> num=1,den=1 */
    const int ds_num = big ? 3 : 2;
    const int ds_den = big ? 2 : 2;
    int dw = BASE_DW * ds_num / ds_den;
    int dh = BASE_DH * ds_num / ds_den;
    int d_gap = dw + 6 * ds_num / ds_den;
    int col_w_px = BASE_COL * ds_num / ds_den;

    fb_rect(fb, 6, 6, W - 12, H - 12, COLOR_BLACK);
    fb_rect(fb, 9, 9, W - 18, H - 18, COLOR_BLACK);

    int total_w = 4 * d_gap + col_w_px - 6 * ds_num / ds_den;
    int time_x = (W - total_w) / 2;
    int time_y = H * 30 / 480;

    int cx = time_x;
    draw_digit_s(fb, cx, time_y, hour / 10, COLOR_BLACK, ds_num, ds_den); cx += d_gap;
    draw_digit_s(fb, cx, time_y, hour % 10, COLOR_BLACK, ds_num, ds_den); cx += d_gap;
    draw_colon_s(fb, cx, time_y, COLOR_BLACK, ds_num, ds_den);             cx += col_w_px;
    draw_digit_s(fb, cx, time_y, min / 10, COLOR_BLACK, ds_num, ds_den);  cx += d_gap;
    draw_digit_s(fb, cx, time_y, min % 10, COLOR_BLACK, ds_num, ds_den);

    char date_str[64];
    snprintf(date_str, sizeof(date_str),
             "%04d\xe5\xb9\xb4%02d\xe6\x9c\x88%02d\xe6\x97\xa5",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

    char week_str[32];
    int wday = tm.tm_wday;
    if (wday < 0 || wday > 6) wday = 0;
    snprintf(week_str, sizeof(week_str),
             "\xe6\x98\x9f\xe6\x9c\x9f%s",
             weekday_zh[wday]);

    int date_y = time_y + dh + 16;
    fb_utf8_scaled(fb, W * 120 / 648, date_y, date_str, COLOR_BLACK, sc);
    fb_utf8_scaled(fb, W * 400 / 648, date_y, week_str, COLOR_BLACK, sc);

    int sep_y = date_y + 16 * sc + 10;
    fb_hline(fb, MX + 10, sep_y, W - 2 * MX - 20, COLOR_BLACK);
    fb_hline(fb, MX + 10, sep_y + 1, W - 2 * MX - 20, COLOR_BLACK);

    const weather_data_t *wd = NULL;
    weather_config_t wcfg = {0};
    bool has_weather = false;

    if (s_cfg.show_weather) {
        wd = weather_get_data();
        if (wd && wd->valid) {
            weather_get_config(&wcfg);
            has_weather = true;
        }
    }

    int bot_y = sep_y + 16;

    if (has_weather) {
        char line1[96];
        if (wcfg.city_name[0]) {
            snprintf(line1, sizeof(line1), "%s   %s   %d'C",
                     wcfg.city_name, wd->now.text, wd->now.temp);
        } else {
            snprintf(line1, sizeof(line1), "%s   %d'C",
                     wd->now.text, wd->now.temp);
        }
        {
            int lx = W * 100 / 648;
            int lw = W - lx - MX - 8;
            if (lw < 24) lw = 24;
            fb_utf8_scaled_maxw(fb, lx, bot_y, line1, COLOR_BLACK, sc, lw);
        }

        char line2[96];
        snprintf(line2, sizeof(line2),
                 "\xe4\xbd\x93\xe6\x84\x9f%d'C   "
                 "\xe6\xb9\xbf\xe5\xba\xa6%d%%   "
                 "%s%d\xe7\xba\xa7",
                 wd->now.feels_like,
                 wd->now.humidity,
                 wd->now.wind_dir, wd->now.wind_scale);
        {
            int lx2 = W * 60 / 648;
            int lw2 = W - lx2 - MX - 8;
            if (lw2 < 24) lw2 = 24;
            fb_utf8_scaled_maxw(fb, lx2, bot_y + 16 * sc + 8, line2, COLOR_BLACK, sc, lw2);
        }

        int dot_y = bot_y + 2 * (16 * sc + 8) + 4;
        for (int dx = MX + 10; dx < W - MX - 10; dx += 6)
            fb_pixel(fb, dx, dot_y, COLOR_BLACK);

        int fcol = (W - 2 * MX - 40) / 3;
        if (fcol < 32) fcol = 32;
        const int fpad = 6;
        const int max_fc = fcol - fpad;
        int row_y = dot_y + 10;
        const int line_gap = 16 * sc + 4;

        for (int i = 0; i < wd->daily_count && i < 3; i++) {
            int fx = MX + 20 + i * fcol;
            char line_a[64];
            char line_b[48];

            if (i == 0) {
                snprintf(line_a, sizeof(line_a),
                         "\xe4\xbb\x8a\xe5\xa4\xa9 %d/%d'C",
                         wd->daily[i].temp_max, wd->daily[i].temp_min);
            } else {
                const char *d = wd->daily[i].date;
                char md[16];
                if (!d) {
                    md[0] = '\0';
                } else if (strlen(d) >= 10) {
                    snprintf(md, sizeof(md), "%s", d + 5);
                } else {
                    snprintf(md, sizeof(md), "%s", d);
                }
                snprintf(line_a, sizeof(line_a), "%s %d/%d'C",
                         md, wd->daily[i].temp_max, wd->daily[i].temp_min);
            }
            snprintf(line_b, sizeof(line_b), "%s", wd->daily[i].text_day);

            fb_utf8_scaled_maxw(fb, fx, row_y, line_a, COLOR_BLACK, sc, max_fc);
            fb_utf8_scaled_maxw(fb, fx, row_y + line_gap, line_b, COLOR_BLACK, sc, max_fc);
        }
    } else if (s_cfg.show_weather) {
        /* 已勾选「显示天气摘要」但尚未拉到 API 数据：提示原因，避免误以为开关无效 */
        int lx = MX + 16;
        int lw = W - 2 * MX - 32;
        if (lw < 24) lw = 24;
        fb_utf8_scaled_maxw(fb, lx, bot_y, k_wx_placeholder_l1, COLOR_BLACK, sc, lw);
        fb_utf8_scaled_maxw(fb, lx, bot_y + 16 * sc + 6, k_wx_placeholder_l2, COLOR_BLACK, sc, lw);
    } else {
        fb_hline(fb, W / 2 - 60, bot_y + 30, 120, COLOR_BLACK);
        fb_hline(fb, W / 2 - 60, bot_y + 32, 120, COLOR_BLACK);

        char ampm[16];
        if (hour < 6)       snprintf(ampm, sizeof(ampm), "\xe5\x87\x8c\xe6\x99\xa8");
        else if (hour < 12) snprintf(ampm, sizeof(ampm), "\xe4\xb8\x8a\xe5\x8d\x88");
        else if (hour < 18) snprintf(ampm, sizeof(ampm), "\xe4\xb8\x8b\xe5\x8d\x88");
        else                snprintf(ampm, sizeof(ampm), "\xe4\xb8\x8b\xe5\x8d\x88");
        fb_utf8_scaled(fb, W / 2 - 16 * sc, bot_y + 50, ampm, COLOR_BLACK, sc);

        fb_fill_rect(fb, 16, 16, 16, 4, COLOR_BLACK);
        fb_fill_rect(fb, 16, 16, 4, 16, COLOR_BLACK);
        fb_fill_rect(fb, W - 32, 16, 16, 4, COLOR_BLACK);
        fb_fill_rect(fb, W - 20, 16, 4, 16, COLOR_BLACK);
        fb_fill_rect(fb, 16, H - 20, 16, 4, COLOR_BLACK);
        fb_fill_rect(fb, 16, H - 32, 4, 16, COLOR_BLACK);
        fb_fill_rect(fb, W - 32, H - 20, 16, 4, COLOR_BLACK);
        fb_fill_rect(fb, W - 20, H - 32, 4, 16, COLOR_BLACK);
    }

    /* ── export and display ── */
    const char *raw_path = "/spiffs/image.bin";
    esp_err_t err = fb_export(fb, raw_path);
    fb_destroy(fb);

    if (err == ESP_OK) {
        epd_display_from_file(raw_path);
        scheduler_notify_manual_show();
        ESP_LOGI(TAG, "Clock displayed: %02d:%02d", hour, min);
    }

    if (s_render_mutex)
        xSemaphoreGive(s_render_mutex);
}

/* ── auto-refresh task ────────────────────────────────────────────── */

static void clock_task(void *arg)
{
    ESP_LOGI(TAG, "Clock task running");

    int last_minute = -1;

    for (;;) {
        if (!s_cfg.enabled || !display_policy_clock_may_auto_refresh()) {
            last_minute = -1;
            xEventGroupWaitBits(s_event, BIT_CFG_CHANGED | BIT_WEATHER_DATA,
                                pdTRUE, pdFALSE, portMAX_DELAY);
            continue;
        }

        struct tm tm;
        if (time_sync_get_local(&tm)) {
            if (tm.tm_min != last_minute) {
                render_clock();
                last_minute = tm.tm_min;
            }
        }

        EventBits_t bits = xEventGroupWaitBits(
            s_event, BIT_CFG_CHANGED | BIT_WEATHER_DATA,
            pdTRUE, pdFALSE, pdMS_TO_TICKS(30000));

        if (bits & BIT_CFG_CHANGED)
            last_minute = -1;

        if (bits & BIT_WEATHER_DATA) {
            if (time_sync_get_local(&tm) && display_policy_clock_may_auto_refresh()) {
                render_clock();
                last_minute = tm.tm_min;
            }
        }
    }
}

/* ── public API ───────────────────────────────────────────────────── */

esp_err_t clock_display_init(void)
{
    s_event = xEventGroupCreate();
    if (!s_event) return ESP_ERR_NO_MEM;

    s_render_mutex = xSemaphoreCreateMutex();

    nvs_load();

    BaseType_t ok = xTaskCreate(clock_task, "clock_disp", 8192, NULL, 3, NULL);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Clock init (enabled=%d, style=%d, weather=%d)",
             s_cfg.enabled, s_cfg.style, s_cfg.show_weather);
    return ESP_OK;
}

esp_err_t clock_display_get_config(clock_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = s_cfg;
    return ESP_OK;
}

esp_err_t clock_display_set_config(const clock_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;
    nvs_save();
    ESP_LOGI(TAG, "Config saved: enabled=%d, style=%d, weather=%d",
             s_cfg.enabled, s_cfg.style, s_cfg.show_weather);
    /* HTTP 可在 clock_display_init() 之前 POST /clock_config（见 app_main 顺序） */
    if (s_event)
        xEventGroupSetBits(s_event, BIT_CFG_CHANGED);
    if (s_cfg.show_weather)
        weather_request_embedded_refresh();
    return ESP_OK;
}

esp_err_t clock_display_show(void)
{
    if (!time_sync_is_synced()) {
        ESP_LOGW(TAG, "Time not synced, cannot show clock");
        return ESP_ERR_INVALID_STATE;
    }
    display_policy_set_manual_screen_active(false);
    if (s_cfg.show_weather)
        weather_request_embedded_refresh();
    render_clock();
    if (s_event)
        xEventGroupSetBits(s_event, BIT_CFG_CHANGED);
    return ESP_OK;
}

void clock_display_notify_weather_data(void)
{
    if (!s_event)
        return;
    if (!display_policy_clock_may_auto_refresh())
        return;
    xEventGroupSetBits(s_event, BIT_WEATHER_DATA);
}

void clock_display_notify_home_changed(void)
{
    if (s_event)
        xEventGroupSetBits(s_event, BIT_CFG_CHANGED);
}

void clock_display_notify_time_resync(void)
{
    if (s_event)
        xEventGroupSetBits(s_event, BIT_CFG_CHANGED);
}
