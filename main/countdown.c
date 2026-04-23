#include "countdown.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "nvs.h"

#include "epd.h"
#include "fb_render.h"
#include "time_sync.h"
#include "display_policy.h"
#include "scheduler.h"

static const char *TAG = "countdown";

#define NVS_NS   "countdown"
#define NVS_BLOB "cfg"

static countdown_config_t s_cfg;
static uint8_t s_page_idx;

/* ── NVS persistence ──────────────────────────────────────────────── */

static void nvs_load(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_cfg);
    if (nvs_get_blob(h, NVS_BLOB, &s_cfg, &len) != ESP_OK || len != sizeof(s_cfg))
        memset(&s_cfg, 0, sizeof(s_cfg));
    if (s_cfg.count > COUNTDOWN_MAX_ITEMS)
        s_cfg.count = 0;
    nvs_close(h);
}

static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_BLOB, &s_cfg, sizeof(s_cfg));
    nvs_commit(h);
    nvs_close(h);
}

/* ── Helpers ──────────────────────────────────────────────────────── */

static int days_remaining(const countdown_item_t *item)
{
    struct tm now_tm;
    if (!time_sync_get_local(&now_tm)) return -1;

    struct tm target = {0};
    target.tm_year = item->year - 1900;
    target.tm_mon  = item->month - 1;
    target.tm_mday = item->day;
    target.tm_hour = 0;

    time_t t_now    = mktime(&now_tm);
    time_t t_target = mktime(&target);

    now_tm.tm_hour = 0;
    now_tm.tm_min  = 0;
    now_tm.tm_sec  = 0;
    t_now = mktime(&now_tm);

    double diff = difftime(t_target, t_now);
    return (int)(diff / 86400.0);
}

/* ── EPD render ───────────────────────────────────────────────────── */

/** YYYY-MM-DD into buf[11]; clamps fields so no truncation warnings. */
static void format_date_ymd(char buf[11], int y, int mo, int da)
{
    if (y < 0) y = 0;
    else if (y > 9999) y = 9999;
    if (mo < 1) mo = 1;
    else if (mo > 12) mo = 12;
    if (da < 1) da = 1;
    else if (da > 31) da = 31;
    buf[0] = (char)('0' + (y / 1000) % 10);
    buf[1] = (char)('0' + (y / 100) % 10);
    buf[2] = (char)('0' + (y / 10) % 10);
    buf[3] = (char)('0' + y % 10);
    buf[4] = '-';
    buf[5] = (char)('0' + mo / 10);
    buf[6] = (char)('0' + mo % 10);
    buf[7] = '-';
    buf[8] = (char)('0' + da / 10);
    buf[9] = (char)('0' + da % 10);
    buf[10] = '\0';
}

static int text_px_width(const char *s, int scale)
{
    int w = 0;
    const uint8_t *p = (const uint8_t *)s;
    while (*p) {
        if (*p < 0x80) {
            w += 8 * scale;
            p++;
        } else {
            w += 16 * scale;
            p++;
            while (*p && (*p & 0xC0) == 0x80) p++;
        }
    }
    return w;
}

static void draw_header(fb_t *fb, int W)
{
    /* 标题与今日日期上下错开，避免窄屏横向挤在一起 */
    fb_utf8_scaled(fb, 10, 4, "倒计时", COLOR_BLACK, 2);

    struct tm now;
    if (time_sync_get_local(&now)) {
        char today[11];
        format_date_ymd(today, now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);
        int tw = 10 * 8;
        fb_utf8_scaled(fb, W - tw - 8, 6, today, COLOR_BLACK, 1);
    }

    fb_hline(fb, 0, 34, W, COLOR_BLACK);
    fb_hline(fb, 0, 35, W, COLOR_BLACK);
}

