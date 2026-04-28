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
#include "battery_mon.h"
#include "button.h"
#include "fb_render.h"
#include "display_policy.h"
#include "power_mgr.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"

static const char *TAG = "app";

static int normalize_saved_mode(int saved_mode, int mode_count, bool *write_back)
{
    if (write_back)
        *write_back = false;

    if (saved_mode < 0 || saved_mode >= mode_count) {
        if (write_back)
            *write_back = true;
        return DISPLAY_MODE_CLOCK;
    }

    if (saved_mode == DISPLAY_MODE_SLIDESHOW && !power_mgr_saved_mode_has_marker()) {
        ESP_LOGW(TAG, "Saved slideshow mode has no v2 marker; treating it as legacy stale state");
        if (write_back)
            *write_back = true;
        return DISPLAY_MODE_CLOCK;
    }

    return saved_mode;
}

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
        /* 标题栏右侧电量（紧凑），避免与中标题重叠 */
        int pct_w = 5 * 8 * sc;
        battery_mon_draw_percent_compact(fb, W - pct_w - pad, 2 * sc, COLOR_WHITE, sc);
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
    {
        char line[80];
        snprintf(line, sizeof(line), " 密码: %s", device_identity_get_ap_password());
        fb_utf8_scaled(fb, pad, y, line, COLOR_BLACK, sc);
        y += ln + gap;
    }
    fb_utf8_scaled(fb, pad, y, " AP: 192.168.4.1", COLOR_BLACK, sc); y += ln + gap * 2;

    battery_mon_draw_on_fb(fb, pad, y, COLOR_BLACK, sc);
    y += ln + gap;

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

static esp_err_t render_spiffs_recovery_screen(esp_err_t spiffs_err)
{
    fb_t *fb = fb_create();
    if (!fb) {
        ESP_LOGE(TAG, "SPIFFS recovery FB alloc failed");
        return ESP_ERR_NO_MEM;
    }
    fb_clear(fb);

    int W = fb->width;
    int H = fb->height;
    int sc = (W >= 600) ? 2 : 1;
    int ln = 16 * sc;
    int gap = 3 * sc;
    int pad = 6 * sc;
    int y = 0;

    int bar_h = ln + 4 * sc;
    fb_fill_rect(fb, 0, 0, W, bar_h, COLOR_BLACK);
    {
        const char *title = "文件系统恢复";
        int tw = 6 * 16 * sc;
        fb_utf8_scaled(fb, (W - tw) / 2, 2 * sc, title, COLOR_WHITE, sc);
    }
    y = bar_h + 6 * sc;

    fb_utf8_scaled(fb, pad, y, "文件系统挂载失败", COLOR_BLACK, sc);
    y += ln + gap;
    fb_utf8_scaled(fb, pad, y, "用户数据未被格式化", COLOR_BLACK, sc);
    y += ln + gap * 2;

    {
        char line[96];
        snprintf(line, sizeof(line), "热点: %s", device_identity_get_ap_ssid());
        fb_utf8_scaled(fb, pad, y, line, COLOR_BLACK, sc);
        y += ln + gap;
        snprintf(line, sizeof(line), "密码: %s", device_identity_get_ap_password());
        fb_utf8_scaled(fb, pad, y, line, COLOR_BLACK, sc);
        y += ln + gap;
    }

    fb_utf8_scaled(fb, pad, y, "打开网页: 192.168.4.1/config", COLOR_BLACK, sc);
    y += ln + gap * 2;
    fb_utf8_scaled(fb, pad, y, "1.先点重试文件系统", COLOR_BLACK, sc);
    y += ln + gap;
    fb_utf8_scaled(fb, pad, y, "2.确认不要图片后再格式化", COLOR_BLACK, sc);
    y += ln + gap;
    fb_utf8_scaled(fb, pad, y, "确认词: FORMAT_SPIFFS", COLOR_BLACK, sc);

    {
        char info[80];
        snprintf(info, sizeof(info), "错误:%s | 密码12345678",
                 esp_err_to_name(spiffs_err));
        int bot_h = ln + 2 * sc;
        fb_fill_rect(fb, 0, H - bot_h, W, bot_h, COLOR_BLACK);
        fb_utf8_scaled_maxw(fb, pad, H - bot_h + sc, info, COLOR_WHITE, sc,
                            W - pad * 2);
    }

    return epd_display_fb_free(fb);
}

static void try_show_spiffs_recovery_screen(esp_err_t spiffs_err)
{
    ESP_LOGW(TAG, "SPIFFS diagnostic: trying to show recovery screen");

    fb_reserve_planes_early();
    esp_err_t epd_err = epd_init();
    if (epd_err != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS diagnostic display skipped: EPD init failed (%s)",
                 esp_err_to_name(epd_err));
        fb_release_reserved_planes();
        return;
    }

    (void)display_policy_begin_manual_display();
    esp_err_t disp_err = render_spiffs_recovery_screen(spiffs_err);
    if (disp_err == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS recovery screen displayed");
    } else {
        ESP_LOGW(TAG, "SPIFFS recovery screen failed: %s",
                 esp_err_to_name(disp_err));
    }
    fb_release_reserved_planes();
}

