#pragma once
#include <errno.h>
#include <stdatomic.h>
#include <stdarg.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_SIZE 3
#define ESP_ERR_NO_MEM 4
#define ESP_ERR_NOT_SUPPORTED 5
#define ESP_ERR_INVALID_STATE 6
#define ESP_ERR_NOT_FOUND 7
#define HTTPD_404_NOT_FOUND 404
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_413_CONTENT_TOO_LARGE 413
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
#define HTTPD_RESP_USE_STRLEN (-1)
#define DISPLAY_MODE_SLIDESHOW 1
#define DISPLAY_MODE_CLOCK 0
#define EPD_REFRESH_FULL 1
#define OTA_WITH_SEQUENTIAL_WRITES 0
#define CANVAS_NAME_MAX 16
#define CANVAS_IMAGE_MAX_BYTES 100000
#define CANVAS_LAYOUT_MAX_BYTES 12000
#define CANVAS_IMAGES_DIR "/spiffs/cimg"
#define pdMS_TO_TICKS(value) (value)
#define pdTRUE 1

static const char *TAG = "host";
static void host_log(const char *tag, const char *format, ...) {}
#define ESP_LOGI(...) host_log(__VA_ARGS__)
#define ESP_LOGW(...) host_log(__VA_ARGS__)
#define ESP_LOGE(...) host_log(__VA_ARGS__)
static const char *esp_err_to_name(esp_err_t err) { return "fake error"; }

static bool http_auth_is_enabled(void) { return true; }
static bool http_check_basic_auth(httpd_req_t *request) { return true; }
static bool http_require_epd_ready(httpd_req_t *request) { return true; }
static bool http_require_mode_available(httpd_req_t *request, int mode) { return true; }
static int httpd_resp_set_type(httpd_req_t *request, const char *type) { return ESP_OK; }
static int response_status;
static char response_body[512];
static int httpd_resp_set_status(httpd_req_t *request, const char *status) { response_status=atoi(status);return ESP_OK; }
static int httpd_resp_sendstr(httpd_req_t *request, const char *body) { snprintf(response_body,sizeof(response_body),"%s",body);return ESP_OK; }
static int httpd_resp_send(httpd_req_t *request, const char *body, int size) { return httpd_resp_sendstr(request,body); }
static int httpd_resp_send_err(httpd_req_t *request, int status, const char *body) { response_status=status;return httpd_resp_sendstr(request,body); }
static const char *query_text;
static size_t httpd_req_get_url_query_len(httpd_req_t *req) { return query_text ? strlen(query_text) : 0; }
static esp_err_t httpd_req_get_url_query_str(httpd_req_t *req, char *out, size_t size)
{ if (!query_text || strlen(query_text)>=size) return ESP_ERR_INVALID_SIZE;strcpy(out,query_text);return ESP_OK; }
static esp_err_t httpd_query_key_value(const char *query, const char *key, char *out, size_t size)
{
    size_t key_len=strlen(key);
    for(const char *p=query;*p;) {
        size_t n=strcspn(p,"&");
        if(n>key_len && !strncmp(p,key,key_len) && p[key_len]=='=') {
            size_t len=n-key_len-1;if(len>=size)return ESP_ERR_INVALID_SIZE;
            memcpy(out,p+key_len+1,len);out[len]=0;return ESP_OK;
        }
        p+=n;if(*p)p++;
    }
    return ESP_ERR_NOT_FOUND;
}
typedef enum {DISPLAY_RESULT_PENDING,DISPLAY_RESULT_OK,DISPLAY_RESULT_CANCELED,DISPLAY_RESULT_ERROR} display_result_state_t;
typedef struct {display_result_state_t state;esp_err_t error;} display_result_t;
static display_result_t cached_result;
static unsigned queried_epoch;
static display_result_t display_policy_get_result(unsigned epoch) { queried_epoch=epoch;return cached_result; }
static const char *query_name;
static bool http_get_query_param(httpd_req_t *request, const char *key, char *out, size_t size)
{
    assert(query_name && snprintf(out, size, "%s", query_name) < (int)size);
    return true;
}
static bool canvas_validate_name(httpd_req_t *request, const char *name) { return true; }
static bool is_safe_filename(const char *name) { return true; }

static int live_allocations;
static bool fail_allocation;
static void *host_malloc(size_t size)
{
    if (fail_allocation) return NULL;
    void *ptr = malloc(size);
    assert(ptr);
    live_allocations++;
    return ptr;
}
static void host_free(void *ptr)
{
    if (ptr) { assert(live_allocations > 0); live_allocations--; }
    free(ptr);
}

