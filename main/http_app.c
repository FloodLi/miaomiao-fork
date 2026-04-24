#include "http_app.h"
#include "http_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_random.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "nvs.h"
#include "mbedtls/base64.h"

#include "epd.h"
#include "image_convert.h"
#include "device_identity.h"
#include "wifi_manager.h"
#include "time_sync.h"
#include "scheduler.h"
#include "weather.h"
#include "clock_display.h"
#include "display_policy.h"
#include "message_board.h"
#include "canvas_board.h"
#include "calendar_display.h"
#include "timetable.h"
#include "todo.h"
#include "power_mgr.h"
#include "button.h"
#include "display_mode.h"
#include "countdown.h"
#include "battery_mon.h"
#include "cJSON.h"

static const char *TAG = "http_app";

httpd_handle_t    http_server_handle;
http_app_config_t http_app_cfg;
const char       *http_images_dir   = "/spiffs/images";
const int         http_upload_max_bytes = 2 * 1024 * 1024;

/* backward compat aliases used within this file */
#define s_server  http_server_handle
#define s_cfg     http_app_cfg
#define IMAGES_DIR http_images_dir
#define UPLOAD_MAX_BYTES http_upload_max_bytes

/* ── Basic Auth ───────────────────────────────────────────────────── */

#define AUTH_NVS_NS   "http_auth"
#define AUTH_NVS_USER "user"
#define AUTH_NVS_PASS "pass"

static char s_auth_user[32];
static char s_auth_pass[64];

void http_auth_load_from_nvs(void)
{
    s_auth_user[0] = '\0';
    s_auth_pass[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(AUTH_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t lu = sizeof(s_auth_user), lp = sizeof(s_auth_pass);
    nvs_get_str(h, AUTH_NVS_USER, s_auth_user, &lu);
    nvs_get_str(h, AUTH_NVS_PASS, s_auth_pass, &lp);
    nvs_close(h);
}

bool http_auth_is_enabled(void)
{
    return s_auth_user[0] != '\0' && s_auth_pass[0] != '\0';
}

bool http_check_basic_auth(httpd_req_t *req)
{
    if (!http_auth_is_enabled()) return true;

    char hdr[160] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"EPD\"");
        httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
        return false;
    }

    const char *prefix = "Basic ";
    if (strncmp(hdr, prefix, 6) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
        return false;
    }

    char expected[128];
    snprintf(expected, sizeof(expected), "%s:%s", s_auth_user, s_auth_pass);
    unsigned char encoded[192];
    size_t olen = 0;
    if (mbedtls_base64_encode(encoded, sizeof(encoded) - 1, &olen,
                              (const unsigned char *)expected, strlen(expected)) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "Auth internal error", HTTPD_RESP_USE_STRLEN);
        return false;
    }
    encoded[olen] = '\0';

    if (strcmp(hdr + 6, (const char *)encoded) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"EPD\"");
        httpd_resp_send(req, "Wrong credentials", HTTPD_RESP_USE_STRLEN);
        return false;
    }
    return true;
}

static esp_err_t auth_config_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON 内存不足");
        return ESP_OK;
    }
    cJSON_AddBoolToObject(root, "enabled", http_auth_is_enabled());
    cJSON_AddStringToObject(root, "user", s_auth_user[0] ? s_auth_user : "");
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON 内存不足");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, str, HTTPD_RESP_USE_STRLEN);
    free(str);
    return ESP_OK;
}

