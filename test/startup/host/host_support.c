#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "host_support.h"
#include "display_policy.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "ui_theme.h"

bool host_fail_task_create;
bool host_fail_framebuffer;
bool host_fail_canvas_malloc;
bool host_supersede_during_display;
bool host_supersede_before_epd_lock;
const char *host_canvas_layout;
esp_err_t host_export_result;
esp_err_t host_display_result;
unsigned host_display_calls;

static struct { void (*run)(void *); void *arg; } s_tasks[16];
static unsigned s_task_count;
static bool s_running_task;

void host_reset_faults(void)
{
    assert(s_task_count == 0);
    host_fail_task_create = false;
    host_fail_framebuffer = false;
    host_fail_canvas_malloc = false;
    host_supersede_during_display = false;
    host_supersede_before_epd_lock = false;
    host_canvas_layout = "[]";
    host_export_result = ESP_OK;
    host_display_result = ESP_OK;
    host_display_calls = 0;
}

/* Delayed starts deterministically exercise submissions before worker entry. */
BaseType_t xTaskCreate(void (*run)(void *), const char *name, unsigned stack,
                       void *arg, unsigned priority, TaskHandle_t *out)
{
    if (host_fail_task_create)
        return pdFAIL;
    assert(s_task_count < 16);
    s_tasks[s_task_count].run = run;
    s_tasks[s_task_count++].arg = arg;
    return pdPASS;
}

void host_run_tasks(void)
{
    while (s_task_count) {
        void (*run)(void *) = s_tasks[0].run;
        void *arg = s_tasks[0].arg;
        memmove(s_tasks, s_tasks + 1, --s_task_count * sizeof(s_tasks[0]));
        s_running_task = true;
        run(arg);
        s_running_task = false;
    }
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return (TaskHandle_t)(uintptr_t)pthread_self();
}
void vTaskDelete(TaskHandle_t task) { }
void vTaskDelay(TickType_t ticks)
{
    if (!s_running_task)
        host_run_tasks();
}

struct host_semaphore {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    unsigned count;
};
SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    SemaphoreHandle_t sem = calloc(1, sizeof(*sem));
    assert(sem);
    pthread_mutex_init(&sem->lock, NULL);
    pthread_cond_init(&sem->changed, NULL);
    return sem;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    sem->count = 1;
    return sem;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t timeout)
{
    pthread_mutex_lock(&sem->lock);
    while (!sem->count && timeout == portMAX_DELAY)
        pthread_cond_wait(&sem->changed, &sem->lock);
    BaseType_t taken = sem->count ? pdTRUE : pdFALSE;
    if (taken)
        sem->count--;
    pthread_mutex_unlock(&sem->lock);
    return taken;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    pthread_mutex_lock(&sem->lock);
    assert(sem->count == 0);
    sem->count = 1;
    pthread_cond_signal(&sem->changed);
    pthread_mutex_unlock(&sem->lock);
    return pdTRUE;
}
void vSemaphoreDelete(SemaphoreHandle_t sem)
{
    pthread_cond_destroy(&sem->changed);
    pthread_mutex_destroy(&sem->lock);
    free(sem);
}

const char *esp_err_to_name(esp_err_t error) { return error == ESP_OK ? "ESP_OK" : "error"; }
size_t esp_get_free_heap_size(void) { return 1000000; }
size_t heap_caps_get_largest_free_block(unsigned caps) { return 1000000; }
void *heap_caps_malloc(size_t size, unsigned caps) { return malloc(size); }
bool epd_is_ready(void) { return true; }
bool epd_has_yellow(void) { return false; }
epd_panel_t epd_get_panel(void) { return EPD_PANEL_42_BWR; }

