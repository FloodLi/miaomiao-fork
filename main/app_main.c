#include <string.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "device_identity.h"
#include "wifi_manager.h"
#include "spiffs_mount.h"
#include "http_app.h"
#include "epd.h"
#include "mdns.h"
#include "time_sync.h"
#include "scheduler.h"
#include "weather.h"
#include "clock_display.h"
#include "message_board.h"
#include "canvas_board.h"
#include "calendar_display.h"
#include "timetable.h"
#include "todo.h"
#include "countdown.h"
#include "nvs_utils.h"
#include "display_mode.h"
#include "button.h"
#include "fb_render.h"
#include "display_policy.h"
#include "power_mgr.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"

static const char *TAG = "app";

static void render_welcome_screen(void)
{
    fb_t *fb = fb_create();
    if (!fb) { ESP_LOGE(TAG, "FB alloc failed"); return; }
    fb_clear(fb);

    int W = fb->width;
    int H = fb->height;
    int sc = (W >= 600) ? 2 : 1;
    int ln = 16 * sc;
    int gap = 2 * sc;
    int pad = 6 * sc;
    int y = 0;

    int bar_h = ln + 4 * sc;
    fb_fill_rect(fb, 0, 0, W, bar_h, COLOR_BLACK);
    {
        const char *title = "ESP32 E-Paper Display";
        int tw = (int)strlen(title) * 8 * sc;
        fb_utf8_scaled(fb, (W - tw) / 2, 2 * sc, title, COLOR_WHITE, sc);
    }
    y = bar_h + 2 * sc;

    fb_utf8_scaled(fb, pad, y, "WiFi", COLOR_BLACK, sc);
    y += ln;
    fb_hline(fb, pad, y, W - pad * 2, COLOR_BLACK);
    y += gap;

    bool sta = wifi_manager_sta_connected();
    if (sta) {
        char buf[80];
        snprintf(buf, sizeof(buf), " 已连接: %s", wifi_manager_get_sta_ssid());
        fb_utf8_scaled(fb, pad, y, buf, COLOR_BLACK, sc); y += ln + gap;
        snprintf(buf, sizeof(buf), " IP: %s", wifi_manager_get_sta_ip());
        fb_utf8_scaled(fb, pad, y, buf, COLOR_BLACK, sc); y += ln + gap;
    } else {
        fb_utf8_scaled(fb, pad, y, " 未连接路由器", COLOR_BLACK, sc);
        y += ln + gap;
    }
    {
        char line[80];
        snprintf(line, sizeof(line), " 热点: %s", device_identity_get_ap_ssid());
        fb_utf8_scaled(fb, pad, y, line, COLOR_BLACK, sc);
        y += ln + gap;
    }
    fb_utf8_scaled(fb, pad, y, " 密码: 12345678", COLOR_BLACK, sc); y += ln + gap;
    fb_utf8_scaled(fb, pad, y, " AP: 192.168.4.1", COLOR_BLACK, sc); y += ln + gap * 2;

    fb_utf8_scaled(fb, pad, y, "使用方法", COLOR_BLACK, sc);
    y += ln;
    fb_hline(fb, pad, y, W - pad * 2, COLOR_BLACK);
    y += gap;

    {
        char line[88];
        snprintf(line, sizeof(line), " 1.连接热点 %s", device_identity_get_ap_ssid());
        fb_utf8_scaled(fb, pad, y, line, COLOR_BLACK, sc);
    }
    y += ln + gap;
    fb_utf8_scaled(fb, pad, y, " 2.浏览器打开 192.168.4.1", COLOR_BLACK, sc);
    y += ln + gap;
    fb_utf8_scaled(fb, pad, y, " 3.配置页面连接家庭WiFi", COLOR_BLACK, sc);
    y += ln + gap;
    {
        char line[96];
        snprintf(line, sizeof(line), " 4.连接后用 %s.local 访问",
                 device_identity_get_mdns_hostname());
        fb_utf8_scaled(fb, pad, y, line, COLOR_BLACK, sc);
        y += ln + gap;
    }

    {
        const esp_app_desc_t *app = esp_app_get_description();
        char info[80];
        snprintf(info, sizeof(info), "v%s | %dx%d | %s",
                 app->version, W, H,
                 epd_has_red() ? "BW+Red" : "BW");
        int iw = (int)strlen(info) * 8 * sc;
        int bot_h = ln + 2 * sc;
        fb_fill_rect(fb, 0, H - bot_h, W, bot_h, COLOR_BLACK);
        fb_utf8_scaled(fb, (W - iw) / 2, H - bot_h + sc, info, COLOR_WHITE, sc);
    }

    epd_display_fb_free(fb);
    ESP_LOGI(TAG, "Welcome screen displayed");
}

