/**
 * @file http_canvas.c
 *
 * HTTP 处理器：画布留言板 WYSIWYG 编辑器。
 *
 * 路由：
 *   GET  /board               — 返回 board.html 编辑器页面
 *   GET  /canvas_layout       — 返回当前布局 JSON
 *   POST /canvas_layout       — 保存布局 JSON（可选立即渲染）
 *   POST /canvas_show         — 触发渲染并推送到墨水屏
 *   GET  /canvas_icons        — 返回 {"builtin":[...], "user":[...]}
 *   POST /canvas_icon_upload  — 上传用户图标（multipart/form-data 或 raw）
 *   POST /canvas_icon_delete  — 删除用户图标（?name=xxx）
 */

#include "http_internal.h"
#include <sys/stat.h>
#include <sys/unistd.h>
#include <errno.h>
#include "canvas_board.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "http_canvas";

/* ── GET /board ─────────────────────────────────────────────────────── */

esp_err_t canvas_board_ui_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_send(req, (const char *)board_html_start,
                    (ssize_t)(board_html_end - board_html_start));
    return ESP_OK;
}

/* ── GET /canvas_layout ─────────────────────────────────────────────── */

esp_err_t canvas_layout_get(httpd_req_t *req)
{
    char *buf = malloc(CANVAS_LAYOUT_MAX_BYTES + 4);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "内存不足");
        return ESP_OK;
    }
    esp_err_t err = canvas_board_get_layout(buf, CANVAS_LAYOUT_MAX_BYTES + 2);
    if (err != ESP_OK) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "读取布局失败");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return ESP_OK;
}

/* ── POST /canvas_layout ────────────────────────────────────────────── */

esp_err_t canvas_layout_post(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;

    int len = req->content_len;
    if (len <= 0 || len > CANVAS_LAYOUT_MAX_BYTES) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "内容长度无效");
        return ESP_OK;
    }

    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "内存不足");
        return ESP_OK;
    }

    int total = 0;
    while (total < len) {
        int r = httpd_req_recv(req, buf + total, len - total);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "接收失败");
            return ESP_OK;
        }
        total += r;
    }
    buf[total] = '\0';

    esp_err_t err = canvas_board_set_layout(buf, (size_t)total);
    free(buf);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "保存布局失败");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── POST /canvas_show ──────────────────────────────────────────────── */

esp_err_t canvas_show_post(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;
    if (!http_require_epd_ready(req)) return ESP_OK;

    /* 消耗请求体 */
    char tmp[64];
    while (req->content_len > 0) {
        int r = httpd_req_recv(req, tmp, sizeof(tmp));
        if (r <= 0) break;
        req->content_len -= (size_t)r;
    }

    esp_err_t err = canvas_board_show();
    /* 不切换循环模式，留言板属于手动触发内容 */

    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":%s}", err == ESP_OK ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── GET /canvas_icons ──────────────────────────────────────────────── */

esp_err_t canvas_icons_get(httpd_req_t *req)
{
    char *builtin = malloc(2048);
    char *user    = malloc(1024);
    if (!builtin || !user) {
        free(builtin); free(user);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "内存不足");
        return ESP_OK;
    }

    canvas_board_list_builtin_icons(builtin, 2048);
    canvas_board_list_user_icons(user, 1024);

    size_t total = strlen(builtin) + strlen(user) + 32;
    char *resp = malloc(total);
    if (!resp) {
        free(builtin); free(user);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "内存不足");
        return ESP_OK;
    }
    snprintf(resp, total, "{\"builtin\":%s,\"user\":%s}", builtin, user);
    free(builtin); free(user);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    free(resp);
    return ESP_OK;
}

/* ── POST /canvas_icon_upload ───────────────────────────────────────── */
/*
 * 接受两种格式：
 *   A) Content-Type: application/octet-stream  —  body 即 32 字节 raw 1-bit 位图
 *   B) multipart/form-data（未来扩展，此处先实现 A）
 *
 * 图标名通过 query param ?name=xxx 传递。
 */
esp_err_t canvas_icon_upload_post(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;

    char name[40] = {0};
    if (!http_get_query_param(req, "name", name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少 name 参数");
        return ESP_OK;
    }

    /* 验证名称 */
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-')) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "图标名称含非法字符");
            return ESP_OK;
        }
    }

    int len = req->content_len;
    if (len <= 0 || len > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "内容长度无效（期望 32 字节 1-bit 位图）");
        return ESP_OK;
    }

    uint8_t *buf = malloc((size_t)len);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "内存不足");
        return ESP_OK;
    }
    int total = 0;
    while (total < len) {
        int r = httpd_req_recv(req, (char *)buf + total, len - total);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { free(buf); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "接收失败"); return ESP_OK; }
        total += r;
    }

    /* 仅接受 32 字节 */
    if (total != 32) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "图标必须是 16×16 1-bit 位图（32 字节）");
        return ESP_OK;
    }

    esp_err_t err = canvas_board_save_user_icon(name, buf, 32);
    free(buf);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "保存图标失败");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "User icon saved: %s", name);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── GET /canvas_image_list ─────────────────────────────────────────── */

