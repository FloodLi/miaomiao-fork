#include "weather.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include "miniz.h"

#include "fb_render.h"
#include "epd.h"
#include "scheduler.h"
#include "time_sync.h"
#include "display_policy.h"
#include "clock_display.h"

static const char *TAG    = "weather";
static const char *NVS_NS = "weather";

#define BIT_CFG_CHANGED     BIT0
#define BIT_EMBED_REQUEST   BIT1
#define MAX_RESP_LEN     4096

static weather_config_t s_cfg = {
    .enabled     = false,
    .api_key     = "",
    .api_host    = "",
    .location    = "",
    .city_name   = "",
    .refresh_min = 60,
};

static weather_data_t   s_data;
static EventGroupHandle_t s_event;
static SemaphoreHandle_t  s_fetch_mutex;
static volatile bool    s_skip_initial_task_fetch;

/* ── weather icons (24x24 bitmaps, 72 bytes each) ────────────────── */

static const uint8_t icon_sunny[72] = {
    0x00,0x18,0x00, 0x00,0x18,0x00, 0x02,0x18,0x40, 0x01,0x00,0x80,
    0x00,0x81,0x00, 0x00,0xFF,0x00, 0x01,0xFF,0x80, 0x03,0xFF,0xC0,
    0x03,0xFF,0xC0, 0x07,0xFF,0xE0, 0x67,0xFF,0xE6, 0xF7,0xFF,0xEF,
    0x67,0xFF,0xE6, 0x07,0xFF,0xE0, 0x03,0xFF,0xC0, 0x03,0xFF,0xC0,
    0x01,0xFF,0x80, 0x00,0xFF,0x00, 0x00,0x81,0x00, 0x01,0x00,0x80,
    0x02,0x18,0x40, 0x00,0x18,0x00, 0x00,0x18,0x00, 0x00,0x00,0x00,
};

static const uint8_t icon_cloudy[72] = {
    0x00,0x00,0x00, 0x00,0x00,0x00, 0x00,0x00,0x00, 0x00,0x7E,0x00,
    0x01,0xFF,0x80, 0x03,0xFF,0xC0, 0x07,0xFF,0xE0, 0x07,0xFF,0xE0,
    0x0F,0xFF,0xF0, 0x0F,0xFF,0xF0, 0x1F,0xFF,0xF8, 0x3F,0xFF,0xFC,
    0x7F,0xFF,0xFE, 0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF, 0x7F,0xFF,0xFE, 0x3F,0xFF,0xFC, 0x00,0x00,0x00,
    0x00,0x00,0x00, 0x00,0x00,0x00, 0x00,0x00,0x00, 0x00,0x00,0x00,
};

static const uint8_t icon_rain[72] = {
    0x00,0x00,0x00, 0x00,0x7E,0x00, 0x01,0xFF,0x80, 0x03,0xFF,0xC0,
    0x07,0xFF,0xE0, 0x0F,0xFF,0xF0, 0x3F,0xFF,0xFC, 0x7F,0xFF,0xFE,
    0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF, 0x7F,0xFF,0xFE, 0x3F,0xFF,0xFC,
    0x00,0x00,0x00, 0x08,0x82,0x08, 0x08,0x82,0x08, 0x04,0x41,0x04,
    0x04,0x41,0x04, 0x02,0x20,0x82, 0x02,0x20,0x82, 0x01,0x10,0x41,
    0x00,0x00,0x00, 0x00,0x00,0x00, 0x00,0x00,0x00, 0x00,0x00,0x00,
};

static const uint8_t icon_snow[72] = {
    0x00,0x00,0x00, 0x00,0x7E,0x00, 0x01,0xFF,0x80, 0x03,0xFF,0xC0,
    0x07,0xFF,0xE0, 0x0F,0xFF,0xF0, 0x3F,0xFF,0xFC, 0x7F,0xFF,0xFE,
    0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF, 0x7F,0xFF,0xFE, 0x3F,0xFF,0xFC,
    0x00,0x00,0x00, 0x04,0x42,0x20, 0x02,0x24,0x40, 0x01,0x18,0x80,
    0x02,0x24,0x40, 0x04,0x42,0x20, 0x00,0x00,0x00, 0x02,0x10,0x80,
    0x01,0x09,0x00, 0x00,0x86,0x00, 0x01,0x09,0x00, 0x02,0x10,0x80,
};

