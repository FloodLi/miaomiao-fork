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

static weather_data_t     s_data;
static EventGroupHandle_t s_event;
static SemaphoreHandle_t  s_fetch_mutex;
static portMUX_TYPE       s_cfg_mux = portMUX_INITIALIZER_UNLOCKED;
static bool               s_skip_initial_task_fetch;

static void weather_config_snapshot(weather_config_t *out)
{
    portENTER_CRITICAL(&s_cfg_mux);
    *out = s_cfg;
    portEXIT_CRITICAL(&s_cfg_mux);
}

static bool weather_config_has_api(const weather_config_t *cfg)
{
    return cfg->api_key[0] != '\0' &&
           cfg->location[0] != '\0' &&
           cfg->api_host[0] != '\0';
}

/* 鈹€鈹€ weather icons (24x24 bitmaps, 72 bytes each) 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€ */

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

/* 鈹€鈹€ NVS 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€ */

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

static void nvs_save_snapshot(const weather_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h,  "enabled",  cfg->enabled ? 1 : 0);
    nvs_set_str(h, "api_key",  cfg->api_key);
    nvs_set_str(h, "api_host", cfg->api_host);
    nvs_set_str(h, "location", cfg->location);
    nvs_set_str(h, "city",     cfg->city_name);
    nvs_set_u32(h, "refresh",  cfg->refresh_min);
    nvs_commit(h);
    nvs_close(h);
}

/* 鈹€鈹€ HTTP fetch helper 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€ */

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

#define WEATHER_HTTP_TIMEOUT_MS 25000
#define WEATHER_HTTP_RETRIES    3