esp_err_t canvas_image_list_get(httpd_req_t *req)
{
    char *list = malloc(2048);
    if (!list) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "内存不足");
        return ESP_OK;
    }
    canvas_board_list_images(list, 2048);

    size_t total = strlen(list) + 16;
    char *resp = malloc(total);
    if (!resp) {
        free(list);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "内存不足");
        return ESP_OK;
    }
    snprintf(resp, total, "{\"images\":%s}", list);
    free(list);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    free(resp);
    return ESP_OK;
}

/* ── GET /canvas_image ──────────────────────────────────────────────── */
/* 流式读取文件发送，避免大 malloc                                       */

esp_err_t canvas_image_get(httpd_req_t *req)
{
    char name[40] = {0};
    if (!http_get_query_param(req, "name", name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少 name 参数");
        return ESP_OK;
    }
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-')) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "名称非法");
            return ESP_OK;
        }
    }
    char path[128];
    snprintf(path, sizeof(path), "%s/%s.bin", CANVAS_IMAGES_DIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "图片不存在");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    char chunk[512];
    size_t r;
    while ((r = fread(chunk, 1, sizeof(chunk), f)) > 0)
        httpd_resp_send_chunk(req, chunk, (ssize_t)r);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── POST /canvas_image_upload ──────────────────────────────────────── */
/* 流式接收并直接写入 SPIFFS，无需大 malloc                              */

esp_err_t canvas_image_upload_post(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;

    char name[40] = {0};
    if (!http_get_query_param(req, "name", name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少 name 参数");
        return ESP_OK;
    }
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-')) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "图片名称含非法字符");
            return ESP_OK;
        }
    }

    int content_len = req->content_len;
    if (content_len <= 4 || content_len > CANVAS_IMAGE_MAX_BYTES) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "内容长度无效");
        return ESP_OK;
    }

    /* 直接流式写入文件 */
    struct stat _st;
    if (stat(CANVAS_IMAGES_DIR, &_st) != 0) mkdir(CANVAS_IMAGES_DIR, 0755);
    char path[128];
    snprintf(path, sizeof(path), "%s/%s.bin", CANVAS_IMAGES_DIR, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s) failed: errno=%d", path, errno);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "创建文件失败");
        return ESP_OK;
    }

    char chunk[512];
    int remaining = content_len, total = 0;
    bool ok = true;
    while (remaining > 0) {
        int to_read = remaining < (int)sizeof(chunk) ? remaining : (int)sizeof(chunk);
        int r = httpd_req_recv(req, chunk, to_read);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { ok = false; break; }
        if ((int)fwrite(chunk, 1, r, f) != r) { ok = false; break; }
        total += r;
        remaining -= r;
    }
    fclose(f);

    if (!ok || total != content_len) {
        remove(path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, ok ? "写入不完整" : "接收失败");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Canvas image saved: %s (%d bytes)", name, total);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── POST /canvas_image_delete ──────────────────────────────────────── */

esp_err_t canvas_image_delete_post(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;

    char name[40] = {0};
    if (!http_get_query_param(req, "name", name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少 name 参数");
        return ESP_OK;
    }
    char tmp[64];
    while (req->content_len > 0) {
        int r = httpd_req_recv(req, tmp, sizeof(tmp));
        if (r <= 0) break;
        req->content_len -= (size_t)r;
    }

    esp_err_t err = canvas_board_delete_image(name);
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":%s}", err == ESP_OK ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}


esp_err_t canvas_icon_delete_post(httpd_req_t *req)
{
    if (!http_check_basic_auth(req)) return ESP_OK;

    char name[40] = {0};
    if (!http_get_query_param(req, "name", name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少 name 参数");
        return ESP_OK;
    }

    /* 消耗请求体 */
    char tmp[64];
    while (req->content_len > 0) {
        int r = httpd_req_recv(req, tmp, sizeof(tmp));
        if (r <= 0) break;
        req->content_len -= (size_t)r;
    }

    esp_err_t err = canvas_board_delete_user_icon(name);
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":%s}", err == ESP_OK ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