static const uint8_t *get_icon(int code)
{
    if (code >= 100 && code <= 103) return icon_sunny;
    if (code >= 104 && code <= 199) return icon_cloudy;
    if (code >= 300 && code <= 399) return icon_rain;
    if (code >= 400 && code <= 499) return icon_snow;
    return icon_cloudy;
}

/* ── NVS ──────────────────────────────────────────────────────────── */

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    uint8_t v8;
    if (nvs_get_u8(h, "enabled", &v8) == ESP_OK) s_cfg.enabled = (v8 != 0);

    size_t len;
    len = sizeof(s_cfg.api_key);
    nvs_get_str(h, "api_key", s_cfg.api_key, &len);
    len = sizeof(s_cfg.api_host);
    nvs_get_str(h, "api_host", s_cfg.api_host, &len);
    len = sizeof(s_cfg.location);
    nvs_get_str(h, "location", s_cfg.location, &len);
    len = sizeof(s_cfg.city_name);
    nvs_get_str(h, "city", s_cfg.city_name, &len);

    uint32_t v32;
    if (nvs_get_u32(h, "refresh", &v32) == ESP_OK) s_cfg.refresh_min = v32;

    nvs_close(h);
}

static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h,  "enabled",  s_cfg.enabled ? 1 : 0);
    nvs_set_str(h, "api_key",  s_cfg.api_key);
    nvs_set_str(h, "api_host", s_cfg.api_host);
    nvs_set_str(h, "location", s_cfg.location);
    nvs_set_str(h, "city",     s_cfg.city_name);
    nvs_set_u32(h, "refresh",  s_cfg.refresh_min);
    nvs_commit(h);
    nvs_close(h);
}

/* ── HTTP fetch helper ────────────────────────────────────────────── */

typedef struct {
    char  *buf;
    int    len;
    int    cap;
} resp_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && rb && evt->data && evt->data_len > 0) {
        int avail = rb->cap - 1 - rb->len;
        if (avail > 0) {
            int n = evt->data_len;
            if (n > avail)
                n = avail;
            memcpy(rb->buf + rb->len, evt->data, (size_t)n);
            rb->len += n;
            rb->buf[rb->len] = '\0';
        }
    }
    return ESP_OK;
}

static char *gunzip(const char *src, int src_len)
{
    if (src_len < 18 || (uint8_t)src[0] != 0x1F || (uint8_t)src[1] != 0x8B)
        return NULL;

    int pos = 10;
    uint8_t flags = (uint8_t)src[3];
    if (flags & 0x04) {
        if (pos + 2 > src_len) return NULL;
        int xlen = (uint8_t)src[pos] | ((uint8_t)src[pos + 1] << 8);
        pos += 2 + xlen;
    }
    if (flags & 0x08) { while (pos < src_len && src[pos]) pos++; pos++; }
    if (flags & 0x10) { while (pos < src_len && src[pos]) pos++; pos++; }
    if (flags & 0x02) pos += 2;
    if (pos >= src_len - 8) return NULL;

    uint32_t orig_size =
        (uint8_t)src[src_len - 4]       | ((uint8_t)src[src_len - 3] << 8) |
        ((uint8_t)src[src_len - 2] << 16) | ((uint8_t)src[src_len - 1] << 24);
    if (orig_size == 0 || orig_size > MAX_RESP_LEN - 1) return NULL;

    char *out = malloc(orig_size + 1);
    if (!out) return NULL;

    size_t r = tinfl_decompress_mem_to_mem(
        out, orig_size, src + pos, src_len - pos - 8, 0);
    if (r == (size_t)(-1)) { free(out); return NULL; }

    out[r] = '\0';
    ESP_LOGI(TAG, "gunzip: %d -> %u bytes", src_len, (unsigned)r);
    return out;
}