static esp_err_t auth_config_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;

    char buf[160] = {0};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "请求体为空");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON 格式错误");
        return ESP_OK;
    }

    cJSON *ju = cJSON_GetObjectItem(root, "user");
    cJSON *jp = cJSON_GetObjectItem(root, "pass");

    nvs_handle_t h;
    esp_err_t nerr = nvs_open(AUTH_NVS_NS, NVS_READWRITE, &h);
    if (nerr != ESP_OK) {
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"NVS 打开失败\"}");
        return ESP_OK;
    }

    esp_err_t op = ESP_OK;
    if (ju && cJSON_IsString(ju) && jp && cJSON_IsString(jp) &&
        strlen(ju->valuestring) > 0 && strlen(jp->valuestring) > 0) {
        char user_trunc[32], pass_trunc[64];
        strncpy(user_trunc, ju->valuestring, sizeof(user_trunc) - 1);
        user_trunc[sizeof(user_trunc) - 1] = '\0';
        strncpy(pass_trunc, jp->valuestring, sizeof(pass_trunc) - 1);
        pass_trunc[sizeof(pass_trunc) - 1] = '\0';
        op = nvs_set_str(h, AUTH_NVS_USER, user_trunc);
        if (op == ESP_OK)
            op = nvs_set_str(h, AUTH_NVS_PASS, pass_trunc);
    } else {
        (void)nvs_erase_key(h, AUTH_NVS_USER);
        (void)nvs_erase_key(h, AUTH_NVS_PASS);
    }
    if (op == ESP_OK)
        op = nvs_commit(h);
    nvs_close(h);
    cJSON_Delete(root);

    if (op != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"凭据未保存\"}");
        return ESP_OK;
    }

    http_auth_load_from_nvs();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* embedded HTML externs are in http_internal.h */

/* ── helpers ───────────────────────────────────────────────────────── */

int http_stat_size_or_neg(const char *path)
{
    struct stat st;
    if (!path) return -1;
    if (stat(path, &st) == 0) return (int)st.st_size;
    return -1;
}

/** EPD 未就绪时返回 JSON 503，避免 httpd 与 epd_init 并行时刷硬件 */
bool http_require_epd_ready(httpd_req_t *req)
{
    if (epd_is_ready())
        return true;
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"墨水屏正在初始化，请几秒后重试\"}");
    return false;
}

bool http_get_query_param(httpd_req_t *req, const char *key,
                          char *out, size_t out_len)
{
    int qlen = httpd_req_get_url_query_len(req);
    if (qlen <= 0 || !out || out_len == 0) return false;
    char *q = (char *)malloc((size_t)qlen + 1);
    if (!q) return false;
    if (httpd_req_get_url_query_str(req, q, (size_t)qlen + 1) != ESP_OK) {
        free(q);
        return false;
    }
    bool ok = (httpd_query_key_value(q, key, out, out_len) == ESP_OK);
    free(q);
    return ok;
}

/* ── GET / ─────────────────────────────────────────────────────────── */

static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_send(req, (const char *)index_html_start,
                    (ssize_t)(index_html_end - index_html_start));
    return ESP_OK;
}

/* ── GET /status ───────────────────────────────────────────────────── */

static esp_err_t status_get_handler(httpd_req_t *req)
{
    const int upload_sz = http_stat_size_or_neg(s_cfg.upload_path);
    const int raw_sz    = http_stat_size_or_neg("/spiffs/image.bin");

    size_t total = 0, used = 0;
    esp_spiffs_info(NULL, &total, &used);

    char time_str[32] = "";
    time_sync_get_str(time_str, sizeof(time_str));

    battery_status_t bat;
    battery_mon_get_status(&bat);

    char json[768];
    snprintf(json, sizeof(json),
             "{\"upload_path\":\"%s\",\"upload_bytes\":%d,"
             "\"raw_path\":\"/spiffs/image.bin\",\"raw_bytes\":%d,"
             "\"spiffs_total\":%u,\"spiffs_used\":%u,"
             "\"time\":\"%s\",\"time_synced\":%s,"
             "\"current_mode\":%d,"
             "\"free_heap\":%u,\"min_free_heap\":%u,"
             "\"battery_pct\":%u,\"battery_valid\":%s,\"charging\":%s,\"battery_adc\":%d}",
             s_cfg.upload_path, upload_sz, raw_sz,
             (unsigned)total, (unsigned)used,
             time_str, time_sync_is_synced() ? "true" : "false",
             button_get_current_mode(),
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)bat.percent,
             bat.valid ? "true" : "false",
             bat.charging ? "true" : "false",
             bat.adc_raw);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* gallery handlers moved to http_gallery.c */

/* ── GET /config ───────────────────────────────────────────────────── */

static esp_err_t config_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_send(req, (const char *)config_html_start,
                    (ssize_t)(config_html_end - config_html_start));
    return ESP_OK;
}

/* timetable/todo/countdown UI handlers moved to http_features.c */

/* ── GET /wifi_status ──────────────────────────────────────────────── */

