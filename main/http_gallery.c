#include "http_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"

#include "clock_display.h"
#include "display_policy.h"
#include "epd.h"
#include "image_convert.h"
#include "scheduler.h"
#include "weather.h"

static const char *TAG = "http_gal";

/** 以图片为主页时关闭全页天气定时拉取；若时钟已开且要天气摘要，则保留 API 供嵌入式拉取 */
static void weather_disable_if_enabled_for_image_focus(void)
{
    weather_config_t wcfg;
    clock_config_t ccfg;
    if (weather_get_config(&wcfg) != ESP_OK || !wcfg.enabled)
        return;
    if (clock_display_get_config(&ccfg) == ESP_OK && ccfg.enabled && ccfg.show_weather)
        return;
    wcfg.enabled = false;
    (void)weather_set_config(&wcfg);
}

esp_err_t gallery_images_get_handler(httpd_req_t *req)
{
    DIR *dir = opendir(http_images_dir);
    if (!dir) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "目录读取失败");
        return ESP_OK;
    }

    size_t cap = 2048;
    char *buf = malloc(cap);
    if (!buf) {
        closedir(dir);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "内存分配失败");
        return ESP_OK;
    }

    size_t len = 0;
    len += snprintf(buf + len, cap - len, "{\"items\":[");

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

        if (!first) buf[len++] = ',';
        first = false;
        len += snprintf(buf + len, cap - len,
                        "{\"name\":\"%s\",\"bytes\":%d}", ent->d_name, sz);
    }
    closedir(dir);

    len += snprintf(buf + len, cap - len, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, (ssize_t)len);
    free(buf);
    return ESP_OK;
}

esp_err_t gallery_image_get_handler(httpd_req_t *req)
{
    char name[64];
    if (!http_get_query_param(req, "name", name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少文件名");
        return ESP_OK;
    }
    if (strchr(name, '/') || strstr(name, "..")) {
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
    if (!http_require_epd_ready(req))
        return ESP_OK;

    char name[64];
    if (!http_get_query_param(req, "name", name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少文件名");
        return ESP_OK;
    }
    if (strchr(name, '/') || strstr(name, "..")) {
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

    const char *raw_path = "/spiffs/image.bin";
    esp_err_t cerr = image_convert_file_to_epd_raw(jpeg_path, raw_path);
    if (cerr != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "图片转换失败");
        return ESP_OK;
    }
    esp_err_t derr = epd_display_from_file(raw_path);
    if (derr != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "墨水屏刷新失败");
        return ESP_OK;
    }
    /* 禁止时钟/天气摘要任务继续写 image.bin 抢屏，直到用户点「立即显示时钟」 */
    display_policy_set_manual_screen_active(true);
    weather_disable_if_enabled_for_image_focus();
    scheduler_notify_manual_show();
    httpd_resp_sendstr(req, "OK");
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
    if (strchr(name, '/') || strstr(name, "..")) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "文件名无效");
        return ESP_OK;
    }

    char jpeg_path[160];
    snprintf(jpeg_path, sizeof(jpeg_path), "%s/%s", http_images_dir, name);
    int ok = (remove(jpeg_path) == 0) ? 1 : 0;

    char msg[96];
    snprintf(msg, sizeof(msg), "delete %.40s: %s", name, ok ? "ok" : "not found");
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

esp_err_t gallery_delete_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    char tmp[64];
    while (req->content_len > 0) {
        int r = httpd_req_recv(req, tmp, sizeof(tmp));
        if (r <= 0) break;
        req->content_len -= r;
    }

    int ok1 = (remove(http_app_cfg.upload_path)  == 0) ? 1 : 0;
    int ok2 = (remove("/spiffs/image.bin") == 0) ? 1 : 0;

    if (epd_is_ready())
        (void)epd_clear_screen();
    display_policy_set_manual_screen_active(false);
    clock_display_notify_home_changed();

    char msg[96];
    snprintf(msg, sizeof(msg), "已删除：upload.jpg=%s，image.bin=%s；并已清屏",
             ok1 ? "是" : "否", ok2 ? "是" : "否");
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

esp_err_t gallery_upload_post_handler(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "请求体为空");
        return ESP_OK;
    }
    if (req->content_len > http_upload_max_bytes) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "文件过大");
        return ESP_OK;
    }

    FILE *f = fopen(http_app_cfg.upload_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s) failed: errno=%d", http_app_cfg.upload_path, errno);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "文件打开失败");
        return ESP_OK;
    }

    char buf[1024];
    int remaining = req->content_len;
    size_t total_written = 0;
    bool first_chunk = true;
    while (remaining > 0) {
        int to_recv = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int r = httpd_req_recv(req, buf, to_recv);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r < 0) {
            fclose(f);
            remove(http_app_cfg.upload_path);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "接收数据失败");
            return ESP_OK;
        }
        if (r == 0) {
            fclose(f);
            remove(http_app_cfg.upload_path);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "客户端断开连接");
            return ESP_OK;
        }
        if (first_chunk && r >= 4) {
            ESP_LOGI(TAG, "First chunk magic: %02X %02X %02X %02X (recv %d bytes)",
                     (uint8_t)buf[0], (uint8_t)buf[1], (uint8_t)buf[2], (uint8_t)buf[3], r);
            first_chunk = false;
        }
        size_t written = fwrite(buf, 1, (size_t)r, f);
        if (written != (size_t)r) {
            ESP_LOGE(TAG, "fwrite failed: wrote %u of %d", (unsigned)written, r);
            fclose(f);
            remove(http_app_cfg.upload_path);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "写入失败");
            return ESP_OK;
        }
        total_written += written;
        remaining -= r;
    }
    fclose(f);
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
    if (strchr(name, '/') || strstr(name, "..")) {
        snprintf(name, sizeof(name), "img_%d.jpg", (int)esp_random());
    }
    char final_path[160];
    snprintf(final_path, sizeof(final_path), "%s/%s", http_images_dir, name);
    FILE *src = fopen(http_app_cfg.upload_path, "rb");
    FILE *dst = src ? fopen(final_path, "wb") : NULL;
    if (src && dst) {
        char cbuf[512];
        size_t r;
        while ((r = fread(cbuf, 1, sizeof(cbuf), src)) > 0) {
            fwrite(cbuf, 1, r, dst);
        }
        fclose(dst);
        ESP_LOGI(TAG, "Stored gallery copy at %s", final_path);
    }
    if (src) fclose(src);

    ESP_LOGI(TAG, "Free heap before convert: %lu bytes",
             (unsigned long)esp_get_free_heap_size());

    if (!http_require_epd_ready(req))
        return ESP_OK;

    const char *raw_path = "/spiffs/image.bin";
    esp_err_t cerr = image_convert_file_to_epd_raw(http_app_cfg.upload_path, raw_path);
    if (cerr != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "图片转换失败：请上传标准的 JPEG、PNG 或 BMP 图片");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Raw ready at %s, free heap: %lu",
             raw_path, (unsigned long)esp_get_free_heap_size());

    esp_err_t derr = epd_display_from_file(raw_path);
    if (derr != ESP_OK) {
        ESP_LOGW(TAG, "EPD display failed: %s", esp_err_to_name(derr));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "图片已转换但墨水屏刷新失败");
        return ESP_OK;
    }

    display_policy_set_manual_screen_active(true);
    weather_disable_if_enabled_for_image_focus();
    scheduler_notify_manual_show();
    httpd_resp_sendstr(req, "OK — 已转换并触发墨水屏刷新。");
    return ESP_OK;
}