static char *http_get(const char *url)
{
    resp_buf_t rb = {
        .buf = malloc(MAX_RESP_LEN),
        .len = 0,
        .cap = MAX_RESP_LEN,
    };
    if (!rb.buf) return NULL;

    esp_err_t err  = ESP_FAIL;
    int       status = 0;

    for (int attempt = 0; attempt < WEATHER_HTTP_RETRIES; attempt++) {
        rb.len = 0;
        rb.buf[0] = '\0';

        if (attempt > 0) {
            ESP_LOGW(TAG, "HTTP GET retry %d/%d after connect failure", attempt + 1,
                     WEATHER_HTTP_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        esp_http_client_config_t config = {
            .url               = url,
            .event_handler     = http_event_handler,
            .user_data         = &rb,
            .timeout_ms        = WEATHER_HTTP_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(TAG, "esp_http_client_init failed for %s", url);
            free(rb.buf);
            return NULL;
        }
        esp_http_client_set_header(client, "Accept-Encoding", "identity");
        err = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err == ESP_OK && status == 200)
            break;
        ESP_LOGW(TAG, "HTTP GET attempt %d: err=%s status=%d", attempt + 1,
                 esp_err_to_name(err), status);
    }

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "HTTP GET %s failed after %d tries: err=%s status=%d", url,
                 WEATHER_HTTP_RETRIES, esp_err_to_name(err), status);
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

/* 鈹€鈹€ parse QWeather JSON 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€ */

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

/* 鈹€鈹€ render weather to EPD 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€ */

static const char *weekday_zh[] = {
    "\xe6\x97\xa5",  /* 鏃?*/
    "\xe4\xb8\x80",  /* 涓€ */
    "\xe4\xba\x8c",  /* 浜?*/
    "\xe4\xb8\x89",  /* 涓?*/
    "\xe5\x9b\x9b",  /* 鍥?*/
    "\xe4\xba\x94",  /* 浜?*/
    "\xe5\x85\xad",  /* 鍏?*/
};

static void render_weather(void)
{
    weather_config_t cfg_local;
    weather_config_snapshot(&cfg_local);

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
    bool time_ok = time_sync_get_local(&tm);
    char date_str[32];
    if (time_ok) {
        snprintf(date_str, sizeof(date_str), "%04d/%02d/%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    } else {
        snprintf(date_str, sizeof(date_str), "--/--/--");
    }
    fb_utf8_scaled(fb, MX, H * 16 / 480, date_str, COLOR_BLACK, sc);

    char weekday_str[32];
    if (time_ok) {
        snprintf(weekday_str, sizeof(weekday_str), "\xe6\x98\x9f\xe6\x9c\x9f%s",
                 weekday_zh[tm.tm_wday]);
        fb_utf8_scaled(fb, W * 260 / 648, H * 16 / 480, weekday_str, COLOR_BLACK, sc);
    }

    if (cfg_local.city_name[0]) {
        int city_x = W * 480 / 648;
        int city_max = W - city_x - MX;
        if (city_max < 16) city_max = 16;
        fb_utf8_scaled_maxw(fb, city_x, H * 16 / 480, cfg_local.city_name, COLOR_BLACK, sc, city_max);
    }

    int hdr_y = H * 54 / 480;
    fb_hline(fb, MX, hdr_y, W - 2 * MX, COLOR_BLACK);
    fb_hline(fb, MX, hdr_y + 1, W - 2 * MX, COLOR_BLACK);

    const uint8_t *icon = get_icon(s_data.now.icon);
    fb_bitmap(fb, MX + 10, H * 76 / 480, 24, 24, icon, COLOR_BLACK);

    char temp_str[16];
    snprintf(temp_str, sizeof(temp_str), "%d'C", s_data.now.temp);
    fb_utf8_scaled(fb, MX + 60, H * 68 / 480, temp_str, COLOR_BLACK, big ? 4 : 2);

    int rx = W * 360 / 648;
    {
        int txt_x = MX + 60;
        int txt_max = rx - txt_x - 8;
        if (txt_max < 16) txt_max = 16;
        fb_utf8_scaled_maxw(fb, txt_x, H * 140 / 480, s_data.now.text, COLOR_BLACK, sc, txt_max);
    }

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
        int col_max = col_w - 16;
        if (col_max < 16) col_max = 16;
        fb_utf8_scaled_maxw(fb, bx + 8, by, md, COLOR_BLACK, sc, col_max);

        const uint8_t *di = get_icon(s_data.daily[i].icon_day);
        int icon_x = bx + col_w - 30;
        if (icon_x < bx + 8) icon_x = bx + 8;
        fb_bitmap(fb, icon_x, by + 4, 24, 24, di, COLOR_BLACK);

        char t_str[32];
        snprintf(t_str, sizeof(t_str), "%d/%d'C",
                 s_data.daily[i].temp_max, s_data.daily[i].temp_min);
        fb_utf8_scaled_maxw(fb, bx + 8, by + H * 40 / 480, t_str, COLOR_BLACK, sc, col_max);

        fb_utf8_scaled_maxw(fb, bx + 8, by + H * 76 / 480, s_data.daily[i].text_day, COLOR_BLACK, sc, col_max);
    }

    char press_str[48];
    snprintf(press_str, sizeof(press_str), "\xe6\xb0\x94\xe5\x8e\x8b %dhPa",
             s_data.now.pressure);
    fb_utf8(fb, MX, H - 60, press_str, COLOR_BLACK);

    char footer[64];
    snprintf(footer, sizeof(footer), "\xe6\x9b\xb4\xe6\x96\xb0 %s",
             s_data.update_time);
    /* "鏇存柊"=2脳16px + space=8px + "HH:MM"鈮?脳8px = 80px at scale 1 */
    int footer_w = 2 * 16 + 8 + (int)strlen(s_data.update_time) * 8;
    int footer_x = W - footer_w - MX;
    if (footer_x < MX) footer_x = MX;
    fb_utf8(fb, footer_x, H - 60, footer, COLOR_BLACK);

    /* export and display */
    const char *raw_path = "/spiffs/image.bin";
    fb_raw_file_lock();
    esp_err_t err = fb_export(fb, raw_path);
    fb_destroy(fb);

    if (err == ESP_OK && epd_is_ready()) {
        esp_err_t disp_err = epd_display_from_file(raw_path);
        if (disp_err == ESP_OK) {
            scheduler_notify_manual_show();
            ESP_LOGI(TAG, "Weather displayed on EPD");
        } else {
            ESP_LOGE(TAG, "Weather display failed: %s", esp_err_to_name(disp_err));
        }
    } else if (err == ESP_OK) {
        ESP_LOGW(TAG, "Weather frame ready but EPD not ready, skip refresh");
    }
    fb_raw_file_unlock();
}

/* 鈹€鈹€ fetch + display 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€ */

esp_err_t weather_fetch_and_display(bool force_fullscreen)
{
    weather_config_t cfg;
    weather_config_snapshot(&cfg);
    if (!weather_config_has_api(&cfg)) {
        ESP_LOGW(TAG, "Weather API key, host or location not configured");
        return ESP_ERR_INVALID_STATE;
    }

    /* 杞挱鍗犲睆鏃剁姝㈠悗鍙板ぉ姘?HTTPS锛屼笌骞荤伅鐗?HTTP 鏈嶅姟閿欏紑锛岄伩鍏?lwIP 鍘嬪姏鍙犱箻 */
    if (!display_policy_weather_may_network_fetch(force_fullscreen)) {
        ESP_LOGI(TAG, "Weather auto fetch skipped by display policy");
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
    bool notify_clock = false;

    ESP_LOGI(TAG, "Fetching weather for location=%s (host=%s) ...",
             cfg.location, cfg.api_host);

    char url[256];

    /* fetch current weather */
    snprintf(url, sizeof(url),
             "https://%s/v7/weather/now?location=%s&key=%s",
             cfg.api_host, cfg.location, cfg.api_key);
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
    ESP_LOGI(TAG, "Current: %d掳C, %s, icon=%d", s_data.now.temp, s_data.now.text, s_data.now.icon);

    /* Let the first TLS connection fully tear down in tcpip before opening the second; back-to-back
     * HTTPS on STA+AP has triggered pbuf_free(ref==0) in tcp_receive on some builds. */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* fetch 3-day forecast */
    snprintf(url, sizeof(url),
             "https://%s/v7/weather/3d?location=%s&key=%s",
             cfg.api_host, cfg.location, cfg.api_key);
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
        notify_clock = true;
    }

out:
    xSemaphoreGive(s_fetch_mutex);
    if (ret == ESP_OK && notify_clock)
        clock_display_notify_weather_data();
    return ret;
}

/* 鈹€鈹€ auto-refresh task 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€ */

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
    weather_config_t cfg;
    weather_config_snapshot(&cfg);
    if (!cfg.enabled || !weather_config_has_api(&cfg))
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

    /* 鑱旂綉绋嶇ǔ鍚庡啀鎷夛紝閬垮厤寮€鏈虹珛鍒诲け璐?*/
    vTaskDelay(pdMS_TO_TICKS(2500));
    bool skip_first = s_skip_initial_task_fetch;
    s_skip_initial_task_fetch = false;
    weather_config_t cfg;
    weather_config_snapshot(&cfg);
    if (!skip_first && cfg.enabled && weather_config_has_api(&cfg))
        weather_fetch_and_display(false);

    for (;;) {
        weather_config_snapshot(&cfg);
        /* 鍏ㄩ〉澶╂皵鍏虫垨鎵嬪姩鍒锋柊鍛ㄦ湡=0锛氫笉璺戝畾鏃舵媺鍙栵紝浣嗛』鍝嶅簲 BIT_EMBED_REQUEST锛堟椂閽熴€屽ぉ姘旀憳瑕併€嶏級 */
        if (!cfg.enabled || cfg.refresh_min == 0) {
            EventBits_t bits = xEventGroupWaitBits(
                s_event, BIT_CFG_CHANGED | BIT_EMBED_REQUEST,
                pdTRUE, pdFALSE, portMAX_DELAY);
            weather_config_snapshot(&cfg);
            bool want = false;
            if ((bits & BIT_EMBED_REQUEST) && weather_config_has_api(&cfg))
                want = true;
            if ((bits & BIT_CFG_CHANGED) && cfg.enabled && weather_config_has_api(&cfg))
                want = true;
            if (want)
                weather_fetch_and_display(false);
            continue;
        }

        uint32_t wait_ms = cfg.refresh_min * 60 * 1000;
        EventBits_t bits = xEventGroupWaitBits(
            s_event, BIT_CFG_CHANGED | BIT_EMBED_REQUEST,
            pdTRUE, pdFALSE, pdMS_TO_TICKS(wait_ms));
        weather_config_snapshot(&cfg);

        /* 缃戦〉/NVS 鏇存柊閰嶇疆鍚庡簲绔嬪嵆鎷変竴娆★紝涓嶈兘浠?continue 绛夊埌涓嬩釜鍛ㄦ湡 */
        if (bits & BIT_CFG_CHANGED) {
            if (cfg.enabled && weather_config_has_api(&cfg))
                weather_fetch_and_display(false);
            continue;
        }
        if (!cfg.enabled)
            continue;
        if (bits & BIT_EMBED_REQUEST) {
            if (weather_config_has_api(&cfg))
                weather_fetch_and_display(false);
            continue;
        }

        if (weather_config_has_api(&cfg))
            weather_fetch_and_display(false);
    }
}

