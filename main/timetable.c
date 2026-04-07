/**
 * @file timetable.c
 *
 * Period-based weekly timetable with semester week tracking.
 * Grid model: grid[day_mon0][period] -> {name, room, weeks_bitmask}.
 * EPD rendering: today's course card list with RED highlight for current period.
 */

#include "timetable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "display_policy.h"
#include "epd.h"
#include "esp_log.h"
#include "fb_render.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "time_sync.h"

static const char *TAG    = "timetable";
static const char *NVS_NS = "timetable";
static const char *NVS_KEY = "tt_cfg";

static timetable_config_t s_cfg;
static bool s_inited;


/* ── NVS ─────────────────────────────────────────────────────────── */

static esp_err_t tt_nvs_load(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK)
        return err;

    size_t len = sizeof(s_cfg);
    err = nvs_get_blob(h, NVS_KEY, &s_cfg, &len);
    nvs_close(h);

    if (err != ESP_OK || len != sizeof(s_cfg)) {
        memset(&s_cfg, 0, sizeof(s_cfg));
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

static esp_err_t tt_nvs_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;

    err = nvs_set_blob(h, NVS_KEY, &s_cfg, sizeof(s_cfg));
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ── Time helpers ────────────────────────────────────────────────── */

static int tt_minutes(uint8_t h, uint8_t m)
{
    return (int)h * 60 + (int)m;
}

/** 0=Mon ... 6=Sun, -1 if time not synced */
static int tt_weekday_mon0(const struct tm *tm)
{
    int w = tm->tm_wday;
    return (w == 0) ? 6 : (w - 1);
}

static int tt_now_minutes(const struct tm *tm)
{
    return tm->tm_hour * 60 + tm->tm_min;
}

/* ── Week calculation ────────────────────────────────────────────── */

int timetable_current_week(void)
{
    if (s_cfg.semester_start == 0)
        return 0;

    struct tm tm;
    if (!time_sync_get_local(&tm))
        return 0;

    time_t now = mktime(&tm);
    if (now == (time_t)-1)
        return 0;

    int diff_sec = (int)(now - (time_t)s_cfg.semester_start);
    if (diff_sec < 0)
        return 0;

    int week = diff_sec / (7 * 24 * 3600) + 1;
    return (week >= 1 && week <= TT_MAX_WEEKS) ? week : 0;
}

static bool tt_cell_active(const tt_cell_t *c, int week)
{
    if (c->name[0] == '\0')
        return false;
    if (week <= 0)
        return true;
    return (c->weeks & (1u << (week - 1))) != 0;
}

/* ── Grid query (current / next course) ──────────────────────────── */

static const tt_cell_t *tt_find_current(int wd, int now_min, int week)
{
    if (wd < 0 || wd >= TT_DAYS)
        return NULL;

    for (int p = 0; p < s_cfg.period_count; p++) {
        const tt_period_def_t *pd = &s_cfg.periods[p];
        int a = tt_minutes(pd->start_hour, pd->start_minute);
        int b = tt_minutes(pd->end_hour, pd->end_minute);
        if (now_min >= a && now_min < b) {
            const tt_cell_t *c = &s_cfg.grid[wd][p];
            if (tt_cell_active(c, week))
                return c;
        }
    }
    return NULL;
}

/** Find current period index, -1 if none */
static int tt_current_period_idx(int wd, int now_min)
{
    for (int p = 0; p < s_cfg.period_count; p++) {
        const tt_period_def_t *pd = &s_cfg.periods[p];
        int a = tt_minutes(pd->start_hour, pd->start_minute);
        int b = tt_minutes(pd->end_hour, pd->end_minute);
        if (now_min >= a && now_min < b)
            return p;
    }
    return -1;
}

static const tt_cell_t *tt_find_next(int wd, int now_min, int week,
                                     int *out_wait)
{
    if (wd < 0 || wd >= TT_DAYS)
        return NULL;

    for (int p = 0; p < s_cfg.period_count; p++) {
        const tt_period_def_t *pd = &s_cfg.periods[p];
        int a = tt_minutes(pd->start_hour, pd->start_minute);
        if (a <= now_min)
            continue;
        const tt_cell_t *c = &s_cfg.grid[wd][p];
        if (tt_cell_active(c, week)) {
            if (out_wait)
                *out_wait = a - now_min;
            return c;
        }
    }
    return NULL;
}

/* ── Public: config / query ──────────────────────────────────────── */

esp_err_t timetable_init(void)
{
    if (s_inited)
        return ESP_OK;
    tt_nvs_load();
    s_inited = true;
    ESP_LOGI(TAG, "init ok (enabled=%d, periods=%d)", s_cfg.enabled, s_cfg.period_count);
    return ESP_OK;
}

esp_err_t timetable_get_config(timetable_config_t *out)
{
    if (!out)
        return ESP_ERR_INVALID_ARG;
    *out = s_cfg;
    return ESP_OK;
}

esp_err_t timetable_set_config(const timetable_config_t *cfg)
{
    if (!cfg)
        return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;
    if (s_cfg.period_count > TT_MAX_PERIODS)
        s_cfg.period_count = TT_MAX_PERIODS;
    if (s_cfg.show_days != 7)
        s_cfg.show_days = 5;
    return tt_nvs_save();
}

bool timetable_get_current_course(const char **name, const char **room,
                                  int *minutes_remaining)
{
    if (!s_cfg.enabled)
        return false;

    struct tm tm;
    if (!time_sync_get_local(&tm))
        return false;

    int wd = tt_weekday_mon0(&tm);
    int now = tt_now_minutes(&tm);
    int week = timetable_current_week();

    const tt_cell_t *c = tt_find_current(wd, now, week);
    if (!c)
        return false;

    int cur_p = tt_current_period_idx(wd, now);
    if (cur_p < 0)
        return false;

    if (name)  *name = c->name;
    if (room)  *room = c->room;
    if (minutes_remaining) {
        int end = tt_minutes(s_cfg.periods[cur_p].end_hour,
                             s_cfg.periods[cur_p].end_minute);
        *minutes_remaining = end - now;
    }
    return true;
}

bool timetable_get_next_course(const char **name, const char **room,
                               int *minutes_until_start)
{
    if (!s_cfg.enabled)
        return false;

    struct tm tm;
    if (!time_sync_get_local(&tm))
        return false;

    int wd = tt_weekday_mon0(&tm);
    int now = tt_now_minutes(&tm);
    int week = timetable_current_week();

    int wait = 0;
    const tt_cell_t *c = tt_find_next(wd, now, week, &wait);

    if (!c) {
        int nd = (wd + 1) % TT_DAYS;
        c = tt_find_next(nd, -1, week, &wait);
        if (c) {
            int mins_left_today = 24 * 60 - now;
            wait += mins_left_today;
        }
    }

    if (!c)
        return false;

    if (name)  *name = c->name;
    if (room)  *room = c->room;
    if (minutes_until_start)
        *minutes_until_start = wait;
    return true;
}

/* ── EPD: today's course card view ──────────────────────────────── */

static const char *const TT_DAY_FULL[] = {
    "\xe5\x91\xa8\xe4\xb8\x80", "\xe5\x91\xa8\xe4\xba\x8c",
    "\xe5\x91\xa8\xe4\xb8\x89", "\xe5\x91\xa8\xe5\x9b\x9b",
    "\xe5\x91\xa8\xe4\xba\x94", "\xe5\x91\xa8\xe5\x85\xad",
    "\xe5\x91\xa8\xe6\x97\xa5",
};

static int tt_collect_today(int wd, int week, int *out, int max)
{
    int n = 0;
    for (int p = 0; p < s_cfg.period_count && n < max; p++) {
        if (tt_cell_active(&s_cfg.grid[wd][p], week))
            out[n++] = p;
    }
    return n;
}

static int tt_text_px(const char *s, int scale)
{
    int w = 0;
    while (*s) {
        unsigned char c = *(const unsigned char *)s;
        if (c < 0x80)        { w += 8 * scale; s += 1; }
        else if (c < 0xE0)   { w += 16 * scale; s += 2; }
        else if (c < 0xF0)   { w += 16 * scale; s += 3; }
        else                  { w += 16 * scale; s += 4; }
    }
    return w;
}

esp_err_t timetable_show_day(int day)
{
    if (!s_cfg.enabled)
        return ESP_ERR_INVALID_STATE;
    if (!epd_is_ready())
        return ESP_ERR_INVALID_STATE;
    if (day < 0 || day >= TT_DAYS)
        return ESP_ERR_INVALID_ARG;

    struct tm tm;
    bool have_time = time_sync_get_local(&tm);
    if (!have_time) {
        ESP_LOGW(TAG, "time not synced");
        return ESP_ERR_INVALID_STATE;
    }

    fb_t *fb = fb_create();
    if (!fb)
        return ESP_ERR_NO_MEM;
    fb_clear(fb);

    int W = fb->width;
    int H = fb->height;
    int s = (W >= 600) ? 2 : 1;

    int today   = tt_weekday_mon0(&tm);
    int now_min = tt_now_minutes(&tm);
    int week    = timetable_current_week();
    bool is_today = (day == today);
    int cur_p   = is_today ? tt_current_period_idx(day, now_min) : -1;

    /* ── title bar ── */
    int title_h = 22 * s;
    fb_fill_rect(fb, 0, 0, W, title_h, COLOR_BLACK);

    char title[64];
    if (week > 0)
        snprintf(title, sizeof(title), "%s \xc2\xb7 \xe7\xac\xac%d\xe5\x91\xa8",
                 TT_DAY_FULL[day], week);
    else
        snprintf(title, sizeof(title), "%s", TT_DAY_FULL[day]);
    fb_utf8_scaled(fb, 6 * s, 3 * s, title, COLOR_WHITE, s);

    if (is_today) {
        char ts[16];
        snprintf(ts, sizeof(ts), "%02d:%02d", tm.tm_hour, tm.tm_min);
        fb_utf8_scaled(fb, W - 5 * 8 * s - 6 * s, 3 * s, ts, COLOR_WHITE, s);
    }

    /* ── collect courses for the requested day ── */
    int order[TT_MAX_PERIODS];
    int count = tt_collect_today(day, week, order, TT_MAX_PERIODS);

    if (count == 0) {
        /* "X无课" */
        char msg[32];
        snprintf(msg, sizeof(msg), "%s\xe6\x97\xa0\xe8\xaf\xbe", TT_DAY_FULL[day]);
        int mw = tt_text_px(msg, s + 1);
        int mx = (W - mw) / 2;
        int my = H / 2 - 12 * s;
        fb_utf8_scaled(fb, mx, my, msg, COLOR_BLACK, s + 1);
        int ly = my + 8 * (s + 1);
        fb_hline(fb, mx - 28 * s, ly, 20 * s, COLOR_BLACK);
        fb_hline(fb, mx + mw + 8 * s, ly, 20 * s, COLOR_BLACK);
        esp_err_t err = epd_display_from_buffer(fb);
        fb_destroy(fb);
        return err;
    }

    /* ── agenda layout ── */
    int lm       = 14 * s;
    int rm       = 10 * s;
    int bar_w    = 4 * s;
    int bar_gap  = 3 * s;
    int footer_h = 16;
    int y_top    = title_h + 6 * s;
    int avail    = H - y_top - footer_h - 2;
    int block_h  = avail / count;

    bool two_line;
    int name_sc;
    if (block_h >= 56 * s) {
        block_h = 56 * s;
        name_sc = s + 1;
        two_line = true;
    } else if (block_h >= 36 * s) {
        name_sc = s;
        two_line = true;
    } else {
        name_sc = s;
        two_line = false;
    }

    for (int i = 0; i < count; i++) {
        int p = order[i];
        const tt_cell_t *cell = &s_cfg.grid[day][p];
        const tt_period_def_t *pd = &s_cfg.periods[p];
        bool is_now = (p == cur_p);

        int by = y_top + i * block_h;
        if (by + 16 * s > H - footer_h)
            break;

        if (two_line) {
            int text_h = 16 * s + 2 * s + 16 * name_sc;
            int ty = by + (block_h - text_h) / 2;

            if (is_now)
                fb_fill_rect(fb, lm - bar_w - bar_gap, ty,
                             bar_w, text_h, COLOR_RED);

            char info[48];
            snprintf(info, sizeof(info),
                     "\xe7\xac\xac%d\xe8\x8a\x82  %02d:%02d-%02d:%02d",
                     p + 1,
                     pd->start_hour, pd->start_minute,
                     pd->end_hour, pd->end_minute);
            fb_utf8_scaled(fb, lm, ty, info, COLOR_BLACK, s);

            if (is_now) {
                const char *tag = "\xe8\xbf\x9b\xe8\xa1\x8c\xe4\xb8\xad";
                int tw = tt_text_px(tag, s);
                fb_utf8_scaled(fb, W - rm - tw, ty, tag, COLOR_RED, s);
            }

            int ny = ty + 16 * s + 2 * s;
            int content_w = W - lm - rm;

            int room_w = 0;
            if (cell->room[0])
                room_w = tt_text_px(cell->room, s);

            int name_max = content_w;
            if (room_w > 0) {
                name_max = content_w - room_w - 8 * s;
                int room_y = ny + (16 * name_sc - 16 * s) / 2;
                fb_utf8_scaled(fb, W - rm - room_w, room_y,
                               cell->room, COLOR_BLACK, s);
            }
            fb_utf8_scaled_maxw(fb, lm, ny, cell->name,
                                COLOR_BLACK, name_sc, name_max);

            if (i < count - 1) {
                int sep_y = by + block_h - 1;
                fb_hline(fb, lm, sep_y, W - lm - rm, COLOR_BLACK);
            }
        } else {
            int ly = by + (block_h - 16 * s) / 2;

            if (is_now)
                fb_fill_rect(fb, lm - bar_w - bar_gap, ly,
                             bar_w, 16 * s, COLOR_RED);

            char prefix[24];
            snprintf(prefix, sizeof(prefix), "%d %02d:%02d",
                     p + 1, pd->start_hour, pd->start_minute);
            int pw = fb_utf8_scaled(fb, lm, ly, prefix, COLOR_BLACK, s);

            int nx = lm + pw + 8 * s;
            int nmax = W - nx - rm;

            if (cell->room[0]) {
                int rw = tt_text_px(cell->room, s);
                nmax -= rw + 8 * s;
                fb_utf8_scaled(fb, W - rm - rw, ly,
                               cell->room, COLOR_BLACK, s);
            }
            if (nmax < 16 * s) nmax = 16 * s;
            fb_utf8_scaled_maxw(fb, nx, ly, cell->name,
                                COLOR_BLACK, s, nmax);

            if (i < count - 1) {
                int sep_y = by + block_h - 1;
                fb_hline(fb, lm, sep_y, W - lm - rm, COLOR_BLACK);
            }
        }
    }

    /* ── footer ── */
    {
        char foot[48];
        snprintf(foot, sizeof(foot),
                 "%s %d \xe8\x8a\x82\xe8\xaf\xbe",
                 TT_DAY_FULL[day], count);
        int fw = tt_text_px(foot, 1);
        fb_utf8_scaled(fb, (W - fw) / 2, H - footer_h, foot, COLOR_BLACK, 1);
    }

    esp_err_t err = epd_display_from_buffer(fb);
    fb_destroy(fb);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "epd: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "show day=%d: %d courses (week=%d)", day, count, week);
    return ESP_OK;
}

esp_err_t timetable_show(void)
{
    struct tm tm;
    if (!time_sync_get_local(&tm))
        return ESP_ERR_INVALID_STATE;
    return timetable_show_day(tt_weekday_mon0(&tm));
}

esp_err_t timetable_show_current(void)
{
    return timetable_show();
}

/* ── Weeks bitmask <-> string ────────────────────────────────────── */

static uint32_t tt_parse_weeks(const char *s)
{
    if (!s || !s[0])
        return 0x01FFFFFFu; /* all 25 weeks */

    uint32_t mask = 0;
    const char *p = s;

    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;

        int a = (int)strtol(p, (char **)&p, 10);
        if (a < 1 || a > TT_MAX_WEEKS) {
            while (*p && *p != ',') p++;
            continue;
        }

        if (*p == '-') {
            p++;
            int b = (int)strtol(p, (char **)&p, 10);
            if (b < a) b = a;
            if (b > TT_MAX_WEEKS) b = TT_MAX_WEEKS;
            for (int i = a; i <= b; i++)
                mask |= (1u << (i - 1));
        } else {
            mask |= (1u << (a - 1));
        }
    }
    return mask ? mask : 0x01FFFFFFu;
}