static char *http_get(const char *url)
{
    resp_buf_t rb = {
        .buf = malloc(MAX_RESP_LEN),
        .len = 0,
        .cap = MAX_RESP_LEN,
    };
    if (!rb.buf) return NULL;
    rb.buf[0] = '\0';

    esp_http_client_config_t config = {
        .url              = url,
        .event_handler    = http_event_handler,
        .user_data        = &rb,
        .timeout_ms       = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init failed for %s", url);
        free(rb.buf);
        return NULL;
    }
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "HTTP GET %s failed: err=%s status=%d", url,
                 esp_err_to_name(err), status);
        free(rb.buf);
        return NULL;
    }

    ESP_LOGI(TAG, "HTTP GET OK (%d bytes)", rb.len);

    if (rb.len >= 2 && (uint8_t)rb.buf[0] == 0x1F && (uint8_t)rb.buf[1] == 0x8B) {
        char *plain = gunzip(rb.buf, rb.len);
        free(rb.buf);
        if (!plain) {
            ESP_LOGE(TAG, "gzip decompression failed");
            return NULL;
        }
        return plain;
    }

    return rb.buf;
}

/* ── parse QWeather JSON ──────────────────────────────────────────── */

static bool parse_now(const char *json)
{
    ESP_LOGI(TAG, "parse_now: %.120s", json);

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGE(TAG, "parse_now: cJSON_Parse failed (gzip or bad data?)");
        return false;
    }

    const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(root, "code"));
    if (!code || strcmp(code, "200") != 0) {
        ESP_LOGW(TAG, "API error code: %s", code ? code : "null");
        cJSON_Delete(root);
        return false;
    }

    cJSON *now = cJSON_GetObjectItem(root, "now");
    if (!now) {
        ESP_LOGE(TAG, "parse_now: 'now' object missing");
        cJSON_Delete(root);
        return false;
    }

    const char *v;
    if ((v = cJSON_GetStringValue(cJSON_GetObjectItem(now, "temp"))))
        s_data.now.temp = atoi(v);
    if ((v = cJSON_GetStringValue(cJSON_GetObjectItem(now, "feelsLike"))))
        s_data.now.feels_like = atoi(v);
    if ((v = cJSON_GetStringValue(cJSON_GetObjectItem(now, "humidity"))))
        s_data.now.humidity = atoi(v);
    if ((v = cJSON_GetStringValue(cJSON_GetObjectItem(now, "pressure"))))
        s_data.now.pressure = atoi(v);
    if ((v = cJSON_GetStringValue(cJSON_GetObjectItem(now, "text"))))
        snprintf(s_data.now.text, sizeof(s_data.now.text), "%s", v);
    if ((v = cJSON_GetStringValue(cJSON_GetObjectItem(now, "windDir"))))
        snprintf(s_data.now.wind_dir, sizeof(s_data.now.wind_dir), "%s", v);
    if ((v = cJSON_GetStringValue(cJSON_GetObjectItem(now, "windScale"))))
        s_data.now.wind_scale = atoi(v);
    if ((v = cJSON_GetStringValue(cJSON_GetObjectItem(now, "icon"))))
        s_data.now.icon = atoi(v);

    cJSON_Delete(root);
    return true;
}