static esp_err_t wifi_status_get_handler(httpd_req_t *req)
{
    char json[384];
    snprintf(json, sizeof(json),
             "{\"mode\":%d,\"sta_connected\":%s,\"sta_ssid\":\"%s\",\"sta_ip\":\"%s\","
             "\"ap_ssid\":\"%s\",\"mdns_host\":\"%s\",\"id6\":\"%s\"}",
             (int)wifi_manager_get_mode(),
             wifi_manager_sta_connected() ? "true" : "false",
             wifi_manager_get_sta_ssid(),
             wifi_manager_get_sta_ip(),
             wifi_manager_get_ap_ssid(),
             device_identity_get_mdns_hostname(),
             device_identity_get_id6_upper());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── GET /scan ─────────────────────────────────────────────────────── */

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    wifi_mgr_ap_record_t aps[20];
    int count = wifi_manager_scan(aps, 20);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON 内存不足");
        return ESP_OK;
    }
    cJSON *arr  = cJSON_AddArrayToObject(root, "aps");
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (!item) continue;
        cJSON_AddStringToObject(item, "ssid", aps[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", aps[i].rssi);
        cJSON_AddNumberToObject(item, "auth", aps[i].authmode);
        cJSON_AddItemToArray(arr, item);
    }
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON 内存不足");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, str, HTTPD_RESP_USE_STRLEN);
    free(str);
    return ESP_OK;
}

/* ── POST /wifi_connect ────────────────────────────────────────────── */

static esp_err_t wifi_connect_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    char buf[256] = {0};
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "请求体错误");
        return ESP_OK;
    }
    int total = 0;
    while (total < len) {
        int r = httpd_req_recv(req, buf + total, len - total);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "接收失败");
            return ESP_OK;
        }
        total += r;
    }
    buf[total] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON 格式错误");
        return ESP_OK;
    }
    const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "ssid"));
    const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(root, "password"));
    if (!ssid || ssid[0] == '\0') {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少 WiFi 名称");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "WiFi connect request: SSID=\"%s\"", ssid);
    esp_err_t err = wifi_manager_connect(ssid, pass ? pass : "");
    cJSON_Delete(root);

    cJSON *resp = cJSON_CreateObject();
    if (err == ESP_OK) {
        cJSON_AddBoolToObject(resp, "ok", true);
        cJSON_AddStringToObject(resp, "sta_ip", wifi_manager_get_sta_ip());
        cJSON_AddStringToObject(resp, "mdns_host", device_identity_get_mdns_hostname());
        cJSON_AddStringToObject(resp, "ap_ssid", wifi_manager_get_ap_ssid());
    } else {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "msg", "Connection failed");
    }
    char *str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!str) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON 内存不足");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, str, HTTPD_RESP_USE_STRLEN);
    free(str);
    return ESP_OK;
}

/* ── POST /wifi_forget ─────────────────────────────────────────────── */

static esp_err_t wifi_forget_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    char tmp[64];
    while (req->content_len > 0) {
        int r = httpd_req_recv(req, tmp, sizeof(tmp));
        if (r <= 0) break;
        req->content_len -= r;
    }

    esp_err_t err = wifi_manager_forget();
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":%s}", err == ESP_OK ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── GET /slideshow ────────────────────────────────────────────────── */

static esp_err_t slideshow_get_handler(httpd_req_t *req)
{
    slideshow_config_t cfg;
    scheduler_get_config(&cfg);
    const char *current = scheduler_get_current_image();
    char esc_img[80];
    json_escape(esc_img, sizeof(esc_img), current ? current : "");

    char json[320];
    snprintf(json, sizeof(json),
             "{\"enabled\":%s,\"interval_sec\":%lu,\"mode\":%d,\"clock_overlay\":%s,\"current_image\":\"%s\"}",
             cfg.enabled ? "true" : "false",
             (unsigned long)cfg.interval_sec,
             (int)cfg.mode,
             cfg.clock_overlay ? "true" : "false",
             esc_img);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── POST /slideshow ──────────────────────────────────────────────── */

static esp_err_t slideshow_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    char buf[128] = {0};
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "请求体错误");
        return ESP_OK;
    }
    int total = 0;
    while (total < len) {
        int r = httpd_req_recv(req, buf + total, len - total);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "接收失败");
            return ESP_OK;
        }
        total += r;
    }
    buf[total] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON 格式错误");
        return ESP_OK;
    }

    slideshow_config_t cfg;
    scheduler_get_config(&cfg);

    cJSON *j;
    if ((j = cJSON_GetObjectItem(root, "enabled")) && cJSON_IsBool(j))
        cfg.enabled = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItem(root, "interval_sec")) && cJSON_IsNumber(j))
        cfg.interval_sec = (uint32_t)j->valuedouble;
    if ((j = cJSON_GetObjectItem(root, "mode")) && cJSON_IsNumber(j))
        cfg.mode = (slideshow_mode_t)j->valueint;
    if ((j = cJSON_GetObjectItem(root, "clock_overlay")) && cJSON_IsBool(j))
        cfg.clock_overlay = cJSON_IsTrue(j);

    cJSON_Delete(root);

    esp_err_t err = scheduler_set_config(&cfg);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "配置无效");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── GET /version ──────────────────────────────────────────────────── */

