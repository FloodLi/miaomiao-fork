#include "scheduler.h"

#include <string.h>
#include <dirent.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "image_convert.h"
#include "epd.h"
#include "clock_display.h"
#include "display_policy.h"
#include "display_mode.h"
#include "power_mgr.h"
#include "button.h"
#include "weather.h"
#include "fb_render.h"
#include "time_sync.h"
#include "ui_theme.h"

static const char *TAG        = "scheduler";
static const char *IMAGES_DIR = "/spiffs/images";
static const char *NVS_NS     = "slideshow";

#define BIT_CFG_CHANGED   BIT0
#define BIT_MANUAL_SHOW   BIT1
#define BIT_BOOT_COMPLETE BIT2
#define MAX_GALLERY      32

static slideshow_config_t s_config = {
    .enabled      = false,
    .interval_sec = 3600,
    .mode         = SLIDESHOW_SEQ,
};

static EventGroupHandle_t s_event;
static portMUX_TYPE       s_cfg_mux = portMUX_INITIALIZER_UNLOCKED;
static char  s_current_image[64];
static char  s_last_image[64];
static int   s_current_index;

#define NVS_KEY_INDEX  "cur_idx"
#define NVS_KEY_LAST_IMAGE "last_img"

/* ── NVS helpers ──────────────────────────────────────────────────── */

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    uint8_t v8;
    if (nvs_get_u8(h, "enabled", &v8) == ESP_OK)
        s_config.enabled = (v8 != 0);
    uint32_t v32;
    if (nvs_get_u32(h, "interval", &v32) == ESP_OK)
        s_config.interval_sec = v32;
    if (nvs_get_u8(h, "mode", &v8) == ESP_OK)
        s_config.mode = (slideshow_mode_t)v8;
    if (nvs_get_u8(h, "clk_ovl", &v8) == ESP_OK)
        s_config.clock_overlay = (v8 != 0);

    int32_t idx32;
    if (nvs_get_i32(h, NVS_KEY_INDEX, &idx32) == ESP_OK && idx32 >= 0)
        s_current_index = (int)idx32;
    size_t last_len = sizeof(s_last_image);
    if (nvs_get_str(h, NVS_KEY_LAST_IMAGE, s_last_image, &last_len) != ESP_OK)
        s_last_image[0] = '\0';

    nvs_close(h);
    ESP_LOGI(TAG, "NVS loaded: enabled=%d, interval=%lus, mode=%d",
             s_config.enabled, (unsigned long)s_config.interval_sec, s_config.mode);
}

static void nvs_save(void)
{
    slideshow_config_t config;
    int current_index;
    char last_image[sizeof(s_last_image)];
    portENTER_CRITICAL(&s_cfg_mux);
    config = s_config;
    current_index = s_current_index;
    snprintf(last_image, sizeof(last_image), "%s", s_last_image);
    portEXIT_CRITICAL(&s_cfg_mux);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    nvs_set_u8(h,  "enabled",  config.enabled ? 1 : 0);
    nvs_set_u32(h, "interval", config.interval_sec);
    nvs_set_u8(h,  "mode",     (uint8_t)config.mode);
    nvs_set_u8(h,  "clk_ovl", config.clock_overlay ? 1 : 0);
    nvs_set_i32(h, NVS_KEY_INDEX, (int32_t)current_index);
    nvs_set_str(h, NVS_KEY_LAST_IMAGE, last_image);
    nvs_commit(h);
    nvs_close(h);
}

/* ── gallery listing ──────────────────────────────────────────────── */

static void nvs_save_index(void)
{
    int current_index;
    char last_image[sizeof(s_last_image)];
    portENTER_CRITICAL(&s_cfg_mux);
    current_index = s_current_index;
    snprintf(last_image, sizeof(last_image), "%s", s_last_image);
    portEXIT_CRITICAL(&s_cfg_mux);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, NVS_KEY_INDEX, (int32_t)current_index);
    nvs_set_str(h, NVS_KEY_LAST_IMAGE, last_image);
    nvs_commit(h);
    nvs_close(h);
}

