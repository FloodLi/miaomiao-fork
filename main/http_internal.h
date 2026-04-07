#pragma once

/**
 * Internal shared declarations for the HTTP subsystem.
 * Used by http_app.c, http_gallery.c, http_config.c, http_features.c, http_system.c.
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_http_server.h"
#include "http_app.h"

extern httpd_handle_t    http_server_handle;
extern http_app_config_t http_app_cfg;
extern const char       *http_images_dir;
extern const int         http_upload_max_bytes;

bool  http_check_basic_auth(httpd_req_t *req);
bool  http_auth_is_enabled(void);
void  http_auth_load_from_nvs(void);
bool  http_require_epd_ready(httpd_req_t *req);
int   http_stat_size_or_neg(const char *path);
bool  http_get_query_param(httpd_req_t *req, const char *key, char *out, size_t out_len);

/* embedded HTML blobs */
extern const uint8_t index_html_start[]     asm("_binary_index_html_start");
extern const uint8_t index_html_end[]       asm("_binary_index_html_end");
extern const uint8_t config_html_start[]    asm("_binary_config_html_start");
extern const uint8_t config_html_end[]      asm("_binary_config_html_end");
extern const uint8_t timetable_html_start[] asm("_binary_timetable_html_start");
extern const uint8_t timetable_html_end[]   asm("_binary_timetable_html_end");
extern const uint8_t todo_html_start[]      asm("_binary_todo_html_start");
extern const uint8_t todo_html_end[]        asm("_binary_todo_html_end");
extern const uint8_t countdown_html_start[] asm("_binary_countdown_html_start");
extern const uint8_t countdown_html_end[]   asm("_binary_countdown_html_end");

/* ── gallery handlers (http_gallery.c) ── */
esp_err_t gallery_upload_post_handler(httpd_req_t *req);
esp_err_t gallery_show_post_handler(httpd_req_t *req);
esp_err_t gallery_delete_image_post_handler(httpd_req_t *req);
esp_err_t gallery_delete_post_handler(httpd_req_t *req);
esp_err_t gallery_images_get_handler(httpd_req_t *req);
esp_err_t gallery_image_get_handler(httpd_req_t *req);

/* ── feature handlers (http_features.c) ── */
esp_err_t feat_timetable_ui_get(httpd_req_t *req);
esp_err_t feat_timetable_json_get(httpd_req_t *req);
esp_err_t feat_timetable_post(httpd_req_t *req);
esp_err_t feat_timetable_show_post(httpd_req_t *req);
esp_err_t feat_todo_ui_get(httpd_req_t *req);
esp_err_t feat_todo_json_get(httpd_req_t *req);
esp_err_t feat_todo_post(httpd_req_t *req);
esp_err_t feat_todo_show_post(httpd_req_t *req);
esp_err_t feat_countdown_ui_get(httpd_req_t *req);
esp_err_t feat_countdown_config_get(httpd_req_t *req);
esp_err_t feat_countdown_config_post(httpd_req_t *req);
esp_err_t feat_countdown_show_post(httpd_req_t *req);
esp_err_t feat_calendar_show_post(httpd_req_t *req);