static bool parse_daily(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;

    const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(root, "code"));
    if (!code || strcmp(code, "200") != 0) {
        cJSON_Delete(root);
        return false;
    }

    cJSON *daily = cJSON_GetObjectItem(root, "daily");
    if (!daily) { cJSON_Delete(root); return false; }

    int count = cJSON_GetArraySize(daily);
    if (count > 3) count = 3;
    s_data.daily_count = count;

    for (int i = 0; i < count; i++) {
        cJSON *d = cJSON_GetArrayItem(daily, i);
        const char *v;
        if ((v = cJSON_GetStringValue(cJSON_GetObjectItem(d, "fxDate"))))
            snprintf(s_data.daily[i].date, sizeof(s_data.daily[i].date), "%s", v);
        if ((v = cJSON_GetStringValue(cJSON_GetObjectItem(d, "tempMax"))))
            s_data.daily[i].temp_max = atoi(v);
        if ((v = cJSON_GetStringValue(cJSON_GetObjectItem(d, "tempMin"))))
            s_data.daily[i].temp_min = atoi(v);
        if ((v = cJSON_GetStringValue(cJSON_GetObjectItem(d, "textDay"))))
            snprintf(s_data.daily[i].text_day, sizeof(s_data.daily[i].text_day), "%s", v);
        if ((v = cJSON_GetStringValue(cJSON_GetObjectItem(d, "iconDay"))))
            s_data.daily[i].icon_day = atoi(v);
    }

    cJSON_Delete(root);
    return true;
}

/* ── render weather to EPD ────────────────────────────────────────── */

static const char *weekday_zh[] = {
    "\xe6\x97\xa5",  /* 日 */
    "\xe4\xb8\x80",  /* 一 */
    "\xe4\xba\x8c",  /* 二 */
    "\xe4\xb8\x89",  /* 三 */
    "\xe5\x9b\x9b",  /* 四 */
    "\xe4\xba\x94",  /* 五 */
    "\xe5\x85\xad",  /* 六 */
};