static int list_images(char out[][64], int max)
{
    DIR *dir = opendir(IMAGES_DIR);
    if (!dir) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < max) {
        if (ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN) continue;
        strncpy(out[count], ent->d_name, 63);
        out[count][63] = '\0';
        count++;
    }
    closedir(dir);
    return count;
}

static bool slideshow_image_compatible_for_panel(const char *path, const char *name)
{
    image_panel_compat_t info;
    esp_err_t err = image_file_check_panel_compat(path, &info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Compat check failed for '%s': %s; falling back to converter",
                 name ? name : path, esp_err_to_name(err));
        return true;
    }
    if (info.known && !info.compatible) {
        ESP_LOGW(TAG,
                 "Skip slideshow image '%s': image colors=%d mask=0x%02lx exceed panel colors=%d mask=0x%02lx",
                 name ? name : path, info.image_colors, (unsigned long)info.image_mask,
                 info.panel_colors, (unsigned long)info.panel_mask);
        return false;
    }
    return true;
}

/* ── show next image ──────────────────────────────────────────────── */

static void show_next_image(void)
{
    unsigned observed_epoch = display_policy_display_epoch();
    if (display_policy_manual_screen_active()) {
        ESP_LOGI(TAG, "Slideshow skipped: manual screen active");
        return;
    }
    slideshow_mode_t mode;
    bool clock_ovl;
    portENTER_CRITICAL(&s_cfg_mux);
    mode       = s_config.mode;
    clock_ovl  = s_config.clock_overlay;
    portEXIT_CRITICAL(&s_cfg_mux);

    /* heap-allocate the gallery list to save ~2KB stack */
    char (*names)[64] = calloc(MAX_GALLERY, 64);
    if (!names) {
        ESP_LOGE(TAG, "gallery list alloc failed");
        return;
    }

    int count = list_images(names, MAX_GALLERY);
    if (count == 0) {
        free(names);
        ESP_LOGW(TAG, "Gallery empty, skipping cycle");
        return;
    }

    char path[160];
    char picked[64];
    int idx = -1;
    int start_idx;
    if (mode == SLIDESHOW_RANDOM) {
        start_idx = (int)(esp_random() % (uint32_t)count);
    } else {
        portENTER_CRITICAL(&s_cfg_mux);
        start_idx = s_current_index % count;
        portEXIT_CRITICAL(&s_cfg_mux);
    }
    for (int attempt = 0; attempt < count; attempt++) {
        int cand = (start_idx + attempt) % count;
        strncpy(picked, names[cand], sizeof(picked) - 1);
        picked[sizeof(picked) - 1] = '\0';
        snprintf(path, sizeof(path), "%s/%s", IMAGES_DIR, picked);
        if (slideshow_image_compatible_for_panel(path, picked)) {
            idx = cand;
            break;
        }
    }
    if (idx < 0) {
        free(names);
        ESP_LOGW(TAG, "No gallery image compatible with current panel, skipping cycle");
        return;
    }
    ESP_LOGI(TAG, "Slideshow: '%s' (%d/%d)", picked, idx + 1, count);

    free(names);

    int64_t t0 = esp_timer_get_time();
    display_request_token_t token;
    if (!display_policy_try_begin_background(DISPLAY_MODE_SLIDESHOW, observed_epoch,
                                              EPD_REFRESH_FULL, &token))
        return;
    unsigned epoch = token.epoch;

    const char *raw_path = "/spiffs/image.bin";
    fb_raw_file_lock();
    if (display_policy_display_epoch() != epoch) {
        fb_raw_file_unlock();
        ESP_LOGI(TAG, "Slideshow skipped after lock: manual screen active");
        (void)display_policy_finish_request(&token, ESP_ERR_INVALID_STATE,
                                            scheduler_manual_screen_after_show());
        return;
    }
    ESP_LOGI(TAG, "Slideshow convert begin");
    fb_t *fast_fb = NULL;
    esp_err_t err = image_convert_file_to_epd_fb(path, &fast_fb);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Slideshow fast framebuffer convert selected");
    } else {
        if (err != ESP_ERR_NOT_SUPPORTED)
            ESP_LOGI(TAG, "Slideshow fast framebuffer convert unavailable: %s",
                     esp_err_to_name(err));
        err = image_convert_file_to_epd_raw(path, raw_path);
    }
    if (err != ESP_OK) {
        fb_raw_file_unlock();
        (void)display_policy_finish_request(&token, err,
                                            scheduler_manual_screen_after_show());
        ESP_LOGW(TAG, "Convert failed: %s", esp_err_to_name(err));
        return;
    }
    int64_t t_convert = esp_timer_get_time();
    ESP_LOGI(TAG, "Slideshow convert done in %lld ms, display begin",
             (long long)((t_convert - t0) / 1000LL));

    if (clock_ovl && time_sync_is_synced()) {
        fb_t *fb = fast_fb ? fast_fb : fb_create();
        if (fb) {
            if (!fast_fb)
                fb_import(fb, raw_path);
            struct tm tm;
            if (time_sync_get_local(&tm)) {
                char ts[8];
                snprintf(ts, sizeof(ts), "%02d:%02d", tm.tm_hour, tm.tm_min);
                int W = epd_width();
                int tx = W - 8 * 2 * 5 - 8;
                fb_fill_rect(fb, tx - 4, 2, 8 * 2 * 5 + 12, 22, COLOR_WHITE);
                ui_draw_fixed_text(fb, tx, 4, ts, COLOR_BLACK, 2);
            }
            if (!fast_fb) {
                fb_export(fb, raw_path);
                fb_destroy(fb);
            }
        }
    }

    if (display_policy_display_epoch() != epoch) {
        if (fast_fb)
            fb_destroy(fast_fb);
        fb_raw_file_unlock();
        ESP_LOGI(TAG, "Slideshow display canceled by newer request");
        (void)display_policy_finish_request(&token, ESP_ERR_INVALID_STATE,
                                            scheduler_manual_screen_after_show());
        return;
    }
    if (fast_fb) {
        err = epd_display_fb_free_mode_epoch(fast_fb, EPD_REFRESH_FULL, epoch);
        fast_fb = NULL;
    } else {
        err = epd_display_from_file_mode_epoch(raw_path, EPD_REFRESH_FULL, epoch);
    }
    fb_raw_file_unlock();
    if (err != ESP_OK) {
        (void)display_policy_finish_request(&token, err,
                                            scheduler_manual_screen_after_show());
        ESP_LOGW(TAG, "Display failed: %s", esp_err_to_name(err));
        return;
    }
    if (!display_policy_finish_request(&token, ESP_OK,
                                       scheduler_manual_screen_after_show())) {
        ESP_LOGI(TAG, "Slideshow display completed but was superseded");
        return;
    }
    int64_t t_display = esp_timer_get_time();
    ESP_LOGI(TAG, "Slideshow display done in %lld ms, total %lld ms",
             (long long)((t_display - t_convert) / 1000LL),
             (long long)((t_display - t0) / 1000LL));

    button_set_current_mode(DISPLAY_MODE_SLIDESHOW);

    if (!display_policy_epoch_is_current(epoch))
        return;

    /* 顺序模式：仅在转换+刷屏均成功后再前进索引并落盘，避免失败时跳过画廊项 */
    portENTER_CRITICAL(&s_cfg_mux);
    if (mode != SLIDESHOW_RANDOM)
        s_current_index = (idx + 1) % count;
    strncpy(s_current_image, picked, sizeof(s_current_image) - 1);
    s_current_image[sizeof(s_current_image) - 1] = '\0';
    strncpy(s_last_image, picked, sizeof(s_last_image) - 1);
    s_last_image[sizeof(s_last_image) - 1] = '\0';
    portEXIT_CRITICAL(&s_cfg_mux);
    nvs_save_index();
}

