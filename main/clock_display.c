#include "clock_display.h"

#include <string.h>
#include <stdio.h>
#include <stdint.h>
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
#include "display_mode.h"
#include "ui_theme.h"
#include "calendar_display.h"
#include "battery_mon.h"

static const char *TAG    = "clock";
static const char *NVS_NS = "clock";

static const char *clock_weekday_long(int wday)
{
    static const char *const names[] = {
        "星期日", "星期一", "星期二", "星期三",
        "星期四", "星期五", "星期六"
    };
    return names[(wday >= 0 && wday < 7) ? wday : 0];
}

static const char *clock_day_period(int hour)
{
    if (hour < 6) return "凌晨";
    if (hour < 12) return "上午";
    if (hour < 18) return "下午";
    return "晚上";
}

#define BIT_CFG_CHANGED   BIT0
#define BIT_WEATHER_DATA  BIT1
/* 纯唤醒位：用于让 clock_task 从 disabled 长睡里出来重新评估
 * （比如外部 clock_display_show 把模式切回时钟），但不强制渲染。 */
#define BIT_WAKE          BIT2

/* Automatic clock refresh interval. The driver resolves PARTIAL to FULL on
 * panels whose experimental partial-refresh capability is disabled. */
#define CLOCK_REFRESH_STEP_MIN   1

static clock_config_t s_cfg = {
    .enabled      = false,
    .style        = 0,   /* TODO: style > 0 not yet implemented in render_clock() */
    .show_weather = true,
};

static EventGroupHandle_t s_event;
static SemaphoreHandle_t  s_render_mutex;
static portMUX_TYPE       s_cfg_mux = portMUX_INITIALIZER_UNLOCKED;

/* ── 渲染状态镜像 ──────────────────────────────────────────────────
 *
 * 任务循环、外部 clock_display_show()、天气通知三条路径共用此状态。
 * 任何成功完成的 render_clock() 都会更新它，保证 task 不会再多刷一次
 * （之前 task 与 show 不互通，导致一次配置保存 + 一次 weather 通知能在
 * 同一次循环里把同一分钟刷 2 次）。
 *
 * 写入：render_clock 末尾、disabled→idle 复位、weather 通知去重。
 * 读取：clock_task 顶部决策。
 */
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    int  last_minute;        /* tm.tm_min of last successful render, -1 = never */
    int  last_hour;
    int  last_mday;
    int  last_weather_sig;   /* 见 compute_weather_sig()，-1 = 无数据 */
    bool last_show_weather;  /* 上次渲染时 show_weather 的实际值 */
    bool force_full_next;    /* 下一次渲染是否强制全刷 */
} s_state = {
    .last_minute = -1,
    .last_hour = -1,
    .last_mday = -1,
    .last_weather_sig = -1,
    .last_show_weather = false,
    .force_full_next = true,
};

/* 计算天气数据签名：temp/icon/humidity/daily_count 拼成 32 位整型。
 * 只在显示天气时用来判断"是否真正变了"，避免每次 BIT_WEATHER_DATA 通知都强制全刷。 */
static int compute_weather_sig(void)
{
    return weather_data_signature();
}

static void state_reset(void)
{
    portENTER_CRITICAL(&s_state_mux);
    s_state.last_minute = -1;
    s_state.last_hour = -1;
    s_state.last_mday = -1;
    s_state.last_weather_sig = -1;
    s_state.last_show_weather = false;
    s_state.force_full_next = true;
    portEXIT_CRITICAL(&s_state_mux);
}

static bool clock_get_local_or_system(struct tm *out)
{
    if (!out)
        return false;
    if (time_sync_get_local_relaxed(out))
        return true;

    time_t now = time(NULL);
    localtime_r(&now, out);
    return false;
}

/* ── time headline rendering ─────────────────────────────────────── */

/*
 * Original Gitee clock face: seven-segment headline digits.
 * Keep this isolated from the generic styled font path so other pages can use
 * normal large text without changing the clock identity.
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
    SEG_B|SEG_C,                                /* 1 */
    SEG_A|SEG_B|SEG_D|SEG_E|SEG_G,              /* 2 */
    SEG_A|SEG_B|SEG_C|SEG_D|SEG_G,              /* 3 */
    SEG_B|SEG_C|SEG_F|SEG_G,                    /* 4 */
    SEG_A|SEG_C|SEG_D|SEG_F|SEG_G,              /* 5 */
    SEG_A|SEG_C|SEG_D|SEG_E|SEG_F|SEG_G,        /* 6 */
    SEG_A|SEG_B|SEG_C,                          /* 7 */
    SEG_A|SEG_B|SEG_C|SEG_D|SEG_E|SEG_F|SEG_G,  /* 8 */
    SEG_A|SEG_B|SEG_C|SEG_D|SEG_F|SEG_G,        /* 9 */
};

static const int seg_base[7][4] = {
    { 10,  0, 36,  8 },   /* A */
    { 48, 10,  8, 31 },   /* B */
    { 48, 49,  8, 31 },   /* C */
    { 10, 80, 36,  8 },   /* D */
    {  0, 49,  8, 31 },   /* E */
    {  0, 10,  8, 31 },   /* F */
    { 10, 40, 36,  8 },   /* G */
};

#define BASE_DW   56
#define BASE_DH   88
#define BASE_COL  18

static void clock_fill_beveled_rect(fb_t *fb, int x, int y, int w, int h,
                                    fb_color_t color)
{
    if (!fb || w <= 0 || h <= 0)
        return;
    fb_fill_rect(fb, x, y, w, h, color);

    int bevel = (w < h ? w : h) / 2;
    if (bevel < 2)
        return;
    for (int i = 0; i < bevel; i++) {
        int len = bevel - i;
        fb_hline(fb, x, y + i, len, COLOR_WHITE);
        fb_hline(fb, x + w - len, y + i, len, COLOR_WHITE);
        fb_hline(fb, x, y + h - 1 - i, len, COLOR_WHITE);
        fb_hline(fb, x + w - len, y + h - 1 - i, len, COLOR_WHITE);
    }
}