static void draw_hero(fb_t *fb, int W, int H, const countdown_item_t *item, int days)
{
    const int body_top   = 40;
    const int date_line_y = H - 20; /* 目标日固定在底部 */
    const int gap        = 8;

    char days_str[16];
    int abs_days = days < 0 ? -days : days;
    snprintf(days_str, sizeof(days_str), "%d", abs_days);

    /* 先定数字倍率，再按剩余垂直空间居中整块内容，避免与底栏重叠 */
    int d_scale = 6;
    if (abs_days >= 1000) d_scale = 4;
    else if (abs_days >= 100) d_scale = 5;
    if (H < 400) d_scale--;
    if (H < 320) d_scale--;
    if (d_scale < 2) d_scale = 2;

    int num_h = 16 * d_scale;
    const char *suf;
    fb_color_t suf_c = COLOR_BLACK;
    if (days > 0) {
        suf   = "天后";
        suf_c = COLOR_BLACK;
    } else if (days == 0) {
        suf   = "就是今天";
        suf_c = COLOR_RED;
    } else {
        suf   = "天前";
        suf_c = COLOR_BLACK;
    }
    int suf_h = 16 * 2; /* scale 2 中文行高 */
    const int title_h = 32; /* scale 2 单行约 32px */

    for (;;) {
        num_h = 16 * d_scale;
        int block_h = num_h + gap + suf_h;
        int need    = title_h + gap + block_h + gap + 18; /* 底栏上方留白 */
        if (date_line_y - body_top >= need || d_scale <= 2)
            break;
        d_scale--;
    }

    /* 标题：居中，过长则缩宽 */
    int title_max = W - 16;
    int tw_full   = text_px_width(item->title, 2);
    int title_y   = body_top + 4;
    if (tw_full <= title_max)
        fb_utf8_scaled(fb, (W - tw_full) / 2, title_y, item->title, COLOR_BLACK, 2);
    else
        fb_utf8_scaled_maxw(fb, 8, title_y, item->title, COLOR_BLACK, 2, title_max);

    int y_after_title = title_y + title_h + gap;

    /* 数字 + 副标题：在 title 与 date 行之间垂直居中 */
    int mid_top    = y_after_title;
    int mid_bottom = date_line_y - gap - 4;
    int mid_h      = mid_bottom - mid_top;
    int block_h    = num_h + gap + suf_h;
    int block_y    = mid_top + (mid_h > block_h ? (mid_h - block_h) / 2 : 0);
    if (block_y + block_h > mid_bottom)
        block_y = mid_bottom - block_h;
    if (block_y < mid_top)
        block_y = mid_top;

    if (days == 0) {
        int tw2 = text_px_width("TODAY", d_scale);
        fb_utf8_scaled(fb, (W - tw2) / 2, block_y, "TODAY", COLOR_RED, d_scale);
    } else {
        int dw = (int)strlen(days_str) * 8 * d_scale;
        fb_utf8_scaled(fb, (W - dw) / 2, block_y, days_str, COLOR_RED, d_scale);
    }

    int suf_y = block_y + num_h + gap;
    int sw    = text_px_width(suf, 2);
    fb_utf8_scaled(fb, (W - sw) / 2, suf_y, suf, suf_c, 2);

    char date_buf[11];
    format_date_ymd(date_buf, item->year, item->month, item->day);
    int dtw = 10 * 8;
    fb_utf8_scaled(fb, (W - dtw) / 2, date_line_y, date_buf, COLOR_BLACK, 1);
}

static void draw_cards(fb_t *fb, int W, int H, int active_count)
{
    const int body_y   = 40;
    const int margin   = 8;
    const int gap      = 6;

    int active_idx[COUNTDOWN_MAX_ITEMS];
    int n = 0;
    for (int i = 0; i < s_cfg.count && i < COUNTDOWN_MAX_ITEMS; i++) {
        if (s_cfg.items[i].active) active_idx[n++] = i;
    }
    if (n <= 0) return;

    int pages = (n + 3) / 4;
    if (pages < 1) pages = 1;
    int page = (int)(s_page_idx % pages);
    int start = page * 4;
    int show_n = n - start;
    if (show_n > 4) show_n = 4;
    if (show_n < 1) show_n = 1;

    int body_h         = H - body_y - 6;
    int card_h         = (body_h - gap * (show_n - 1)) / show_n;
    if (card_h > 84) card_h = 84;
    if (card_h < 36) card_h = 36;

    /* 右侧固定留给「天数+说明」，避免与左侧标题叠字 */
    int right_col = 120;
    if (W < 320) right_col = 100;
    int left_maxw = W - margin * 2 - right_col - 6;
    if (left_maxw < 80) left_maxw = 80;

    int y = body_y;
    for (int k = 0; k < show_n; k++) {
        int i = active_idx[start + k];
        int days = days_remaining(&s_cfg.items[i]);
        int abs_days = days < 0 ? -days : days;

        fb_fill_rect(fb, margin, y, W - margin * 2, 2, COLOR_RED);
        fb_rect(fb, margin, y + 2, W - margin * 2, card_h - 2, COLOR_BLACK);

        int title_scale = (card_h >= 58) ? 2 : 1;
        int title_y     = y + 6;
        int text_x      = margin + 8;
        fb_utf8_scaled_maxw(fb, text_x, title_y, s_cfg.items[i].title,
                            COLOR_BLACK, title_scale, left_maxw);

        char date_buf[11];
        format_date_ymd(date_buf, s_cfg.items[i].year,
                        s_cfg.items[i].month, s_cfg.items[i].day);
        int date_y = title_y + 16 * title_scale + 6;
        if (date_y + 16 > y + card_h - 4)
            date_y = y + card_h - 18;
        fb_utf8_scaled(fb, text_x, date_y, date_buf, COLOR_BLACK, 1);

        char days_str[16];
        snprintf(days_str, sizeof(days_str), "%d", days == 0 ? 0 : abs_days);

        int d_scale = 3;
        if (card_h < 52) d_scale = 2;
        if (abs_days >= 1000) d_scale = 2;

        int d_num_h = 16 * d_scale;
        const char *label;
        fb_color_t label_c = COLOR_BLACK;
        if (days > 0) label = "天后";
        else if (days == 0) {
            label  = "今天";
            label_c = COLOR_RED;
        } else
            label = "天前";

        int label_h = 16;
        int stack_h = d_num_h + 4 + label_h;
        int stack_y = y + 4 + (card_h - 4 - stack_h) / 2;
        if (stack_y < y + 4) stack_y = y + 4;

        int col_r = W - margin - 6;
        int dw    = (int)strlen(days_str) * 8 * d_scale;
        int num_x = col_r - dw;
        fb_utf8_scaled(fb, num_x, stack_y, days_str, COLOR_RED, d_scale);

        int lw     = text_px_width(label, 1);
        int label_x = col_r - lw;
        fb_utf8_scaled(fb, label_x, stack_y + d_num_h + 4, label, label_c, 1);

        y += card_h + gap;
    }

    if (pages > 1) {
        char page_str[16];
        snprintf(page_str, sizeof(page_str), "%d/%d", page + 1, pages);
        int pw = (int)strlen(page_str) * 8;
        fb_utf8_scaled(fb, W - pw - 8, H - 18, page_str, COLOR_BLACK, 1);
    }
}