/* ── FreeRTOS task ────────────────────────────────────────────────── */

static void slideshow_task(void *arg)
{
    ESP_LOGI(TAG, "Slideshow task running");

    /* 等主流程完成欢迎屏/切到记忆模式后再判断 manual，避免首帧被跳过后又睡满 interval */
    if (s_event) {
        (void)xEventGroupWaitBits(s_event, BIT_BOOT_COMPLETE,
                                  pdTRUE, pdFALSE, portMAX_DELAY);
    }

    slideshow_config_t boot_snap;
    portENTER_CRITICAL(&s_cfg_mux);
    boot_snap = s_config;
    bool already_showed_image = s_current_image[0] != '\0';
    portEXIT_CRITICAL(&s_cfg_mux);
    if (boot_snap.enabled && !already_showed_image &&
        !display_policy_manual_screen_active()) {
        ESP_LOGI(TAG, "Boot complete, showing first slideshow image now");
        show_next_image();
        already_showed_image = s_current_image[0] != '\0';
    }

    bool shorten_first_interval = !already_showed_image;

    for (;;) {
        slideshow_config_t snap;
        portENTER_CRITICAL(&s_cfg_mux);
        snap = s_config;
        portEXIT_CRITICAL(&s_cfg_mux);

        if (!snap.enabled) {
            xEventGroupWaitBits(s_event, BIT_CFG_CHANGED,
                                pdTRUE, pdFALSE, portMAX_DELAY);
            portENTER_CRITICAL(&s_cfg_mux);
            snap = s_config;
            portEXIT_CRITICAL(&s_cfg_mux);
            if (snap.enabled && !display_policy_manual_screen_active()) {
                ESP_LOGI(TAG, "Slideshow enabled, showing first image now");
                show_next_image();
            }
            continue;
        }

        uint32_t wait_ms = snap.interval_sec * 1000;
        if (shorten_first_interval) {
            shorten_first_interval = false;
            if (wait_ms > 15000)
                wait_ms = 15000;
        }

        EventBits_t bits = xEventGroupWaitBits(
            s_event, BIT_CFG_CHANGED | BIT_MANUAL_SHOW,
            pdTRUE, pdFALSE, pdMS_TO_TICKS(wait_ms));

        if (bits & BIT_CFG_CHANGED) {
            portENTER_CRITICAL(&s_cfg_mux);
            snap = s_config;
            portEXIT_CRITICAL(&s_cfg_mux);
            if (snap.enabled && s_current_image[0] == '\0' &&
                !display_policy_manual_screen_active()) {
                ESP_LOGI(TAG, "Config changed, showing first slideshow image");
                show_next_image();
            }
            continue;
        }
        if (bits & BIT_MANUAL_SHOW) continue;

        if (!snap.enabled) continue;

        /* 全页天气/日历等占屏时暂停切下一张，避免与 HTTPS 刷新同一时段抢网络与刷屏 */
        if (display_policy_manual_screen_active()) {
            (void)xEventGroupWaitBits(s_event, BIT_CFG_CHANGED | BIT_MANUAL_SHOW,
                                    pdTRUE, pdFALSE, pdMS_TO_TICKS(500));
            continue;
        }

        show_next_image();
    }
}