static esp_err_t version_get_handler(httpd_req_t *req)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next    = esp_ota_get_next_update_partition(NULL);

    char json[384];
    snprintf(json, sizeof(json),
             "{\"version\":\"%s\",\"date\":\"%s\",\"time\":\"%s\","
             "\"idf\":\"%s\",\"running\":\"%s\",\"next_ota\":\"%s\"}",
             desc->version, desc->date, desc->time,
             desc->idf_ver,
             running ? running->label : "unknown",
             next    ? next->label    : "none");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── POST /ota ─────────────────────────────────────────────────────── */

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    if (!http_auth_is_enabled()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"OTA requires authentication. Set credentials in /auth_config first.\"}");
        return ESP_OK;
    }
    if (!http_check_basic_auth(req)) return ESP_OK;
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"No OTA partition\"}");
        return ESP_OK;
    }

    if (req->content_len <= 0 || req->content_len > 2 * 1024 * 1024) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"Invalid firmware size\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA start: %d bytes -> '%s' @ 0x%lx",
             req->content_len, update->label, (unsigned long)update->address);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"OTA begin failed\"}");
        return ESP_OK;
    }

    char *buf = malloc(4096);
    if (!buf) {
        esp_ota_abort(handle);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"malloc failed\"}");
        return ESP_OK;
    }

    int remaining = req->content_len;
    int received  = 0;
    bool failed   = false;

    while (remaining > 0) {
        int to_recv = remaining > 4096 ? 4096 : remaining;
        int r = httpd_req_recv(req, buf, to_recv);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            ESP_LOGE(TAG, "OTA recv error at %d/%d bytes", received, req->content_len);
            failed = true;
            break;
        }

        err = esp_ota_write(handle, buf, r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            failed = true;
            break;
        }

        received  += r;
        remaining -= r;
    }

    free(buf);

    if (failed) {
        esp_ota_abort(handle);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"Firmware upload interrupted\"}");
        return ESP_OK;
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "{\"ok\":false,\"msg\":\"Firmware validation failed: %s\"}",
                 esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"Set boot partition failed\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA success: %d bytes written to '%s', rebooting...",
             received, update->label);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"OTA success, rebooting...\"}");

    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* ── GET /weather_config ───────────────────────────────────────────── */

static esp_err_t weather_config_get_handler(httpd_req_t *req)
{
    weather_config_t cfg;
    weather_get_config(&cfg);
    const weather_data_t *d = weather_get_data();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", cfg.enabled);
    /* 不在 GET 中下发明文 Key，避免局域网嗅探；前端用 api_key_set + 留空保存保留原密钥 */
    cJSON_AddStringToObject(root, "api_key", "");
    cJSON_AddBoolToObject(root, "api_key_set", cfg.api_key[0] != '\0');
    cJSON_AddStringToObject(root, "api_host", cfg.api_host);
    cJSON_AddStringToObject(root, "location", cfg.location);
    cJSON_AddStringToObject(root, "city_name", cfg.city_name);
    cJSON_AddNumberToObject(root, "refresh_min", cfg.refresh_min);
    cJSON_AddBoolToObject(root, "has_data", d->valid);
    if (d->valid) {
        cJSON_AddNumberToObject(root, "temp", d->now.temp);
        cJSON_AddStringToObject(root, "text", d->now.text);
        cJSON_AddStringToObject(root, "update_time", d->update_time);
    }
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON 内存不足");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, str, HTTPD_RESP_USE_STRLEN);
    free(str);
    return ESP_OK;
}