esp_err_t countdown_show(void)
{
    if (!s_cfg.enabled) {
        ESP_LOGW(TAG, "Countdown disabled");
        return ESP_ERR_INVALID_STATE;
    }

    fb_t *fb = fb_create();
    if (!fb) return ESP_ERR_NO_MEM;
    fb_clear(fb);

    int W = epd_width(), H = epd_height();

    int active_count = 0;
    int first_active = -1;
    for (int i = 0; i < s_cfg.count && i < COUNTDOWN_MAX_ITEMS; i++) {
        if (s_cfg.items[i].active) {
            if (first_active < 0) first_active = i;
            active_count++;
        }
    }

    draw_header(fb, W);

    if (active_count == 0) {
        int tw = text_px_width("暂无倒计时", 2);
        fb_utf8_scaled(fb, (W - tw) / 2, H / 2 - 16, "暂无倒计时", COLOR_BLACK, 2);
        int tw2 = text_px_width("请通过网页添加", 1);
        fb_utf8_scaled(fb, (W - tw2) / 2, H / 2 + 24, "请通过网页添加", COLOR_BLACK, 1);
    } else if (active_count == 1) {
        int days = days_remaining(&s_cfg.items[first_active]);
        draw_hero(fb, W, H, &s_cfg.items[first_active], days);
    } else {
        draw_cards(fb, W, H, active_count);
        if (active_count > 4) {
            int pages = (active_count + 3) / 4;
            s_page_idx = (uint8_t)((s_page_idx + 1) % pages);
        } else {
            s_page_idx = 0;
        }
    }

    esp_err_t exp_err = fb_export(fb, "/spiffs/image.bin");
    fb_destroy(fb);
    if (exp_err != ESP_OK) {
        ESP_LOGE(TAG, "fb_export failed: %s", esp_err_to_name(exp_err));
        return exp_err;
    }
    epd_display_from_file("/spiffs/image.bin");

    display_policy_set_manual_screen_active(true);
    scheduler_notify_manual_show();

    ESP_LOGI(TAG, "Countdown displayed (%d active items)", active_count);
    return ESP_OK;
}

/* ── Public API ───────────────────────────────────────────────────── */

esp_err_t countdown_init(void)
{
    nvs_load();
    ESP_LOGI(TAG, "init ok (enabled=%d, items=%d)", s_cfg.enabled, s_cfg.count);
    return ESP_OK;
}

const countdown_config_t *countdown_get_config(void)
{
    return &s_cfg;
}

esp_err_t countdown_set_config(const countdown_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    memcpy(&s_cfg, cfg, sizeof(s_cfg));
    if (s_cfg.count > COUNTDOWN_MAX_ITEMS)
        s_cfg.count = COUNTDOWN_MAX_ITEMS;
    nvs_save();
    ESP_LOGI(TAG, "Config updated (enabled=%d, items=%d)", s_cfg.enabled, s_cfg.count);
    return ESP_OK;
}
