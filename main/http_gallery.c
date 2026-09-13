#include "http_internal.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "clock_display.h"
#include "display_policy.h"
#include "epd.h"
#include "fb_render.h"
#include "image_convert.h"
#include "scheduler.h"
#include "spiffs_mount.h"
#include "weather.h"
#include "button.h"
#include "display_mode.h"
#include "power_mgr.h"

static const char *TAG = "http_gal";
static atomic_bool s_upload_active;
static atomic_bool s_show_active;
static atomic_uint s_show_epoch;

typedef struct {
    char name[64];
    char path[160];
    int size;
    unsigned upload_epoch;
    unsigned display_epoch;
    unsigned show_epoch;
    display_request_token_t display_token;
} gallery_show_job_t;

static esp_err_t gallery_send_empty(httpd_req_t *req, bool spiffs_ok)
{
    char json[48];
    snprintf(json, sizeof(json), "{\"items\":[],\"spiffs_ok\":%s}",
             spiffs_ok ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
static atomic_uint s_upload_epoch;

static unsigned gallery_upload_begin(void)
{
    atomic_store(&s_upload_active, true);
    return atomic_fetch_add(&s_upload_epoch, 1) + 1;
}

static void gallery_upload_end(unsigned epoch)
{
    if (atomic_load(&s_upload_epoch) == epoch)
        atomic_store(&s_upload_active, false);
}

static bool gallery_upload_busy(void)
{
    return atomic_load(&s_upload_active);
}

static unsigned gallery_upload_epoch(void)
{
    return atomic_load(&s_upload_epoch);
}

/** 追加 JSON 片段；*len 为已用长度（不含 '\\0'），cap 为 buf 总容量 */
static bool gallery_json_append(char *buf, size_t cap, size_t *len, const char *fmt, ...)
{
    if (*len >= cap) return true;
    size_t avail = cap - *len;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + *len, avail, fmt, ap);
    va_end(ap);
    if (w < 0) return false;
    if ((size_t)w >= avail) {
        *len = cap - 1;
        buf[*len] = '\0';
        return true;
    }
    *len += (size_t)w;
    return true;
}

/* 白名单验证文件名：只允许字母、数字、点、下划线、连字符 */
static bool is_safe_filename(const char *name)
{
    if (!name || name[0] == '\0') return false;
    size_t len = strlen(name);
    if (len > 60 || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return false;
    }
    return true;
}

/** Image focus is transient. Display policy pauses background weather without rewriting NVS. */
static void weather_disable_if_enabled_for_image_focus(void)
{
    /* Kept as a call-site marker; do not disable the user's weather configuration here. */
}

static bool gallery_needs_precise_image_clear(void)
{
    return epd_get_panel() == EPD_PANEL_ATC_SSD1619_29_BWR;
}

static esp_err_t gallery_prepare_image_refresh(const char *reason, unsigned epoch)
{
    if (!gallery_needs_precise_image_clear()) {
        epd_request_full_refresh_next();
        return ESP_OK;
    }

    ESP_LOGI(TAG, "%s: clearing 2.9 SSD1619 before manual image refresh", reason);
    esp_err_t err = epd_clear_screen_epoch(epoch);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: pre-clear failed: %s", reason, esp_err_to_name(err));
        return err;
    }
    epd_request_full_refresh_next();
    return ESP_OK;
}

static esp_err_t gallery_display_raw_like_canvas(const char *raw_path, unsigned epoch)
{
    fb_t *fb = fb_create();
    if (!fb)
        return ESP_ERR_NO_MEM;

    esp_err_t err = fb_import(fb, raw_path);
    if (err != ESP_OK) {
        fb_destroy(fb);
        return err;
    }
    return epd_display_fb_free_mode_epoch(fb, EPD_REFRESH_FULL, epoch);
}

