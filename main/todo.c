#include "todo.h"

#include <string.h>
#include <stdio.h>

#include "cJSON.h"
#include "display_policy.h"
#include "epd.h"
#include "esp_log.h"
#include "http_internal.h"
#include "fb_render.h"
#include "nvs.h"
#include "time_sync.h"

static const char *TAG    = "todo";
static const char *NVS_NS = "todo";

static todo_config_t s_cfg;

/* ── NVS ─────────────────────────────────────────────────────────── */

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    uint8_t v8;
    if (nvs_get_u8(h, "enabled", &v8) == ESP_OK)
        s_cfg.enabled = (v8 != 0);

    size_t sz = sizeof(s_cfg.items);
    if (nvs_get_blob(h, "items", s_cfg.items, &sz) == ESP_OK)
        s_cfg.count = sz / sizeof(todo_item_t);

    nvs_close(h);
}

static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    nvs_set_u8(h, "enabled", s_cfg.enabled ? 1 : 0);
    nvs_set_blob(h, "items", s_cfg.items,
                 (size_t)s_cfg.count * sizeof(todo_item_t));
    nvs_commit(h);
    nvs_close(h);
}

/* ── EPD rendering ───────────────────────────────────────────────── */

static int count_pending(void)
{
    int n = 0;
    for (int i = 0; i < s_cfg.count; i++)
        if (!s_cfg.items[i].done) n++;
    return n;
}

static void render_todo(void)
{
    fb_t *fb = fb_create();
    if (!fb) return;
    fb_clear(fb);

    const int W = fb->width;
    const int H = fb->height;
    const int s = (W >= 600) ? 2 : 1;

    int done_n = 0;
    for (int i = 0; i < s_cfg.count; i++)
        if (s_cfg.items[i].done) done_n++;

    /* ── title bar ── */
    const int title_h = 26 * s;
    fb_fill_rect(fb, 0, 0, W, title_h, COLOR_BLACK);

    fb_utf8_scaled(fb, 8 * s, 5 * s,
                   "\xe5\xbe\x85\xe5\x8a\x9e\xe4\xba\x8b\xe9\xa1\xb9",
                   COLOR_WHITE, s);

    struct tm now;
    if (time_sync_get_local(&now)) {
        char tbuf[16];
        snprintf(tbuf, sizeof(tbuf), "%02d:%02d", now.tm_hour, now.tm_min);
        fb_utf8_scaled(fb, W - 5 * 8 * s - 8 * s, 5 * s, tbuf, COLOR_WHITE, s);
    }

    if (s_cfg.count == 0) {
        const char *empty = "\xe6\x9a\x82\xe6\x97\xa0\xe5\xbe\x85\xe5\x8a\x9e";
        int esc = s + 1;
        int mw = 4 * 16 * esc;
        fb_utf8_scaled(fb, (W - mw) / 2, H / 2 - 8 * esc, empty, COLOR_BLACK, esc);
        epd_display_fb_free(fb);
        return;
    }

    /* ── progress section ── */
    int prog_y = title_h + 6 * s;
    int pct = done_n * 100 / s_cfg.count;
    char pstr[64];
    snprintf(pstr, sizeof(pstr),
             "\xe5\xb7\xb2\xe5\xae\x8c\xe6\x88\x90 %d/%d  %d%%",
             done_n, (int)s_cfg.count, pct);
    fb_utf8_scaled(fb, 8 * s, prog_y, pstr, COLOR_BLACK, s);

    int bar_y = prog_y + 18 * s;
    int bar_x = 8 * s;
    int bar_w = W - 16 * s;
    int bar_h = 6 * s;
    fb_rect(fb, bar_x, bar_y, bar_w, bar_h, COLOR_BLACK);
    if (done_n > 0) {
        int fill = (bar_w - 2) * done_n / s_cfg.count;
        if (fill < 1) fill = 1;
        fb_fill_rect(fb, bar_x + 1, bar_y + 1, fill, bar_h - 2,
                     (pct >= 100) ? COLOR_BLACK : COLOR_RED);
    }

    /* ── item list ── */
    int list_y = bar_y + bar_h + 8 * s;
    int avail  = H - list_y - 4 * s;
    int n_show = s_cfg.count < 10 ? (int)s_cfg.count : 10;
    int item_h = avail / n_show;
    if (item_h > 28 * s) item_h = 28 * s;
    int sc   = (item_h >= 30) ? 2 : 1;
    int ch_h = 16 * sc;

    for (int i = 0; i < n_show; i++) {
        const todo_item_t *it = &s_cfg.items[i];
        int y      = list_y + i * item_h;
        int text_y = y + (item_h - ch_h) / 2;

        /* left priority bar */
        if (it->priority >= 2)
            fb_fill_rect(fb, 0, y + 2, 4 * s, item_h - 4, COLOR_RED);
        else if (it->priority == 1)
            fb_fill_rect(fb, 0, y + 2, 4 * s, item_h - 4, COLOR_BLACK);

        /* checkbox */
        int cb_x  = 10 * s;
        int cb_sz = ch_h - 2;
        if (cb_sz > 14 * s) cb_sz = 14 * s;
        if (cb_sz < 10) cb_sz = 10;
        int cb_y  = text_y + (ch_h - cb_sz) / 2;

        if (it->done) {
            fb_fill_rect(fb, cb_x, cb_y, cb_sz, cb_sz, COLOR_BLACK);
            /* white checkmark (✓) */
            for (int t = 0; t < 2; t++) {
                for (int d = 0; d <= cb_sz / 3; d++)
                    fb_pixel(fb,
                             cb_x + cb_sz / 4 + d,
                             cb_y + cb_sz * 2 / 5 + d + t, COLOR_WHITE);
                for (int d = 0; d <= cb_sz / 2; d++)
                    fb_pixel(fb,
                             cb_x + cb_sz / 4 + cb_sz / 3 + d,
                             cb_y + cb_sz * 2 / 5 + cb_sz / 3 - d + t,
                             COLOR_WHITE);
            }
        } else {
            fb_rect(fb, cb_x, cb_y, cb_sz, cb_sz, COLOR_BLACK);
        }

        /* text (reserve space for priority label if needed) */
        int tx    = cb_x + cb_sz + 8 * s;
        int pri_w = (it->priority > 0) ? (2 * 16 * s + 12 * s) : 0;
        int max_w = W - tx - pri_w - 8 * s;
        fb_utf8_scaled_maxw(fb, tx, text_y, it->text, COLOR_BLACK, sc, max_w);

        /* strikethrough for done */
        if (it->done) {
            int ly = text_y + ch_h / 2;
            fb_hline(fb, tx, ly, max_w, COLOR_BLACK);
        }

        /* priority label on right */
        if (it->priority >= 2)
            fb_utf8_scaled(fb, W - 2 * 16 * s - 8 * s, text_y,
                           "\xe7\xb4\xa7\xe6\x80\xa5", COLOR_RED, s);
        else if (it->priority == 1)
            fb_utf8_scaled(fb, W - 2 * 16 * s - 8 * s, text_y,
                           "\xe9\x87\x8d\xe8\xa6\x81", COLOR_BLACK, s);

        /* dotted separator */
        if (i < n_show - 1)
            for (int dx = 8 * s; dx < W - 8 * s; dx += 4)
                fb_pixel(fb, dx, y + item_h - 1, COLOR_BLACK);
    }

    epd_display_fb_free(fb);
}

