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
#include "battery_mon.h"

/* 摘要区占位（无有效天气数据时） */
static const char k_wx_placeholder_l1[] =
    "\xe5\xa4\xa9\xe6\xb0\x94\xe6\x95\xb0\xe6\x8d\xae\xe6\x9c\xaa\xe5\xb0\xb1\xe7\xbb\xaa"; /* 天气数据未就绪 */
static const char k_wx_placeholder_l2[] =
    "\xe8\xaf\xb7\xe5\x9c\xa8\xe3\x80\x8c\xe5\xa4\xa9\xe6\xb0\x94\xe3\x80\x8d\xe9\xa1\xb5\xe5\xa1\xab API \xe5\x90\x8e\xe7\x82\xb9\xe5\x88\xb7\xe6\x96\xb0"; /* 请在「天气」页填 API 后点刷新 */

static const char *TAG    = "clock";
static const char *NVS_NS = "clock";

#define BIT_CFG_CHANGED   BIT0
#define BIT_WEATHER_DATA  BIT1
/* 纯唤醒位：用于让 clock_task 从 disabled 长睡里出来重新评估
 * （比如外部 clock_display_show 把模式切回时钟），但不强制渲染。 */
#define BIT_WAKE          BIT2

/* 时钟自动刷新的分钟粒度。
 * 1 = 每分钟全刷（时间始终准确，4.2" 约 10s / 5.83" 约 18s 刷新一次）；
 * 5 = 每 5 分钟全刷（23:00/05/10/15...）；
 * 整点/跨日/配置/天气变更仍立即全刷，与此值无关。 */
#define CLOCK_FULL_REFRESH_STEP_MIN   1

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
    weather_data_t wd;
    weather_get_data_copy(&wd);
    if (!wd.valid) return -1;
    int s = ((wd.now.temp + 100) & 0x1FF);          /* 9 bit, -100..411 */
    s |= (wd.now.icon & 0xFFF) << 9;                 /* 12 bit */
    s |= (wd.now.humidity & 0x7F) << 21;             /* 7 bit, 0..127 */
    s |= (wd.daily_count & 0x7) << 28;               /* 3 bit */
    return s;
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
 * 当前 BWR（黑/白/红）屏的局部刷新在 SSD1619/UC8179 上质量都不稳定
 * （SSD1619 会让整屏背景发灰、下半屏文字变淡；UC8179 PTL partial 仍接近
 * 全刷耗时且会刷花画面），因此本模块统一走全刷，画面质量优先。
 * 参数 @p prefer_partial 暂保留接口位置，已不参与决策。
 */