static bool gallery_image_compatible_for_panel(const char *path, const char *name,
                                               image_panel_compat_t *info_out)
{
    image_panel_compat_t info;
    esp_err_t err = image_file_check_panel_compat(path, &info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Gallery compat check failed for '%s': %s; falling back to converter",
                 name ? name : path, esp_err_to_name(err));
        if (info_out)
            memset(info_out, 0, sizeof(*info_out));
        return true;
    }
    if (info_out)
        *info_out = info;
    if (info.known && !info.compatible) {
        ESP_LOGW(TAG,
                 "Gallery image '%s' skipped: image colors=%d mask=0x%02lx exceed panel colors=%d mask=0x%02lx",
                 name ? name : path, info.image_colors, (unsigned long)info.image_mask,
                 info.panel_colors, (unsigned long)info.panel_mask);
        return false;
    }
    return true;
}

static esp_err_t gallery_send_incompatible_response(httpd_req_t *req,
                                                    const image_panel_compat_t *info)
{
    char json[160];
    snprintf(json, sizeof(json),
             "{\"ok\":false,\"err\":\"image_colors_exceed_panel\",\"image_colors\":%d,\"panel_colors\":%d}",
             info ? info->image_colors : 0,
             info ? info->panel_colors : 0);
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

static bool gallery_show_begin(unsigned *epoch_out)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_show_active, &expected, true))
        return false;
    *epoch_out = atomic_fetch_add(&s_show_epoch, 1) + 1;
    return true;
}

static void gallery_show_end(unsigned epoch)
{
    if (atomic_load(&s_show_epoch) == epoch)
        atomic_store(&s_show_active, false);
}

static bool gallery_show_job_current(const gallery_show_job_t *job)
{
    return atomic_load(&s_show_epoch) == job->show_epoch &&
           gallery_upload_epoch() == job->upload_epoch &&
           !gallery_upload_busy() &&
           display_policy_epoch_is_current(job->display_epoch);
}