/* ── POST /weather_config ─────────────────────────────────────────── */

static esp_err_t weather_config_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    char buf[512] = {0};
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "请求体错误");
        return ESP_OK;
    }
    int total = 0;
    while (total < len) {
        int r = httpd_req_recv(req, buf + total, len - total);
        if (r <= 0) { if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "接收失败"); return ESP_OK; }
        total += r;
    }
    buf[total] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON 格式错误"); return ESP_OK; }

    weather_config_t cfg;
    weather_get_config(&cfg);

    cJSON *j;
    if ((j = cJSON_GetObjectItem(root, "enabled")) && cJSON_IsBool(j))
        cfg.enabled = cJSON_IsTrue(j);
    const char *s;
    cJSON *jk = cJSON_GetObjectItem(root, "api_key");
    if (jk && cJSON_IsString(jk) && jk->valuestring && jk->valuestring[0] != '\0')
        snprintf(cfg.api_key, sizeof(cfg.api_key), "%s", jk->valuestring);
    j = cJSON_GetObjectItem(root, "api_key_clear");
    if (j && cJSON_IsTrue(j))
        cfg.api_key[0] = '\0';
    if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(root, "api_host"))))
        snprintf(cfg.api_host, sizeof(cfg.api_host), "%s", s);
    if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(root, "location"))))
        snprintf(cfg.location, sizeof(cfg.location), "%s", s);
    if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(root, "city_name"))))
        snprintf(cfg.city_name, sizeof(cfg.city_name), "%s", s);
    if ((j = cJSON_GetObjectItem(root, "refresh_min")) && cJSON_IsNumber(j))
        cfg.refresh_min = (uint32_t)j->valuedouble;
    cJSON_Delete(root);

    weather_set_config(&cfg);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── POST /weather_show ───────────────────────────────────────────── */

static esp_err_t weather_show_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    if (!http_require_epd_ready(req))
        return ESP_OK;

    char tmp[64];
    while (req->content_len > 0) {
        int r = httpd_req_recv(req, tmp, sizeof(tmp));
        if (r <= 0) {
            break;
        }
    }

    esp_err_t err = weather_fetch_and_display(true);
    if (err == ESP_OK) {
        button_set_current_mode(DISPLAY_MODE_WEATHER);
        power_mgr_save_mode(DISPLAY_MODE_WEATHER);
    }
    char json[96];
    snprintf(json, sizeof(json), "{\"ok\":%s}", err == ESP_OK ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── GET /clock_config ─────────────────────────────────────────────── */

static esp_err_t clock_config_get_handler(httpd_req_t *req)
{
    clock_config_t cfg;
    clock_display_get_config(&cfg);

    char json[128];
    snprintf(json, sizeof(json),
             "{\"enabled\":%s,\"style\":%d,\"show_weather\":%s}",
             cfg.enabled ? "true" : "false",
             (int)cfg.style,
             cfg.show_weather ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── POST /clock_config ───────────────────────────────────────────── */

static esp_err_t clock_config_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    char buf[128] = {0};
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "请求体错误");
        return ESP_OK;
    }
    int total = 0;
    while (total < len) {
        int r = httpd_req_recv(req, buf + total, len - total);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "接收失败");
            return ESP_OK;
        }
        total += r;
    }
    buf[total] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON 格式错误");
        return ESP_OK;
    }

    clock_config_t cfg;
    clock_display_get_config(&cfg);

    cJSON *j;
    if ((j = cJSON_GetObjectItem(root, "enabled")) && cJSON_IsBool(j))
        cfg.enabled = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItem(root, "style")) && cJSON_IsNumber(j))
        cfg.style = (uint8_t)j->valueint;
    if ((j = cJSON_GetObjectItem(root, "show_weather")) && cJSON_IsBool(j))
        cfg.show_weather = cJSON_IsTrue(j);
    cJSON_Delete(root);

    clock_display_set_config(&cfg);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── POST /clock_show ─────────────────────────────────────────────── */