typedef int esp_ota_handle_t;
typedef struct { size_t size; const char *label; unsigned address; } esp_partition_t;
static const esp_partition_t partition = { 1000000, "ota_1", 0x10000 };
static int ota_begins, ota_aborts, ota_writes, ota_ends, ota_boots, reboots;
static bool ota_active, ota_write_failure, ota_end_failure;
static const esp_partition_t *esp_ota_get_next_update_partition(const void *unused) { return &partition; }
static esp_err_t esp_ota_begin(const esp_partition_t *part, int mode, esp_ota_handle_t *handle)
{
    assert(part == &partition && !ota_active);
    ota_active = true;
    ota_begins++;
    *handle = 42;
    return ESP_OK;
}
static esp_err_t esp_ota_abort(esp_ota_handle_t handle)
{
    assert(handle == 42 && ota_active);
    ota_active = false;
    ota_aborts++;
    return ESP_OK;
}
static esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t size)
{
    assert(handle == 42 && ota_active && size > 0);
    ota_writes++;
    return ota_write_failure ? ESP_FAIL : ESP_OK;
}
static esp_err_t esp_ota_end(esp_ota_handle_t handle)
{
    assert(handle == 42 && ota_active);
    ota_active = false;
    ota_ends++;
    return ota_end_failure ? ESP_FAIL : ESP_OK;
}
static esp_err_t esp_ota_set_boot_partition(const esp_partition_t *part)
{
    assert(part == &partition && !ota_active && ota_ends == 1 && !ota_end_failure);
    ota_boots++;
    return ESP_OK;
}
static void reboot_task(void *arg) { assert(!"reboot must only be scheduled"); }
static int xTaskCreate(void (*task)(void *), const char *name, int stack,
                       void *arg, int priority, void *handle)
{
    assert(task == reboot_task && ota_boots == 1);
    reboots++;
    return pdTRUE;
}

static atomic_bool s_upload_active;
static atomic_uint s_upload_epoch;
static const int http_upload_max_bytes = 2000000;
static const char *http_images_dir = "/spiffs/images";
static struct { const char *mount_path; const char *upload_path; } http_app_cfg = {
    "/spiffs", "/spiffs/upload.jpg"
};
typedef struct { unsigned epoch; } display_request_token_t;
static int display_begins, display_finishes, raw_locks, layout_takes, layout_gives;
static bool display_active, raw_locked, layout_locked;
static bool display_superseded, epd_ready;
static bool supersede_on_raw_lock;
static unsigned clear_calls;
static esp_err_t clear_error;
static esp_err_t display_result;
static display_request_token_t display_policy_begin_request(int mode, int refresh)
{
    assert(!display_active);
    display_active = true;
    display_begins++;
    return (display_request_token_t){ 17 };
}
static bool display_policy_finish_request(display_request_token_t *token, esp_err_t result, bool manual)
{
    assert(token->epoch == 17 && display_active);
    display_active = false;
    display_finishes++;
    display_result = result;
    return !display_superseded;
}
static bool display_policy_epoch_is_current(unsigned epoch) { return !display_superseded; }
static bool epd_is_ready(void) { return epd_ready; }
static esp_err_t epd_clear_screen_epoch(unsigned epoch) { assert(epoch==17);clear_calls++;return clear_error; }
static void clock_display_notify_home_changed(void) {}
static void fb_raw_file_lock(void) { assert(!raw_locked);raw_locked=true;raw_locks++;if(supersede_on_raw_lock){assert(display_active);display_superseded=true;} }
static void fb_raw_file_unlock(void) { assert(raw_locked); raw_locked = false; }
static int conversion_calls;
static bool compatibility_failure, conversion_failure;
typedef struct { int unused; } image_panel_compat_t;
static bool gallery_image_compatible_for_panel(const char *path, const char *name, image_panel_compat_t *out)
{
    assert(strstr(path, ".tmp"));
    expect_file("/spiffs/images/same.jpg", "valid gallery");
    return !compatibility_failure;
}
static esp_err_t gallery_send_incompatible_response(httpd_req_t *request, image_panel_compat_t *compat)
{
    return ESP_OK;
}
static esp_err_t image_convert_file_to_epd_raw(const char *source, const char *destination)
{
    assert(raw_locked);
    expect_file("/spiffs/images/same.jpg", "valid gallery");
    conversion_calls++;
    return conversion_failure ? ESP_FAIL : ESP_OK;
}
static esp_err_t gallery_prepare_image_refresh(const char *reason, unsigned epoch) { return ESP_OK; }
static esp_err_t gallery_display_raw_like_canvas(const char *path, unsigned epoch)
{
    assert(epoch == 17);
    return ESP_OK;
}
static unsigned esp_random(void) { return 12; }
static unsigned esp_get_free_heap_size(void) { return 100000; }
static void scheduler_set_current_image_name(const char *name) {}
static void weather_disable_if_enabled_for_image_focus(void) {}
static void scheduler_notify_manual_show(void) {}
static void button_set_current_mode(int mode) {}
static void power_mgr_save_mode(int mode) {}

typedef int SemaphoreHandle_t;
static SemaphoreHandle_t layout_lock(void) { return 1; }
static int xSemaphoreTake(SemaphoreHandle_t lock, int timeout)
{
    assert(lock == 1 && !layout_locked);
    layout_locked = true;
    layout_takes++;
    return pdTRUE;
}
static int xSemaphoreGive(SemaphoreHandle_t lock)
{
    assert(lock == 1 && layout_locked);
    layout_locked = false;
    layout_gives++;
    return pdTRUE;
}
static esp_err_t canvas_board_commit_layout_from_file(const char *path)
{
    assert(!"an interrupted layout must never commit");
    return ESP_FAIL;
}