static void gallery_show_task(void *arg)
{
    gallery_show_job_t *job = (gallery_show_job_t *)arg;
    const char *raw_path = "/spiffs/image.bin";
    int64_t t0 = esp_timer_get_time();
    esp_err_t cerr = ESP_OK;
    esp_err_t perr = ESP_OK;
    esp_err_t derr = ESP_OK;
    bool skipped = false;
    fb_t *fast_fb = NULL;

    ESP_LOGI(TAG, "Gallery show task start: '%s', bytes=%d, show_epoch=%u, display_epoch=%u, upload_epoch=%u",
             job->name, job->size, job->show_epoch, job->display_epoch, job->upload_epoch);

    fb_raw_file_lock();
    int64_t t_lock = esp_timer_get_time();
    ESP_LOGI(TAG, "Gallery show raw-file lock acquired in %lld ms",
             (long long)((t_lock - t0) / 1000LL));

    if (!gallery_show_job_current(job)) {
        skipped = true;
        ESP_LOGI(TAG, "Gallery show '%s' skipped before convert: newer request owns display",
                 job->name);
    }

    if (!skipped && !gallery_image_compatible_for_panel(job->path, job->name, NULL)) {
        skipped = true;
        cerr = ESP_ERR_NOT_SUPPORTED;
        ESP_LOGW(TAG, "Gallery show '%s' skipped before convert: incompatible colors",
                 job->name);
    }

    if (!skipped) {
        ESP_LOGI(TAG, "Gallery show convert begin: %s", job->path);
        cerr = image_convert_file_to_epd_fb(job->path, &fast_fb);
        if (cerr == ESP_OK) {
            ESP_LOGI(TAG, "Gallery show fast framebuffer convert selected");
        } else {
            if (cerr != ESP_ERR_NOT_SUPPORTED)
                ESP_LOGI(TAG, "Gallery show fast framebuffer convert unavailable: %s",
                         esp_err_to_name(cerr));
            cerr = image_convert_file_to_epd_raw(job->path, raw_path);
        }
        int64_t t_convert = esp_timer_get_time();
        if (cerr == ESP_OK) {
            ESP_LOGI(TAG, "Gallery show convert done in %lld ms",
                     (long long)((t_convert - t_lock) / 1000LL));
        } else {
            ESP_LOGW(TAG, "Gallery show convert failed after %lld ms: %s",
                     (long long)((t_convert - t_lock) / 1000LL), esp_err_to_name(cerr));
        }
    }

    if (!skipped && cerr == ESP_OK) {
        if (!gallery_show_job_current(job)) {
            skipped = true;
            ESP_LOGI(TAG, "Gallery show '%s' skipped after convert: newer request owns display",
                     job->name);
        } else {
            int64_t t_prepare0 = esp_timer_get_time();
            ESP_LOGI(TAG, "Gallery show prepare refresh begin");
            perr = gallery_prepare_image_refresh("gallery show async", job->display_epoch);
            int64_t t_prepare1 = esp_timer_get_time();
            if (perr == ESP_OK) {
                ESP_LOGI(TAG, "Gallery show prepare refresh done in %lld ms",
                         (long long)((t_prepare1 - t_prepare0) / 1000LL));
            } else {
                ESP_LOGW(TAG, "Gallery show prepare refresh failed after %lld ms: %s",
                         (long long)((t_prepare1 - t_prepare0) / 1000LL), esp_err_to_name(perr));
            }
        }
    }

    if (!skipped && cerr == ESP_OK && perr == ESP_OK) {
        if (!gallery_show_job_current(job)) {
            skipped = true;
            ESP_LOGI(TAG, "Gallery show '%s' skipped before display: newer request owns display",
                     job->name);
        } else {
            int64_t t_display0 = esp_timer_get_time();
            ESP_LOGI(TAG, "Gallery show display begin from %s",
                     fast_fb ? "fast framebuffer" : "raw file");
            if (fast_fb) {
                derr = epd_display_fb_free_mode_epoch(fast_fb, EPD_REFRESH_FULL,
                                                       job->display_epoch);
                fast_fb = NULL;
            } else {
                derr = epd_display_from_file_mode_epoch(raw_path, EPD_REFRESH_FULL,
                                                        job->display_epoch);
            }
            int64_t t_display1 = esp_timer_get_time();
            if (derr == ESP_OK) {
                ESP_LOGI(TAG, "Gallery show display done in %lld ms, total %lld ms",
                         (long long)((t_display1 - t_display0) / 1000LL),
                         (long long)((t_display1 - t0) / 1000LL));
            } else {
                ESP_LOGW(TAG, "Gallery show display failed after %lld ms: %s",
                         (long long)((t_display1 - t_display0) / 1000LL), esp_err_to_name(derr));
            }
        }
    }

    if (fast_fb) {
        fb_destroy(fast_fb);
        fast_fb = NULL;
    }
    fb_raw_file_unlock();

    esp_err_t final_err = cerr != ESP_OK ? cerr : (perr != ESP_OK ? perr : derr);
    bool committed = !skipped && final_err == ESP_OK &&
                     display_policy_finish_request(&job->display_token,
                                                   ESP_OK, true) &&
                     display_policy_epoch_is_current(job->display_epoch);
    if (final_err != ESP_OK)
        (void)display_policy_finish_request(&job->display_token, final_err, true);
    if (committed) {
        scheduler_set_current_image_name(job->name);
        weather_disable_if_enabled_for_image_focus();
        scheduler_notify_manual_show();
        button_set_current_mode(DISPLAY_MODE_SLIDESHOW);
        power_mgr_save_mode(DISPLAY_MODE_SLIDESHOW);
    } else if (!skipped) {
        ESP_LOGI(TAG, "Gallery show '%s' completed but was superseded", job->name);
    }

    ESP_LOGI(TAG, "Gallery show task finish: '%s', result convert=%s prepare=%s display=%s",
             job->name, esp_err_to_name(cerr), esp_err_to_name(perr), esp_err_to_name(derr));
    gallery_show_end(job->show_epoch);
    free(job);
    vTaskDelete(NULL);
}