/* ── public API ───────────────────────────────────────────────────── */

esp_err_t scheduler_init(void)
{
    s_event = xEventGroupCreate();
    if (!s_event) return ESP_ERR_NO_MEM;

    s_current_image[0] = '\0';
    s_current_index    = 0;

    nvs_load();

    BaseType_t ok = xTaskCreate(slideshow_task, "slideshow", 10240, NULL, 4, NULL);
    if (ok != pdPASS) {
        vEventGroupDelete(s_event);
        s_event = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Scheduler initialized (enabled=%d)", s_config.enabled);
    return ESP_OK;
}

void scheduler_boot_complete(void)
{
    if (s_event)
        xEventGroupSetBits(s_event, BIT_BOOT_COMPLETE);
}

esp_err_t scheduler_get_config(slideshow_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_cfg_mux);
    *out = s_config;
    portEXIT_CRITICAL(&s_cfg_mux);
    return ESP_OK;
}

esp_err_t scheduler_set_config(const slideshow_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    if (cfg->interval_sec < 60) return ESP_ERR_INVALID_ARG;
    if (cfg->interval_sec > 86400) return ESP_ERR_INVALID_ARG;

    portENTER_CRITICAL(&s_cfg_mux);
    bool slideshow_was_on = s_config.enabled;
    s_config = *cfg;
    bool slideshow_stopped = slideshow_was_on && !s_config.enabled;
    portEXIT_CRITICAL(&s_cfg_mux);
    nvs_save();

    ESP_LOGI(TAG, "Config updated: enabled=%d, interval=%lus, mode=%d",
             s_config.enabled, (unsigned long)s_config.interval_sec, s_config.mode);
    /* Enabling slideshow config no longer changes last_mode; explicit image show does. */
    if (slideshow_stopped && display_mode_active() == DISPLAY_MODE_SLIDESHOW) {
        clock_config_t cc;
        if (clock_display_get_config(&cc) == ESP_OK && cc.enabled &&
            display_mode_is_available(DISPLAY_MODE_CLOCK)) {
            unsigned epoch = 0;
            esp_err_t show_err = display_mode_show_request(DISPLAY_MODE_CLOCK,
                                                           &epoch);
            if (show_err == ESP_OK && display_policy_epoch_is_current(epoch)) {
                button_set_current_mode(DISPLAY_MODE_CLOCK);
                power_mgr_save_mode(DISPLAY_MODE_CLOCK);
            } else {
                ESP_LOGW(TAG, "Clock restore after slideshow stop failed: %s",
                         esp_err_to_name(show_err));
            }
        }
    }
    /* HTTP may call this before scheduler_init() creates the event group (see app_main order). */
    if (s_event)
        xEventGroupSetBits(s_event, BIT_CFG_CHANGED);
    clock_display_notify_home_changed();
    if (slideshow_stopped)
        weather_notify_slideshow_stopped();
    return ESP_OK;
}

