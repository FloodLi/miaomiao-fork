/**
 * @file calendar_display.c
 *
 * 月历视图：参考常见电子纸日历（如 InkyDash / Inkycal 类项目）的极简层次 —
 * 高对比标题条、星期行、规整网格、当日高亮与农历/节日辅信息。
 *
 * 布局模型：固定 6×7 可见格，由格索引推导公历日（当月空白格不绘制）。
 */

#include "calendar_display.h"
#include "lunar.h"
#include "fb_render.h"
#include "ui_theme.h"
#include "epd.h"
#include "display_policy.h"
#include "time_sync.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"

/* -------------------------------------------------------------------------- */
/* 配置                                                                       */
/* -------------------------------------------------------------------------- */

static const char *TAG = "calendar";

enum {
    CAL_GRID_ROWS = 6,
    CAL_GRID_COLS = 7,
    CAL_GRID_CELLS = CAL_GRID_ROWS * CAL_GRID_COLS,
    CAL_YEAR_MIN = 1900,
    CAL_YEAR_MAX = 2100,
};

/* -------------------------------------------------------------------------- */
/* 并发：单月历渲染互斥（HTTP 可早于 init 调 show，故惰性建 mutex）              */
/* -------------------------------------------------------------------------- */

static SemaphoreHandle_t s_mutex;

esp_err_t calendar_display_init(void)
{
    s_mutex = xSemaphoreCreateBinary();
    if (!s_mutex)
        return ESP_ERR_NO_MEM;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

/** 与 clock/timetable 一致：优先 SNTP 后的本地时区；未同步则退回系统 localtime */
static void cal_today_ymd(int *out_y, int *out_m, int *out_d)
{
    struct tm loc;
    if (time_sync_get_local(&loc)) {
        *out_y = loc.tm_year + 1900;
        *out_m = loc.tm_mon + 1;
        *out_d = loc.tm_mday;
        return;
    }
    time_t now = time(NULL);
    localtime_r(&now, &loc);
    *out_y = loc.tm_year + 1900;
    *out_m = loc.tm_mon + 1;
    *out_d = loc.tm_mday;
}

/* -------------------------------------------------------------------------- */
/* 公历工具                                                                   */
/* -------------------------------------------------------------------------- */

static int cal_days_in_month(int y, int m)
{
    static const int dm[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)))
        return 29;
    return dm[m - 1];
}

/** 0=周日 … 6=周六；mktime 为主，正午规避时区/DST；失败则 Sakamoto */
static int cal_solar_weekday_sun0(int year, int month, int day)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = 12;
    t.tm_isdst = -1;
    time_t tt = mktime(&t);
    if (tt != (time_t)-1) {
        localtime_r(&tt, &t);
        return t.tm_wday;
    }
    static const int s[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int yy = year;
    if (month < 3)
        yy--;
    return (yy + yy / 4 - yy / 100 + yy / 400 + s[month - 1] + day) % 7;
}

static inline int cal_day_for_cell(int cell, int dow1, int dim)
{
    int d = cell - dow1 + 1;
    return (d >= 1 && d <= dim) ? d : 0;
}

/* -------------------------------------------------------------------------- */
/* 布局（由屏宽一次性算出，避免魔法数散落）                                    */
/* -------------------------------------------------------------------------- */

typedef struct {
    int fb_w, fb_h;
    int sc;
    int title_h;
    int wday_h;
    int footer_h;
    int grid_x;
    int grid_w;
    int grid_top;
    int grid_h;
    int row_h;
    int col_w;
} cal_layout_t;

static void cal_layout_build(cal_layout_t *L, int w, int h)
{
    L->fb_w = w;
    L->fb_h = h;
    L->sc = (w >= 600) ? 2 : 1;
    int s = L->sc;
    int mx = 12 * s;

    /* Month view is dense by nature. Keep the title readable, but render the
     * grid text at 1x so the page does not become a wall of heavy pixels. */
    L->title_h = 28 * s;
    L->wday_h  = 16 * s;
    L->footer_h = 26 * s;
    L->grid_x = mx;
    L->grid_w = w - 2 * mx;
    if (L->grid_w < CAL_GRID_COLS)
        L->grid_w = CAL_GRID_COLS;
    L->grid_top = L->title_h + L->wday_h;
    L->grid_h = h - L->grid_top - L->footer_h;
    if (L->grid_h < 1)
        L->grid_h = 1;
    L->row_h = L->grid_h / CAL_GRID_ROWS;
    L->col_w = L->grid_w / CAL_GRID_COLS;
    if (L->row_h < 1)
        L->row_h = 1;
    if (L->col_w < 1)
        L->col_w = 1;
}