static void draw_digit_s(fb_t *fb, int x, int y, int digit,
                         fb_color_t color, int num, int den)
{
    if (digit < 0 || digit > 9)
        return;
    if (digit == 1) {
        const int seg_w = seg_base[1][2] * num / den;
        const int seg_h = seg_base[1][3] * num / den;
        int one_x = x + (BASE_DW * num / den - seg_w) / 2;
        clock_fill_beveled_rect(fb, one_x,
                                y + seg_base[1][1] * num / den,
                                seg_w, seg_h, color);
        clock_fill_beveled_rect(fb, one_x,
                                y + seg_base[2][1] * num / den,
                                seg_w, seg_h, color);
        return;
    }
    uint8_t segs = digit_segs[digit];
    for (int s = 0; s < 7; s++) {
        if (segs & (1 << s)) {
            clock_fill_beveled_rect(fb,
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
    int dot = 8 * num / den;
    if (dot < 4)
        dot = 4;
    int slot_w = BASE_COL * num / den;
    int cx = x + (slot_w - dot) / 2;
    fb_fill_rect(fb, cx, y + dh * 3 / 10, dot, dot, color);
    fb_fill_rect(fb, cx, y + dh * 7 / 10, dot, dot, color);
}

static int clock_text_px(const char *s, int scale)
{
    if (scale < 1)
        scale = 1;
    return ui_fixed_text_width(NULL, s, scale);
}

static int clock_draw_text(fb_t *fb, int x, int y, const char *s,
                           fb_color_t color, int scale)
{
    if (scale < 1)
        scale = 1;
    return ui_draw_fixed_text(fb, x, y, s, color, scale);
}

static int clock_draw_text_maxw(fb_t *fb, int x, int y, const char *s,
                                fb_color_t color, int scale, int max_w)
{
    if (scale < 1)
        scale = 1;
    return ui_draw_fixed_text_maxw(fb, x, y, s, color, scale, max_w);
}

static void clock_draw_footer(fb_t *fb, const char *left, const char *right)
{
    ui_draw_footer(fb, left, right);
}

/* ── weekday strings ──────────────────────────────────────────────── */

/* ── NVS ──────────────────────────────────────────────────────────── */

static void clock_plot_circle_points(fb_t *fb, int cx, int cy, int x, int y,
                                     fb_color_t color)
{
    fb_pixel(fb, cx + x, cy + y, color);
    fb_pixel(fb, cx - x, cy + y, color);
    fb_pixel(fb, cx + x, cy - y, color);
    fb_pixel(fb, cx - x, cy - y, color);
    fb_pixel(fb, cx + y, cy + x, color);
    fb_pixel(fb, cx - y, cy + x, color);
    fb_pixel(fb, cx + y, cy - x, color);
    fb_pixel(fb, cx - y, cy - x, color);
}

static void clock_draw_circle(fb_t *fb, int cx, int cy, int r, int thickness,
                              fb_color_t color)
{
    if (!fb || r <= 0)
        return;
    if (thickness < 1)
        thickness = 1;
    for (int rr = r; rr > r - thickness && rr > 0; rr--) {
        int x = 0;
        int y = rr;
        int d = 1 - rr;
        while (x <= y) {
            clock_plot_circle_points(fb, cx, cy, x, y, color);
            if (d < 0) {
                d += 2 * x + 3;
            } else {
                d += 2 * (x - y) + 5;
                y--;
            }
            x++;
        }
    }
}

static void clock_draw_calendar_icon(fb_t *fb, int x, int y, fb_color_t color)
{
    fb_rect(fb, x, y + 4, 18, 18, color);
    fb_hline(fb, x + 2, y + 9, 14, color);
    fb_fill_rect(fb, x + 4, y, 2, 7, color);
    fb_fill_rect(fb, x + 12, y, 2, 7, color);
    fb_fill_rect(fb, x + 4, y + 13, 3, 3, color);
    fb_fill_rect(fb, x + 10, y + 13, 3, 3, color);
    fb_fill_rect(fb, x + 4, y + 18, 3, 2, color);
    fb_fill_rect(fb, x + 10, y + 18, 3, 2, color);
}

static void clock_draw_status_clock_icon(fb_t *fb, int cx, int cy,
                                         fb_color_t color)
{
    clock_draw_circle(fb, cx, cy, 20, 3, color);
    fb_fill_rect(fb, cx - 2, cy - 13, 4, 15, color);
    for (int i = 0; i < 11; i++)
        fb_fill_rect(fb, cx + i, cy + i / 2, 3, 3, color);
}

static void clock_draw_ref_card(fb_t *fb, int x, int y, int w, int h)
{
    fb_rect(fb, x, y, w, h, COLOR_BLACK);
    fb_rect(fb, x + 1, y + 1, w - 2, h - 2, COLOR_BLACK);
}

static void clock_draw_ref_battery(fb_t *fb, int right_x, int y)
{
    ui_draw_battery_badge_for(fb, UI_COLOR_PAGE_CLOCK, right_x, y);
}

static void render_clock_42_reference(fb_t *fb, const struct tm *tm)
{
    const int W = fb->width;
    const int ds_num = 2;
    const int ds_den = 2;
    const int dw = BASE_DW * ds_num / ds_den;
    const int dh = BASE_DH * ds_num / ds_den;
    const int d_gap = dw + 6 * ds_num / ds_den;
    const int col_w_px = BASE_COL * ds_num / ds_den;
    const int time_w = 3 * d_gap + col_w_px + dw;

    fb_color_t accent = ui_color_resolve(UI_COLOR_PAGE_CLOCK,
                                          UI_COLOR_ROLE_ACCENT,
                                          COLOR_RED);
    fb_color_t notice = ui_color_resolve(UI_COLOR_PAGE_CLOCK,
                                          UI_COLOR_ROLE_NOTICE,
                                          COLOR_RED);
    ui_draw_page_frame_for(fb, UI_COLOR_PAGE_CLOCK,
                           UI_FRAME_RED_ACCENT | UI_FRAME_THIN);
    clock_draw_ref_battery(fb, W - 14, 8);

    int hour = tm->tm_hour;
    int min = tm->tm_min;
    int time_y = 50;

    char date_str[64];
    snprintf(date_str, sizeof(date_str), "%04d年%02d月%02d日",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);

    int wday = tm->tm_wday;
    if (wday < 0 || wday > 6)
        wday = 0;
    char week_str[32];
    snprintf(week_str, sizeof(week_str), "%s", clock_weekday_long(wday));

    const int date_y = time_y + dh + 22;
    const int date_w = clock_text_px(date_str, 1);
    const int week_w = clock_text_px(week_str, 1);
    const int icon_w = 22;
    const int gap1 = 12;
    const int gap2 = 16;
    const int row_w = icon_w + gap1 + date_w + gap2 + 1 + gap2 + week_w;
    int row_x = (W - row_w) / 2;
    if (row_x < 20)
        row_x = 20;

    int time_x = row_x + row_w / 2 - time_w / 2;
    if (time_x < 18)
        time_x = 18;
    int cx = time_x;
    draw_digit_s(fb, cx, time_y, hour / 10, COLOR_BLACK, ds_num, ds_den);
    cx += d_gap;
    draw_digit_s(fb, cx, time_y, hour % 10, COLOR_BLACK, ds_num, ds_den);
    cx += d_gap;
    draw_colon_s(fb, cx, time_y, accent, ds_num, ds_den);
    cx += col_w_px;
    draw_digit_s(fb, cx, time_y, min / 10, COLOR_BLACK, ds_num, ds_den);
    cx += d_gap;
    draw_digit_s(fb, cx, time_y, min % 10, COLOR_BLACK, ds_num, ds_den);

    clock_draw_calendar_icon(fb, row_x, date_y - 3, accent);
    int text_x = row_x + icon_w + gap1;
    clock_draw_text(fb, text_x, date_y, date_str, COLOR_BLACK, 1);
    int bar_x = text_x + date_w + gap2;
    fb_vline(fb, bar_x, date_y - 1, 19, COLOR_BLACK);
    clock_draw_text(fb, bar_x + gap2, date_y, week_str, accent, 1);

    const int line_y = date_y + 31;
    fb_fill_rect(fb, 20, line_y, W - 40, 2, accent);

    const int card_x = 22;
    const int card_y = line_y + 15;
    const int card_w = W - 2 * card_x;
    const int card_h = 78;
    clock_draw_ref_card(fb, card_x, card_y, card_w, card_h);

    int red_x = card_x + card_w - 8;
    int red_y = card_y + 27;
    int red_h = card_h - 27;
    fb_fill_rect(fb, red_x, red_y + 3, 4, red_h - 6, accent);
    fb_hline(fb, red_x + 1, red_y + 1, 3, accent);
    fb_hline(fb, red_x, red_y + 2, 4, accent);
    fb_hline(fb, red_x, red_y + red_h - 3, 4, accent);
    fb_hline(fb, red_x + 1, red_y + red_h - 2, 3, accent);

    int icon_cx = card_x + 38;
    int icon_cy = card_y + card_h / 2;
    clock_draw_status_clock_icon(fb, icon_cx, icon_cy, accent);

    int split_x = card_x + 76;
    fb_vline(fb, split_x, card_y + 13, card_h - 24, COLOR_BLACK);

    char ampm[16];
    snprintf(ampm, sizeof(ampm), "%s", clock_day_period(hour));

    const char *tag = time_sync_is_synced() ? "时间已同步" : "等待网络校时";
    int info_x = split_x + 20;
    int info_w = card_x + card_w - 20 - info_x;
    ui_draw_text_px_maxw(fb, info_x, card_y + 10, ampm, COLOR_BLACK,
                         30, info_w);
    clock_draw_text_maxw(fb, info_x, card_y + 50, tag,
                         time_sync_is_synced() ? COLOR_BLACK : notice,
                         1, info_w);
}

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

static void nvs_save_snapshot(const clock_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "enabled", cfg->enabled ? 1 : 0);
    nvs_set_u8(h, "style",   cfg->style);
    nvs_set_u8(h, "show_w",  cfg->show_weather ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

/* ── render clock ─────────────────────────────────────────────────── */

/**
 * 渲染时钟到帧缓冲并送屏。
 *
 * Manual page entry uses FULL; automatic same-page updates request PARTIAL.
 */
static esp_err_t render_clock(unsigned epoch, bool notify_scheduler,
                              epd_refresh_mode_t refresh_mode)
{
    if (s_render_mutex && xSemaphoreTake(s_render_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "render_clock: another render in progress, skip");
        return ESP_ERR_TIMEOUT;
    }

    /* 在 spinlock 保护下取一份配置快照，整次渲染使用 cfg_local 而非 s_cfg，
     * 避免与 HTTP POST 同时改 s_cfg 时撕裂读取（show_weather 等字段）。 */
    clock_config_t cfg_local;
    portENTER_CRITICAL(&s_cfg_mux);
    cfg_local = s_cfg;
    portEXIT_CRITICAL(&s_cfg_mux);

    fb_t *fb = fb_create();
    if (!fb) {
        ESP_LOGE(TAG, "fb_create failed");
        if (s_render_mutex)
            xSemaphoreGive(s_render_mutex);
        return ESP_ERR_NO_MEM;
    }

    struct tm tm;
    if (!clock_get_local_or_system(&tm)) {
        ESP_LOGW(TAG, "SNTP not synced, using system time");
    }

    int hour = tm.tm_hour;
    int min  = tm.tm_min;

    const int W = fb->width;
    const int H = fb->height;
    const int MX = 20;
    const bool clock42_reference = (W == 400 && H == 300 && !cfg_local.show_weather);
    if (clock42_reference) {
        render_clock_42_reference(fb, &tm);
        goto clock_render_done;
    }

    const ui_layout_class_t layout = ui_layout_for(fb);
    const bool xl = ui_layout_is_xl(fb);
    const bool is_e6 = ui_layout_is_e6_37(fb);
    const bool is_583 = (layout == UI_LAYOUT_583);
    const bool big = (layout == UI_LAYOUT_LARGE);
    const bool wide_clock = is_583 || big;
    const bool compact_clock = (!wide_clock && W >= 360 && H >= 260);
    const bool plain_compact_clock = compact_clock && !cfg_local.show_weather;
    const int sc = xl ? 3 : (wide_clock ? 2 : 1);
    const int ds_num = xl ? 2 : ((is_583 || is_e6) ? 5 : (big ? 3 : 2));
    const int ds_den = xl ? 1 : ((is_583 || is_e6) ? 3 : 2);
    int dw = BASE_DW * ds_num / ds_den;
    int dh = BASE_DH * ds_num / ds_den;
    int d_gap = dw + 6 * ds_num / ds_den;
    int col_w_px = BASE_COL * ds_num / ds_den;

    ui_draw_page_frame_for(fb, UI_COLOR_PAGE_CLOCK,
                           UI_FRAME_RED_ACCENT | UI_FRAME_THIN);
    clock_draw_ref_battery(fb, W - 14, 8);

    int total_w = 4 * d_gap + col_w_px - 6 * ds_num / ds_den;
    int time_x = (W - total_w) / 2;
    int time_y = xl ? 48 : (is_583 ? 56 : (is_e6 ? 42 : (H * 34 / 480)));
    if (compact_clock && !is_583) {
        if (!plain_compact_clock)
            time_x -= 12;
        if (time_x < 16)
            time_x = 16;
        time_y = plain_compact_clock ? 22 : 16;
    }

    fb_color_t digit_colors[4] = { COLOR_BLACK, COLOR_BLACK, COLOR_BLACK, COLOR_BLACK };
    fb_color_t colon_color = COLOR_BLACK;
    if (is_e6) {
        digit_colors[0] = COLOR_BLUE;
        digit_colors[1] = COLOR_GREEN;
        digit_colors[2] = COLOR_RED;
        digit_colors[3] = COLOR_YELLOW;
        colon_color = COLOR_BLACK;
    }

    int cx = time_x;
    draw_digit_s(fb, cx, time_y, hour / 10, digit_colors[0], ds_num, ds_den);
    cx += d_gap;
    draw_digit_s(fb, cx, time_y, hour % 10, digit_colors[1], ds_num, ds_den);
    cx += d_gap;
    draw_colon_s(fb, cx, time_y, colon_color, ds_num, ds_den);
    cx += col_w_px;
    draw_digit_s(fb, cx, time_y, min / 10, digit_colors[2], ds_num, ds_den);
    cx += d_gap;
    draw_digit_s(fb, cx, time_y, min % 10, digit_colors[3], ds_num, ds_den);

    char date_str[64];
    snprintf(date_str, sizeof(date_str), "%04d年%02d月%02d日",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

    char week_str[32];
    int wday = tm.tm_wday;
    if (wday < 0 || wday > 6) wday = 0;
    snprintf(week_str, sizeof(week_str), "%s", clock_weekday_long(wday));

    int date_y = time_y + dh + (xl ? 28 : ((is_583 || is_e6) ? 20 : (compact_clock ? 10 : 18)));
    int date_w = clock_text_px(date_str, sc);
    int week_w = clock_text_px(week_str, sc);
    int date_gap = xl ? 30 * sc : (big ? 22 * sc : 18);
    if (compact_clock && !is_583)
        date_gap = 16;
    int date_x = (W - date_w - week_w - date_gap) / 2;
    if (compact_clock && !is_583 && cfg_local.show_weather)
        date_x = MX + 32;
    if (date_x < 16)
        date_x = 16;
    clock_draw_text(fb, date_x, date_y, date_str,
                    is_e6 ? COLOR_BLUE : COLOR_BLACK, sc);
    clock_draw_text(fb, date_x + date_w + date_gap, date_y, week_str,
                    is_e6 ? COLOR_GREEN :
                    ui_color_resolve(UI_COLOR_PAGE_CLOCK,
                                     UI_COLOR_ROLE_ACCENT, COLOR_RED), sc);

    weather_summary_t wd_local = {0};
    weather_config_t wcfg = {0};
    bool has_weather = false;

    if (cfg_local.show_weather) {
        weather_get_summary_copy(&wd_local);
        if (wd_local.valid) {
            weather_get_config(&wcfg);
            has_weather = true;
        }
    }
    const weather_summary_t *wd = &wd_local;
    const char *now_weather_text = weather_text_localized(wd->now.icon,
                                                           wd->now.text);
    const char *now_wind_dir = weather_wind_dir_localized(wd->now.wind_degree,
                                                           wd->now.wind_dir);

    int sep_y = date_y + 16 * sc + (xl ? 24 : (is_583 ? 18 : (compact_clock ? 9 : 12)));
    if (wide_clock || has_weather || cfg_local.show_weather) {
        ui_draw_dotted_hline(fb, MX + 12, sep_y, W - 2 * MX - 24,
                             is_e6 ? COLOR_BLUE : COLOR_BLACK, 6);
    } else {
        fb_fill_rect(fb, W / 2 - 34, sep_y, 68, 2,
                     ui_color_resolve(UI_COLOR_PAGE_CLOCK,
                                      UI_COLOR_ROLE_ACCENT, COLOR_RED));
    }

    int bot_y = sep_y + (xl ? 22 : (is_583 ? 18 : (compact_clock ? 12 : 14)));
    const int wx_head_sc = xl ? 2 : (wide_clock ? 2 : 1);
    const int wx_body_sc = 1;
    const int wx_head_gap = 16 * wx_head_sc + 8;

    if (has_weather) {
        if (compact_clock) {
            const int current_y = bot_y;
            const int icon_x = MX + 18;
            const int icon_y = current_y + 13;
            const int left_x = icon_x + WEATHER_ICON_W + 10;
            const int split_x = W / 2 + 8;
            const int right_x = split_x + 16;

            weather_draw_icon(fb, icon_x, icon_y, wd->now.icon, wd->now.text,
                              WEATHER_ICON_W, UI_COLOR_PAGE_CLOCK,
                              COLOR_BLACK);
            ui_draw_dotted_vline(fb, split_x, current_y + 3, 55, COLOR_BLACK, 6);

            char place[80];
            if (wcfg.city_name[0]) {
                snprintf(place, sizeof(place), "%.31s", wcfg.city_name);
            } else {
                snprintf(place, sizeof(place), "%s", "当前天气");
            }

            char now_line[80];
            snprintf(now_line, sizeof(now_line), "%.23s  %d\xc2\xb0""C",
                     now_weather_text, wd->now.temp);

            clock_draw_text_maxw(fb, left_x, current_y + 3, place,
                                  COLOR_BLACK, 1, split_x - left_x - 10);
            clock_draw_text_maxw(fb, left_x, current_y + 27, now_line,
                                  weather_icon_color_for(UI_COLOR_PAGE_CLOCK,
                                                         wd->now.icon,
                                                         wd->now.text,
                                                         COLOR_RED),
                                  1, split_x - left_x - 10);

            char feels[64];
            char humid[64];
            char wind[80];
            snprintf(feels, sizeof(feels), "体感 %d°C", wd->now.feels_like);
            snprintf(humid, sizeof(humid), "湿度 %d%%", wd->now.humidity);
            snprintf(wind, sizeof(wind), "%.15s %d级",
                     now_wind_dir, wd->now.wind_scale);

            clock_draw_text_maxw(fb, right_x, current_y + 0, feels,
                                  COLOR_BLACK, 1, W - right_x - MX);
            clock_draw_text_maxw(fb, right_x, current_y + 20, humid,
                                  COLOR_BLACK, 1, W - right_x - MX);
            clock_draw_text_maxw(fb, right_x, current_y + 40, wind,
                                  COLOR_BLACK, 1, W - right_x - MX);

            int dot_y = current_y + 66;
            ui_draw_dotted_hline(fb, MX + 10, dot_y, W - 2 * MX - 20,
                                 COLOR_BLACK, 6);

            const int gap = 8;
            const int fcol = (W - 2 * MX - 2 * gap) / 3;
            const int row_y = dot_y + 9;
            for (int i = 0; i < wd->daily_count && i < 3; i++) {
                int fx = MX + i * (fcol + gap);
                if (i > 0) {
                    ui_draw_dotted_vline(fb, fx - gap / 2, row_y + 2, 38,
                                         COLOR_BLACK, 6);
                }

                char label[24];
                if (i == 0) {
                    snprintf(label, sizeof(label), "%s", "今天");
                } else {
                    const char *d = wd->daily[i].date;
                    if (!d) {
                        label[0] = '\0';
                    } else if (strlen(d) >= 10) {
                        snprintf(label, sizeof(label), "%s", d + 5);
                    } else {
                        snprintf(label, sizeof(label), "%s", d);
                    }
                }

                char detail[80];
                const char *day_text = weather_text_localized(
                    wd->daily[i].icon_day, wd->daily[i].text_day);
                snprintf(detail, sizeof(detail), "%s %d/%d",
                         day_text,
                         wd->daily[i].temp_max, wd->daily[i].temp_min);

                int icon_fx = fx + fcol - WEATHER_ICON_W - 6;
                weather_draw_icon(fb, icon_fx, row_y + 4,
                                  wd->daily[i].icon_day,
                                  wd->daily[i].text_day, WEATHER_ICON_W,
                                  UI_COLOR_PAGE_CLOCK, COLOR_BLACK);

                int text_w = icon_fx - fx - 5;
                if (text_w < 32)
                    text_w = fcol - 8;
                clock_draw_text_maxw(fb, fx + 4, row_y, label,
                                      (i == 0)
                                      ? ui_color_resolve(UI_COLOR_PAGE_CLOCK,
                                                         UI_COLOR_ROLE_ACCENT,
                                                         COLOR_RED)
                                      : COLOR_BLACK,
                                      1, text_w);
                clock_draw_text_maxw(fb, fx + 4, row_y + 21, detail,
                                      COLOR_BLACK, 1, text_w);
            }
        } else {
        char line1[280];
        if (wcfg.city_name[0]) {
            snprintf(line1, sizeof(line1), "%.31s  %.23s  %d\xc2\xb0""C",
                     wcfg.city_name, now_weather_text, wd->now.temp);
        } else {
            snprintf(line1, sizeof(line1), "%.23s  %d\xc2\xb0""C",
                     now_weather_text, wd->now.temp);
        }
        {
            int icon_x = W * 66 / 648;
            int icon_y = bot_y + (16 * wx_head_sc - WEATHER_ICON_H) / 2;
            if (icon_y < bot_y)
                icon_y = bot_y;
            weather_draw_icon(fb, icon_x, icon_y, wd->now.icon, wd->now.text,
                              WEATHER_ICON_W, UI_COLOR_PAGE_CLOCK,
                              COLOR_BLACK);

            int lx = icon_x + WEATHER_ICON_W + 10;
            int lw = W - lx - MX - 8;
            if (lw < 24) lw = 24;
            clock_draw_text_maxw(fb, lx, bot_y, line1, COLOR_BLACK, wx_head_sc, lw);
        }

        char line2[280];
        snprintf(line2, sizeof(line2),
                 "体感%d°C  湿度%d%%  %.15s%d级",
                 wd->now.feels_like,
                 wd->now.humidity,
                 now_wind_dir, wd->now.wind_scale);
        {
            int lx2 = W * 80 / 648;
            int lw2 = W - lx2 - MX - 8;
            if (lw2 < 24) lw2 = 24;
            clock_draw_text_maxw(fb, lx2, bot_y + wx_head_gap, line2, COLOR_BLACK, wx_head_sc, lw2);
        }

        int dot_y = bot_y + 2 * wx_head_gap + 2;
        ui_draw_dotted_hline(fb, MX + 10, dot_y, W - 2 * MX - 20, COLOR_BLACK, 6);

        int fcol = (W - 2 * MX - 40) / 3;
        if (fcol < 32) fcol = 32;
        const int fpad = 6;
        const int max_fc = fcol - fpad;
        int row_y = dot_y + 10;
        const int line_gap = 16 * wx_body_sc + 4;

        for (int i = 0; i < wd->daily_count && i < 3; i++) {
            int fx = MX + 20 + i * fcol;
            char line_a[64];
            char line_b[48];

            if (i == 0) {
                snprintf(line_a, sizeof(line_a),
                         "今天 %d/%d°C",
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
                snprintf(line_a, sizeof(line_a), "%.15s %d/%d\xc2\xb0""C",
                         md, wd->daily[i].temp_max, wd->daily[i].temp_min);
            }
            snprintf(line_b, sizeof(line_b), "%.23s",
                     weather_text_localized(wd->daily[i].icon_day,
                                            wd->daily[i].text_day));

            int icon_x = fx + max_fc - WEATHER_ICON_W;
            if (icon_x < fx)
                icon_x = fx;
            weather_draw_icon(fb, icon_x, row_y + 1,
                              wd->daily[i].icon_day,
                              wd->daily[i].text_day, WEATHER_ICON_W,
                              UI_COLOR_PAGE_CLOCK, COLOR_BLACK);

            int text_max = max_fc - WEATHER_ICON_W - 6;
            if (text_max < 32)
                text_max = max_fc;
            clock_draw_text_maxw(fb, fx, row_y, line_a,
                                  (i == 0)
                                  ? ui_color_resolve(UI_COLOR_PAGE_CLOCK,
                                                     UI_COLOR_ROLE_ACCENT,
                                                     COLOR_RED)
                                  : COLOR_BLACK,
                                  wx_body_sc, text_max);
            clock_draw_text_maxw(fb, fx, row_y + line_gap, line_b,
                                  COLOR_BLACK, wx_body_sc, text_max);
        }
        }
    } else if (cfg_local.show_weather) {
        /* 已勾选「显示天气摘要」但尚未拉到 API 数据：提示原因，避免误以为开关无效 */
        int card_x = wide_clock ? 52 : 34;
        int card_w = W - 2 * card_x;
        int card_h = wide_clock ? 76 : 54;
        int card_y = bot_y + (wide_clock ? 4 : 0);
        int line_gap = wide_clock ? 30 : 22;
        int text_px = wide_clock ? 24 : 16;
        if (card_w < 80)
            card_w = W - 2 * MX;
        ui_draw_card_for(fb, UI_COLOR_PAGE_CLOCK,
                         card_x, card_y, card_w, card_h, true);
        ui_draw_text_px_maxw(fb, card_x + 12, card_y + (wide_clock ? 10 : 8),
                             "天气数据未就绪", COLOR_BLACK, text_px,
                             card_w - 24);
        ui_draw_text_px_maxw(fb, card_x + 12,
                             card_y + (wide_clock ? 10 : 8) + line_gap,
                             "请先在天气页填写城市",
                             ui_color_resolve(UI_COLOR_PAGE_CLOCK,
                                              UI_COLOR_ROLE_NOTICE,
                                              COLOR_RED), text_px,
                             card_w - 24);
    } else {
        char ampm[16];
        snprintf(ampm, sizeof(ampm), "%s", clock_day_period(hour));
        if (!big) {
            const int card_x = is_583 ? 48 : 24;
            const int card_y = bot_y + (is_583 ? 8 : (plain_compact_clock ? 16 : 6));
            const int card_w = W - 2 * card_x;
            const int card_h = is_583 ? 104 : 72;
            ui_draw_card_for(fb, UI_COLOR_PAGE_CLOCK,
                             card_x, card_y, card_w, card_h, true);

            const char *tag = time_sync_is_synced()
                                  ? "时间已同步" : "等待网络校时";
            int ampm_sc = is_583 ? 3 : 2;
            int aw = clock_text_px(ampm, ampm_sc);
            int tw = clock_text_px(tag, 1);
            clock_draw_text(fb, (W - aw) / 2, card_y + (is_583 ? 18 : 14),
                            ampm, COLOR_BLACK, ampm_sc);
            clock_draw_text(fb, (W - tw) / 2, card_y + (is_583 ? 72 : 48), tag,
                            time_sync_is_synced() ? COLOR_BLACK :
                            ui_color_resolve(UI_COLOR_PAGE_CLOCK,
                                             UI_COLOR_ROLE_NOTICE,
                                             COLOR_RED), 1);
        } else {
            ui_draw_dotted_hline(fb, W / 2 - 70, bot_y + 30, 140, COLOR_BLACK, 6);
            int aw = clock_text_px(ampm, sc);
            clock_draw_text(fb, (W - aw) / 2, bot_y + 50, ampm, COLOR_BLACK, sc);
        }
    }

    if (big || has_weather || cfg_local.show_weather)
        clock_draw_footer(fb, "时钟", cfg_local.show_weather ? "天气" : "时间");

clock_render_done:
    if (!display_policy_epoch_is_current(epoch)) {
        fb_destroy(fb);
        if (s_render_mutex)
            xSemaphoreGive(s_render_mutex);
        ESP_LOGI(TAG, "Clock display skipped before EPD refresh: stale request");
        return ESP_ERR_INVALID_STATE;
    }

    /* Display directly from RAM; clock refreshes should not rewrite /spiffs/image.bin. */
    esp_err_t err = epd_display_fb_free_mode_epoch(fb, refresh_mode, epoch);
    if (err != ESP_OK) {
        if (s_render_mutex)
            xSemaphoreGive(s_render_mutex);
        return err;
    }

    if (!display_policy_epoch_is_current(epoch)) {
        if (s_render_mutex)
            xSemaphoreGive(s_render_mutex);
        ESP_LOGI(TAG, "Clock display completed but was superseded");
        return ESP_ERR_INVALID_STATE;
    }

    if (notify_scheduler)
        scheduler_notify_manual_show();
    ESP_LOGI(TAG, "Clock displayed: %02d:%02d", hour, min);

    /* 同步全局渲染状态：任何成功的 render_clock() 都更新它，
     * 这样 task 与外部 show 共用一份"最近渲染了什么"，不会再各自重复刷屏。 */
    int sig_now = cfg_local.show_weather ? compute_weather_sig() : -1;
    portENTER_CRITICAL(&s_state_mux);
    s_state.last_minute = tm.tm_min;
    s_state.last_hour = tm.tm_hour;
    s_state.last_mday = tm.tm_mday;
    s_state.last_show_weather = cfg_local.show_weather;
    if (sig_now != -1)
        s_state.last_weather_sig = sig_now;
    s_state.force_full_next = false;
    portEXIT_CRITICAL(&s_state_mux);

    if (s_render_mutex)
        xSemaphoreGive(s_render_mutex);
    return ESP_OK;
}

/* ── auto-refresh task ────────────────────────────────────────────── */

/*
 * 重写说明（2026-04-26）：
 *
 * 旧实现把 minute-check 和 bits-handler 写成两段独立路径，会在同一次循环里
 * 触发两次渲染（典型场景：配置保存→BIT_CFG_CHANGED 唤醒任务→minute path 全刷
 * 22:19→等 bits→BIT_WEATHER_DATA 已 pending→bit handler 又全刷 22:19），
 * 浪费 18.8s × N 次。
 *
 * 新实现：每轮顶部读完所有信号 + 时间，做 ONE 次决策，最多 ONE 次渲染。
 *
 *   - 等 bits（带 30~60s 超时，对齐到下一分钟边界）；
 *   - 唤醒后核对：分钟变化 / 整点 / 日期 / 配置 / 天气可见性 / 天气数据签名 /
 *     force_full_next（时间重同步等）；
 *   - same-page updates request PARTIAL; unsupported panels fall back to FULL;
 *   - 收到 BIT_WEATHER_DATA 但天气数据签名没变 → 不刷。
 *
 * 状态镜像 s_state 由 render_clock() 在每次成功渲染时同步，使外部
 * clock_display_show()（HTTP / 按键路径）也参与状态去重，不会再让任务多
 * 走一次"伪 cfg_changed → 重复全刷"。
 */
static void clock_task(void *arg)
{
    ESP_LOGI(TAG, "Clock task running");

    for (;;) {
        if (!display_policy_clock_may_auto_refresh() &&
            !display_policy_calendar_may_midnight_refresh()) {
            state_reset();
            /* A pending display can roll back without a configuration event. */
            xEventGroupWaitBits(s_event,
                                BIT_CFG_CHANGED | BIT_WEATHER_DATA | BIT_WAKE,
                                pdTRUE, pdFALSE, pdMS_TO_TICKS(30000));
            continue;
        }

        /* 等待时长对齐到下一分钟边界（最长 60s），让分钟跳变能尽快响应。 */
        TickType_t timeout = pdMS_TO_TICKS(30000);
        struct tm tm_pre;
        if (clock_get_local_or_system(&tm_pre)) {
            int wait_sec = 60 - tm_pre.tm_sec;
            if (wait_sec < 1) wait_sec = 1;
            if (wait_sec > 60) wait_sec = 60;
            timeout = pdMS_TO_TICKS(wait_sec * 1000);
        }

        EventBits_t bits = xEventGroupWaitBits(
            s_event,
            BIT_CFG_CHANGED | BIT_WEATHER_DATA | BIT_WAKE,
            pdTRUE, pdFALSE, timeout);

        /* 唤醒后再核一次活跃条件（wait 期间可能被切到其他模式） */
        unsigned observed_epoch = display_policy_display_epoch();
        bool clock_may_refresh = display_policy_clock_may_auto_refresh();
        bool calendar_may_refresh = display_policy_calendar_may_midnight_refresh();
        if (!clock_may_refresh && !calendar_may_refresh) {
            continue;
        }

        struct tm tm;
        if (!clock_get_local_or_system(&tm)) {
            ESP_LOGW(TAG, "SNTP not synced, auto-refresh uses system time");
        }

        bool show_weather;
        portENTER_CRITICAL(&s_cfg_mux);
        show_weather = s_cfg.show_weather;
        portEXIT_CRITICAL(&s_cfg_mux);

        /* 取一份当前状态镜像，避免与外部 show 比较时撕裂。 */
        int  s_last_min, s_last_hr, s_last_md, s_last_sig;
        bool s_last_sw, s_force_full;
        portENTER_CRITICAL(&s_state_mux);
        s_last_min   = s_state.last_minute;
        s_last_hr    = s_state.last_hour;
        s_last_md    = s_state.last_mday;
        s_last_sig   = s_state.last_weather_sig;
        s_last_sw    = s_state.last_show_weather;
        s_force_full = s_state.force_full_next;
        portEXIT_CRITICAL(&s_state_mux);

        bool minute_changed = (tm.tm_min != s_last_min);
        bool hour_changed   = (s_last_hr >= 0 && tm.tm_hour != s_last_hr);
        bool day_changed    = (s_last_md >= 0 && tm.tm_mday != s_last_md);
        bool cfg_changed    = (bits & BIT_CFG_CHANGED) != 0;
        bool weather_evt    = (bits & BIT_WEATHER_DATA) != 0;
        bool sw_visibility_changed = (s_last_sw != show_weather);

        /* 仅当前正在显示天气时才比对签名；不显示时签名变化无关紧要 */
        bool weather_data_changed = false;
        if (weather_evt && show_weather) {
            int sig = compute_weather_sig();
            weather_data_changed = (sig != s_last_sig);
        }

        bool refresh_step_due = minute_changed &&
                                (CLOCK_REFRESH_STEP_MIN <= 1 ||
                                 (tm.tm_min % CLOCK_REFRESH_STEP_MIN) == 0);

        if (calendar_may_refresh && minute_changed &&
            calendar_display_needs_midnight_refresh()) {
            ESP_LOGI(TAG, "calendar date changed, refreshing calendar page");
            if (display_mode_show_background(DISPLAY_MODE_CALENDAR, observed_epoch) == ESP_OK)
                calendar_display_wait_render_idle();
            portENTER_CRITICAL(&s_state_mux);
            s_state.last_minute = tm.tm_min;
            s_state.last_hour = tm.tm_hour;
            s_state.last_mday = tm.tm_mday;
            portEXIT_CRITICAL(&s_state_mux);
            continue;
        }

        if (!clock_may_refresh) {
            if (minute_changed) {
                portENTER_CRITICAL(&s_state_mux);
                s_state.last_minute = tm.tm_min;
                s_state.last_hour = tm.tm_hour;
                s_state.last_mday = tm.tm_mday;
                portEXIT_CRITICAL(&s_state_mux);
            }
            continue;
        }

        bool need_render = false;

        if (cfg_changed)                          need_render = true;
        if (sw_visibility_changed)                need_render = true;
        if (hour_changed || day_changed)          need_render = true;
        if (show_weather && weather_data_changed) need_render = true;
        if (refresh_step_due)                     need_render = true;
        if (s_force_full)                         need_render = true;

        if (!need_render) {
            /* 比如：BIT_WEATHER_DATA 触发但数据没变；或者非 step 边界的分钟跳变。
             * 仍然更新时间镜像，避免下一轮反复把同一分钟当作 minute_changed。 */
            if (minute_changed) {
                portENTER_CRITICAL(&s_state_mux);
                s_state.last_minute = tm.tm_min;
                s_state.last_hour = tm.tm_hour;
                s_state.last_mday = tm.tm_mday;
                portEXIT_CRITICAL(&s_state_mux);
            }
            continue;
        }

        epd_refresh_mode_t refresh_mode = s_force_full
                                              ? EPD_REFRESH_FULL
                                              : EPD_REFRESH_PARTIAL;
        display_request_token_t token;
        if (!display_policy_try_begin_background(DISPLAY_MODE_CLOCK, observed_epoch,
                                                  refresh_mode, &token))
            continue;
        ESP_LOGI(TAG,
                 "tick %02d:%02d -> render clock refresh=%s min=%d step=%d "
                 "hr=%d day=%d cfg=%d sw_vis=%d wx=%d ff=%d",
                 tm.tm_hour, tm.tm_min, epd_refresh_mode_name(refresh_mode),
                 minute_changed, refresh_step_due,
                 hour_changed, day_changed, cfg_changed,
                 sw_visibility_changed, weather_data_changed, s_force_full);
        esp_err_t result = render_clock(token.epoch, false, refresh_mode);
        (void)display_policy_finish_request(&token, result, false);
    }
}

/* ── public API ───────────────────────────────────────────────────── */

esp_err_t clock_display_init(void)
{
    s_event = xEventGroupCreate();
    if (!s_event) return ESP_ERR_NO_MEM;

    s_render_mutex = xSemaphoreCreateMutex();
    if (!s_render_mutex) {
        vEventGroupDelete(s_event);
        s_event = NULL;
        return ESP_ERR_NO_MEM;
    }

    nvs_load();

    BaseType_t ok = xTaskCreate(clock_task, "clock_disp", 8192, NULL, 3, NULL);
    if (ok != pdPASS) {
        vSemaphoreDelete(s_render_mutex);
        s_render_mutex = NULL;
        vEventGroupDelete(s_event);
        s_event = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Clock init (enabled=%d, style=%d, weather=%d)",
             s_cfg.enabled, s_cfg.style, s_cfg.show_weather);
    return ESP_OK;
}

esp_err_t clock_display_get_config(clock_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_cfg_mux);
    *out = s_cfg;
    portEXIT_CRITICAL(&s_cfg_mux);
    return ESP_OK;
}

esp_err_t clock_display_set_config(const clock_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    /* 与旧值比较：如果只是写了同样的内容（前端表单重复提交场景），无需打扰 task。 */
    bool actually_changed;
    portENTER_CRITICAL(&s_cfg_mux);
    actually_changed = (s_cfg.enabled != cfg->enabled) ||
                       (s_cfg.style != cfg->style) ||
                       (s_cfg.show_weather != cfg->show_weather);
    s_cfg = *cfg;
    portEXIT_CRITICAL(&s_cfg_mux);

    /* 使用入参打日志，避免再次读 s_cfg 造成不必要的并发暴露 */
    nvs_save_snapshot(cfg);
    ESP_LOGI(TAG, "Config saved: enabled=%d, style=%d, weather=%d (changed=%d)",
             cfg->enabled, cfg->style, cfg->show_weather, actually_changed);

    if (s_event && actually_changed && display_policy_clock_may_auto_refresh())
        xEventGroupSetBits(s_event, BIT_CFG_CHANGED);
    if (cfg->show_weather && display_policy_clock_may_auto_refresh())
        weather_request_embedded_refresh();
    return ESP_OK;
}

esp_err_t clock_display_show(void)
{
    if (!time_sync_is_synced()) {
        ESP_LOGW(TAG, "Time not synced, using system time for clock");
    }

    bool show_weather;
    portENTER_CRITICAL(&s_cfg_mux);
    show_weather = s_cfg.show_weather;
    portEXIT_CRITICAL(&s_cfg_mux);

    /* 手动 show（按键/网页/启动）→ 强制下一次全刷。
     * render_clock 内部成功后会把 force_full_next 清回 false，
     * 这里只是保证从空闲/异常态恢复时第一次绝对是全刷。 */
    portENTER_CRITICAL(&s_state_mux);
    s_state.force_full_next = true;
    portEXIT_CRITICAL(&s_state_mux);

    /* 同步渲染（耗时 ~18.8s 全刷）。render_clock 末尾会更新 s_state，
     * 任务循环看到 s_state.last_minute 已是当前分钟 → 不会立即重复刷。 */
    unsigned epoch = display_mode_render_epoch();
    esp_err_t err = render_clock(epoch, true, EPD_REFRESH_FULL);
    if (err != ESP_OK)
        return err;

    if (show_weather)
        weather_request_embedded_refresh();

    /* 用 BIT_WAKE 而不是 BIT_CFG_CHANGED：唤醒任务从 disabled 等待里出来
     * 接管后续分钟刷新，但不会被任务当作"配置变了"再多刷一次。
     * 任务的渲染决策完全基于 s_state 与当前时间的差异，而 s_state 已被
     * 当前 render_clock 更新到最新值。 */
    if (s_event)
        xEventGroupSetBits(s_event, BIT_WAKE);
    return ESP_OK;
}

void clock_display_notify_weather_data(void)
{
    if (!s_event)
        return;
    if (!display_policy_clock_may_auto_refresh())
        return;

    /* 入口去重：如果天气数据签名与上次成功渲染时一致，根本不需要唤醒任务。
     * 这是观察到的"同分钟连刷两次 22:19"问题的源头之一。 */
    int sig = compute_weather_sig();
    int last_sig;
    portENTER_CRITICAL(&s_state_mux);
    last_sig = s_state.last_weather_sig;
    portEXIT_CRITICAL(&s_state_mux);
    if (sig != -1 && sig == last_sig) {
        return;
    }
    xEventGroupSetBits(s_event, BIT_WEATHER_DATA);
}

void clock_display_notify_home_changed(void)
{
    if (s_event)
        xEventGroupSetBits(s_event, BIT_CFG_CHANGED);
}

void clock_display_notify_time_resync(void)
{
    /* 时间重同步：不一定意味着内容要变，让任务正常按分钟流程刷即可。
     * 但 last_minute 可能因时区切换而对不上当前分钟，强制全刷一次更稳妥。 */
    portENTER_CRITICAL(&s_state_mux);
    s_state.force_full_next = true;
    portEXIT_CRITICAL(&s_state_mux);
    if (s_event)
        xEventGroupSetBits(s_event, BIT_CFG_CHANGED);
}