void scheduler_set_current_image_name(const char *basename)
{
    if (!basename || !basename[0]) {
        portENTER_CRITICAL(&s_cfg_mux);
        s_current_image[0] = '\0';
        s_last_image[0] = '\0';
        portEXIT_CRITICAL(&s_cfg_mux);
        nvs_save_index();
        return;
    }

    int next_index = -1;
    char (*names)[64] = calloc(MAX_GALLERY, 64);
    if (names) {
        int count = list_images(names, MAX_GALLERY);
        for (int i = 0; i < count; i++) {
            if (strcmp(names[i], basename) == 0) {
                next_index = (i + 1) % count;
                break;
            }
        }
        free(names);
    }

    portENTER_CRITICAL(&s_cfg_mux);
    strncpy(s_current_image, basename, sizeof(s_current_image) - 1);
    s_current_image[sizeof(s_current_image) - 1] = '\0';
    strncpy(s_last_image, basename, sizeof(s_last_image) - 1);
    s_last_image[sizeof(s_last_image) - 1] = '\0';
    if (next_index >= 0) {
        s_current_index = next_index;
    }
    portEXIT_CRITICAL(&s_cfg_mux);

    nvs_save_index();
}

void scheduler_notify_manual_show(void)
{
    /* 轮播未开启时无需置位；避免在无意义路径上频繁 xEventGroupSetBits（SMP 下偶发损坏等待链表） */
    bool en;
    portENTER_CRITICAL(&s_cfg_mux);
    en = s_config.enabled;
    portEXIT_CRITICAL(&s_cfg_mux);
    if (!en)
        return;
    if (!s_event)
        return;
    xEventGroupSetBits(s_event, BIT_MANUAL_SHOW);
}

const char *scheduler_get_current_image(void)
{
    /* The web status endpoint only needs a short-lived best-effort pointer. */
    return s_current_image[0] ? s_current_image : NULL;
}

bool scheduler_manual_screen_after_show(void)
{
    slideshow_config_t cfg;
    return scheduler_get_config(&cfg) != ESP_OK || !cfg.enabled;
}

esp_err_t scheduler_restore_last_image(void)
{
    return scheduler_restore_last_image_epoch(display_mode_render_epoch());
}