/* ── public API ──────────────────────────────────────────────────── */

esp_err_t todo_init(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    nvs_load();
    ESP_LOGI(TAG, "init ok (enabled=%d, items=%d)", s_cfg.enabled, s_cfg.count);
    return ESP_OK;
}

esp_err_t todo_get_config(todo_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = s_cfg;
    return ESP_OK;
}

esp_err_t todo_set_config(const todo_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;
    if (s_cfg.count > TODO_MAX_ITEMS)
        s_cfg.count = TODO_MAX_ITEMS;
    nvs_save();
    return ESP_OK;
}

esp_err_t todo_show(void)
{
    if (!s_cfg.enabled)
        return ESP_ERR_INVALID_STATE;
    if (!epd_is_ready())
        return ESP_ERR_INVALID_STATE;

    render_todo();
    display_policy_set_manual_screen_active(true);
    ESP_LOGI(TAG, "displayed %d items (%d pending)",
             s_cfg.count, count_pending());
    return ESP_OK;
}

/* ── HTTP: GET /todo.json ────────────────────────────────────────── */

esp_err_t todo_http_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", s_cfg.enabled);
    cJSON *arr = cJSON_AddArrayToObject(root, "items");

    for (int i = 0; i < s_cfg.count; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "text", s_cfg.items[i].text);
        cJSON_AddBoolToObject(obj, "done", s_cfg.items[i].done);
        cJSON_AddNumberToObject(obj, "priority", s_cfg.items[i].priority);
        cJSON_AddItemToArray(arr, obj);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

/* ── HTTP: POST /todo ────────────────────────────────────────────── */

esp_err_t todo_http_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    int len = req->content_len;
    if (len <= 0 || len > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
        return ESP_FAIL;
    }

    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int received = 0;
    while (received < len) {
        int got = httpd_req_recv(req, buf + received, (size_t)(len - received));
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) { free(buf); return ESP_FAIL; }
        received += got;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }

    cJSON *j;
    j = cJSON_GetObjectItem(root, "enabled");
    if (cJSON_IsBool(j))
        s_cfg.enabled = cJSON_IsTrue(j);

    j = cJSON_GetObjectItem(root, "items");
    if (cJSON_IsArray(j)) {
        int n = cJSON_GetArraySize(j);
        if (n > TODO_MAX_ITEMS) n = TODO_MAX_ITEMS;
        s_cfg.count = (uint8_t)n;
        memset(s_cfg.items, 0, sizeof(s_cfg.items));

        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(j, i);
            if (!item) continue;

            cJSON *t = cJSON_GetObjectItem(item, "text");
            if (cJSON_IsString(t) && t->valuestring) {
                strncpy(s_cfg.items[i].text, t->valuestring, TODO_TEXT_LEN - 1);
                s_cfg.items[i].text[TODO_TEXT_LEN - 1] = '\0';
            }

            cJSON *d = cJSON_GetObjectItem(item, "done");
            if (cJSON_IsBool(d))
                s_cfg.items[i].done = cJSON_IsTrue(d);

            cJSON *p = cJSON_GetObjectItem(item, "priority");
            if (cJSON_IsNumber(p))
                s_cfg.items[i].priority = (uint8_t)p->valueint;
        }
    }

    nvs_save();
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── HTTP: POST /todo_show ───────────────────────────────────────── */

esp_err_t todo_show_http_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    if (!epd_is_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"EPD busy\"}");
        return ESP_OK;
    }

    /* allow showing even if not "enabled" — treat as manual push */
    if (s_cfg.count == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"no items\"}");
        return ESP_OK;
    }

    render_todo();
    display_policy_set_manual_screen_active(true);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}