esp_err_t gallery_images_get_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;

    if (!spiffs_mount_is_mounted()) {
        return gallery_send_empty(req, false);
    }

    DIR *dir = opendir(http_images_dir);
    if (!dir) {
        (void)mkdir(http_images_dir, 0755);
        dir = opendir(http_images_dir);
    }
    if (!dir) {
        return gallery_send_empty(req, true);
    }

    size_t cap = 2048;
    char *buf = malloc(cap);
    if (!buf) {
        closedir(dir);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "内存分配失败");
        return ESP_OK;
    }

    size_t len = 0;
    (void)gallery_json_append(buf, cap, &len, "{\"items\":[");

    struct dirent *ent;
    bool first = true;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN) continue;
        char path[160];
        snprintf(path, sizeof(path), "%s/%.80s", http_images_dir, ent->d_name);
        int sz = http_stat_size_or_neg(path);
        if (sz < 0) continue;

        size_t need = strlen(ent->d_name) + 48;
        if (len + need >= cap) {
            size_t newcap = cap * 2;
            char *tmp = realloc(buf, newcap);
            if (!tmp) {
                ESP_LOGW(TAG, "/images: realloc %u failed, truncating list", (unsigned)newcap);
                break;
            }
            buf = tmp;
            cap = newcap;
        }

        if (!first) {
            if (len + 1 < cap)
                buf[len++] = ',';
            else
                break;
        }
        first = false;
        char esc_name[80];
        json_escape(esc_name, sizeof(esc_name), ent->d_name);
        if (!gallery_json_append(buf, cap, &len, "{\"name\":\"%s\",\"bytes\":%d}", esc_name, sz))
            break;
    }
    closedir(dir);

    (void)gallery_json_append(buf, cap, &len, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, (ssize_t)len);
    free(buf);
    return ESP_OK;
}

esp_err_t gallery_image_get_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;

    char name[64];
    if (!http_get_query_param(req, "name", name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少文件名");
        return ESP_OK;
    }
    if (!is_safe_filename(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "文件名无效");
        return ESP_OK;
    }

    char path[160];
    snprintf(path, sizeof(path), "%s/%s", http_images_dir, name);

    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "文件未找到");
        return ESP_OK;
    }

    const char *mime = "image/jpeg";
    size_t nlen = strlen(name);
    if (nlen > 4 && strcasecmp(name + nlen - 4, ".png") == 0) mime = "image/png";
    if (nlen > 4 && strcasecmp(name + nlen - 4, ".bmp") == 0) mime = "image/bmp";
    httpd_resp_set_type(req, mime);

    char buf[1024];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, r) != ESP_OK) {
            fclose(f);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_OK;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

esp_err_t gallery_show_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    unsigned upload_epoch = gallery_upload_epoch();
    if (gallery_upload_busy()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "图片正在上传，请稍后再显示图库图片");
        return ESP_OK;
    }
    if (!http_require_epd_ready(req))
        return ESP_OK;
    if (!http_require_mode_available(req, DISPLAY_MODE_SLIDESHOW))
        return ESP_OK;

    char name[64];
    if (!http_get_query_param(req, "name", name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少文件名");
        return ESP_OK;
    }
    if (!is_safe_filename(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "文件名无效");
        return ESP_OK;
    }

    char jpeg_path[160];
    snprintf(jpeg_path, sizeof(jpeg_path), "%s/%s", http_images_dir, name);
    int sz = http_stat_size_or_neg(jpeg_path);
    if (sz <= 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "文件未找到");
        return ESP_OK;
    }

    image_panel_compat_t compat;
    if (!gallery_image_compatible_for_panel(jpeg_path, name, &compat)) {
        return gallery_send_incompatible_response(req, &compat);
    }

    unsigned show_epoch = 0;
    if (!gallery_show_begin(&show_epoch)) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "gallery show already running");
        return ESP_OK;
    }

    gallery_show_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        gallery_show_end(show_epoch);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory for gallery show job");
        return ESP_OK;
    }

    display_request_token_t display_token = display_policy_begin_request(
        DISPLAY_MODE_SLIDESHOW, EPD_REFRESH_FULL);
    unsigned display_epoch = display_token.epoch;

    strncpy(job->name, name, sizeof(job->name) - 1);
    strncpy(job->path, jpeg_path, sizeof(job->path) - 1);
    job->size = sz;
    job->upload_epoch = upload_epoch;
    job->display_epoch = display_epoch;
    job->display_token = display_token;
    job->show_epoch = show_epoch;

    BaseType_t ok = xTaskCreate(gallery_show_task, "gal_show", 10240, job, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create gallery show task for '%s'", name);
        (void)display_policy_finish_request(&display_token, ESP_ERR_NO_MEM, true);
        gallery_show_end(show_epoch);
        free(job);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to start gallery show task");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Gallery show queued: '%s', bytes=%d, show_epoch=%u", name, sz, show_epoch);
    httpd_resp_set_type(req, "application/json");
    char response[96];
    snprintf(response, sizeof(response), "{\"ok\":true,\"queued\":true,\"epoch\":%u}",
             display_epoch);
    httpd_resp_sendstr(req, response);
    return ESP_OK;
}