static esp_err_t clock_show_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    if (!http_require_epd_ready(req))
        return ESP_OK;

    char tmp[64];
    while (req->content_len > 0) {
        int r = httpd_req_recv(req, tmp, sizeof(tmp));
        if (r <= 0) break;
    }

    esp_err_t err = clock_display_show();
    if (err == ESP_OK) {
        button_set_current_mode(DISPLAY_MODE_CLOCK);
        power_mgr_save_mode(DISPLAY_MODE_CLOCK);
    }
    char json[96];
    snprintf(json, sizeof(json), "{\"ok\":%s}", err == ESP_OK ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── GET /msg_config ───────────────────────────────────────────────── */

static esp_err_t msg_config_get_handler(httpd_req_t *req)
{
    msg_config_t cfg;
    message_board_get_config(&cfg);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "text", cfg.text);
    cJSON_AddNumberToObject(root, "font_size", cfg.font_size);
    cJSON_AddNumberToObject(root, "align", (int)cfg.align);
    cJSON_AddNumberToObject(root, "color", cfg.color);
    cJSON_AddNumberToObject(root, "x_offset", cfg.x_offset);
    cJSON_AddNumberToObject(root, "y_offset", cfg.y_offset);
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON 内存不足");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, str, HTTPD_RESP_USE_STRLEN);
    free(str);
    return ESP_OK;
}

/* ── POST /msg_config ─────────────────────────────────────────────── */

static esp_err_t msg_config_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    char *buf = malloc(MSG_MAX_LEN + 128);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "内存分配失败");
        return ESP_OK;
    }
    int len = req->content_len;
    if (len <= 0 || len >= MSG_MAX_LEN + 128) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "请求体错误");
        return ESP_OK;
    }
    int total = 0;
    while (total < len) {
        int r = httpd_req_recv(req, buf + total, len - total);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "接收失败");
            return ESP_OK;
        }
        total += r;
    }
    buf[total] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON 格式错误");
        return ESP_OK;
    }

    msg_config_t cfg;
    message_board_get_config(&cfg);

    const char *s;
    if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text")))) {
        snprintf(cfg.text, sizeof(cfg.text), "%s", s);
    }
    cJSON *j;
    if ((j = cJSON_GetObjectItem(root, "font_size")) && cJSON_IsNumber(j))
        cfg.font_size = (uint8_t)j->valueint;
    if ((j = cJSON_GetObjectItem(root, "align")) && cJSON_IsNumber(j))
        cfg.align = (msg_align_t)j->valueint;
    if ((j = cJSON_GetObjectItem(root, "color")) && cJSON_IsNumber(j))
        cfg.color = (uint8_t)j->valueint;
    if ((j = cJSON_GetObjectItem(root, "x_offset")) && cJSON_IsNumber(j))
        cfg.x_offset = (int16_t)j->valueint;
    if ((j = cJSON_GetObjectItem(root, "y_offset")) && cJSON_IsNumber(j))
        cfg.y_offset = (int16_t)j->valueint;

    cJSON_Delete(root);

    message_board_set_config(&cfg);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── POST /msg_show ───────────────────────────────────────────────── */

static esp_err_t msg_show_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    if (!http_require_epd_ready(req))
        return ESP_OK;

    char tmp[64];
    while (req->content_len > 0) {
        int r = httpd_req_recv(req, tmp, sizeof(tmp));
        if (r <= 0) break;
    }

    esp_err_t err = message_board_show();
    char json[96];
    snprintf(json, sizeof(json), "{\"ok\":%s}", err == ESP_OK ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* calendar_show moved to http_features.c */

/* ── GET /panel_config ─────────────────────────────────────────────── */

static esp_err_t panel_config_get_handler(httpd_req_t *req)
{
    char json[160];
    snprintf(json, sizeof(json),
             "{\"panel\":%d,\"width\":%d,\"height\":%d,\"has_red\":%s,\"busy_idle\":%d}",
             (int)epd_get_panel(), epd_width(), epd_height(),
             epd_has_red() ? "true" : "false", epd_busy_idle_level());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── POST /panel_config ────────────────────────────────────────────── */

static esp_err_t panel_config_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    char buf[128];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "请求体为空");
        return ESP_OK;
    }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON 格式错误");
        return ESP_OK;
    }

    cJSON *jp = cJSON_GetObjectItem(root, "panel");
    if (jp && cJSON_IsNumber(jp)) {
        epd_set_panel((epd_panel_t)jp->valueint);
    }
    cJSON *jb = cJSON_GetObjectItem(root, "busy_idle");
    if (jb && cJSON_IsNumber(jb)) {
        epd_set_busy_idle(jb->valueint);
    }
    cJSON_Delete(root);

    char json[160];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"panel\":%d,\"width\":%d,\"height\":%d,\"busy_idle\":%d,\"note\":\"reboot if panel changed\"}",
             (int)epd_get_panel(), epd_width(), epd_height(), epd_busy_idle_level());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── favicon (suppress 404 spam) ──────────────────────────────────── */