static void render_weather(void)
{
    fb_t *fb = fb_create();
    if (!fb) {
        ESP_LOGE(TAG, "fb_create failed");
        return;
    }

    const int W = fb->width;
    const int H = fb->height;
    const int MX = W * 30 / 648;
    const bool big = (W >= 600);
    const int sc = big ? 2 : 1;

    fb_rect(fb, 4, 4, W - 8, H - 8, COLOR_BLACK);

    struct tm tm;
    time_sync_get_local(&tm);
    char date_str[32];
    snprintf(date_str, sizeof(date_str), "%04d/%02d/%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    fb_utf8_scaled(fb, MX, H * 16 / 480, date_str, COLOR_BLACK, sc);

    char weekday_str[32];
    snprintf(weekday_str, sizeof(weekday_str), "\xe6\x98\x9f\xe6\x9c\x9f%s",
             weekday_zh[tm.tm_wday]);
    fb_utf8_scaled(fb, W * 260 / 648, H * 16 / 480, weekday_str, COLOR_BLACK, sc);

    if (s_cfg.city_name[0])
        fb_utf8_scaled(fb, W * 480 / 648, H * 16 / 480, s_cfg.city_name, COLOR_BLACK, sc);

    int hdr_y = H * 54 / 480;
    fb_hline(fb, MX, hdr_y, W - 2 * MX, COLOR_BLACK);
    fb_hline(fb, MX, hdr_y + 1, W - 2 * MX, COLOR_BLACK);

    const uint8_t *icon = get_icon(s_data.now.icon);
    fb_bitmap(fb, MX + 10, H * 76 / 480, 24, 24, icon, COLOR_BLACK);

    char temp_str[16];
    snprintf(temp_str, sizeof(temp_str), "%d'C", s_data.now.temp);
    fb_utf8_scaled(fb, MX + 60, H * 68 / 480, temp_str, COLOR_BLACK, big ? 4 : 2);

    fb_utf8_scaled(fb, MX + 60, H * 140 / 480, s_data.now.text, COLOR_BLACK, sc);

    int rx = W * 360 / 648;

    char fl_str[48];
    snprintf(fl_str, sizeof(fl_str), "\xe4\xbd\x93\xe6\x84\x9f %d\xc2\xb0""C",
             s_data.now.feels_like);
    fb_utf8_scaled(fb, rx, H * 76 / 480, fl_str, COLOR_BLACK, sc);

    char hum_str[48];
    snprintf(hum_str, sizeof(hum_str), "\xe6\xb9\xbf\xe5\xba\xa6 %d%%",
             s_data.now.humidity);
    fb_utf8_scaled(fb, rx, H * 112 / 480, hum_str, COLOR_BLACK, sc);

    char wind_str[48];
    snprintf(wind_str, sizeof(wind_str), "%s %d\xe7\xba\xa7",
             s_data.now.wind_dir, s_data.now.wind_scale);
    fb_utf8_scaled(fb, rx, H * 148 / 480, wind_str, COLOR_BLACK, sc);

    int div_y = H * 190 / 480;
    fb_hline(fb, MX, div_y, W - 2 * MX, COLOR_BLACK);
    fb_hline(fb, MX, div_y + 1, W - 2 * MX, COLOR_BLACK);

    int col_w = (W - 2 * MX) / 3;
    for (int i = 0; i < s_data.daily_count && i < 3; i++) {
        int bx = MX + i * col_w;
        int by = H * 206 / 480;

        if (i > 0) fb_vline(fb, bx - 4, by, H * 100 / 480, COLOR_BLACK);

        const char *d = s_data.daily[i].date;
        char md[16];
        if (strlen(d) >= 10)
            snprintf(md, sizeof(md), "%s", d + 5);
        else
            snprintf(md, sizeof(md), "%s", d);
        fb_utf8_scaled(fb, bx + 8, by, md, COLOR_BLACK, sc);

        const uint8_t *di = get_icon(s_data.daily[i].icon_day);
        fb_bitmap(fb, bx + col_w - 30, by + 4, 24, 24, di, COLOR_BLACK);

        char t_str[32];
        snprintf(t_str, sizeof(t_str), "%d/%d'C",
                 s_data.daily[i].temp_max, s_data.daily[i].temp_min);
        fb_utf8_scaled(fb, bx + 8, by + H * 40 / 480, t_str, COLOR_BLACK, sc);

        fb_utf8_scaled(fb, bx + 8, by + H * 76 / 480, s_data.daily[i].text_day, COLOR_BLACK, sc);
    }

    char press_str[48];
    snprintf(press_str, sizeof(press_str), "\xe6\xb0\x94\xe5\x8e\x8b %dhPa",
             s_data.now.pressure);
    fb_utf8(fb, MX, H - 60, press_str, COLOR_BLACK);

    char footer[64];
    snprintf(footer, sizeof(footer), "\xe6\x9b\xb4\xe6\x96\xb0 %s",
             s_data.update_time);
    fb_utf8(fb, W - 200, H - 60, footer, COLOR_BLACK);

    /* export and display */
    const char *raw_path = "/spiffs/image.bin";
    esp_err_t err = fb_export(fb, raw_path);
    fb_destroy(fb);

    if (err == ESP_OK) {
        epd_display_from_file(raw_path);
        scheduler_notify_manual_show();
        ESP_LOGI(TAG, "Weather displayed on EPD");
    }
}

/* ── fetch + display ──────────────────────────────────────────────── */

esp_err_t weather_fetch_and_display(bool force_fullscreen)
{
    if (s_cfg.api_key[0] == '\0' || s_cfg.location[0] == '\0' ||
        s_cfg.api_host[0] == '\0') {
        ESP_LOGW(TAG, "Weather API key, host or location not configured");
        return ESP_ERR_INVALID_STATE;
    }

    /* 轮播占屏时禁止后台天气 HTTPS，与幻灯片/HTTP 服务错开，避免 lwIP 压力叠乘 */
    if (!display_policy_weather_may_network_fetch(force_fullscreen)) {
        ESP_LOGI(TAG, "Slideshow is active; weather fetch skipped (use web 「显示天气」 for full page)");
        return ESP_OK;
    }

    /* weather_task, boot_wx, and POST /weather_show must not overlap: shared s_data + concurrent
     * esp_http_client can corrupt lwIP/tcpip internal lists (LoadProhibited in vListInsert). */
    if (!s_fetch_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_fetch_mutex, pdMS_TO_TICKS(120000)) != pdTRUE) {
        ESP_LOGW(TAG, "Weather fetch already in progress, skipped");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;

    ESP_LOGI(TAG, "Fetching weather for location=%s (host=%s) ...",
             s_cfg.location, s_cfg.api_host);

    char url[256];

    /* fetch current weather */
    snprintf(url, sizeof(url),
             "https://%s/v7/weather/now?location=%s&key=%s",
             s_cfg.api_host, s_cfg.location, s_cfg.api_key);
    char *resp = http_get(url);
    if (!resp) {
        ret = ESP_FAIL;
        goto out;
    }
    bool ok = parse_now(resp);
    free(resp);
    if (!ok) {
        ESP_LOGE(TAG, "parse_now failed, aborting");
        ret = ESP_FAIL;
        goto out;
    }
    ESP_LOGI(TAG, "Current: %d°C, %s, icon=%d", s_data.now.temp, s_data.now.text, s_data.now.icon);

    /* Let the first TLS connection fully tear down in tcpip before opening the second; back-to-back
     * HTTPS on STA+AP has triggered pbuf_free(ref==0) in tcp_receive on some builds. */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* fetch 3-day forecast */
    snprintf(url, sizeof(url),
             "https://%s/v7/weather/3d?location=%s&key=%s",
             s_cfg.api_host, s_cfg.location, s_cfg.api_key);
    resp = http_get(url);
    if (resp) {
        parse_daily(resp);
        free(resp);
    }

    /* update time */
    time_sync_get_str(s_data.update_time, sizeof(s_data.update_time));
    if (strlen(s_data.update_time) > 5) {
        /* truncate to HH:MM */
        char *sp = strchr(s_data.update_time, ' ');
        if (sp) {
            memmove(s_data.update_time, sp + 1, strlen(sp + 1) + 1);
            if (strlen(s_data.update_time) > 5)
                s_data.update_time[5] = '\0';
        }
    }

    s_data.valid = true;

    if (display_policy_weather_use_full_page(force_fullscreen)) {
        render_weather();
        if (force_fullscreen)
            display_policy_set_manual_screen_active(true);
    } else {
        clock_display_notify_weather_data();
    }

out:
    xSemaphoreGive(s_fetch_mutex);
    return ret;
}

/* ── auto-refresh task ────────────────────────────────────────────── */

void weather_request_embedded_refresh(void)
{
    if (!display_policy_weather_may_network_fetch(false))
        return;
    if (s_event)
        xEventGroupSetBits(s_event, BIT_EMBED_REQUEST);
}

void weather_notify_slideshow_stopped(void)
{
    if (!s_event)
        return;
    clock_config_t cc;
    if (clock_display_get_config(&cc) != ESP_OK || !cc.enabled || !cc.show_weather)
        return;
    if (!s_cfg.enabled || s_cfg.api_key[0] == '\0' || s_cfg.location[0] == '\0' ||
        s_cfg.api_host[0] == '\0')
        return;
    xEventGroupSetBits(s_event, BIT_EMBED_REQUEST);
}

void weather_skip_initial_task_fetch_once(void)
{
    s_skip_initial_task_fetch = true;
}

void weather_skip_initial_task_fetch_cancel(void)
{
    s_skip_initial_task_fetch = false;
}

static void weather_task(void *arg)
{
    ESP_LOGI(TAG, "Weather task running");

    /* 联网稍稳后再拉，避免开机立刻失败 */
    vTaskDelay(pdMS_TO_TICKS(2500));
    bool skip_first = s_skip_initial_task_fetch;
    s_skip_initial_task_fetch = false;
    if (!skip_first && s_cfg.enabled && s_cfg.api_key[0] && s_cfg.location[0] && s_cfg.api_host[0])
        weather_fetch_and_display(false);

    for (;;) {
        /* 全页天气关或手动刷新周期=0：不跑定时拉取，但须响应 BIT_EMBED_REQUEST（时钟「天气摘要」） */
        if (!s_cfg.enabled || s_cfg.refresh_min == 0) {
            EventBits_t bits = xEventGroupWaitBits(
                s_event, BIT_CFG_CHANGED | BIT_EMBED_REQUEST,
                pdTRUE, pdFALSE, portMAX_DELAY);
            bool want = false;
            if ((bits & BIT_EMBED_REQUEST) && s_cfg.api_key[0] && s_cfg.location[0] &&
                s_cfg.api_host[0])
                want = true;
            if ((bits & BIT_CFG_CHANGED) && s_cfg.enabled && s_cfg.api_key[0] &&
                s_cfg.location[0] && s_cfg.api_host[0])
                want = true;
            if (want)
                weather_fetch_and_display(false);
            continue;
        }

        uint32_t wait_ms = s_cfg.refresh_min * 60 * 1000;
        EventBits_t bits = xEventGroupWaitBits(
            s_event, BIT_CFG_CHANGED | BIT_EMBED_REQUEST,
            pdTRUE, pdFALSE, pdMS_TO_TICKS(wait_ms));

        /* 网页/NVS 更新配置后应立即拉一次，不能仅 continue 等到下个周期 */
        if (bits & BIT_CFG_CHANGED) {
            if (s_cfg.enabled && s_cfg.api_key[0] && s_cfg.location[0] && s_cfg.api_host[0])
                weather_fetch_and_display(false);
            continue;
        }
        if (!s_cfg.enabled)
            continue;
        if (bits & BIT_EMBED_REQUEST) {
            if (s_cfg.api_key[0] && s_cfg.location[0] && s_cfg.api_host[0])
                weather_fetch_and_display(false);
            continue;
        }

        if (s_cfg.api_key[0] && s_cfg.location[0] && s_cfg.api_host[0])
            weather_fetch_and_display(false);
    }
}

/* ── public API ───────────────────────────────────────────────────── */

esp_err_t weather_init(void)
{
    s_event = xEventGroupCreate();
    if (!s_event) return ESP_ERR_NO_MEM;

    s_fetch_mutex = xSemaphoreCreateMutex();
    if (!s_fetch_mutex) {
        vEventGroupDelete(s_event);
        s_event = NULL;
        return ESP_ERR_NO_MEM;
    }

    memset(&s_data, 0, sizeof(s_data));
    nvs_load();

    BaseType_t ok = xTaskCreate(weather_task, "weather", 8192, NULL, 3, NULL);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Weather init (enabled=%d, location=%s, refresh=%lum)",
             s_cfg.enabled, s_cfg.location, (unsigned long)s_cfg.refresh_min);
    return ESP_OK;
}

esp_err_t weather_get_config(weather_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = s_cfg;
    return ESP_OK;
}

esp_err_t weather_set_config(const weather_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;
    nvs_save();
    ESP_LOGI(TAG, "Config saved: enabled=%d, loc=%s, refresh=%lum",
             s_cfg.enabled, s_cfg.location, (unsigned long)s_cfg.refresh_min);
    if (s_event)
        xEventGroupSetBits(s_event, BIT_CFG_CHANGED);
    return ESP_OK;
}

const weather_data_t *weather_get_data(void)
{
    return &s_data;
}

esp_err_t weather_display_cached(void)
{
    if (!s_data.valid) {
        ESP_LOGW(TAG, "no cached weather data");
        return ESP_ERR_INVALID_STATE;
    }
    if (!epd_is_ready())
        return ESP_ERR_INVALID_STATE;

    render_weather();
    display_policy_set_manual_screen_active(true);
    return ESP_OK;
}

void weather_get_data_copy(weather_data_t *out)
{
    if (!out)
        return;
    if (s_fetch_mutex)
        xSemaphoreTake(s_fetch_mutex, portMAX_DELAY);
    *out = s_data;
    if (s_fetch_mutex)
        xSemaphoreGive(s_fetch_mutex);
}
