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
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "cJSON.h"

#include "miniz.h"

#include "fb_render.h"
#include "epd.h"
#include "scheduler.h"
#include "time_sync.h"
#include "display_policy.h"
#include "display_mode.h"
#include "clock_display.h"
#include "power_mgr.h"
#include "ui_theme.h"

static const char *TAG    = "weather";
static const char *NVS_NS = "weather";

#define BIT_CFG_CHANGED     BIT0
#define BIT_EMBED_REQUEST   BIT1
#define BIT_FULLPAGE_REQUEST BIT2
#define BIT_FULLPAGE_DONE    BIT3
#define BIT_FULLPAGE_FAILED  BIT4
#define MAX_RESP_LEN     12288
#define WEATHER_ACTIVE_REFRESH_MIN 30
#define WEATHER_EMBED_CACHE_TTL_US (300LL * 1000000LL)
#define WEATHER_TEMP_HISTORY_VERSION 1
#define WEATHER_TEMP_HISTORY_POINTS 24
#define WEATHER_TEMP_HISTORY_MIN_POINTS 2
#define NVS_KEY_TEMP_HISTORY "hist24"

static weather_config_t s_cfg = {
    .enabled     = false,
    .api_key     = "",
    .api_host    = "",
    .location    = "",
    .city_name   = "",
    .refresh_min = WEATHER_ACTIVE_REFRESH_MIN,
};

static weather_data_t     s_data;
static EventGroupHandle_t s_event;
static SemaphoreHandle_t  s_fetch_mutex;
static portMUX_TYPE       s_cfg_mux = portMUX_INITIALIZER_UNLOCKED;
static bool               s_skip_initial_task_fetch;
static int64_t            s_data_fetched_us;
static char               s_data_source_host[sizeof(((weather_config_t *)0)->api_host)];
static char               s_data_source_location[sizeof(((weather_config_t *)0)->location)];
static unsigned           s_fullpage_request_epoch;
static bool               s_fullpage_inflight;
static portMUX_TYPE       s_fullpage_mux = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    int64_t hour_epoch;
    int16_t temp;
    uint8_t valid;
    uint8_t reserved;
} weather_temp_history_point_t;

typedef struct {
    uint32_t version;
    uint32_t reserved;
    char api_host[sizeof(((weather_config_t *)0)->api_host)];
    char location[sizeof(((weather_config_t *)0)->location)];
    weather_temp_history_point_t points[WEATHER_TEMP_HISTORY_POINTS];
} weather_temp_history_store_t;

static weather_temp_history_store_t s_temp_history;

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

static bool weather_config_equal(const weather_config_t *a, const weather_config_t *b)
{
    return a->enabled == b->enabled &&
           a->refresh_min == b->refresh_min &&
           strcmp(a->api_key, b->api_key) == 0 &&
           strcmp(a->api_host, b->api_host) == 0 &&
           strcmp(a->location, b->location) == 0 &&
           strcmp(a->city_name, b->city_name) == 0;
}

static uint32_t weather_normalize_refresh_min(uint32_t refresh_min)
{
    return refresh_min == 0 ? 0 : WEATHER_ACTIVE_REFRESH_MIN;
}

static uint32_t weather_effective_refresh_min(const weather_config_t *cfg,
                                              bool *low_power_enabled)
{
    power_config_t pcfg = {0};
    if (power_mgr_get_config(&pcfg) == ESP_OK && pcfg.enabled) {
        if (low_power_enabled)
            *low_power_enabled = true;
        return pcfg.interval_min > 0 ? (uint32_t)pcfg.interval_min : 1;
    }

    if (low_power_enabled)
        *low_power_enabled = false;
    return weather_normalize_refresh_min(cfg->refresh_min);
}

static void weather_note_fetch_success_locked(const weather_config_t *cfg)
{
    s_data_fetched_us = esp_timer_get_time();
    snprintf(s_data_source_host, sizeof(s_data_source_host), "%s", cfg->api_host);
    snprintf(s_data_source_location, sizeof(s_data_source_location), "%s", cfg->location);
}

static bool weather_cached_fresh_locked(const weather_config_t *cfg)
{
    if (!s_data.valid || s_data_fetched_us <= 0)
        return false;
    if (strcmp(s_data_source_host, cfg->api_host) != 0 ||
        strcmp(s_data_source_location, cfg->location) != 0) {
        return false;
    }
    return (esp_timer_get_time() - s_data_fetched_us) < WEATHER_EMBED_CACHE_TTL_US;
}

static bool weather_try_notify_from_fresh_cache(const weather_config_t *cfg)
{
    if (!s_fetch_mutex)
        return false;
    if (xSemaphoreTake(s_fetch_mutex, pdMS_TO_TICKS(50)) != pdTRUE)
        return false;
    bool fresh = weather_cached_fresh_locked(cfg);
    xSemaphoreGive(s_fetch_mutex);
    if (fresh) {
        ESP_LOGI(TAG, "Embedded weather uses fresh cache, skip HTTPS");
        clock_display_notify_weather_data();
    }
    return fresh;
}

static bool weather_history_source_matches_locked(const weather_config_t *cfg)
{
    return s_temp_history.version == WEATHER_TEMP_HISTORY_VERSION &&
           strcmp(s_temp_history.api_host, cfg->api_host) == 0 &&
           strcmp(s_temp_history.location, cfg->location) == 0;
}

static void weather_history_reset_locked(const weather_config_t *cfg)
{
    memset(&s_temp_history, 0, sizeof(s_temp_history));
    s_temp_history.version = WEATHER_TEMP_HISTORY_VERSION;
    snprintf(s_temp_history.api_host, sizeof(s_temp_history.api_host), "%s",
             cfg->api_host);
    snprintf(s_temp_history.location, sizeof(s_temp_history.location), "%s",
             cfg->location);
}

static int weather_history_find_slot_locked(int64_t hour_epoch)
{
    for (int i = 0; i < WEATHER_TEMP_HISTORY_POINTS; i++) {
        const weather_temp_history_point_t *p = &s_temp_history.points[i];
        if (p->valid && p->hour_epoch == hour_epoch)
            return i;
    }
    return -1;
}

static int weather_history_replacement_slot_locked(void)
{
    int oldest = 0;
    bool have_oldest = false;
    for (int i = 0; i < WEATHER_TEMP_HISTORY_POINTS; i++) {
        const weather_temp_history_point_t *p = &s_temp_history.points[i];
        if (!p->valid)
            return i;
        if (!have_oldest || p->hour_epoch < s_temp_history.points[oldest].hour_epoch) {
            oldest = i;
            have_oldest = true;
        }
    }
    return oldest;
}

static int weather_history_count_recent_locked(int64_t now_hour)
{
    int count = 0;
    int64_t first_hour = now_hour - (WEATHER_TEMP_HISTORY_POINTS - 1);
    for (int i = 0; i < WEATHER_TEMP_HISTORY_POINTS; i++) {
        const weather_temp_history_point_t *p = &s_temp_history.points[i];
        if (p->valid && p->hour_epoch >= first_hour && p->hour_epoch <= now_hour)
            count++;
    }
    return count;
}