static void render_clock(bool prefer_partial)
{
    if (s_render_mutex && xSemaphoreTake(s_render_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "render_clock: another render in progress, skip");
        return;
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
        return;
    }

    struct tm tm;
    if (!time_sync_get_local(&tm)) {
        time_t now = time(NULL);
        localtime_r(&now, &tm);
        ESP_LOGW(TAG, "SNTP not synced, using system time");
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
    /* 与日期同行靠左；字号用 1，避免小屏上与大号日期字重叠 */
    battery_mon_draw_percent_compact(fb, 14, date_y, COLOR_BLACK, 1);
    fb_utf8_scaled(fb, W * 120 / 648, date_y, date_str, COLOR_BLACK, sc);
    fb_utf8_scaled(fb, W * 400 / 648, date_y, week_str, COLOR_BLACK, sc);

    int sep_y = date_y + 16 * sc + 10;
    fb_hline(fb, MX + 10, sep_y, W - 2 * MX - 20, COLOR_BLACK);
    fb_hline(fb, MX + 10, sep_y + 1, W - 2 * MX - 20, COLOR_BLACK);

    weather_data_t wd_local = {0};
    weather_config_t wcfg = {0};
    bool has_weather = false;

    if (cfg_local.show_weather) {
        weather_get_data_copy(&wd_local);
        if (wd_local.valid) {
            weather_get_config(&wcfg);
            has_weather = true;
        }
    }
    const weather_data_t *wd = &wd_local;

    int bot_y = sep_y + 16;

    if (has_weather) {
        char line1[280];
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

        char line2[280];
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
    } else if (cfg_local.show_weather) {
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
        else                snprintf(ampm, sizeof(ampm), "\xe6\x99\x9a\xe4\xb8\x8a");
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

    /* ── export to SPIFFS（保留 wake-restore 缓存）── */
    const char *raw_path = "/spiffs/image.bin";
    fb_raw_file_lock();
    esp_err_t err = fb_export(fb, raw_path);
    fb_raw_file_unlock();

    if (err != ESP_OK) {
        fb_destroy(fb);
        if (s_render_mutex)
            xSemaphoreGive(s_render_mutex);
        return;
    }

    (void)prefer_partial;
    (void)raw_path;

    /* 全刷：用 epd_display_fb_free，SPI 完成立即释放 fb，省 ~18s 内存占用 */
    epd_display_fb_free(fb);

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
 *   - 时钟一律全刷（BWR 局刷在 SSD1619/UC8179 上质量不可靠，已移除）；
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
        bool enabled;
        portENTER_CRITICAL(&s_cfg_mux);
        enabled = s_cfg.enabled;
        portEXIT_CRITICAL(&s_cfg_mux);

        if (!enabled || !display_policy_clock_may_auto_refresh()) {
            state_reset();
            xEventGroupWaitBits(s_event,
                                BIT_CFG_CHANGED | BIT_WEATHER_DATA | BIT_WAKE,
                                pdTRUE, pdFALSE, portMAX_DELAY);
            continue;
        }

        /* 等待时长对齐到下一分钟边界（最长 60s），让分钟跳变能尽快响应。 */
        TickType_t timeout = pdMS_TO_TICKS(30000);
        struct tm tm_pre;
        if (time_sync_get_local(&tm_pre)) {
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
        if (!display_policy_clock_may_auto_refresh()) {
            continue;
        }

        struct tm tm;
        if (!time_sync_get_local(&tm)) {
            continue;
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

        bool full_step_due = minute_changed &&
                             (CLOCK_FULL_REFRESH_STEP_MIN <= 1 ||
                              (tm.tm_min % CLOCK_FULL_REFRESH_STEP_MIN) == 0);

        bool need_render = false;

        if (cfg_changed)                          need_render = true;
        if (sw_visibility_changed)                need_render = true;
        if (hour_changed || day_changed)          need_render = true;
        if (show_weather && weather_data_changed) need_render = true;
        if (full_step_due)                        need_render = true;
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

        ESP_LOGI(TAG,
                 "tick %02d:%02d -> render full  min=%d step=%d hr=%d day=%d cfg=%d "
                 "sw_vis=%d wx=%d ff=%d",
                 tm.tm_hour, tm.tm_min,
                 minute_changed, full_step_due, hour_changed, day_changed, cfg_changed,
                 sw_visibility_changed, weather_data_changed, s_force_full);

        render_clock(false);
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

    if (s_event && actually_changed)
        xEventGroupSetBits(s_event, BIT_CFG_CHANGED);
    if (cfg->show_weather)
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

    display_policy_set_manual_screen_active(false);

    /* 手动 show（按键/网页/启动）→ 强制下一次全刷。
     * render_clock 内部成功后会把 force_full_next 清回 false，
     * 这里只是保证从空闲/异常态恢复时第一次绝对是全刷。 */
    portENTER_CRITICAL(&s_state_mux);
    s_state.force_full_next = true;
    portEXIT_CRITICAL(&s_state_mux);

    /* 同步渲染（耗时 ~18.8s 全刷）。render_clock 末尾会更新 s_state，
     * 任务循环看到 s_state.last_minute 已是当前分钟 → 不会立即重复刷。 */
    render_clock(false);
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