/* -------------------------------------------------------------------------- */
/* 文本 / 图形                                                                */
/* -------------------------------------------------------------------------- */

static int cal_utf8_width_px(const char *str, int ascii_w, int cjk_w)
{
    int w = 0;
    for (const char *p = str; *p;) {
        if ((*p & 0x80) == 0) {
            w += ascii_w;
            p++;
        } else if ((*p & 0xE0) == 0xC0) {
            w += cjk_w;
            p += 2;
        } else if ((*p & 0xF0) == 0xE0) {
            w += cjk_w;
            p += 3;
        } else {
            w += cjk_w;
            p += 4;
        }
    }
    return w;
}

static int cal_isqrt_u32(uint32_t x)
{
    if (x == 0)
        return 0;
    uint32_t r = x;
    while (r * r > x)
        r = (r + x / r) >> 1;
    return (int)r;
}

static void cal_draw_circle_outline(fb_t *fb, int cx, int cy, int radius, fb_color_t c)
{
    if (radius <= 0)
        return;
    uint32_t r2 = (uint32_t)radius * (uint32_t)radius;
    uint32_t inner_r2 = (uint32_t)(radius - 1) * (uint32_t)(radius - 1);
    for (int dy = -radius; dy <= radius; dy++) {
        uint32_t outer = r2 - (uint32_t)(dy * dy);
        int outer_half = cal_isqrt_u32(outer);
        int inner_half = -1;
        if ((uint32_t)(dy * dy) <= inner_r2)
            inner_half = cal_isqrt_u32(inner_r2 - (uint32_t)(dy * dy));

        if (inner_half < 0) {
            fb_hline(fb, cx - outer_half, cy + dy, 2 * outer_half + 1, c);
        } else {
            int left_w = outer_half - inner_half;
            int right_x = cx + inner_half + 1;
            if (left_w > 0) {
                fb_hline(fb, cx - outer_half, cy + dy, left_w, c);
                fb_hline(fb, right_x, cy + dy, left_w, c);
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* 绘制：标题 / 星期 / 网格线 / 日格                                          */
/* -------------------------------------------------------------------------- */

static void cal_draw_title(fb_t *fb, const cal_layout_t *L, int year, int month)
{
    int W = L->fb_w;
    int s = L->sc;

    char title[40];
    snprintf(title, sizeof(title), "%d年%d月", year, month);
    char meta[48] = "";

    lunar_date_t ld;
    int mid = cal_days_in_month(year, month) / 2;
    if (lunar_from_solar(year, month, mid, &ld)) {
        snprintf(meta, sizeof(meta), "%s年[%s]",
                 lunar_year_gz(ld.year), lunar_year_sx(ld.year));
    }
    fb_fill_rect(fb, L->grid_x, 8 * s, 2 * s, 12 * s, COLOR_RED);
    fb_utf8_scaled(fb, L->grid_x + 8 * s, 6 * s, title, COLOR_BLACK, s);
    if (meta[0]) {
        int tw = cal_utf8_width_px(meta, 8, 16);
        fb_utf8_scaled(fb, W - tw - L->grid_x, 10 * s, meta, COLOR_BLACK, 1);
    }
    ui_draw_dotted_hline(fb, L->grid_x, L->title_h - 4 * s,
                         L->col_w * CAL_GRID_COLS,
                         COLOR_BLACK, 6);
}

static void cal_draw_weekday_row(fb_t *fb, const cal_layout_t *L)
{
    static const char *WD[] = {"日", "一", "二", "三", "四", "五", "六"};
    int y = L->title_h + (L->wday_h - 16) / 2;

    for (int c = 0; c < CAL_GRID_COLS; c++) {
        int x = L->grid_x + c * L->col_w + (L->col_w - 16) / 2;
        fb_color_t col = (c == 0 || c == CAL_GRID_COLS - 1) ? COLOR_RED : COLOR_BLACK;
        fb_utf8_scaled(fb, x, y, WD[c], col, 1);
    }
}

/** 先画线再画数字，避免格线与文字争同一像素（常见月历 widget 顺序） */
static void cal_draw_grid_lines(fb_t *fb, const cal_layout_t *L)
{
    int H   = L->fb_h;
    int top = L->grid_top;
    int x0  = L->grid_x;
    int w   = L->col_w * CAL_GRID_COLS;

    /* 横线：每行上边界。1px dotted keeps the page closer to a paper planner. */
    for (int r = 0; r <= CAL_GRID_ROWS; r++) {
        int y = top + r * L->row_h;
        if (y < H)
            ui_draw_dotted_hline(fb, x0, y, w, COLOR_BLACK, 6);
    }
    /* 竖线：仅画内部分隔，不再形成厚重外框 */
    int actual_gh = CAL_GRID_ROWS * L->row_h + 1;
    if (top + actual_gh > H) actual_gh = H - top;
    for (int c = 1; c < CAL_GRID_COLS; c++) {
        int x = x0 + c * L->col_w;
        if (x < L->fb_w)
            ui_draw_dotted_vline(fb, x, top, actual_gh, COLOR_BLACK, 6);
    }
}

static void cal_draw_day_cell(fb_t *fb, const cal_layout_t *L,
                              int year, int month, int day, int col,
                              int cell_x, int cell_y,
                              int today_y, int today_m, int today_d)
{
    int cw = L->col_w;
    int text_sc = 1;

    bool is_today = (year == today_y && month == today_m && day == today_d);
    bool weekend  = (col == 0 || col == CAL_GRID_COLS - 1);

    int sub_gap = 3;
    int total_h = 16 * text_sc + sub_gap + 16 * text_sc;
    int top_pad = (L->row_h - total_h) / 2;
    if (top_pad < 3)
        top_pad = 3;

    if (is_today) {
        int ccx    = cell_x + cw / 2;
        int ccy    = cell_y + top_pad + 8 * text_sc;
        int radius = 10;
        if (radius > cw / 2 - 1)      radius = cw / 2 - 1;
        if (radius > ccy - cell_y)     radius = ccy - cell_y;  /* 不超出格顶 */
        if (radius < 4)                radius = 4;
        cal_draw_circle_outline(fb, ccx, ccy, radius, COLOR_RED);
    }

    char ds[12];
    snprintf(ds, sizeof(ds), "%d", day);
    int dw = (int)strlen(ds) * 8 * text_sc;
    int dx = cell_x + (cw - dw) / 2;
    int dy = cell_y + top_pad;
    fb_color_t num_c = weekend ? COLOR_RED : COLOR_BLACK;
    fb_utf8_scaled(fb, dx, dy, ds, num_c, text_sc);

    lunar_date_t ld;
    const char *label  = NULL;
    bool        special = false;

    if (lunar_from_solar(year, month, day, &ld)) {
        const char *fest = lunar_festival(year, month, day, &ld);
        const char *term = lunar_solar_term(year, month, day);
        if (fest) {
            label   = fest;
            special = true;
        } else if (term) {
            label   = term;
            special = true;
        } else if (ld.day == 1) {
            label   = lunar_month_str(ld.month);
            special = true;
        } else {
            label = lunar_day_str(ld.day);
        }
    }

    if (!label)
        return;

    int ly = cell_y + top_pad + 16 * text_sc + sub_gap;
    fb_color_t sub_c = (special || weekend) ? COLOR_RED : COLOR_BLACK;

    int max_w = cw - 6;
    if (max_w < 8) max_w = 8;
    int lw = cal_utf8_width_px(label, 8 * text_sc, 16 * text_sc);
    int lx = (lw <= max_w) ? cell_x + (cw - lw) / 2 : cell_x + 3;
    fb_utf8_scaled_maxw(fb, lx, ly, label, sub_c, text_sc, max_w);
}

/* -------------------------------------------------------------------------- */
/* 合成一帧                                                                   */
/* -------------------------------------------------------------------------- */

static void cal_render_month(fb_t *fb, int year, int month,
                             int today_y, int today_m, int today_d)
{
    cal_layout_t L;
    cal_layout_build(&L, fb->width, fb->height);

    fb_clear(fb);
    ui_draw_page_frame(fb, UI_FRAME_RED_ACCENT | UI_FRAME_THIN);

    cal_draw_title(fb, &L, year, month);
    cal_draw_weekday_row(fb, &L);
    cal_draw_grid_lines(fb, &L);

    int dim = cal_days_in_month(year, month);
    int dow1 = cal_solar_weekday_sun0(year, month, 1);

    for (int cell = 0; cell < CAL_GRID_CELLS; cell++) {
        int d = cal_day_for_cell(cell, dow1, dim);
        if (d == 0)
            continue;

        int col = cell % CAL_GRID_COLS;
        int row = cell / CAL_GRID_COLS;
        int cx = L.grid_x + col * L.col_w;
        int cy = L.grid_top + row * L.row_h;

        cal_draw_day_cell(fb, &L, year, month, d, col, cx, cy,
                          today_y, today_m, today_d);
    }

    char today[24];
    snprintf(today, sizeof(today), "%04d-%02d-%02d", today_y, today_m, today_d);
    ui_draw_footer(fb, "CALENDAR", today);
}

static void cal_run_render(int year, int month, unsigned epoch)
{
    fb_t *fb = fb_create();
    if (!fb) {
        ESP_LOGE(TAG, "framebuffer alloc failed");
        return;
    }

    int ty, tm, td;
    cal_today_ymd(&ty, &tm, &td);

    cal_render_month(fb, year, month, ty, tm, td);

    if (!display_policy_epoch_is_current(epoch)) {
        fb_destroy(fb);
        ESP_LOGI(TAG, "skip stale calendar render %d-%02d", year, month);
        return;
    }

    esp_err_t err = epd_display_fb_free(fb);

    if (err == ESP_OK) {
        display_policy_set_manual_screen_active(true);
        ESP_LOGI(TAG, "shown %d-%02d", year, month);
    } else {
        ESP_LOGE(TAG, "epd_display_from_buffer: %s", esp_err_to_name(err));
    }
}

/* -------------------------------------------------------------------------- */
/* 任务入口（避免在 HTTP 栈上大块分配 / 长时间刷屏）                           */
/* -------------------------------------------------------------------------- */

typedef struct {
    int year;
    int month;
    unsigned epoch;
} cal_task_arg_t;

static void calendar_task(void *arg)
{
    cal_task_arg_t *a = (cal_task_arg_t *)arg;
    cal_run_render(a->year, a->month, a->epoch);
    free(a);
    xSemaphoreGive(s_mutex);
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------- */
/* 公开 API                                                                   */
/* -------------------------------------------------------------------------- */

esp_err_t calendar_display_show(int year, int month)
{
    if (!s_mutex)
        return ESP_ERR_INVALID_STATE;

    if (year == 0 || month == 0) {
        struct tm t;
        if (time_sync_get_local(&t)) {
            if (year == 0)
                year = t.tm_year + 1900;
            if (month == 0)
                month = t.tm_mon + 1;
        } else {
            time_t now = time(NULL);
        localtime_r(&now, &t);
            if (year == 0)
                year = t.tm_year + 1900;
            if (month == 0)
                month = t.tm_mon + 1;
        }
    }

    if (month < 1 || month > 12 || year < CAL_YEAR_MIN || year > CAL_YEAR_MAX)
        return ESP_ERR_INVALID_ARG;

    if (!epd_is_ready()) {
        ESP_LOGW(TAG, "EPD not ready");
        return ESP_ERR_INVALID_STATE;
    }

    unsigned epoch = display_policy_display_epoch();

    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        ESP_LOGW(TAG, "render already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    cal_task_arg_t *a = malloc(sizeof(*a));
    if (!a) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    a->year = year;
    a->month = month;
    a->epoch = epoch;

    if (xTaskCreate(calendar_task, "cal_render", 8192, a, 5, NULL) != pdPASS) {
        free(a);
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t calendar_display_show_current(void)
{
    struct tm tm;
    if (!time_sync_get_local(&tm)) {
        /* SNTP not yet synced — fall back to system localtime
         * (may be epoch 1970 on first boot, but better than refusing to render) */
        time_t now = time(NULL);
        localtime_r(&now, &tm);
        ESP_LOGW(TAG, "SNTP not synced, using system time for calendar");
    }
    return calendar_display_show(tm.tm_year + 1900, tm.tm_mon + 1);
}

void calendar_display_wait_render_idle(void)
{
    if (!s_mutex)
        return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    xSemaphoreGive(s_mutex);
}