esp_err_t scheduler_restore_last_image_epoch(unsigned epoch)
{
    if (!epd_is_ready() || !display_policy_epoch_is_current(epoch))
        return ESP_ERR_INVALID_STATE;

    char (*names)[64] = calloc(MAX_GALLERY, 64);
    if (!names)
        return ESP_ERR_NO_MEM;
    int count = list_images(names, MAX_GALLERY);
    if (count <= 0) {
        free(names);
        return ESP_ERR_NOT_FOUND;
    }

    char saved_last[64];
    int saved_index;
    slideshow_mode_t saved_mode;
    portENTER_CRITICAL(&s_cfg_mux);
    snprintf(saved_last, sizeof(saved_last), "%s", s_last_image);
    saved_index = s_current_index;
    saved_mode = s_config.mode;
    portEXIT_CRITICAL(&s_cfg_mux);

    char picked[64] = {0};
    int picked_index = -1;
    for (int i = 0; i < count; i++) {
        if (saved_last[0] && strcmp(names[i], saved_last) == 0) {
            picked_index = i;
            break;
        }
    }
    if (picked_index < 0) {
        if (saved_last[0]) {
            free(names);
            if (!display_policy_epoch_is_current(epoch))
                return ESP_ERR_INVALID_STATE;
            portENTER_CRITICAL(&s_cfg_mux);
            s_last_image[0] = '\0';
            portEXIT_CRITICAL(&s_cfg_mux);
            nvs_save_index();
            return ESP_ERR_NOT_FOUND;
        }
        if (saved_mode == SLIDESHOW_SEQ) {
            picked_index = (saved_index + count - 1) % count;
        } else {
            for (int i = 0; i < count; i++) {
                char candidate_path[160];
                snprintf(candidate_path, sizeof(candidate_path), "%s/%s",
                         IMAGES_DIR, names[i]);
                if (slideshow_image_compatible_for_panel(candidate_path,
                                                         names[i])) {
                    picked_index = i;
                    break;
                }
            }
            if (picked_index < 0) {
                free(names);
                return ESP_ERR_NOT_SUPPORTED;
            }
        }
        ESP_LOGI(TAG, "Migrating missing last_img from gallery index %d",
                 picked_index);
    }
    snprintf(picked, sizeof(picked), "%s", names[picked_index]);
    free(names);

    char path[160];
    snprintf(path, sizeof(path), "%s/%s", IMAGES_DIR, picked);
    if (!slideshow_image_compatible_for_panel(path, picked)) {
        if (!display_policy_epoch_is_current(epoch))
            return ESP_ERR_INVALID_STATE;
        portENTER_CRITICAL(&s_cfg_mux);
        s_last_image[0] = '\0';
        portEXIT_CRITICAL(&s_cfg_mux);
        nvs_save_index();
        return ESP_ERR_NOT_SUPPORTED;
    }

    fb_raw_file_lock();
    fb_t *fb = NULL;
    esp_err_t err = image_convert_file_to_epd_fb(path, &fb);
    if (err != ESP_OK)
        err = image_convert_file_to_epd_raw(path, "/spiffs/image.bin");
    if (err == ESP_OK && display_policy_epoch_is_current(epoch)) {
        if (fb) {
            err = epd_display_fb_free_mode_epoch(fb, EPD_REFRESH_FULL, epoch);
            fb = NULL;
        } else {
            err = epd_display_from_file_mode_epoch("/spiffs/image.bin", EPD_REFRESH_FULL, epoch);
        }
    } else if (err == ESP_OK) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (fb)
        fb_destroy(fb);
    fb_raw_file_unlock();

    if (err != ESP_OK || !display_policy_epoch_is_current(epoch))
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    if (!display_policy_finish_epoch(epoch, ESP_OK,
                                     scheduler_manual_screen_after_show()))
        return ESP_ERR_INVALID_STATE;

    if (!display_policy_epoch_is_current(epoch))
        return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_cfg_mux);
    snprintf(s_current_image, sizeof(s_current_image), "%s", picked);
    snprintf(s_last_image, sizeof(s_last_image), "%s", picked);
    portEXIT_CRITICAL(&s_cfg_mux);
    nvs_save_index();
    ESP_LOGI(TAG, "Restored last gallery image: %s", picked);
    return ESP_OK;
}