static void boot_display_route(void)
{
    ESP_LOGI(TAG, "boot_display_route: starting");
    display_policy_set_manual_screen_active(true);

    ESP_LOGI(TAG, "boot_display_route: showing welcome screen");
    render_welcome_screen();
    ESP_LOGI(TAG, "boot_display_route: welcome screen done, delaying 10s");
    vTaskDelay(pdMS_TO_TICKS(10000));

    int mode = power_mgr_load_mode();
    int n = display_mode_count();
    if (mode < 0 || mode >= n) mode = 0;
    ESP_LOGI(TAG, "boot_display_route: switching to mode %d (%s)",
             mode, display_mode_name(mode));
    display_mode_show(mode);
    button_set_current_mode(mode);
    power_mgr_save_mode(mode);

    ESP_LOGI(TAG, "boot_display_route: done, mode %d active", mode);
}

static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(device_identity_get_mdns_hostname());
    mdns_instance_name_set(device_identity_get_mdns_instance());
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: http://%s.local/", device_identity_get_mdns_hostname());
}

/* ── Quick-refresh path: minimal init, refresh content, back to sleep ── */

static void quick_refresh_and_sleep(void)
{
    ESP_LOGI(TAG, "Quick-refresh path: timer wake-up");

    ESP_ERROR_CHECK(spiffs_mount_init());
    display_policy_init();
    device_identity_init();

    /* STA-only WiFi (no AP, no mDNS, no HTTP) */
    ESP_ERROR_CHECK(wifi_manager_init(device_identity_get_ap_ssid(), "12345678"));

    if (wifi_manager_sta_connected()) {
        time_sync_init();
        /* Wait up to 8s for SNTP sync */
        for (int i = 0; i < 80; i++) {
            struct tm tm;
            if (time_sync_get_local(&tm)) break;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    ESP_ERROR_CHECK(epd_init());

    /* Init content modules (they load NVS config) */
    ESP_ERROR_CHECK(scheduler_init());
    scheduler_boot_complete();
    ESP_ERROR_CHECK(weather_init());
    ESP_ERROR_CHECK(clock_display_init());
    ESP_ERROR_CHECK(timetable_init());
    ESP_ERROR_CHECK(todo_init());
    ESP_ERROR_CHECK(countdown_init());

    display_mode_register(&(display_mode_entry_t){ "clock",     "时钟",     clock_display_show });
    display_mode_register(&(display_mode_entry_t){ "calendar",  "日历",     calendar_display_show_current });
    display_mode_register(&(display_mode_entry_t){ "timetable", "课程表",   timetable_show });
    display_mode_register(&(display_mode_entry_t){ "weather",   "天气",     weather_display_cached });
    display_mode_register(&(display_mode_entry_t){ "slideshow", "轮播图",   scheduler_show_next_image });
    display_mode_register(&(display_mode_entry_t){ "todo",      "待办事项", todo_show });
    display_mode_register(&(display_mode_entry_t){ "countdown", "倒计时",   countdown_show });

    display_policy_set_manual_screen_active(true);

    int mode = power_mgr_load_mode();
    ESP_LOGI(TAG, "Quick-refresh: restoring mode %d (%s)",
             mode, display_mode_name(mode));

    esp_err_t err = display_mode_show(mode);
    int      ok_mode = mode;

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Quick-refresh: mode %d failed, trying clock", mode);
        err    = display_mode_show(0);
        ok_mode = 0;
    }

    if (err == ESP_OK) {
        power_mgr_save_mode(ok_mode);
        ESP_LOGI(TAG, "Quick-refresh: content displayed");
        /* 月历在独立任务里刷屏；若立即深睡会在刷新完成前掉电，屏不更新或异常 */
        if (strcmp(display_mode_name(ok_mode), "calendar") == 0)
            calendar_display_wait_render_idle();
    } else {
        ESP_LOGW(TAG, "Quick-refresh: clock fallback also failed");
    }

    ESP_LOGI(TAG, "Quick-refresh done, going back to sleep");
    power_mgr_enter_sleep();
}

/* ── Full boot path (normal operation) ─────────────────────────────── */

static void full_boot(void)
{
    ESP_ERROR_CHECK(spiffs_mount_init());
    display_policy_init();
    device_identity_init();

    ESP_ERROR_CHECK(wifi_manager_init(device_identity_get_ap_ssid(), "12345678"));
    wifi_manager_set_sta_hostname(device_identity_get_mdns_hostname());
    start_mdns();

    if (wifi_manager_sta_connected()) {
        ESP_LOGI(TAG, "Connected to \"%s\", STA IP: %s",
                 wifi_manager_get_sta_ssid(), wifi_manager_get_sta_ip());
        ESP_LOGI(TAG, "Access http://%s.local/ or http://%s/",
                 device_identity_get_mdns_hostname(), wifi_manager_get_sta_ip());
        time_sync_init();
    } else {
        ESP_LOGI(TAG, "AP-only: connect to SSID \"%s\" (password 12345678)",
                 wifi_manager_get_ap_ssid());
    }

    ESP_ERROR_CHECK(epd_init());

    display_policy_set_manual_screen_active(true);

    ESP_ERROR_CHECK(scheduler_init());
    ESP_ERROR_CHECK(weather_init());
    ESP_ERROR_CHECK(clock_display_init());
    ESP_ERROR_CHECK(message_board_init());
    ESP_ERROR_CHECK(canvas_board_init());
    ESP_ERROR_CHECK(calendar_display_init());
    ESP_ERROR_CHECK(timetable_init());
    ESP_ERROR_CHECK(todo_init());
    ESP_ERROR_CHECK(countdown_init());

    display_mode_register(&(display_mode_entry_t){ "clock",     "时钟",     clock_display_show });
    display_mode_register(&(display_mode_entry_t){ "calendar",  "日历",     calendar_display_show_current });
    display_mode_register(&(display_mode_entry_t){ "timetable", "课程表",   timetable_show });
    display_mode_register(&(display_mode_entry_t){ "weather",   "天气",     weather_display_cached });
    display_mode_register(&(display_mode_entry_t){ "slideshow", "轮播图",   scheduler_show_next_image });
    display_mode_register(&(display_mode_entry_t){ "todo",      "待办事项", todo_show });
    display_mode_register(&(display_mode_entry_t){ "countdown", "倒计时",   countdown_show });

    http_app_config_t cfg = {
        .mount_path = "/spiffs",
        .upload_path = "/spiffs/upload.jpg",
    };
    ESP_ERROR_CHECK(http_app_start(&cfg));

    boot_display_route();
    scheduler_boot_complete();

    ESP_ERROR_CHECK(button_init());

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        if (running->subtype != ESP_PARTITION_SUBTYPE_APP_FACTORY) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "OTA firmware validated (rollback cancelled)");
        }
        ESP_LOGI(TAG, "Running from partition '%s' @ 0x%lx",
                 running->label, (unsigned long)running->address);
    }
    ESP_LOGI(TAG, "Ready. http://%s.local/ or http://192.168.4.1/",
             device_identity_get_mdns_hostname());

    power_mgr_arm();
}

/* ── Entry point ───────────────────────────────────────────────────── */

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }
    ESP_ERROR_CHECK(nvs_utils_init());

    ESP_ERROR_CHECK(power_mgr_init());

    if (power_mgr_is_timer_wake()) {
        quick_refresh_and_sleep();
    } else {
        full_boot();
    }
}