static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ── power config handlers ─────────────────────────────────────────── */

static esp_err_t power_config_get_handler(httpd_req_t *req)
{
    power_config_t pc;
    power_mgr_get_config(&pc);
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"enabled\":%s,\"interval\":%d,\"idle\":%d}",
             pc.enabled ? "true" : "false",
             pc.interval_min, pc.idle_timeout_s);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t power_config_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;

    char body[256];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "请求为空"); return ESP_OK; }
    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON 格式错误"); return ESP_OK; }

    power_config_t pc;
    power_mgr_get_config(&pc);

    cJSON *j;
    j = cJSON_GetObjectItem(root, "enabled");
    if (j) pc.enabled = cJSON_IsTrue(j);
    j = cJSON_GetObjectItem(root, "interval");
    if (j && cJSON_IsNumber(j)) pc.interval_min = j->valueint;
    j = cJSON_GetObjectItem(root, "idle");
    if (j && cJSON_IsNumber(j)) pc.idle_timeout_s = j->valueint;

    cJSON_Delete(root);

    power_mgr_set_config(&pc);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* countdown handlers moved to http_features.c */

/* ── session open hook (reset inactivity timer) ───────────────────── */

static esp_err_t http_session_open_hook(httpd_handle_t hd, int sockfd)
{
    (void)hd; (void)sockfd;
    power_mgr_reset_activity();
    return ESP_OK;
}

/* ── server start ──────────────────────────────────────────────────── */

