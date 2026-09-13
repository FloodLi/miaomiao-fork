#include "time_sync.h"

#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "clock_display.h"

static const char *TAG = "time_sync";
static atomic_bool s_synced;
static atomic_bool s_ip_handler_registered;
static atomic_bool s_save_task_running;
static int64_t s_last_saved_epoch;

#define TIME_NVS_NS "time_sync"
#define TIME_NVS_LAST_EPOCH "last_epoch"
#define TIME_SAVE_MIN_INTERVAL_S (24 * 60 * 60)

/** 周期性校时：默认 15 分钟，减小两次 SNTP 之间的晶振漂移（原 1 小时易累计偏差） */
#define SNTP_RESYNC_INTERVAL_MS (15 * 60 * 1000)

static int month_from_abbrev(const char *name)
{
    static const char *const months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    for (int i = 0; i < 12; i++) {
        if (name && strncmp(name, months[i], 3) == 0)
            return i;
    }
    return -1;
}

static time_t firmware_build_epoch(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    if (!app)
        return 0;

    char month[4] = {0};
    int day = 0, year = 0, hour = 0, minute = 0, second = 0;
    if (sscanf(app->date, "%3s %d %d", month, &day, &year) != 3 ||
        sscanf(app->time, "%d:%d:%d", &hour, &minute, &second) != 3)
        return 0;
    int mon = month_from_abbrev(month);
    if (mon < 0 || day < 1 || day > 31 || year < 2024 || year > 2100 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 60)
        return 0;

    struct tm build = {
        .tm_sec = second,
        .tm_min = minute,
        .tm_hour = hour,
        .tm_mday = day,
        .tm_mon = mon,
        .tm_year = year - 1900,
        .tm_isdst = -1,
    };
    return mktime(&build);
}

static int64_t load_saved_epoch(void)
{
    nvs_handle_t h;
    int64_t epoch = 0;
    if (nvs_open(TIME_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i64(h, TIME_NVS_LAST_EPOCH, &epoch) != ESP_OK)
            epoch = 0;
        nvs_close(h);
    }
    return epoch;
}

static void save_time_task(void *arg)
{
    (void)arg;
    int64_t epoch = (int64_t)time(NULL);
    if (epoch >= 1704067200 &&
        (s_last_saved_epoch <= 0 ||
         llabs(epoch - s_last_saved_epoch) >= TIME_SAVE_MIN_INTERVAL_S)) {
        nvs_handle_t h;
        esp_err_t err = nvs_open(TIME_NVS_NS, NVS_READWRITE, &h);
        if (err == ESP_OK) {
            err = nvs_set_i64(h, TIME_NVS_LAST_EPOCH, epoch);
            if (err == ESP_OK)
                err = nvs_commit(h);
            nvs_close(h);
        }
        if (err == ESP_OK) {
            s_last_saved_epoch = epoch;
            ESP_LOGI(TAG, "Saved trusted time for offline calendar");
        } else {
            ESP_LOGW(TAG, "Trusted time save failed: %s", esp_err_to_name(err));
        }
    }
    atomic_store(&s_save_task_running, false);
    vTaskDelete(NULL);
}

static void schedule_trusted_time_save(void)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_save_task_running, &expected, true))
        return;
    if (xTaskCreate(save_time_task, "time_save", 3072, NULL, 3, NULL) != pdPASS) {
        atomic_store(&s_save_task_running, false);
        ESP_LOGW(TAG, "Trusted time save task create failed");
    }
}

static void on_time_sync(struct timeval *tv)
{
    s_synced = true;
    struct tm t;
    localtime_r(&tv->tv_sec, &t);
    ESP_LOGI(TAG, "Time synchronized: %04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
    schedule_trusted_time_save();
    clock_display_notify_time_resync();
}

esp_err_t time_sync_restore_offline_time(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();
    int64_t saved = load_saved_epoch();
    s_last_saved_epoch = saved;
    if (time_sync_system_time_valid())
        return ESP_OK;

    time_t build = firmware_build_epoch();
    int64_t fallback = saved > (int64_t)build ? saved : (int64_t)build;
    if (fallback < 1704067200)
        return ESP_ERR_NOT_FOUND;

    struct timeval tv = {.tv_sec = (time_t)fallback, .tv_usec = 0};
    if (settimeofday(&tv, NULL) != 0)
        return ESP_FAIL;

    ESP_LOGW(TAG, "Offline time restored from %s; waiting for SNTP correction",
             saved >= (int64_t)build ? "last sync" : "firmware build");
    return ESP_OK;
}

static void on_sta_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    if (!esp_sntp_enabled())
        return;
    esp_sntp_restart();
    ESP_LOGI(TAG, "SNTP restarted after STA got IP");
}

static esp_err_t register_sta_got_ip_handler_once(void)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_ip_handler_registered, &expected, true))
        return ESP_OK;

    esp_err_t err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               on_sta_got_ip, NULL);
    if (err != ESP_OK) {
        atomic_store(&s_ip_handler_registered, false);
        ESP_LOGW(TAG, "STA got IP handler register failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t time_sync_init(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();
    (void)time_sync_restore_offline_time();

    esp_err_t handler_err = register_sta_got_ip_handler_once();
    if (esp_sntp_enabled()) {
        esp_sntp_restart();
        ESP_LOGI(TAG, "SNTP already initialized, restart requested");
        return handler_err;
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    /* 收到 NTP 包后立即 settimeofday，避免 SMOOTH 模式在数分钟内缓慢追赶导致表盘滞后 */
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "cn.pool.ntp.org");
    esp_sntp_setservername(2, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(on_time_sync);
    esp_sntp_set_sync_interval(SNTP_RESYNC_INTERVAL_MS);
    esp_sntp_init();

    ESP_LOGI(TAG, "SNTP: TZ=CST-8, mode=IMMED, resync=%ds, servers=ntp.aliyun.com, cn.pool.ntp.org",
             SNTP_RESYNC_INTERVAL_MS / 1000);
    return handler_err;
}

bool time_sync_is_synced(void)
{
    return s_synced;
}

bool time_sync_get_local(struct tm *out)
{
    if (!s_synced || !out) return false;
    time_t now = time(NULL);
    localtime_r(&now, out);
    return true;
}

bool time_sync_system_time_valid(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();

    time_t now = time(NULL);
    if (now < 1704067200) /* 2024-01-01 00:00:00 UTC */
        return false;

    struct tm t;
    localtime_r(&now, &t);
    int year = t.tm_year + 1900;
    return year >= 2024 && year <= 2100;
}

bool time_sync_get_local_relaxed(struct tm *out)
{
    if (!out)
        return false;
    if (time_sync_get_local(out))
        return true;
    if (!time_sync_system_time_valid())
        return false;
    time_t now = time(NULL);
    localtime_r(&now, out);
    return true;
}

bool time_sync_get_str(char *buf, size_t len)
{
    if (!s_synced || !buf || len == 0) return false;
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &t);
    return true;
}