static void boot_display_route(void)
{
    ESP_LOGI(TAG, "boot_display_route: starting");
    display_policy_set_manual_screen_active(true);
    unsigned boot_epoch = display_policy_display_epoch();

    ESP_LOGI(TAG, "boot_display_route: showing welcome screen");
    render_welcome_screen();
    ESP_LOGI(TAG, "boot_display_route: welcome screen done, delaying 10s");
    vTaskDelay(pdMS_TO_TICKS(10000));

    if (display_policy_display_epoch() != boot_epoch) {
        ESP_LOGI(TAG, "boot_display_route: canceled by newer display request");
        return;
    }

    int saved_mode = power_mgr_load_mode();
    int n = display_mode_count();
    bool write_mode_back = false;
    int mode = normalize_saved_mode(saved_mode, n, &write_mode_back);
    ESP_LOGI(TAG, "boot_display_route: switching to mode %d (%s)",
             mode, display_mode_name(mode));
    esp_err_t err = display_mode_show(mode);
    if (err == ESP_OK && mode == DISPLAY_MODE_CALENDAR)
        calendar_display_wait_render_idle();
    if (display_policy_display_epoch() != boot_epoch) {
        ESP_LOGI(TAG, "boot_display_route: mode restore canceled by newer display request");
        return;
    }
    /* 数据源不可用时（典型：轮播图但 SPIFFS 无图）回落到时钟，
     * 保留 NVS 中用户原偏好，等数据补齐后下次启动自动恢复。 */
    if (err == ESP_ERR_NOT_FOUND && mode != 0) {
        ESP_LOGW(TAG, "boot_display_route: mode %d unavailable (%s), falling back to clock",
                 mode, esp_err_to_name(err));
        mode = 0;
        display_mode_show(mode);
    }
    button_set_current_mode(mode);
    /* 仅当原 NVS 值非法被纠正时才回写，避免把临时回落覆盖掉用户偏好 */
    if (write_mode_back) {
        power_mgr_save_mode(mode);
    }

    ESP_LOGI(TAG, "boot_display_route: done, mode %d active%s",
             mode, (mode != saved_mode) ? " (fallback)" : "");
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
    int mode = power_mgr_load_mode();

    esp_err_t spiffs_err = spiffs_mount_init();
    if (spiffs_err != ESP_OK) {
        ESP_LOGE(TAG, "Quick-refresh: SPIFFS unavailable (%s), sleeping instead of rebooting",
                 esp_err_to_name(spiffs_err));
        power_mgr_enter_sleep();
        return;
    }
    display_policy_init();
    display_policy_set_quick_refresh_active(true);
    device_identity_init();

    /* STA-only WiFi (no AP, no mDNS, no HTTP) */
    ESP_ERROR_CHECK(wifi_manager_init(device_identity_get_ap_ssid(),
                                      device_identity_get_ap_password()));

    if (wifi_manager_sta_connected()) {
        time_sync_init();
        /* Wait up to 8s for SNTP sync */
        for (int i = 0; i < 80; i++) {
            struct tm tm;
            if (time_sync_get_local(&tm)) break;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    epd_load_panel_from_nvs();
    fb_reserve_planes_early();
    esp_err_t epd_err = epd_init();
    if (epd_err != ESP_OK) {
        ESP_LOGE(TAG, "Quick-refresh: EPD init failed (%s), sleeping instead of rebooting",
                 esp_err_to_name(epd_err));
        power_mgr_enter_sleep();
        return;
    }

    /* Init content modules (they load NVS config) */
    ESP_ERROR_CHECK(scheduler_init());
    /* NOTE: do NOT call scheduler_boot_complete() here.
     * The slideshow_task must stay blocked — we only need
     * scheduler_show_next_image() which works without it. */
    if (mode == DISPLAY_MODE_WEATHER)
        weather_skip_initial_task_fetch_once();
    ESP_ERROR_CHECK(weather_init());
    ESP_ERROR_CHECK(clock_display_init());
    /* 与 full_boot 一致：日历依赖 s_mutex，未 init 则 calendar_display_show 直接失败并触发回退到时钟 */
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

    display_policy_set_manual_screen_active(true);

    int saved_mode = mode;
    int n = display_mode_count();
    bool write_mode_back = false;
    mode = normalize_saved_mode(saved_mode, n, &write_mode_back);
    ESP_LOGI(TAG, "Quick-refresh: restoring mode %d (%s)",
             mode, display_mode_name(mode));

    esp_err_t err;
    if (mode == DISPLAY_MODE_WEATHER) {
        /* Deep sleep clears the in-RAM weather cache; timer wake must fetch
         * before rendering the weather page instead of using cached data. */
        err = weather_fetch_and_display(true);
    } else {
        err = display_mode_show(mode);
    }
    bool used_fallback = false;

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Quick-refresh: mode %d failed, trying clock", mode);
        err = display_mode_show(0);
        used_fallback = true;
    }

    if (err == ESP_OK) {
        if (write_mode_back)
            power_mgr_save_mode(mode);
        if (!used_fallback) {
            /* Original mode succeeded — no need to update NVS */
            ESP_LOGI(TAG, "Quick-refresh: mode %d displayed", mode);
        } else {
            /* Fell back to clock for *this* wake-up only.
             * Keep the user's preferred mode in NVS so next wake-up
             * retries it (e.g. SNTP may succeed by then). */
            ESP_LOGW(TAG, "Quick-refresh: showing clock as fallback, "
                     "keeping saved mode %d for next wake", mode);
        }
        if (strcmp(display_mode_name(used_fallback ? 0 : mode), "calendar") == 0)
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
    esp_err_t spiffs_err = spiffs_mount_init();
    const bool spiffs_ok = (spiffs_err == ESP_OK);
    if (!spiffs_ok) {
        ESP_LOGE(TAG, "SPIFFS unavailable (%s); booting AP/Web diagnostics without formatting",
                 esp_err_to_name(spiffs_err));
    }
    display_policy_init();
    device_identity_init();

    ESP_ERROR_CHECK(wifi_manager_init(device_identity_get_ap_ssid(),
                                      device_identity_get_ap_password()));
    wifi_manager_set_sta_hostname(device_identity_get_mdns_hostname());
    start_mdns();

    if (wifi_manager_sta_connected()) {
        ESP_LOGI(TAG, "Connected to \"%s\", STA IP: %s",
                 wifi_manager_get_sta_ssid(), wifi_manager_get_sta_ip());
        ESP_LOGI(TAG, "Access http://%s.local/ or http://%s/",
                 device_identity_get_mdns_hostname(), wifi_manager_get_sta_ip());
        time_sync_init();
    } else {
        ESP_LOGI(TAG, "AP-only: connect to SSID \"%s\" (default AP password)",
                 wifi_manager_get_ap_ssid());
    }

    /* 先读 NVS 再起 HTTP：换屏后 NVS 未改时 epd_init 可能长时间等 BUSY，httpd 仍可响应 */
    epd_load_panel_from_nvs();
    ESP_LOGI(TAG, "NVS panel preset: %d (%dx%d) — open /config.html if hardware differs",
             (int)epd_get_panel(), epd_width(), epd_height());
    /* Reserve display planes only when the filesystem is usable; diagnostic
     * mode keeps RAM available for HTTP and recovery actions. */
    if (spiffs_ok) {
        fb_reserve_planes_early();
    }

    http_app_config_t cfg = {
        .mount_path = "/spiffs",
        .upload_path = "/spiffs/upload.jpg",
    };
    ESP_ERROR_CHECK(http_app_start(&cfg));

    if (!spiffs_ok) {
        display_policy_set_manual_screen_active(true);
        try_show_spiffs_recovery_screen(spiffs_err);
        if (scheduler_init() != ESP_OK) ESP_LOGW(TAG, "Diagnostic init: scheduler unavailable");
        if (weather_init() != ESP_OK) ESP_LOGW(TAG, "Diagnostic init: weather unavailable");
        if (clock_display_init() != ESP_OK) ESP_LOGW(TAG, "Diagnostic init: clock unavailable");
        if (message_board_init() != ESP_OK) ESP_LOGW(TAG, "Diagnostic init: message board unavailable");
        if (canvas_board_init() != ESP_OK) ESP_LOGW(TAG, "Diagnostic init: canvas unavailable");
        if (calendar_display_init() != ESP_OK) ESP_LOGW(TAG, "Diagnostic init: calendar unavailable");
        if (timetable_init() != ESP_OK) ESP_LOGW(TAG, "Diagnostic init: timetable unavailable");
        if (todo_init() != ESP_OK) ESP_LOGW(TAG, "Diagnostic init: todo unavailable");
        if (countdown_init() != ESP_OK) ESP_LOGW(TAG, "Diagnostic init: countdown unavailable");
        ESP_LOGW(TAG, "SPIFFS diagnostic mode: content display and sleep arming are disabled");
        ESP_LOGI(TAG, "Ready. http://%s.local/ or http://192.168.4.1/",
                 device_identity_get_mdns_hostname());
        return;
    }

    esp_err_t epd_err = epd_init();
    if (epd_err != ESP_OK) {
        ESP_LOGE(TAG, "EPD init failed (%s); keeping AP/Web online for diagnostics",
                 esp_err_to_name(epd_err));
    }

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

    if (epd_err == ESP_OK) {
        boot_display_route();
    } else {
        ESP_LOGW(TAG, "Skipping boot display because EPD is not ready");
    }
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

    esp_err_t arm_err = power_mgr_arm();
    if (arm_err != ESP_OK)
        ESP_LOGW(TAG, "Power manager arm failed: %s", esp_err_to_name(arm_err));
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

    if (battery_mon_init() != ESP_OK)
        ESP_LOGW(TAG, "Battery monitor unavailable (check BAT_DET / ADC pin)");

    if (power_mgr_is_timer_wake()) {
        quick_refresh_and_sleep();
    } else {
        full_boot();
    }
}