esp_err_t http_app_start(const http_app_config_t *cfg)
{
    if (!cfg || !cfg->mount_path || !cfg->upload_path) return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn      = httpd_uri_match_wildcard;
    config.recv_wait_timeout  = 5;
    config.send_wait_timeout  = 5;
    config.max_uri_handlers   = 60;
    config.stack_size         = 16384;
    config.lru_purge_enable   = true;
    config.open_fn            = http_session_open_hook;

    http_auth_load_from_nvs();

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) return err;

    const httpd_uri_t uris[] = {
        /* core pages */
        { "/",              HTTP_GET,  index_get_handler,          NULL },
        { "/config",        HTTP_GET,  config_get_handler,         NULL },
        { "/favicon.ico",   HTTP_GET,  favicon_get_handler,        NULL },
        { "/status",        HTTP_GET,  status_get_handler,         NULL },
        /* gallery (http_gallery.c) */
        { "/upload",        HTTP_POST, gallery_upload_post_handler,       NULL },
        { "/images",        HTTP_GET,  gallery_images_get_handler,        NULL },
        { "/image",         HTTP_GET,  gallery_image_get_handler,         NULL },
        { "/show",          HTTP_POST, gallery_show_post_handler,         NULL },
        { "/delete_image",  HTTP_POST, gallery_delete_image_post_handler, NULL },
        { "/delete",        HTTP_POST, gallery_delete_post_handler,       NULL },
        /* wifi & panel config (http_app.c) */
        { "/wifi_status",   HTTP_GET,  wifi_status_get_handler,    NULL },
        { "/scan",          HTTP_GET,  scan_get_handler,           NULL },
        { "/wifi_connect",  HTTP_POST, wifi_connect_post_handler,  NULL },
        { "/wifi_forget",   HTTP_POST, wifi_forget_post_handler,   NULL },
        { "/panel_config",  HTTP_GET,  panel_config_get_handler,   NULL },
        { "/panel_config",  HTTP_POST, panel_config_post_handler,  NULL },
        /* slideshow, weather, clock, msg config (http_app.c) */
        { "/slideshow",     HTTP_GET,  slideshow_get_handler,      NULL },
        { "/slideshow",     HTTP_POST, slideshow_post_handler,     NULL },
        { "/weather_config", HTTP_GET,  weather_config_get_handler, NULL },
        { "/weather_config", HTTP_POST, weather_config_post_handler,NULL },
        { "/weather_show",   HTTP_POST, weather_show_post_handler,  NULL },
        { "/clock_config",   HTTP_GET,  clock_config_get_handler,   NULL },
        { "/clock_config",   HTTP_POST, clock_config_post_handler,  NULL },
        { "/clock_show",     HTTP_POST, clock_show_post_handler,    NULL },
        { "/msg_config",     HTTP_GET,  msg_config_get_handler,     NULL },
        { "/msg_config",     HTTP_POST, msg_config_post_handler,    NULL },
        { "/msg_show",       HTTP_POST, msg_show_post_handler,      NULL },
        /* canvas board (http_canvas.c) */
        { "/board",              HTTP_GET,  canvas_board_ui_get,      NULL },
        { "/canvas_layout",      HTTP_GET,  canvas_layout_get,        NULL },
        { "/canvas_layout",      HTTP_POST, canvas_layout_post,       NULL },
        { "/canvas_show",        HTTP_POST, canvas_show_post,         NULL },
        { "/canvas_icons",       HTTP_GET,  canvas_icons_get,         NULL },
        { "/canvas_icon_upload", HTTP_POST, canvas_icon_upload_post,  NULL },
        { "/canvas_icon_delete", HTTP_POST, canvas_icon_delete_post,  NULL },
        { "/canvas_image_list",   HTTP_GET,  canvas_image_list_get,    NULL },
        { "/canvas_image",        HTTP_GET,  canvas_image_get,         NULL },
        { "/canvas_image_upload", HTTP_POST, canvas_image_upload_post, NULL },
        { "/canvas_image_delete", HTTP_POST, canvas_image_delete_post, NULL },
        /* features (http_features.c) */
        { "/calendar_show",  HTTP_POST, feat_calendar_show_post,     NULL },
        { "/timetable",      HTTP_GET,  feat_timetable_ui_get,       NULL },
        { "/timetable.json", HTTP_GET,  feat_timetable_json_get,     NULL },
        { "/timetable",      HTTP_POST, feat_timetable_post,         NULL },
        { "/timetable_show", HTTP_POST, feat_timetable_show_post,    NULL },
        { "/todo",           HTTP_GET,  feat_todo_ui_get,            NULL },
        { "/todo.json",      HTTP_GET,  feat_todo_json_get,          NULL },
        { "/todo",           HTTP_POST, feat_todo_post,              NULL },
        { "/todo_show",      HTTP_POST, feat_todo_show_post,         NULL },
        { "/countdown",      HTTP_GET,  feat_countdown_ui_get,       NULL },
        { "/countdown_config", HTTP_GET,  feat_countdown_config_get,  NULL },
        { "/countdown_config", HTTP_POST, feat_countdown_config_post, NULL },
        { "/countdown_show", HTTP_POST, feat_countdown_show_post,    NULL },
        /* system */
        { "/version",       HTTP_GET,  version_get_handler,        NULL },
        { "/ota",           HTTP_POST, ota_post_handler,           NULL },
        { "/auth_config",   HTTP_GET,  auth_config_get_handler,    NULL },
        { "/auth_config",   HTTP_POST, auth_config_post_handler,   NULL },
        { "/power_config",  HTTP_GET,  power_config_get_handler,   NULL },
        { "/power_config",  HTTP_POST, power_config_post_handler,  NULL },
    };
    int registered = 0;
    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++) {
        esp_err_t rerr = httpd_register_uri_handler(s_server, &uris[i]);
        if (rerr == ESP_OK) {
            registered++;
        } else {
            ESP_LOGE(TAG, "Failed to register %s (%d): %s",
                     uris[i].uri, uris[i].method, esp_err_to_name(rerr));
        }
    }

    ESP_LOGI(TAG, "HTTP server started (%d/%d routes)",
             registered, (int)(sizeof(uris) / sizeof(uris[0])));
    return ESP_OK;
}