static esp_err_t display_result(void)
{
    host_display_calls++;
    if (host_supersede_during_display)
        (void)display_policy_begin_manual_display();
    return host_display_result;
}
esp_err_t epd_display_from_file_mode_epoch(const char *path, epd_refresh_mode_t mode,
                                          unsigned epoch)
{
    if (host_supersede_before_epd_lock)
        (void)display_policy_begin_manual_display();
    if (!display_policy_epoch_is_current(epoch))
        return ESP_ERR_INVALID_STATE;
    return display_result();
}
esp_err_t epd_display_fb_free_mode_epoch(fb_t *fb, epd_refresh_mode_t mode, unsigned epoch)
{
    fb_destroy(fb);
    if (host_supersede_before_epd_lock)
        (void)display_policy_begin_manual_display();
    if (!display_policy_epoch_is_current(epoch))
        return ESP_ERR_INVALID_STATE;
    return display_result();
}
fb_t *fb_create(void)
{
    if (host_fail_framebuffer)
        return NULL;
    fb_t *fb = calloc(1, sizeof(*fb));
    assert(fb);
    fb->width = 400;
    fb->height = 300;
    return fb;
}
void fb_destroy(fb_t *fb) { free(fb); }
void fb_raw_file_lock(void) { }
void fb_raw_file_unlock(void) { }
esp_err_t fb_export(const fb_t *fb, const char *path) { return host_export_result; }
void fb_pixel(fb_t *fb, int x, int y, fb_color_t color) { }
void fb_hline(fb_t *fb, int x, int y, int w, fb_color_t color) { }
void fb_vline(fb_t *fb, int x, int y, int h, fb_color_t color) { }
void fb_rect(fb_t *fb, int x, int y, int w, int h, fb_color_t color) { }
void fb_fill_rect(fb_t *fb, int x, int y, int w, int h, fb_color_t color) { }
void fb_bitmap(fb_t *fb, int x, int y, int w, int h, const uint8_t *data, fb_color_t color) { }
int ui_scale_for(const fb_t *fb) { return 1; }
int ui_text_width_px(const fb_t *fb, const char *s, int px) { return (int)strlen(s) * px / 2; }
int ui_draw_text_px(fb_t *fb, int x, int y, const char *s, fb_color_t color, int px) { return 0; }
void ui_draw_page_frame(fb_t *fb, ui_frame_flags_t flags) { }
void ui_draw_header(fb_t *fb, const char *title, const char *right, bool red) { }
void ui_draw_footer(fb_t *fb, const char *left, const char *right) { }

/* Canvas fixtures live only in temporary streams, never in /spiffs. */
FILE *host_canvas_fopen(const char *path, const char *mode)
{
    if (!host_canvas_layout) {
        errno = ENOENT;
        return NULL;
    }
    FILE *file = tmpfile();
    assert(file);
    fputs(host_canvas_layout, file);
    rewind(file);
    return file;
}
void *host_canvas_malloc(size_t size)
{
    return host_fail_canvas_malloc ? NULL : malloc(size);
}
int __wrap_mkdir(const char *path, mode_t mode) { return 0; }
int __wrap_stat(const char *path, struct stat *st) { errno = ENOENT; return -1; }
int __wrap_access(const char *path, int mode) { errno = ENOENT; return -1; }
int __wrap_rename(const char *old_path, const char *new_path) { assert(0); return -1; }

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle) { return ESP_ERR_NOT_FOUND; }
void nvs_close(nvs_handle_t handle) { }
esp_err_t nvs_commit(nvs_handle_t handle) { return ESP_OK; }
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *v, size_t *n) { return ESP_ERR_NOT_FOUND; }
esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *v) { return ESP_ERR_NOT_FOUND; }
esp_err_t nvs_get_i16(nvs_handle_t h, const char *k, int16_t *v) { return ESP_ERR_NOT_FOUND; }
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v) { return ESP_OK; }
esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v) { return ESP_OK; }
esp_err_t nvs_set_i16(nvs_handle_t h, const char *k, int16_t v) { return ESP_OK; }

void setUp(void) { }
void tearDown(void) { }
void app_main(void);
int main(void) { app_main(); return 0; }