static bool weather_history_has_enough_locked(void)
{
    if (!time_sync_is_synced())
        return false;
    time_t now = time(NULL);
    if (now <= 0)
        return false;
    return weather_history_count_recent_locked((int64_t)(now / 3600)) >=
           WEATHER_TEMP_HISTORY_MIN_POINTS;
}

static esp_err_t weather_history_save_locked(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = nvs_set_blob(h, NVS_KEY_TEMP_HISTORY, &s_temp_history,
                       sizeof(s_temp_history));
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void weather_history_note_current_locked(const weather_config_t *cfg)
{
    if (!time_sync_is_synced()) {
        ESP_LOGI(TAG, "Temp history skipped: time not synchronized");
        return;
    }

    time_t now = time(NULL);
    if (now <= 0)
        return;
    int64_t hour_epoch = (int64_t)(now / 3600);

    if (!weather_history_source_matches_locked(cfg))
        weather_history_reset_locked(cfg);

    int slot = weather_history_find_slot_locked(hour_epoch);
    if (slot < 0)
        slot = weather_history_replacement_slot_locked();

    weather_temp_history_point_t *p = &s_temp_history.points[slot];
    bool changed = !p->valid || p->hour_epoch != hour_epoch ||
                   p->temp != (int16_t)s_data.now.temp;
    if (!changed)
        return;

    p->hour_epoch = hour_epoch;
    p->temp = (int16_t)s_data.now.temp;
    p->valid = 1;
    p->reserved = 0;

    esp_err_t err = weather_history_save_locked();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Temp history save failed: %s", esp_err_to_name(err));
        return;
    }

    time_t label_time = (time_t)(hour_epoch * 3600);
    struct tm t;
    localtime_r(&label_time, &t);
    ESP_LOGI(TAG, "Temp history saved: %02d:00 %dC (%d/%d)",
             t.tm_hour, s_data.now.temp,
             weather_history_count_recent_locked(hour_epoch),
             WEATHER_TEMP_HISTORY_POINTS);
}

static int weather_history_collect_recent_locked(int *values, char labels[][6],
                                                 int max_points)
{
    if (!values || !labels || max_points <= 0 || !time_sync_is_synced())
        return 0;

    time_t now = time(NULL);
    if (now <= 0)
        return 0;
    int64_t now_hour = (int64_t)(now / 3600);
    int64_t first_hour = now_hour - (WEATHER_TEMP_HISTORY_POINTS - 1);
    int count = 0;

    for (int64_t hour = first_hour; hour <= now_hour && count < max_points; hour++) {
        int slot = weather_history_find_slot_locked(hour);
        if (slot < 0)
            continue;

        values[count] = s_temp_history.points[slot].temp;
        time_t label_time = (time_t)(hour * 3600);
        struct tm t;
        localtime_r(&label_time, &t);
        snprintf(labels[count], 6, "%02d:00", t.tm_hour);
        count++;
    }
    return count;
}