esp_err_t gallery_delete_image_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    char name[64];
    if (!http_get_query_param(req, "name", name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少文件名");
        return ESP_OK;
    }
    if (!is_safe_filename(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "文件名无效");
        return ESP_OK;
    }

    char jpeg_path[160];
    snprintf(jpeg_path, sizeof(jpeg_path), "%s/%s", http_images_dir, name);
    int result = remove(jpeg_path);
    int error = result == 0 ? 0 : errno;
    if (error) {
        httpd_resp_send_err(req, error == ENOENT ? HTTPD_404_NOT_FOUND : HTTPD_500_INTERNAL_SERVER_ERROR,
                            error == ENOENT ? "image not found" : "image delete failed");
        return ESP_OK;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "delete %.40s: ok", name);
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

esp_err_t gallery_delete_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    if (!http_receive_drain(req)) return ESP_FAIL;

    /* Capture request order before waiting for another renderer's raw-file lock. */
    display_request_token_t token = display_policy_begin_request(
        DISPLAY_MODE_CLOCK, EPD_REFRESH_FULL);
    bool skip_upload = !display_policy_epoch_is_current(token.epoch);
    int err1 = skip_upload ? 0 : remove(http_app_cfg.upload_path) == 0 ? 0 : errno;
    fb_raw_file_lock();
    bool skip_raw = !display_policy_epoch_is_current(token.epoch);
    int err2 = skip_raw ? 0 : remove("/spiffs/image.bin") == 0 ? 0 : errno;
    fb_raw_file_unlock();

    esp_err_t clear_err = epd_is_ready() && display_policy_epoch_is_current(token.epoch)
        ? epd_clear_screen_epoch(token.epoch) : ESP_ERR_INVALID_STATE;
    bool committed = display_policy_finish_request(&token, clear_err, false);
    if (clear_err == ESP_OK && committed &&
        display_policy_epoch_is_current(token.epoch)) {
        button_set_current_mode(DISPLAY_MODE_CLOCK);
        power_mgr_save_mode(DISPLAY_MODE_CLOCK);
    }
    clock_display_notify_home_changed();

    bool superseded = !committed || !display_policy_epoch_is_current(token.epoch);
    if (superseded)
        httpd_resp_set_status(req, "409 Conflict");
    else if (clear_err != ESP_OK || (err1 && err1 != ENOENT) || (err2 && err2 != ENOENT))
        httpd_resp_set_status(req, "500 Internal Server Error");
    char msg[256];
    snprintf(msg, sizeof(msg), "upload.jpg=%s，image.bin=%s；清屏=%s%s",
             skip_upload ? "未执行" : err1 == 0 ? "已删除" : err1 == ENOENT ? "不存在" : "删除失败",
             skip_raw ? "未执行" : err2 == 0 ? "已删除" : err2 == ENOENT ? "不存在" : "删除失败",
             superseded ? "已被新请求替换" : clear_err == ESP_OK ? "完成" : "失败：",
             !superseded && clear_err != ESP_OK ? esp_err_to_name(clear_err) : "");
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

esp_err_t gallery_upload_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    if (!http_require_epd_ready(req)) return ESP_OK;
    if (!http_require_mode_available(req, DISPLAY_MODE_SLIDESHOW)) return ESP_OK;
    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "请求体为空");
        return ESP_OK;
    }
    if (req->content_len > http_upload_max_bytes) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "文件过大");
        return ESP_OK;
    }

    unsigned upload_epoch = gallery_upload_begin();
    display_request_token_t upload_token = display_policy_begin_request(
        DISPLAY_MODE_SLIDESHOW, EPD_REFRESH_FULL);
    unsigned display_epoch = upload_token.epoch;

    char temporary[160], backup[160];
    snprintf(temporary, sizeof(temporary), "%s/http_upload.tmp", http_app_cfg.mount_path);
    snprintf(backup, sizeof(backup), "%s/http_upload.bak", http_app_cfg.mount_path);
    FILE *f = fopen(temporary, "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s) failed: errno=%d", http_app_cfg.upload_path, errno);
        gallery_upload_end(upload_epoch);
        (void)display_policy_finish_request(&upload_token, ESP_FAIL, true);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "文件打开失败");
        return ESP_OK;
    }

    char buf[1024];
    int remaining = req->content_len;
    size_t total_written = 0;
    bool first_chunk = true;
    http_receive_t receive;
    http_receive_init(&receive, HTTP_RECEIVE_UPLOAD_MS);
    while (remaining > 0) {
        int to_recv = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int r = http_receive_chunk(req, &receive, buf, to_recv);
        if (r <= 0) {
            fclose(f);
            remove(temporary);
            gallery_upload_end(upload_epoch);
            (void)display_policy_finish_request(&upload_token, ESP_FAIL, true);
            return ESP_FAIL;
        }
        if (first_chunk && r >= 4) {
            ESP_LOGI(TAG, "First chunk magic: %02X %02X %02X %02X (recv %d bytes)",
                     (uint8_t)buf[0], (uint8_t)buf[1], (uint8_t)buf[2], (uint8_t)buf[3], r);
            first_chunk = false;
        }
        size_t written = fwrite(buf, 1, (size_t)r, f);
        if (written != (size_t)r) {
            ESP_LOGE(TAG, "fwrite failed: wrote %u of %d", (unsigned)written, r);
            http_receive_abort(req);
            fclose(f);
            remove(temporary);
            gallery_upload_end(upload_epoch);
            (void)display_policy_finish_request(&upload_token, ESP_FAIL, true);
            return ESP_FAIL;
        }
        total_written += written;
        remaining -= r;
    }
    bool closed = fclose(f) == 0;
    if (!closed || !http_receive_commit_file(temporary, http_app_cfg.upload_path, backup)) {
        remove(temporary);
        gallery_upload_end(upload_epoch);
        (void)display_policy_finish_request(&upload_token, ESP_FAIL, true);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload commit failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Saved upload to %s (%u bytes written)", http_app_cfg.upload_path, (unsigned)total_written);

    /* verify the written file */
    {
        struct stat vst;
        if (stat(http_app_cfg.upload_path, &vst) == 0) {
            ESP_LOGI(TAG, "Verify: file size on disk = %ld", (long)vst.st_size);
        } else {
            ESP_LOGE(TAG, "Verify: stat failed errno=%d", errno);
        }
        FILE *vf = fopen(http_app_cfg.upload_path, "rb");
        if (vf) {
            uint8_t hd[4] = {0};
            fread(hd, 1, 4, vf);
            fclose(vf);
            ESP_LOGI(TAG, "Verify: file magic = %02X %02X %02X %02X", hd[0], hd[1], hd[2], hd[3]);
        }
    }

    char name[64];
    if (!http_get_query_param(req, "name", name, sizeof(name))) {
        const char *ext = "jpg";
        uint8_t hdr[4] = {0};
        FILE *hf = fopen(http_app_cfg.upload_path, "rb");
        if (hf) { fread(hdr, 1, 4, hf); fclose(hf); }
        if (hdr[0] == 0x89 && hdr[1] == 0x50) ext = "png";
        if (hdr[0] == 0x42 && hdr[1] == 0x4D) ext = "bmp";
        snprintf(name, sizeof(name), "img_%d.%s", (int)esp_random(), ext);
    }
    if (!is_safe_filename(name)) {
        snprintf(name, sizeof(name), "img_%d.jpg", (int)esp_random());
    }
    char final_path[160];
    snprintf(final_path, sizeof(final_path), "%s/%s", http_images_dir, name);
    char gallery_temporary[192], gallery_backup[192];
    snprintf(gallery_temporary, sizeof(gallery_temporary), "%s.tmp", final_path);
    snprintf(gallery_backup, sizeof(gallery_backup), "%s.bak", final_path);
    struct stat stage_stat;
    if (stat(gallery_temporary, &stage_stat) == 0 || errno != ENOENT) {
        gallery_upload_end(upload_epoch);
        (void)display_policy_finish_request(&upload_token, ESP_FAIL, true);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "gallery staging path unavailable");
        return ESP_FAIL;
    }
    FILE *src = fopen(http_app_cfg.upload_path, "rb");
    FILE *dst = src ? fopen(gallery_temporary, "wb") : NULL;
    bool copy_ok = false;
    if (src && dst) {
        char cbuf[512];
        size_t r;
        copy_ok = true;
        while ((r = fread(cbuf, 1, sizeof(cbuf), src)) > 0) {
            if (fwrite(cbuf, 1, r, dst) != r) {
                copy_ok = false;
                break;
            }
        }
        if (ferror(src)) copy_ok = false;
        if (fclose(dst) != 0) copy_ok = false;
        if (copy_ok) {
            ESP_LOGI(TAG, "Staged gallery copy at %s", gallery_temporary);
        } else {
            ESP_LOGE(TAG, "Failed while copying gallery image to %s", final_path);
        }
    }
    if (src) fclose(src);
    if (!copy_ok) {
        remove(gallery_temporary);
        gallery_upload_end(upload_epoch);
        (void)display_policy_finish_request(&upload_token, ESP_FAIL, true);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "图库保存失败");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Free heap before convert: %lu bytes",
             (unsigned long)esp_get_free_heap_size());

    image_panel_compat_t upload_compat;
    if (!gallery_image_compatible_for_panel(gallery_temporary, name, &upload_compat)) {
        remove(gallery_temporary);
        gallery_upload_end(upload_epoch);
        (void)display_policy_finish_request(&upload_token, ESP_ERR_NOT_SUPPORTED, true);
        return gallery_send_incompatible_response(req, &upload_compat);
    }

    const char *raw_path = "/spiffs/image.bin";
    fb_raw_file_lock();
    if (!display_policy_epoch_is_current(display_epoch) ||
        gallery_upload_epoch() != upload_epoch) {
        fb_raw_file_unlock();
        remove(gallery_temporary);
        gallery_upload_end(upload_epoch);
        (void)display_policy_finish_request(&upload_token,
                                            ESP_ERR_INVALID_STATE, true);
        ESP_LOGI(TAG, "Upload '%s' display skipped: newer request owns display", name);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "upload received, gallery commit skipped by newer request");
        return ESP_OK;
    }
    esp_err_t cerr = image_convert_file_to_epd_raw(http_app_cfg.upload_path, raw_path);
    if (cerr != ESP_OK) {
        fb_raw_file_unlock();
        remove(gallery_temporary);
        gallery_upload_end(upload_epoch);
        (void)display_policy_finish_request(&upload_token, cerr, true);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "图片转换失败：请上传标准的 JPEG、PNG 或 BMP 图片");
        return ESP_OK;
    }

    if (!http_receive_commit_file(gallery_temporary, final_path, gallery_backup)) {
        fb_raw_file_unlock();
        remove(gallery_temporary);
        gallery_upload_end(upload_epoch);
        (void)display_policy_finish_request(&upload_token, ESP_FAIL, true);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "gallery commit failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Raw ready at %s, free heap: %lu",
             raw_path, (unsigned long)esp_get_free_heap_size());

    if (!display_policy_epoch_is_current(display_epoch) ||
        gallery_upload_epoch() != upload_epoch) {
        fb_raw_file_unlock();
        gallery_upload_end(upload_epoch);
        (void)display_policy_finish_request(&upload_token,
                                            ESP_ERR_INVALID_STATE, true);
        ESP_LOGI(TAG, "Upload '%s' display skipped after convert: newer request owns display", name);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "upload saved, display skipped by newer request");
        return ESP_OK;
    }

    esp_err_t derr = gallery_prepare_image_refresh("upload", display_epoch);
    bool upload_skipped_by_newer = false;
    if (derr == ESP_OK && display_policy_epoch_is_current(display_epoch) &&
        gallery_upload_epoch() == upload_epoch) {
        derr = gallery_display_raw_like_canvas(raw_path, display_epoch);
    } else if (derr == ESP_OK) {
        upload_skipped_by_newer = true;
    }
    fb_raw_file_unlock();
    if (upload_skipped_by_newer) {
        gallery_upload_end(upload_epoch);
        (void)display_policy_finish_request(&upload_token,
                                            ESP_ERR_INVALID_STATE, true);
        ESP_LOGI(TAG, "Upload '%s' display skipped after pre-clear: newer request owns display", name);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "upload saved, display skipped by newer request");
        return ESP_OK;
    }
    if (derr != ESP_OK) {
        ESP_LOGW(TAG, "EPD display failed: %s", esp_err_to_name(derr));
        gallery_upload_end(upload_epoch);
        (void)display_policy_finish_request(&upload_token, derr, true);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "图片已转换但墨水屏刷新失败");
        return ESP_OK;
    }

    if (!display_policy_epoch_is_current(display_epoch) ||
        gallery_upload_epoch() != upload_epoch) {
        gallery_upload_end(upload_epoch);
        (void)display_policy_finish_request(&upload_token,
                                            ESP_ERR_INVALID_STATE, true);
        ESP_LOGI(TAG, "Upload '%s' display completed but was superseded", name);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "upload display superseded");
        return ESP_OK;
    }
    if (!display_policy_finish_request(&upload_token, ESP_OK, true)) {
        gallery_upload_end(upload_epoch);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "upload display superseded");
        return ESP_OK;
    }
    if (!display_policy_epoch_is_current(display_epoch)) {
        gallery_upload_end(upload_epoch);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "upload display superseded");
        return ESP_OK;
    }
    scheduler_set_current_image_name(name);
    weather_disable_if_enabled_for_image_focus();
    scheduler_notify_manual_show();
    button_set_current_mode(DISPLAY_MODE_SLIDESHOW);
    power_mgr_save_mode(DISPLAY_MODE_SLIDESHOW);
    gallery_upload_end(upload_epoch);
    httpd_resp_sendstr(req, "OK — 已转换并触发墨水屏刷新。");
    return ESP_OK;
}
