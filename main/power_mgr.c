#include "power_mgr.h"

#include <string.h>

#include "epd.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "hal/usb_serial_jtag_ll.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "power";

#define NVS_NS          "power"
#define NVS_KEY_EN      "enabled"
#define NVS_KEY_INTV    "interval"
#define NVS_KEY_IDLE    "idle"

#define DEFAULT_INTERVAL_MIN   60
#define DEFAULT_IDLE_TIMEOUT_S 120
#define CHECK_PERIOD_MS        10000
#define IDLE_TASK_STACK        4096

#define WAKE_GPIO_SW3  3
#define WAKE_GPIO_SW5  9

static power_config_t s_cfg = {
    .enabled        = false,
    .interval_min   = DEFAULT_INTERVAL_MIN,
    .idle_timeout_s = DEFAULT_IDLE_TIMEOUT_S,
};

static bool              s_timer_wake;
static int64_t           s_last_activity_us;
static portMUX_TYPE      s_activity_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t      s_idle_task;

/* ── NVS helpers ──────────────────────────────────────────────────── */

static void load_config(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    uint8_t v8;
    if (nvs_get_u8(h, NVS_KEY_EN, &v8) == ESP_OK)
        s_cfg.enabled = (v8 != 0);

    int32_t v32;
    if (nvs_get_i32(h, NVS_KEY_INTV, &v32) == ESP_OK && v32 >= 1 && v32 <= 1440)
        s_cfg.interval_min = (int)v32;

    if (nvs_get_i32(h, NVS_KEY_IDLE, &v32) == ESP_OK && v32 >= 30 && v32 <= 600)
        s_cfg.idle_timeout_s = (int)v32;

    nvs_close(h);
}

static void save_config(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_EN, s_cfg.enabled ? 1 : 0);
    nvs_set_i32(h, NVS_KEY_INTV, (int32_t)s_cfg.interval_min);
    nvs_set_i32(h, NVS_KEY_IDLE, (int32_t)s_cfg.idle_timeout_s);
    nvs_commit(h);
    nvs_close(h);
}

/* ── idle monitor task (dedicated stack, no Tmr Svc dependency) ──── */

static void idle_monitor_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CHECK_PERIOD_MS));

        bool enabled;
        int timeout_s;
        portENTER_CRITICAL(&s_activity_mux);
        enabled   = s_cfg.enabled;
        timeout_s = s_cfg.idle_timeout_s;
        int64_t last = s_last_activity_us;
        portEXIT_CRITICAL(&s_activity_mux);

        if (!enabled) continue;

        int64_t idle_us = esp_timer_get_time() - last;
        int64_t threshold_us = (int64_t)timeout_s * 1000000LL;

        if (idle_us >= threshold_us) {
            ESP_LOGI(TAG, "Idle %d s >= %d s, entering deep sleep",
                     (int)(idle_us / 1000000LL), timeout_s);
            power_mgr_enter_sleep();
        }
    }
}

/* ── public API ───────────────────────────────────────────────────── */

esp_err_t power_mgr_init(void)
{
    load_config();

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    s_timer_wake = (cause == ESP_SLEEP_WAKEUP_TIMER);

    portENTER_CRITICAL(&s_activity_mux);
    s_last_activity_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_activity_mux);

    if (cause == ESP_SLEEP_WAKEUP_TIMER) {
        ESP_LOGI(TAG, "Woke from deep sleep (timer)");
    } else if (cause == ESP_SLEEP_WAKEUP_EXT1) {
        ESP_LOGI(TAG, "Woke from deep sleep (button)");
    } else {
        ESP_LOGI(TAG, "Normal power-on (cause=%d)", (int)cause);
    }

    ESP_LOGI(TAG, "Config: enabled=%d, interval=%d min, idle=%d s",
             s_cfg.enabled, s_cfg.interval_min, s_cfg.idle_timeout_s);

    return ESP_OK;
}

bool power_mgr_is_timer_wake(void)
{
    return s_timer_wake && s_cfg.enabled;
}

esp_err_t power_mgr_get_config(power_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_activity_mux);
    *out = s_cfg;
    portEXIT_CRITICAL(&s_activity_mux);
    return ESP_OK;
}

esp_err_t power_mgr_set_config(const power_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    power_config_t validated = *cfg;
    if (validated.interval_min < 1)    validated.interval_min = 1;
    if (validated.interval_min > 1440) validated.interval_min = 1440;
    if (validated.idle_timeout_s < 30)  validated.idle_timeout_s = 30;
    if (validated.idle_timeout_s > 600) validated.idle_timeout_s = 600;
    portENTER_CRITICAL(&s_activity_mux);
    s_cfg = validated;
    portEXIT_CRITICAL(&s_activity_mux);
    save_config();  /* NVS write outside critical section */
    ESP_LOGI(TAG, "Config updated: enabled=%d, interval=%d min, idle=%d s",
             s_cfg.enabled, s_cfg.interval_min, s_cfg.idle_timeout_s);

    /* 开机时若低功耗为关，app_main 里的 power_mgr_arm() 会直接返回且不会创建
     * pwr_idle；之后仅在网页里打开低功耗时，必须在此再次 arm，否则永远不会进深睡。 */
    if (s_cfg.enabled)
        power_mgr_arm();

    return ESP_OK;
}

void power_mgr_reset_activity(void)
{
    portENTER_CRITICAL(&s_activity_mux);
    s_last_activity_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_activity_mux);
}

void power_mgr_arm(void)
{
    if (!s_cfg.enabled) {
        ESP_LOGI(TAG, "Sleep mode disabled, not arming");
        return;
    }

    if (!s_idle_task) {
        xTaskCreate(idle_monitor_task, "pwr_idle", IDLE_TASK_STACK,
                    NULL, 2, &s_idle_task);
    }

    portENTER_CRITICAL(&s_activity_mux);
    s_last_activity_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_activity_mux);
    ESP_LOGI(TAG, "Armed: will sleep after %d s of inactivity", s_cfg.idle_timeout_s);
}

void power_mgr_enter_sleep(void)
{
    uint64_t sleep_us = (uint64_t)s_cfg.interval_min * 60ULL * 1000000ULL;
    esp_sleep_enable_timer_wakeup(sleep_us);

    uint64_t gpio_mask = (1ULL << WAKE_GPIO_SW3) | (1ULL << WAKE_GPIO_SW5);
    esp_sleep_enable_ext1_wakeup(gpio_mask, ESP_EXT1_WAKEUP_ANY_HIGH);

    ESP_LOGI(TAG, "Entering deep sleep for %d min (GPIO 3/9 can wake)",
             s_cfg.interval_min);

    /* Wait for any in-progress EPD refresh before sleeping */
    int wait_count = 0;
    while (!epd_is_ready() && wait_count < 100) {
        vTaskDelay(pdMS_TO_TICKS(200));
        wait_count++;
    }

    nvs_flush_all();
    usb_serial_jtag_ll_phy_enable_pad(false);  // 关闭 USB PHY，避免深睡眠漏电
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_deep_sleep_start();
}

void power_mgr_save_mode(int mode_index)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "last_mode", (int32_t)mode_index);
    nvs_commit(h);
    nvs_close(h);
}

int power_mgr_load_mode(void)
{
    nvs_handle_t h;
    int32_t mode = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    nvs_get_i32(h, "last_mode", &mode);
    nvs_close(h);
    return (int)mode;
}
