#include "battery_mon.h"

#include <stdio.h>
#include <string.h>

#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "battery";

/* 与 otto-robot/power_manager.h 一致：双 100k 分压、12dB 衰减下的经验 ADC 刻度 */
static const uint16_t k_adc_empty = 2050;
static const uint16_t k_adc_full  = 2450;

#define ADC_RING_CAP 10

static adc_oneshot_unit_handle_t s_adc;
static adc_unit_t                s_unit;
static adc_channel_t             s_chan;
static bool                      s_adc_ok;

static uint16_t s_ring[ADC_RING_CAP];
static size_t   s_ring_idx;
static size_t   s_ring_cnt;

static uint8_t  s_percent;
static int      s_last_raw;
static bool     s_valid;
static bool     s_charging;

static esp_timer_handle_t s_timer;
/** adc_oneshot 不可并发读：定时器任务与 UI/HTTP 可能同时触发 */
static SemaphoreHandle_t s_read_mu;

static void calc_percent(uint32_t avg_adc)
{
    if (avg_adc <= k_adc_empty) {
        s_percent = 0;
    } else if (avg_adc >= k_adc_full) {
        s_percent = 100;
    } else {
        float r = (float)(avg_adc - k_adc_empty) / (float)(k_adc_full - k_adc_empty);
        s_percent = (uint8_t)(r * 100.0f + 0.5f);
    }
}

static void read_once(void)
{
    if (!s_adc_ok || !s_read_mu)
        return;
    if (xSemaphoreTake(s_read_mu, pdMS_TO_TICKS(500)) != pdTRUE)
        return;

    int v = 0;
    if (adc_oneshot_read(s_adc, s_chan, &v) != ESP_OK) {
        xSemaphoreGive(s_read_mu);
        return;
    }

    s_last_raw = v;
    s_ring[s_ring_idx] = (uint16_t)v;
    s_ring_idx = (s_ring_idx + 1) % ADC_RING_CAP;
    if (s_ring_cnt < ADC_RING_CAP)
        s_ring_cnt++;

    uint32_t sum = 0;
    for (size_t i = 0; i < s_ring_cnt; i++)
        sum += s_ring[i];
    calc_percent(sum / s_ring_cnt);
    s_valid = true;

    if (BAT_MON_CHG_GPIO != GPIO_NUM_NC)
        s_charging = (gpio_get_level(BAT_MON_CHG_GPIO) == 0);
    else
        s_charging = false;

    xSemaphoreGive(s_read_mu);
}

static void timer_cb(void *arg)
{
    (void)arg;
    read_once();
}

static void setup_chg_pin(void)
{
    gpio_num_t chg_gpio = BAT_MON_CHG_GPIO;
    if (chg_gpio == GPIO_NUM_NC)
        return;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << (uint32_t)chg_gpio,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = 1,
        .pull_down_en = 0,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    ESP_LOGI(TAG, "Charge sense GPIO %d (low=charging)", (int)chg_gpio);
}

esp_err_t battery_mon_init(void)
{
    memset(s_ring, 0, sizeof(s_ring));
    s_ring_idx = 0;
    s_ring_cnt = 0;
    s_percent  = 0;
    s_last_raw = 0;
    s_valid    = false;
    s_charging = false;
    s_adc_ok   = false;

    setup_chg_pin();

    if (adc_oneshot_io_to_channel(BAT_MON_ADC_GPIO, &s_unit, &s_chan) != ESP_OK) {
        ESP_LOGW(TAG, "GPIO %d is not an ADC pin on this chip", (int)BAT_MON_ADC_GPIO);
        return ESP_ERR_NOT_SUPPORTED;
    }

    adc_oneshot_unit_init_cfg_t ucfg = {
        .unit_id  = s_unit,
        .clk_src  = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&ucfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t ccfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten    = ADC_ATTEN_DB_12,
    };
    err = adc_oneshot_config_channel(s_adc, s_chan, &ccfg);
    if (err != ESP_OK) {
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        ESP_LOGE(TAG, "adc_oneshot_config_channel: %s", esp_err_to_name(err));
        return err;
    }

    s_read_mu = xSemaphoreCreateMutex();
    if (!s_read_mu) {
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_adc_ok = true;
    read_once();

    const esp_timer_create_args_t targs = {
        .callback              = &timer_cb,
        .arg                   = NULL,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "battery_mon",
        .skip_unhandled_events = true,
    };
    err = esp_timer_create(&targs, &s_timer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_timer_create failed, only manual poll available");
        s_timer = NULL;
    } else {
        esp_timer_start_periodic(s_timer, 1000000); /* 1 s，与 Otto 一致 */
    }

    ESP_LOGI(TAG, "BAT_DET GPIO %d unit %d ch %d", (int)BAT_MON_ADC_GPIO, (int)s_unit, (int)s_chan);
    return ESP_OK;
}

void battery_mon_deinit(void)
{
    if (s_timer) {
        esp_timer_stop(s_timer);
        esp_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_adc) {
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
    }
    if (s_read_mu) {
        vSemaphoreDelete(s_read_mu);
        s_read_mu = NULL;
    }
    s_adc_ok = false;
}

void battery_mon_poll(void)
{
    read_once();
}

void battery_mon_get_status(battery_status_t *out)
{
    if (!out)
        return;
    out->percent  = s_percent;
    out->charging = s_charging;
    out->adc_raw  = s_last_raw;
    out->valid    = s_valid && s_adc_ok;
}

void battery_mon_draw_on_fb(fb_t *fb, int x, int y, fb_color_t color, int scale)
{
    if (!fb)
        return;
    if (scale < 1)
        scale = 1;

    /* 不在绘制路径上同步采样，避免与 1s 定时器并发 adc_oneshot_read */
    battery_status_t s;
    battery_mon_get_status(&s);

    char line[40];
    if (!s.valid) {
        snprintf(line, sizeof(line), "\xe7\x94\xb5\xe9\x87\x8f --"); /* 电量 -- */
    } else if (s.charging) {
        snprintf(line, sizeof(line),
                 "\xe5\x85\x85\xe7\x94\xb5 %u%%", (unsigned)s.percent); /* 充电 */
    } else {
        snprintf(line, sizeof(line),
                 "\xe7\x94\xb5\xe9\x87\x8f %u%%", (unsigned)s.percent);
    }
    fb_utf8_scaled(fb, x, y, line, color, scale);
}

void battery_mon_draw_percent_compact(fb_t *fb, int x, int y, fb_color_t color, int scale)
{
    if (!fb)
        return;
    if (scale < 1)
        scale = 1;

    battery_status_t s;
    battery_mon_get_status(&s);

    char line[12];
    if (!s.valid)
        snprintf(line, sizeof(line), "--%%");
    else
        snprintf(line, sizeof(line), "%u%%", (unsigned)s.percent);

    fb_utf8_scaled(fb, x, y, line, color, scale);
}