static void tt_weeks_to_str(uint32_t mask, char *buf, int buflen)
{
    if (mask == 0 || mask == 0x01FFFFFFu) {
        snprintf(buf, buflen, "1-%d", TT_MAX_WEEKS);
        return;
    }

    buf[0] = '\0';
    int pos = 0;
    int i = 1;
    while (i <= TT_MAX_WEEKS) {
        if (!(mask & (1u << (i - 1)))) { i++; continue; }
        int start = i;
        while (i <= TT_MAX_WEEKS && (mask & (1u << (i - 1)))) i++;
        int end = i - 1;

        if (pos > 0 && pos < buflen - 1)
            pos += snprintf(buf + pos, buflen - pos, ",");

        if (start == end)
            pos += snprintf(buf + pos, buflen - pos, "%d", start);
        else
            pos += snprintf(buf + pos, buflen - pos, "%d-%d", start, end);
    }
}

/* ── HTTP: GET /timetable.json ───────────────────────────────────── */

esp_err_t timetable_http_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json oom");
        return ESP_OK;
    }

    cJSON_AddBoolToObject(root, "enabled", s_cfg.enabled);
    cJSON_AddNumberToObject(root, "period_count", s_cfg.period_count);
    cJSON_AddNumberToObject(root, "show_days", s_cfg.show_days);
    cJSON_AddNumberToObject(root, "semester_start", s_cfg.semester_start);
    cJSON_AddNumberToObject(root, "current_week", timetable_current_week());

    /* periods */
    cJSON *periods = cJSON_AddArrayToObject(root, "periods");
    for (int p = 0; p < s_cfg.period_count; p++) {
        cJSON *po = cJSON_CreateObject();
        if (!po) continue;
        cJSON_AddNumberToObject(po, "start_hour", s_cfg.periods[p].start_hour);
        cJSON_AddNumberToObject(po, "start_minute", s_cfg.periods[p].start_minute);
        cJSON_AddNumberToObject(po, "end_hour", s_cfg.periods[p].end_hour);
        cJSON_AddNumberToObject(po, "end_minute", s_cfg.periods[p].end_minute);
        cJSON_AddItemToArray(periods, po);
    }

    /* grid[7][period_count] */
    cJSON *grid = cJSON_AddArrayToObject(root, "grid");
    for (int d = 0; d < TT_DAYS; d++) {
        cJSON *day_arr = cJSON_CreateArray();
        for (int p = 0; p < s_cfg.period_count; p++) {
            const tt_cell_t *c = &s_cfg.grid[d][p];
            if (c->name[0] == '\0') {
                cJSON_AddItemToArray(day_arr, cJSON_CreateNull());
                continue;
            }
            cJSON *co = cJSON_CreateObject();
            if (!co) { cJSON_AddItemToArray(day_arr, cJSON_CreateNull()); continue; }
            cJSON_AddStringToObject(co, "name", c->name);
            cJSON_AddStringToObject(co, "room", c->room);
            char wbuf[64];
            tt_weeks_to_str(c->weeks, wbuf, sizeof(wbuf));
            cJSON_AddStringToObject(co, "weeks", wbuf);
            cJSON_AddItemToArray(day_arr, co);
        }
        cJSON_AddItemToArray(grid, day_arr);
    }

    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json oom");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, str, HTTPD_RESP_USE_STRLEN);
    free(str);
    return ESP_OK;
}