esp_err_t scheduler_show_next_image(void)
{
    if (!epd_is_ready())
        return ESP_ERR_INVALID_STATE;
    unsigned epoch = display_mode_render_epoch();

    slideshow_mode_t mode;
    portENTER_CRITICAL(&s_cfg_mux);
    mode = s_config.mode;
    portEXIT_CRITICAL(&s_cfg_mux);

    char (*names)[64] = calloc(MAX_GALLERY, 64);
    if (!names) return ESP_ERR_NO_MEM;

    int count = list_images(names, MAX_GALLERY);
    if (count == 0) {
        free(names);
        ESP_LOGW(TAG, "Gallery empty");
        return ESP_ERR_NOT_FOUND;
    }

    char path[160];
    char picked[64];
    int idx = -1;
    int start_idx;
    if (mode == SLIDESHOW_RANDOM) {
        start_idx = (int)(esp_random() % (uint32_t)count);
    } else {
        portENTER_CRITICAL(&s_cfg_mux);
        start_idx = s_current_index % count;
        portEXIT_CRITICAL(&s_cfg_mux);
    }
    for (int attempt = 0; attempt < count; attempt++) {
        int cand = (start_idx + attempt) % count;
        strncpy(picked, names[cand], sizeof(picked) - 1);
        picked[sizeof(picked) - 1] = '\0';
        snprintf(path, sizeof(path), "%s/%s", IMAGES_DIR, picked);
        if (slideshow_image_compatible_for_panel(path, picked)) {
            idx = cand;
            break;
        }
    }
    if (idx < 0) {
        free(names);
        ESP_LOGW(TAG, "No gallery image compatible with current panel");
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGI(TAG, "Manual show: '%s' (%d/%d)", picked, idx + 1, count);

    free(names);

    const char *raw_path = "/spiffs/image.bin";
    fb_raw_file_lock();
    fb_t *fast_fb = NULL;
    esp_err_t err = image_convert_file_to_epd_fb(path, &fast_fb);
    if (err != ESP_OK)
        err = image_convert_file_to_epd_raw(path, raw_path);
    if (err == ESP_OK) {
        if (display_policy_display_epoch() != epoch) {
            if (fast_fb)
                fb_destroy(fast_fb);
            fb_raw_file_unlock();
            ESP_LOGI(TAG, "Manual slideshow display canceled by newer request");
            return ESP_ERR_INVALID_STATE;
        }
        if (fast_fb) {
            ESP_LOGI(TAG, "Manual slideshow fast framebuffer display");
            err = epd_display_fb_free_mode_epoch(fast_fb, EPD_REFRESH_FULL, epoch);
            fast_fb = NULL;
        } else {
            err = epd_display_from_file_mode_epoch(raw_path, EPD_REFRESH_FULL, epoch);
        }
    }
    fb_raw_file_unlock();
    if (err != ESP_OK)
        return err;

    if (!display_policy_epoch_is_current(epoch)) {
        ESP_LOGI(TAG, "Manual slideshow display completed but was superseded");
        return ESP_ERR_INVALID_STATE;
    }
    if (!display_policy_finish_epoch(epoch, ESP_OK,
                                     scheduler_manual_screen_after_show()))
        return ESP_ERR_INVALID_STATE;

    if (!display_policy_epoch_is_current(epoch))
        return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_cfg_mux);
    if (mode != SLIDESHOW_RANDOM)
        s_current_index = (idx + 1) % count;
    strncpy(s_current_image, picked, sizeof(s_current_image) - 1);
    s_current_image[sizeof(s_current_image) - 1] = '\0';
    strncpy(s_last_image, picked, sizeof(s_last_image) - 1);
    s_last_image[sizeof(s_last_image) - 1] = '\0';
    portEXIT_CRITICAL(&s_cfg_mux);
    nvs_save_index();

    return ESP_OK;
}