static void log_tls_heap_state(const char *label)
{
    ESP_LOGI(TAG, "%s heap: free=%lu largest=%lu internal=%lu/%lu psram=%lu/%lu",
             label,
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static bool weather_embedded_target_active(void)
{
    return display_mode_active() == DISPLAY_MODE_CLOCK &&
           display_policy_clock_may_auto_refresh();
}

/* weather icons (24x24 bitmaps, 72 bytes each) */

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
    if (code == 150) return icon_sunny;
    if (code >= 100 && code <= 103) return icon_sunny;
    if (code >= 104 && code <= 199) return icon_cloudy;
    if (code >= 300 && code <= 399) return icon_rain;
    if (code >= 400 && code <= 499) return icon_snow;
    return icon_cloudy;
}

static const uint8_t *get_icon_by_text(int code, const char *text)
{
    if (text && text[0]) {
        if (strstr(text, "\xe9\x9b\xa8")) return icon_rain;
        if (strstr(text, "\xe9\x9b\xaa")) return icon_snow;
        if (strstr(text, "\xe6\x99\xb4")) return icon_sunny;
        if (strstr(text, "\xe4\xba\x91") || strstr(text, "\xe9\x98\xb4"))
            return icon_cloudy;
    }
    return get_icon(code);
}

const uint8_t *weather_icon_bitmap(int code)
{
    return get_icon(code);
}

/* NVS */

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
    if (nvs_get_u32(h, "refresh", &v32) == ESP_OK)
        s_cfg.refresh_min = weather_normalize_refresh_min(v32);

    weather_temp_history_store_t hist;
    size_t hist_len = sizeof(hist);
    if (nvs_get_blob(h, NVS_KEY_TEMP_HISTORY, &hist, &hist_len) == ESP_OK &&
        hist_len == sizeof(hist) &&
        hist.version == WEATHER_TEMP_HISTORY_VERSION) {
        s_temp_history = hist;
    }

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

/* HTTP fetch helper */

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

static void redact_weather_url(const char *url, char *out, size_t out_len)
{
    if (!url || !out || out_len == 0) return;
    snprintf(out, out_len, "%s", url);
    char *key = strstr(out, "key=");
    if (!key) return;
    key += 4;
    char *end = strchr(key, '&');
    char suffix[96] = {0};
    if (end) snprintf(suffix, sizeof(suffix), "%s", end);
    snprintf(key, (size_t)(out + out_len - key), "<redacted>%s", suffix);
}

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
        log_tls_heap_state("before weather HTTPS");

        esp_http_client_config_t config = {
            .url               = url,
            .event_handler     = http_event_handler,
            .user_data         = &rb,
            .timeout_ms        = WEATHER_HTTP_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            char safe_url[256];
            redact_weather_url(url, safe_url, sizeof(safe_url));
            ESP_LOGE(TAG, "esp_http_client_init failed for %s", safe_url);
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
        char safe_url[256];
        redact_weather_url(url, safe_url, sizeof(safe_url));
        ESP_LOGW(TAG, "HTTP GET %s failed after %d tries: err=%s status=%d", safe_url,
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

/* parse QWeather JSON */

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

/* render weather to EPD */

static bool parse_hourly(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;

    const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(root, "code"));
    if (!code || strcmp(code, "200") != 0) {
        cJSON_Delete(root);
        return false;
    }

    cJSON *hourly = cJSON_GetObjectItem(root, "hourly");
    if (!hourly) {
        cJSON_Delete(root);
        return false;
    }

    int count = cJSON_GetArraySize(hourly);
    if (count > 24) count = 24;
    s_data.hourly_count = 0;

    for (int i = 0; i < count; i++) {
        cJSON *h = cJSON_GetArrayItem(hourly, i);
        const char *v;

        if ((v = cJSON_GetStringValue(cJSON_GetObjectItem(h, "fxTime")))) {
            if (strlen(v) >= 16) {
                snprintf(s_data.hourly[i].time, sizeof(s_data.hourly[i].time),
                         "%.5s", v + 11);
            } else {
                snprintf(s_data.hourly[i].time, sizeof(s_data.hourly[i].time), "--:--");
            }
        } else {
            snprintf(s_data.hourly[i].time, sizeof(s_data.hourly[i].time), "--:--");
        }

        if ((v = cJSON_GetStringValue(cJSON_GetObjectItem(h, "temp"))))
            s_data.hourly[i].temp = atoi(v);
        else
            s_data.hourly[i].temp = s_data.now.temp;

        if ((v = cJSON_GetStringValue(cJSON_GetObjectItem(h, "icon"))))
            s_data.hourly[i].icon = atoi(v);
        else
            s_data.hourly[i].icon = s_data.now.icon;

        s_data.hourly_count++;
    }

    cJSON_Delete(root);
    return s_data.hourly_count > 0;
}

static const char *weekday_zh[] = {
    "\xe6\x97\xa5",  /* 鏃?*/
    "\xe4\xb8\x80",  /* 涓€ */
    "\xe4\xba\x8c",  /* 浜?*/
    "\xe4\xb8\x89",  /* 涓?*/
    "\xe5\x9b\x9b",  /* 鍥?*/
    "\xe4\xba\x94",  /* 浜?*/
    "\xe5\x85\xad",  /* 鍏?*/
};

static int clamp_i(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void draw_line_1px(fb_t *fb, int x0, int y0, int x1, int y1, fb_color_t c)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (1) {
        fb_pixel(fb, x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = err * 2;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_line_thick(fb_t *fb, int x0, int y0, int x1, int y1, fb_color_t c, int sw)
{
    if (sw < 1) sw = 1;
    if (sw == 1) {
        draw_line_1px(fb, x0, y0, x1, y1, c);
        return;
    }
    int half = sw / 2;
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    for (int o = -half; o <= half; o++) {
        if (dx >= dy)
            draw_line_1px(fb, x0, y0 + o, x1, y1 + o, c);
        else
            draw_line_1px(fb, x0 + o, y0, x1 + o, y1, c);
    }
}

static void draw_weather_icon_scaled(fb_t *fb, int x, int y, int scale,
                                     const uint8_t *data, fb_color_t c)
{
    if (!fb || !data)
        return;
    if (scale < 1)
        scale = 1;
    const int stride = (WEATHER_ICON_W + 7) / 8;
    for (int j = 0; j < WEATHER_ICON_H; j++) {
        for (int i = 0; i < WEATHER_ICON_W; i++) {
            if (data[j * stride + i / 8] & (0x80 >> (i % 8))) {
                if (scale == 1) {
                    fb_pixel(fb, x + i, y + j, c);
                } else {
                    fb_fill_rect(fb, x + i * scale, y + j * scale,
                                 scale, scale, c);
                }
            }
        }
    }
}

static void draw_current_weather_card(fb_t *fb, int x, int y, int w, int h,
                                      const weather_config_t *cfg,
                                      const weather_data_t *wd)
{
    ui_draw_card(fb, x, y, w, h, true);
    ui_draw_section_label(fb, x + 8, y + 7,
                          "\xe5\xa4\xa9\xe6\xb0\x94\xe7\x8a\xb6\xe5\x86\xb5",
                          COLOR_BLACK, 1);

    int icon_x = x + 42;
    int icon_y = y + 54;
    draw_weather_icon_scaled(fb, icon_x, icon_y, 3,
                             get_icon_by_text(wd->now.icon, wd->now.text),
                             COLOR_BLACK);

    char temp_num[12];
    snprintf(temp_num, sizeof(temp_num), "%d", wd->now.temp);
    int temp_scale = 5;
    int temp_x = x + 176;
    fb_utf8_scaled_maxw(fb, temp_x, y + 28, temp_num,
                        COLOR_BLACK, temp_scale, w - 146);
    int temp_w = ui_text_width(temp_num, temp_scale);
    fb_utf8_scaled(fb, temp_x + temp_w + 5, y + 36, "\xc2\xb0", COLOR_BLACK, 2);
    fb_utf8_scaled(fb, temp_x + temp_w + 31, y + 44, "C", COLOR_BLACK, 2);

    fb_fill_rect(fb, temp_x, y + 108, 30, 3, COLOR_RED);
    fb_utf8_scaled_maxw(fb, temp_x, y + 118, wd->now.text,
                        COLOR_RED, 2, w - 146);

    int today_low = wd->now.temp;
    int today_high = wd->now.temp;
    if (wd->daily_count > 0) {
        today_low = wd->daily[0].temp_min;
        today_high = wd->daily[0].temp_max;
    }
    char range[48];
    snprintf(range, sizeof(range),
             "\xe4\xbb\x8a\xe6\x97\xa5 %d/%d\xc2\xb0",
             today_high, today_low);
    int range_w = temp_x - x - 42;
    if (range_w < 96 || range_w > w - 44)
        range_w = w - 44;
    fb_utf8_scaled_maxw(fb, x + 28, y + h - 28, range,
                        COLOR_BLACK, 1, range_w);

    const char *city = (cfg && cfg->city_name[0]) ? cfg->city_name : "";
    (void)city; /* City/date already lives in the page header on the large layout. */

}

static void draw_stat_box(fb_t *fb, int x, int y, int w, int h,
                          const char *label, const char *value,
                          fb_color_t accent, int value_scale)
{
    fb_rect(fb, x, y, w, h, COLOR_BLACK);
    fb_fill_rect(fb, x, y, 2, h, accent);
    if (accent == COLOR_RED)
        fb_fill_rect(fb, x, y, w / 4, 2, COLOR_RED);
    fb_utf8_scaled_maxw(fb, x + 9, y + 4, label, accent, 1, w - 18);
    int value_y = (value_scale >= 2) ? (y + h - 16 * value_scale - 1)
                                     : (y + h - 22);
    fb_utf8_scaled_maxw(fb, x + 8, value_y, value,
                        COLOR_BLACK, value_scale, w - 16);
}

static void draw_metric_deck(fb_t *fb, int x, int y, int w, int h,
                             const weather_now_t *now)
{
    ui_draw_card(fb, x, y, w, h, false);
    ui_draw_section_label(fb, x + 8, y + 7,
                          "\xe6\xb0\x94\xe8\xb1\xa1\xe6\x8c\x87\xe6\xa0\x87",
                          COLOR_BLACK, 1);

    char feels_value[24];
    char humidity_value[24];
    char pressure_value[24];
    char wind_value[40];
    snprintf(feels_value, sizeof(feels_value), "%d\xc2\xb0""C", now->feels_like);
    snprintf(humidity_value, sizeof(humidity_value), "%d%%", now->humidity);
    snprintf(pressure_value, sizeof(pressure_value), "%dhPa", now->pressure);
    snprintf(wind_value, sizeof(wind_value), "%s%d\xe7\xba\xa7",
             now->wind_dir[0] ? now->wind_dir : "", now->wind_scale);

    int gx = x + 12;
    int gy = y + 38;
    int tile_gap = 8;
    int tile_w = (w - 24 - tile_gap) / 2;
    int tile_h = (h - 50 - tile_gap) / 2;
    if (tile_h < 54)
        tile_h = 54;

    draw_stat_box(fb, gx, gy, tile_w, tile_h,
                  "\xe4\xbd\x93\xe6\x84\x9f", feels_value, COLOR_RED, 2);
    draw_stat_box(fb, gx + tile_w + tile_gap, gy, tile_w, tile_h,
                  "\xe6\xb9\xbf\xe5\xba\xa6", humidity_value, COLOR_BLACK, 2);
    draw_stat_box(fb, gx, gy + tile_h + tile_gap, tile_w, tile_h,
                  "\xe6\xb0\x94\xe5\x8e\x8b", pressure_value, COLOR_BLACK, 1);
    draw_stat_box(fb, gx + tile_w + tile_gap, gy + tile_h + tile_gap,
                  tile_w, tile_h, "\xe9\xa3\x8e\xe5\x8a\x9b",
                  wind_value, COLOR_RED, 1);
}

static void draw_forecast_orbit(fb_t *fb, int x, int y, int w, int h,
                                const weather_data_t *wd)
{
    ui_draw_card(fb, x, y, w, h, false);
    ui_draw_section_label(fb, x + 8, y + 7,
                          "\xe4\xb8\x89\xe6\x97\xa5\xe9\xa2\x84\xe6\x8a\xa5",
                          COLOR_BLACK, 1);

    int n = wd->daily_count;
    if (n > 3) n = 3;
    if (n <= 0)
        return;

    const int pad = 12;
    const int gap = 8;
    int card_x = x + pad;
    int card_y = y + 32;
    int card_h = h - 42;
    if (card_h < 48)
        card_h = 48;
    int card_w = (w - pad * 2 - gap * 2) / 3;
    for (int i = 0; i < n; i++) {
        int bx = card_x + i * (card_w + gap);
        int bw = (i == 2) ? (x + w - pad - bx) : card_w;
        const weather_daily_t *day = &wd->daily[i];
        char md[16];
        if (strlen(day->date) >= 10)
            snprintf(md, sizeof(md), "%s", day->date + 5);
        else
            snprintf(md, sizeof(md), "%s", day->date);

        fb_color_t accent = (i == 0) ? COLOR_RED : COLOR_BLACK;
        fb_rect(fb, bx, card_y, bw, card_h, COLOR_BLACK);
        if (i == 0)
            fb_fill_rect(fb, bx, card_y, bw / 4, 2, COLOR_RED);
        fb_utf8_scaled_maxw(fb, bx + 8, card_y + 7, md, accent, 1, bw - 48);
        fb_utf8_scaled_maxw(fb, bx + 66, card_y + 7, day->text_day,
                            accent, 1, bw - 108);

        int icon_x = bx + bw - 36;
        draw_weather_icon_scaled(fb, icon_x, card_y + 20, 1,
                                 get_icon_by_text(day->icon_day, day->text_day),
                                 COLOR_BLACK);

        char temp[32];
        snprintf(temp, sizeof(temp), "\xe9\xab\x98%d / \xe4\xbd\x8e%d\xc2\xb0",
                 day->temp_max, day->temp_min);
        fb_utf8_scaled_maxw(fb, bx + 8, card_y + card_h - 18, temp,
                            COLOR_BLACK, 1, bw - 52);
    }
}

static void draw_temp_marker(fb_t *fb, int px, int py, const char *tag,
                             int temp, fb_color_t color,
                             int plot_x, int plot_y, int plot_w, int plot_h)
{
    char label[24];
    snprintf(label, sizeof(label), "%s%d\xc2\xb0", tag, temp);
    int lw = ui_text_width(label, 1);
    int tx = clamp_i(px - lw / 2, plot_x + 1, plot_x + plot_w - lw - 1);
    int ty = (py < plot_y + 18) ? (py + 8) : (py - 18);
    ty = clamp_i(ty, plot_y + 2, plot_y + plot_h - 14);

    fb_fill_rect(fb, px - 3, py - 3, 7, 7, color);
    fb_utf8_scaled(fb, tx, ty, label, color, 1);
}

static void draw_temp_corridor(fb_t *fb, int x, int y, int w, int h,
                               const weather_data_t *wd)
{
    ui_draw_card(fb, x, y, w, h, false);
    ui_draw_section_label(fb, x + 8, y + 7,
                          "24\xe5\xb0\x8f\xe6\x97\xb6\xe6\xb8\xa9\xe5\xba\xa6\xe5\x8f\x98\xe5\x8c\x96",
                          COLOR_BLACK, 1);

    int today_low = wd->now.temp;
    int today_high = wd->now.temp;
    const char *today_text = wd->now.text;
    if (wd->daily_count > 0) {
        const weather_daily_t *today = &wd->daily[0];
        today_low = today->temp_min;
        today_high = today->temp_max;
        if (today->text_day[0])
            today_text = today->text_day;
    }

    int values[25];
    const char *xlabels[25];
    char history_labels[WEATHER_TEMP_HISTORY_POINTS][6];
    int count = 0;
    int history_count = weather_history_collect_recent_locked(
        values, history_labels, WEATHER_TEMP_HISTORY_POINTS);
    bool has_history = history_count >= WEATHER_TEMP_HISTORY_MIN_POINTS;
    bool partial_history = has_history && history_count < WEATHER_TEMP_HISTORY_POINTS;
    bool has_hourly = wd->hourly_count >= 3;
    bool using_forecast = false;

    if (has_history) {
        count = history_count;
        for (int i = 0; i < count; i++)
            xlabels[i] = history_labels[i];
    } else if (has_hourly) {
        using_forecast = true;
        values[count] = wd->now.temp;
        xlabels[count++] = "\xe7\x8e\xb0\xe5\x9c\xa8"; /* now */
        int hn = wd->hourly_count;
        if (hn > 24) hn = 24;
        for (int i = 0; i < hn && count < 25; i++) {
            values[count] = wd->hourly[i].temp;
            xlabels[count] = wd->hourly[i].time;
            count++;
        }
    } else {
        values[count] = today_low;
        xlabels[count++] = "\xe4\xbd\x8e";             /* low */
        values[count] = today_high;
        xlabels[count++] = "\xe9\xab\x98";             /* high */
        values[count] = wd->now.temp;
        xlabels[count++] = "\xe7\x8e\xb0\xe5\x9c\xa8"; /* now */
    }

    if (count < 2)
        return;

    int axis_min = values[0];
    int axis_max = values[0];
    int min_idx = 0;
    int max_idx = 0;
    for (int i = 0; i < count; i++) {
        if (values[i] < axis_min) {
            axis_min = values[i];
            min_idx = i;
        }
        if (values[i] > axis_max) {
            axis_max = values[i];
            max_idx = i;
        }
    }
    int data_min = axis_min;
    int data_max = axis_max;
    if (axis_max - axis_min < 4) {
        int mid = (axis_min + axis_max) / 2;
        axis_min = mid - 2;
        axis_max = mid + 2;
    } else {
        axis_min -= 1;
        axis_max += 1;
    }

    int plot_x = x + 54;
    int plot_y = y + 40;
    int plot_w = w - 82;
    int plot_h = h - 70;
    if (plot_w < 140 || plot_h < 24)
        return;

    char summary[72];
    if (has_history) {
        int delta = values[count - 1] - values[0];
        const char *trend = "\xe5\xb9\xb3";
        if (delta > 0)
            trend = "\xe5\x8d\x87";
        else if (delta < 0)
            trend = "\xe9\x99\x8d";
        if (partial_history) {
            snprintf(summary, sizeof(summary),
                     "\xe5\xae\x9e\xe6\xb5\x8b%d/24  %s%d\xc2\xb0",
                     count, trend, abs(delta));
        } else {
            snprintf(summary, sizeof(summary),
                     "\xe5\xae\x9e\xe6\xb5\x8b""24h  %s%d\xc2\xb0",
                     trend, abs(delta));
        }
    } else if (using_forecast) {
        int delta = values[count - 1] - values[0];
        const char *trend = "\xe5\xb9\xb3";
        if (delta > 0)
            trend = "\xe5\x8d\x87";
        else if (delta < 0)
            trend = "\xe9\x99\x8d";
        snprintf(summary, sizeof(summary),
                 "\xe9\xa2\x84\xe6\x8a\xa5""24h  %s%d\xc2\xb0",
                 trend, abs(delta));
    } else {
        snprintf(summary, sizeof(summary),
                 "%s  \xe9\xab\x98%d/\xe4\xbd\x8e%d\xc2\xb0""C",
                 today_text, today_high, today_low);
    }
    fb_utf8_scaled_maxw(fb, x + w - 278, y + 12, summary,
                        COLOR_RED, 1, 262);

    char axis[16];
    snprintf(axis, sizeof(axis), "%d\xc2\xb0", data_max);
    fb_utf8_scaled(fb, x + 14, plot_y - 2, axis, COLOR_RED, 1);
    snprintf(axis, sizeof(axis), "%d\xc2\xb0", data_min);
    fb_utf8_scaled(fb, x + 14, plot_y + plot_h - 12, axis, COLOR_BLACK, 1);

    fb_vline(fb, plot_x, plot_y, plot_h, COLOR_BLACK);
    fb_hline(fb, plot_x, plot_y + plot_h, plot_w, COLOR_BLACK);
    ui_draw_dotted_hline(fb, plot_x + 3, plot_y + plot_h / 2,
                         plot_w - 6, COLOR_BLACK, 18);
    for (int t = 1; t < 3; t++) {
        int tx = plot_x + t * plot_w / 3;
        ui_draw_dotted_vline(fb, tx, plot_y + 6, plot_h - 10,
                             COLOR_BLACK, 22);
    }

    int px[25];
    int py[25];
    for (int i = 0; i < count; i++) {
        px[i] = plot_x + 4 + i * (plot_w - 8) / (count - 1);
        py[i] = plot_y + 5 + (axis_max - values[i]) * (plot_h - 10) /
                (axis_max - axis_min);
        py[i] = clamp_i(py[i], plot_y + 3, plot_y + plot_h - 4);
    }

    for (int i = 0; i < count - 1; i++)
        draw_line_thick(fb, px[i], py[i] + 3, px[i + 1], py[i + 1] + 3,
                        COLOR_BLACK, 1);
    for (int i = 0; i < count - 1; i++)
        draw_line_thick(fb, px[i], py[i], px[i + 1], py[i + 1], COLOR_RED, 2);
    for (int i = 0; i < count; i++) {
        fb_fill_rect(fb, px[i] - 2, py[i] - 2, 5, 5, COLOR_RED);
        fb_fill_rect(fb, px[i] - 1, py[i] - 1, 3, 3, COLOR_BLACK);
    }

    int mark_idx[2] = {min_idx, max_idx};
    const char *mark_tag[2] = {"\xe4\xbd\x8e", "\xe9\xab\x98"};
    fb_color_t mark_color[2] = {COLOR_BLACK, COLOR_RED};
    for (int m = 0; m < 2; m++) {
        int i = mark_idx[m];
        bool duplicate = false;
        for (int k = 0; k < m; k++) {
            if (mark_idx[k] == i) duplicate = true;
        }
        if (duplicate) continue;
        draw_temp_marker(fb, px[i], py[i], mark_tag[m], values[i],
                         mark_color[m], plot_x, plot_y, plot_w, plot_h);
    }

    int label_idx[4] = {0, count / 3, count * 2 / 3, count - 1};
    int last_label_x = -1000;
    for (int m = 0; m < 4; m++) {
        int i = label_idx[m];
        bool duplicate = false;
        for (int k = 0; k < m; k++) {
            if (label_idx[k] == i) duplicate = true;
        }
        if (duplicate) continue;
        int lw = ui_text_width(xlabels[i], 1);
        int tx = clamp_i(px[i] - lw / 2, plot_x + 1, plot_x + plot_w - lw - 1);
        if (tx - last_label_x > 44) {
            fb_utf8_scaled(fb, tx, plot_y + plot_h + 4,
                           xlabels[i], COLOR_BLACK, 1);
            last_label_x = tx;
        }
    }
}

static void render_weather(unsigned epoch)
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
    const int sc = ui_scale_for(fb);

    struct tm tm;
    bool time_ok = time_sync_get_local(&tm);
    char date_str[32];
    if (time_ok) {
        snprintf(date_str, sizeof(date_str), "%04d/%02d/%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    } else {
        snprintf(date_str, sizeof(date_str), "--/--/--");
    }

    char weekday_str[32];
    if (time_ok) {
        snprintf(weekday_str, sizeof(weekday_str), "\xe6\x98\x9f\xe6\x9c\x9f%s",
                 weekday_zh[tm.tm_wday]);
    } else {
        weekday_str[0] = '\0';
    }

    char right[80];
    if (cfg_local.city_name[0]) {
        snprintf(right, sizeof(right), "%s  %s", cfg_local.city_name, date_str);
    } else if (weekday_str[0]) {
        snprintf(right, sizeof(right), "%s  %s", weekday_str, date_str);
    } else {
        snprintf(right, sizeof(right), "%s", date_str);
    }

    ui_draw_page_frame(fb, UI_FRAME_RED_ACCENT | UI_FRAME_THIN);
    ui_draw_header(fb, "\xe5\xa4\xa9\xe6\xb0\x94", right, true);

    if (big && W >= 760 && H >= 430) {
        const int content_x = MX;
        const int content_w = W - 2 * MX;
        const int gap = 8;
        const int body_top = 28 * sc;
        const int body_bottom = H - 27 * sc;
        const int top_h = 158;

        const int current_w = content_w * 60 / 100;
        const int metric_w = content_w - current_w - gap;
        const int current_x = content_x;
        const int metric_x = current_x + current_w + gap;

        draw_current_weather_card(fb, current_x, body_top, current_w, top_h,
                                  &cfg_local, &s_data);
        draw_metric_deck(fb, metric_x, body_top, metric_w, top_h,
                         &s_data.now);

        int forecast_y = body_top + top_h + 8;
        int forecast_h = 92;
        draw_forecast_orbit(fb, content_x, forecast_y, content_w, forecast_h,
                            &s_data);

        int trend_y = forecast_y + forecast_h + 8;
        int trend_h = body_bottom - trend_y;
        if (trend_h > 0)
            draw_temp_corridor(fb, content_x, trend_y, content_w, trend_h,
                               &s_data);
    } else {
        int body_top = 44 * sc;
        int left_x = MX + 10 * sc;
        int temp_y = body_top + 4 * sc;

        const uint8_t *icon = get_icon(s_data.now.icon);
        fb_bitmap(fb, left_x, temp_y + 13 * sc, 24, 24, icon, COLOR_BLACK);

        char temp_str[16];
        snprintf(temp_str, sizeof(temp_str), "%d\xc2\xb0""C", s_data.now.temp);
        int temp_scale = big ? 5 : 3;
        fb_utf8_scaled(fb, left_x + 34 * sc, temp_y, temp_str,
                       COLOR_BLACK, temp_scale);

        int meta_x = W * 54 / 100;
        if (meta_x < left_x + 210 * sc)
            meta_x = left_x + 210 * sc;
        if (meta_x > W - MX - 120)
            meta_x = W / 2 + 6 * sc;
        int meta_w = W - meta_x - MX;
        if (meta_w < 48)
            meta_w = 48;

        int meta_y = temp_y + 6 * sc;
        const int meta_sc = big ? 2 : 1;
        int meta_gap = 16 * meta_sc + 6;
        fb_utf8_scaled_maxw(fb, meta_x, meta_y, s_data.now.text, COLOR_RED, meta_sc, meta_w);

        char fl_str[48];
        snprintf(fl_str, sizeof(fl_str), "\xe4\xbd\x93\xe6\x84\x9f %d\xc2\xb0""C",
                 s_data.now.feels_like);
        fb_utf8_scaled_maxw(fb, meta_x, meta_y + meta_gap, fl_str,
                            COLOR_BLACK, meta_sc, meta_w);

        char hum_str[48];
        snprintf(hum_str, sizeof(hum_str), "\xe6\xb9\xbf\xe5\xba\xa6 %d%%",
                 s_data.now.humidity);
        fb_utf8_scaled_maxw(fb, meta_x, meta_y + meta_gap * 2, hum_str,
                            COLOR_BLACK, meta_sc, meta_w);

        char wind_str[64];
        snprintf(wind_str, sizeof(wind_str), "%s %d\xe7\xba\xa7  %dhPa",
                 s_data.now.wind_dir, s_data.now.wind_scale, s_data.now.pressure);
        fb_utf8_scaled_maxw(fb, meta_x, meta_y + meta_gap * 3, wind_str,
                            COLOR_BLACK, meta_sc, meta_w);

        int sep_y = meta_y + meta_gap * 4 + 8 * sc;
        ui_draw_dotted_hline(fb, MX, sep_y, W - 2 * MX, COLOR_BLACK, 6);

        int forecast_y = sep_y + 12 * sc;
        ui_draw_section_label(fb, MX, forecast_y,
                              "\xe4\xb8\x89\xe6\x97\xa5\xe9\xa2\x84\xe6\x8a\xa5",
                              COLOR_BLACK, 1);
        forecast_y += 22;

        int col_w = (W - 2 * MX) / 3;
        int forecast_bottom = H - 34 * sc;
        if (forecast_bottom < forecast_y + 40)
            forecast_bottom = forecast_y + 40;

        for (int i = 0; i < s_data.daily_count && i < 3; i++) {
            int bx = MX + i * col_w;
            int col_inner = col_w - 12;
            if (col_inner < 24)
                col_inner = 24;

            if (i > 0)
                ui_draw_dotted_vline(fb, bx, forecast_y - 4 * sc,
                                     forecast_bottom - forecast_y, COLOR_BLACK, 6);

            const char *d = s_data.daily[i].date;
            char md[16];
            if (strlen(d) >= 10)
                snprintf(md, sizeof(md), "%s", d + 5);
            else
                snprintf(md, sizeof(md), "%s", d);

            fb_color_t accent = (i == 0) ? COLOR_RED : COLOR_BLACK;
            fb_utf8_scaled_maxw(fb, bx + 6, forecast_y, md,
                                accent, 1, col_inner);

            const uint8_t *di = get_icon(s_data.daily[i].icon_day);
            int icon_x = bx + col_w - 30;
            if (icon_x < bx + 6)
                icon_x = bx + 6;
            fb_bitmap(fb, icon_x, forecast_y, 24, 24, di, COLOR_BLACK);

            char t_str[32];
            snprintf(t_str, sizeof(t_str), "%d/%d\xc2\xb0""C",
                     s_data.daily[i].temp_max, s_data.daily[i].temp_min);
            fb_utf8_scaled_maxw(fb, bx + 6, forecast_y + 28,
                                t_str, COLOR_BLACK, 1, col_inner);

            fb_utf8_scaled_maxw(fb, bx + 6, forecast_y + 52,
                                s_data.daily[i].text_day, COLOR_BLACK, 1, col_inner);
        }
    }

    ui_draw_footer(fb, "\xe6\xb0\x94\xe8\xb1\xa1", s_data.update_time);

    if (!display_policy_epoch_is_current(epoch)) {
        fb_destroy(fb);
        ESP_LOGI(TAG, "Weather display skipped: stale request");
        return;
    }

    /* export and display */
    const char *raw_path = "/spiffs/image.bin";
    fb_raw_file_lock();
    if (!display_policy_epoch_is_current(epoch)) {
        fb_destroy(fb);
        fb_raw_file_unlock();
        ESP_LOGI(TAG, "Weather display skipped before export: stale request");
        return;
    }
    esp_err_t err = fb_export(fb, raw_path);
    fb_destroy(fb);

    if (err == ESP_OK && epd_is_ready() && display_policy_epoch_is_current(epoch)) {
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

/* fetch + display */

static esp_err_t weather_fetch_and_display_impl(bool force_fullscreen,
                                                bool begin_new_manual_epoch,
                                                unsigned requested_epoch)
{
    weather_config_t cfg;
    weather_config_snapshot(&cfg);
    if (!weather_config_has_api(&cfg)) {
        ESP_LOGW(TAG, "Weather API key, host or location not configured");
        return ESP_ERR_INVALID_STATE;
    }

    /* Avoid background weather HTTPS while another display owner is active. */
    if (!display_policy_weather_may_network_fetch(force_fullscreen)) {
        ESP_LOGI(TAG, "Weather auto fetch skipped by display policy");
        return ESP_OK;
    }

    unsigned epoch = requested_epoch ? requested_epoch : display_policy_display_epoch();
    if (force_fullscreen && begin_new_manual_epoch)
        epoch = display_policy_begin_manual_display();

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
    bool full_page = display_policy_weather_use_full_page(force_fullscreen);
    s_data.hourly_count = 0;

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

    weather_history_note_current_locked(&cfg);
    bool need_hourly = full_page && !weather_history_has_enough_locked();
    if (full_page && !need_hourly)
        ESP_LOGI(TAG, "Hourly forecast skipped; using saved temp history");

    if (need_hourly) {
        vTaskDelay(pdMS_TO_TICKS(100));
        snprintf(url, sizeof(url),
                 "https://%s/v7/weather/24h?location=%s&key=%s",
                 cfg.api_host, cfg.location, cfg.api_key);
        resp = http_get(url);
        if (resp) {
            if (parse_hourly(resp)) {
                ESP_LOGI(TAG, "Hourly forecast: %d points", s_data.hourly_count);
            } else {
                ESP_LOGW(TAG, "parse_hourly failed, trend falls back to daily range");
                s_data.hourly_count = 0;
            }
            free(resp);
        } else {
            ESP_LOGW(TAG, "Hourly forecast fetch failed, trend falls back to daily range");
        }
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
    weather_note_fetch_success_locked(&cfg);

    if (force_fullscreen ? display_policy_weather_use_full_page(true)
                         : display_policy_weather_may_render_full_page()) {
        if (display_policy_epoch_is_current(epoch))
            render_weather(epoch);
        if (force_fullscreen && display_policy_epoch_is_current(epoch))
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

esp_err_t weather_fetch_and_display(bool force_fullscreen)
{
    return weather_fetch_and_display_impl(force_fullscreen, force_fullscreen, 0);
}

esp_err_t weather_request_fullscreen_fetch(void)
{
    if (!epd_is_ready())
        return ESP_ERR_INVALID_STATE;
    if (!s_event)
        return ESP_ERR_INVALID_STATE;

    weather_config_t cfg;
    weather_config_snapshot(&cfg);
    if (!cfg.enabled || !weather_config_has_api(&cfg)) {
        ESP_LOGW(TAG, "Weather fullpage request ignored: API not configured or disabled");
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_fullpage_mux);
    if (s_fullpage_inflight) {
        portEXIT_CRITICAL(&s_fullpage_mux);
        ESP_LOGW(TAG, "Weather fullpage request ignored: request already in flight");
        return ESP_ERR_INVALID_STATE;
    }
    s_fullpage_inflight = true;
    s_fullpage_request_epoch = display_policy_display_epoch();
    portEXIT_CRITICAL(&s_fullpage_mux);

    xEventGroupClearBits(s_event, BIT_FULLPAGE_DONE | BIT_FULLPAGE_FAILED);
    xEventGroupSetBits(s_event, BIT_FULLPAGE_REQUEST);
    return ESP_OK;
}

esp_err_t weather_request_fullscreen_fetch_wait(uint32_t timeout_ms)
{
    esp_err_t err = weather_request_fullscreen_fetch();
    if (err != ESP_OK)
        return err;

    EventBits_t bits = xEventGroupWaitBits(
        s_event, BIT_FULLPAGE_DONE | BIT_FULLPAGE_FAILED,
        pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & BIT_FULLPAGE_DONE)
        return ESP_OK;
    if (bits & BIT_FULLPAGE_FAILED)
        return ESP_FAIL;
    portENTER_CRITICAL(&s_fullpage_mux);
    s_fullpage_inflight = false;
    portEXIT_CRITICAL(&s_fullpage_mux);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t weather_handle_fullpage_request(void)
{
    weather_config_t cfg;
    weather_config_snapshot(&cfg);
    if (!cfg.enabled || !weather_config_has_api(&cfg)) {
        ESP_LOGW(TAG, "Weather fullpage request skipped: API not configured or disabled");
        if (s_event)
            xEventGroupSetBits(s_event, BIT_FULLPAGE_FAILED);
        portENTER_CRITICAL(&s_fullpage_mux);
        s_fullpage_inflight = false;
        portEXIT_CRITICAL(&s_fullpage_mux);
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_fullpage_mux);
    unsigned epoch = s_fullpage_request_epoch;
    portEXIT_CRITICAL(&s_fullpage_mux);
    if (!epoch)
        epoch = display_policy_display_epoch();
    esp_err_t err = weather_fetch_and_display_impl(true, false, epoch);
    if (s_event)
        xEventGroupSetBits(s_event, err == ESP_OK ? BIT_FULLPAGE_DONE : BIT_FULLPAGE_FAILED);
    portENTER_CRITICAL(&s_fullpage_mux);
    s_fullpage_inflight = false;
    portEXIT_CRITICAL(&s_fullpage_mux);
    return err;
}

/* auto-refresh task */

void weather_request_embedded_refresh(void)
{
    if (!weather_embedded_target_active())
        return;
    if (!display_policy_weather_may_network_fetch(false))
        return;
    weather_config_t cfg;
    weather_config_snapshot(&cfg);
    if (!weather_config_has_api(&cfg))
        return;
    if (weather_try_notify_from_fresh_cache(&cfg))
        return;
    if (s_event)
        xEventGroupSetBits(s_event, BIT_EMBED_REQUEST);
}

void weather_notify_slideshow_stopped(void)
{
    if (!s_event)
        return;
    clock_config_t cc;
    if (clock_display_get_config(&cc) != ESP_OK || !cc.show_weather)
        return;
    if (!weather_embedded_target_active())
        return;
    weather_config_t cfg;
    weather_config_snapshot(&cfg);
    if (!cfg.enabled || !weather_config_has_api(&cfg))
        return;
    if (weather_try_notify_from_fresh_cache(&cfg))
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

    /* Let networking settle before the first fetch. */
    vTaskDelay(pdMS_TO_TICKS(2500));
    bool skip_first = s_skip_initial_task_fetch;
    s_skip_initial_task_fetch = false;
    weather_config_t cfg;
    weather_config_snapshot(&cfg);
    if (!skip_first && cfg.enabled && weather_config_has_api(&cfg))
        weather_fetch_and_display(false);

    for (;;) {
        weather_config_snapshot(&cfg);
        uint32_t effective_refresh_min = weather_effective_refresh_min(&cfg, NULL);
        /* Full-page weather off or manual-only normal mode: no periodic fetch,
         * but still respond to clock embedded weather and explicit full-page requests. */
        if (!cfg.enabled || effective_refresh_min == 0) {
            EventBits_t bits = xEventGroupWaitBits(
                s_event, BIT_CFG_CHANGED | BIT_EMBED_REQUEST | BIT_FULLPAGE_REQUEST,
                pdTRUE, pdFALSE, portMAX_DELAY);
            weather_config_snapshot(&cfg);
            if (bits & BIT_FULLPAGE_REQUEST) {
                weather_handle_fullpage_request();
                continue;
            }
            bool want = false;
            if ((bits & BIT_EMBED_REQUEST) && weather_config_has_api(&cfg))
                want = true;
            if ((bits & BIT_CFG_CHANGED) && cfg.enabled && weather_config_has_api(&cfg))
                want = true;
            if ((bits & BIT_EMBED_REQUEST) && !weather_embedded_target_active() &&
                !(bits & BIT_CFG_CHANGED))
                continue;
            if ((bits & BIT_EMBED_REQUEST) && !(bits & BIT_CFG_CHANGED) &&
                weather_try_notify_from_fresh_cache(&cfg))
                continue;
            if (want)
                weather_fetch_and_display(false);
            continue;
        }

        uint32_t wait_ms = effective_refresh_min * 60 * 1000;
        EventBits_t bits = xEventGroupWaitBits(
            s_event, BIT_CFG_CHANGED | BIT_EMBED_REQUEST | BIT_FULLPAGE_REQUEST,
            pdTRUE, pdFALSE, pdMS_TO_TICKS(wait_ms));
        weather_config_snapshot(&cfg);

        if (bits & BIT_FULLPAGE_REQUEST) {
            weather_handle_fullpage_request();
            continue;
        }

        /* Fetch immediately after web/NVS config changes. */
        if (bits & BIT_CFG_CHANGED) {
            if (cfg.enabled && weather_config_has_api(&cfg))
                weather_fetch_and_display(false);
            continue;
        }
        if (!cfg.enabled)
            continue;
        if (bits & BIT_EMBED_REQUEST) {
            if (weather_embedded_target_active() &&
                weather_config_has_api(&cfg) &&
                !weather_try_notify_from_fresh_cache(&cfg))
                weather_fetch_and_display(false);
            continue;
        }

        if (weather_config_has_api(&cfg))
            weather_fetch_and_display(false);
    }
}

/* public API */

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
    memset(&s_temp_history, 0, sizeof(s_temp_history));
    s_temp_history.version = WEATHER_TEMP_HISTORY_VERSION;
    s_data_fetched_us = 0;
    s_data_source_host[0] = '\0';
    s_data_source_location[0] = '\0';
    s_fullpage_request_epoch = 0;
    portENTER_CRITICAL(&s_fullpage_mux);
    s_fullpage_inflight = false;
    portEXIT_CRITICAL(&s_fullpage_mux);
    nvs_load();

    /* HTTPS+TLS is stack-heavy; pin to CPU0 to stay close to lwIP work. */
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
    bool low_power = false;
    uint32_t effective_refresh_min = weather_effective_refresh_min(&init_cfg, &low_power);
    ESP_LOGI(TAG, "Weather init (enabled=%d, location=%s, refresh=%lum, effective=%lum%s)",
             init_cfg.enabled, init_cfg.location, (unsigned long)init_cfg.refresh_min,
             (unsigned long)effective_refresh_min,
             low_power ? " low-power" : "");
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
    snap.refresh_min = weather_normalize_refresh_min(snap.refresh_min);
    bool changed;
    portENTER_CRITICAL(&s_cfg_mux);
    changed = !weather_config_equal(&s_cfg, &snap);
    s_cfg = snap;
    portEXIT_CRITICAL(&s_cfg_mux);
    if (changed) {
        nvs_save_snapshot(&snap);
        ESP_LOGI(TAG, "Config saved: enabled=%d, loc=%s, refresh=%lum (changed=1)",
                 snap.enabled, snap.location, (unsigned long)snap.refresh_min);
    } else {
        ESP_LOGI(TAG, "Config unchanged: enabled=%d, loc=%s, refresh=%lum",
                 snap.enabled, snap.location, (unsigned long)snap.refresh_min);
    }
    if (changed && s_event)
        xEventGroupSetBits(s_event, BIT_CFG_CHANGED);
    return ESP_OK;
}

esp_err_t weather_display_cached(void)
{
    if (!epd_is_ready())
        return ESP_ERR_INVALID_STATE;

    unsigned epoch = display_policy_display_epoch();
    weather_config_t cfg;
    weather_config_snapshot(&cfg);

    /* Serialize with background fetches so render_weather() sees a stable s_data. */
    if (s_fetch_mutex)
        xSemaphoreTake(s_fetch_mutex, portMAX_DELAY);

    if (!s_data.valid) {
        if (s_fetch_mutex)
            xSemaphoreGive(s_fetch_mutex);
        ESP_LOGI(TAG, "no cached weather data, requesting weather task fetch and waiting");
        return weather_request_fullscreen_fetch_wait(180000);
    }

    if (s_data.hourly_count < 3 && !weather_history_has_enough_locked() &&
        cfg.enabled && weather_config_has_api(&cfg)) {
        if (s_fetch_mutex)
            xSemaphoreGive(s_fetch_mutex);
        ESP_LOGI(TAG, "cached weather lacks hourly data, requesting fullpage fetch and waiting");
        return weather_request_fullscreen_fetch_wait(180000);
    }

    render_weather(epoch);

    if (s_fetch_mutex)
        xSemaphoreGive(s_fetch_mutex);

    if (display_policy_epoch_is_current(epoch))
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

void weather_get_summary_copy(weather_summary_t *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (s_fetch_mutex)
        xSemaphoreTake(s_fetch_mutex, portMAX_DELAY);
    out->now = s_data.now;
    memcpy(out->daily, s_data.daily, sizeof(out->daily));
    out->daily_count = s_data.daily_count;
    snprintf(out->update_time, sizeof(out->update_time), "%s", s_data.update_time);
    out->valid = s_data.valid;
    if (s_fetch_mutex)
        xSemaphoreGive(s_fetch_mutex);
}

int weather_data_signature(void)
{
    int sig = -1;
    if (s_fetch_mutex)
        xSemaphoreTake(s_fetch_mutex, portMAX_DELAY);
    if (s_data.valid) {
        sig = ((s_data.now.temp + 100) & 0x1FF);   /* 9 bit, -100..411 */
        sig |= (s_data.now.icon & 0xFFF) << 9;     /* 12 bit */
        sig |= (s_data.now.humidity & 0x7F) << 21; /* 7 bit, 0..127 */
        sig |= (s_data.daily_count & 0x7) << 28;   /* 3 bit */
    }
    if (s_fetch_mutex)
        xSemaphoreGive(s_fetch_mutex);
    return sig;
}