/* 鈹€鈹€ public API 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€ */

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

    /* HTTPS+TLS 鏍堝紑閿€澶э紱鍥哄畾鍒?PRO CPU 0 涓?lwIP 涔犳儻閮ㄧ讲涓€鑷达紝鍑忚交鍙屾牳涓嬪伓鍙?tcpip 閾捐〃鎹熷潖 */
    BaseType_t ok = xTaskCreatePinnedToCore(weather_task, "weather", 16384, NULL, 3, NULL, 0);
    if (ok != pdPASS) {
        vSemaphoreDelete(s_fetch_mutex);
        s_fetch_mutex = NULL;
        vEventGroupDelete(s_event);
        s_event = NULL;
        return ESP_ERR_NO_MEM;
    }

    weather_config_t init_cfg;
    weather_config_snapshot(&init_cfg);
    ESP_LOGI(TAG, "Weather init (enabled=%d, location=%s, refresh=%lum)",
             init_cfg.enabled, init_cfg.location, (unsigned long)init_cfg.refresh_min);
    return ESP_OK;
}

esp_err_t weather_get_config(weather_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    weather_config_snapshot(out);
    return ESP_OK;
}

esp_err_t weather_set_config(const weather_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    weather_config_t snap = *cfg;
    portENTER_CRITICAL(&s_cfg_mux);
    s_cfg = snap;
    portEXIT_CRITICAL(&s_cfg_mux);
    nvs_save_snapshot(&snap);
    ESP_LOGI(TAG, "Config saved: enabled=%d, loc=%s, refresh=%lum",
             snap.enabled, snap.location, (unsigned long)snap.refresh_min);
    if (s_event)
        xEventGroupSetBits(s_event, BIT_CFG_CHANGED);
    return ESP_OK;
}

esp_err_t weather_display_cached(void)
{
    if (!epd_is_ready())
        return ESP_ERR_INVALID_STATE;

    /* 涓庡悗鍙版媺鍙栦换鍔′覆琛岋細fetch 浠诲姟鎸侀攣鏈熼棿浼氬啓 s_data锛?     * 杩欓噷鎸侀攣鍚庡啀璋冪敤 render_weather()锛岄伩鍏嶈鍒版挄瑁傛暟鎹€?*/
    if (s_fetch_mutex)
        xSemaphoreTake(s_fetch_mutex, portMAX_DELAY);

    if (!s_data.valid) {
        if (s_fetch_mutex)
            xSemaphoreGive(s_fetch_mutex);
        ESP_LOGW(TAG, "no cached weather data");
        return ESP_ERR_INVALID_STATE;
    }

    render_weather();

    if (s_fetch_mutex)
        xSemaphoreGive(s_fetch_mutex);

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