/* ── HTTP: POST /timetable ───────────────────────────────────────── */

esp_err_t timetable_http_post_handler(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 8192) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad content length");
        return ESP_OK;
    }

    char *buf = malloc(total + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_OK;
    }

    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r <= 0) { free(buf); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv fail"); return ESP_OK; }
        received += r;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }

    timetable_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cJSON *j;
    j = cJSON_GetObjectItem(root, "enabled");
    cfg.enabled = (j && cJSON_IsTrue(j));

    j = cJSON_GetObjectItem(root, "show_days");
    cfg.show_days = (j && cJSON_IsNumber(j) && j->valueint == 7) ? 7 : 5;

    j = cJSON_GetObjectItem(root, "semester_start");
    if (j && cJSON_IsNumber(j))
        cfg.semester_start = (int32_t)j->valuedouble;

    j = cJSON_GetObjectItem(root, "period_count");
    if (j && cJSON_IsNumber(j)) {
        cfg.period_count = (uint8_t)j->valueint;
        if (cfg.period_count > TT_MAX_PERIODS)
            cfg.period_count = TT_MAX_PERIODS;
    }

    cJSON *periods = cJSON_GetObjectItem(root, "periods");
    if (periods && cJSON_IsArray(periods)) {
        int np = cJSON_GetArraySize(periods);
        if (np > TT_MAX_PERIODS) np = TT_MAX_PERIODS;
        if ((int)cfg.period_count < np)
            cfg.period_count = (uint8_t)np;

        for (int p = 0; p < np; p++) {
            cJSON *po = cJSON_GetArrayItem(periods, p);
            if (!po) continue;
            tt_period_def_t *pd = &cfg.periods[p];

            j = cJSON_GetObjectItem(po, "start_hour");
            if (j && cJSON_IsNumber(j)) pd->start_hour = (uint8_t)j->valueint;
            j = cJSON_GetObjectItem(po, "start_minute");
            if (j && cJSON_IsNumber(j)) pd->start_minute = (uint8_t)j->valueint;
            j = cJSON_GetObjectItem(po, "end_hour");
            if (j && cJSON_IsNumber(j)) pd->end_hour = (uint8_t)j->valueint;
            j = cJSON_GetObjectItem(po, "end_minute");
            if (j && cJSON_IsNumber(j)) pd->end_minute = (uint8_t)j->valueint;
        }
    }

    cJSON *grid = cJSON_GetObjectItem(root, "grid");
    if (grid && cJSON_IsArray(grid)) {
        int nd = cJSON_GetArraySize(grid);
        if (nd > TT_DAYS) nd = TT_DAYS;

        for (int d = 0; d < nd; d++) {
            cJSON *day_arr = cJSON_GetArrayItem(grid, d);
            if (!day_arr || !cJSON_IsArray(day_arr)) continue;

            int ns = cJSON_GetArraySize(day_arr);
            if (ns > TT_MAX_PERIODS) ns = TT_MAX_PERIODS;

            for (int p = 0; p < ns; p++) {
                cJSON *co = cJSON_GetArrayItem(day_arr, p);
                if (!co || cJSON_IsNull(co)) continue;

                tt_cell_t *cell = &cfg.grid[d][p];

                j = cJSON_GetObjectItem(co, "name");
                if (j && cJSON_IsString(j))
                    strncpy(cell->name, j->valuestring, TT_NAME_LEN - 1);

                j = cJSON_GetObjectItem(co, "room");
                if (j && cJSON_IsString(j))
                    strncpy(cell->room, j->valuestring, TT_ROOM_LEN - 1);

                j = cJSON_GetObjectItem(co, "weeks");
                if (j && cJSON_IsString(j))
                    cell->weeks = tt_parse_weeks(j->valuestring);
                else
                    cell->weeks = 0x01FFFFFFu;
            }
        }
    }

    cJSON_Delete(root);

    esp_err_t err = timetable_set_config(&cfg);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── HTTP: POST /timetable_show ──────────────────────────────────── */

esp_err_t timetable_show_http_handler(httpd_req_t *req)
{
    if (!epd_is_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"EPD not ready\"}");
        return ESP_OK;
    }

    /* parse optional {"day":0..6} from body */
    int show_day = -1;
    if (req->content_len > 0 && req->content_len < 128) {
        char buf[128];
        int r = httpd_req_recv(req, buf, sizeof(buf) - 1);
        if (r > 0) {
            buf[r] = '\0';
            cJSON *root = cJSON_Parse(buf);
            if (root) {
                cJSON *jd = cJSON_GetObjectItem(root, "day");
                if (cJSON_IsNumber(jd))
                    show_day = jd->valueint;
                cJSON_Delete(root);
            }
        }
    }

    /* drain remaining body */
    char tmp[64];
    while (req->content_len > 0) {
        int r = httpd_req_recv(req, tmp, sizeof(tmp));
        if (r <= 0) break;
    }

    display_policy_set_manual_screen_active(true);

    esp_err_t err;
    if (show_day >= 0 && show_day < TT_DAYS)
        err = timetable_show_day(show_day);
    else
        err = timetable_show();

    char json[96];
    snprintf(json, sizeof(json), "{\"ok\":%s}", err == ESP_OK ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
