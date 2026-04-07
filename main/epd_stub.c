/**
 * EPD 驱动 — SSD1619（4.2" 400×300 BWR）与 UC8179（5.83" 648×480 BWR）
 *
 * 引脚与工程原理图一致：SCK=4 MOSI=5 DC=7 CS=15 RST=6 BUSY=16，SPI2，Mode0，2MHz。
 * BUSY：Waveshare SSD1619 4.2 B（epd4in2b_V2 ReadBusy，flag=新）为「忙=高、空闲=低」，默认 idle=0。
 * 其它模组（如部分 GDE）可能为空闲高，可在 NVS epd/busy_idle=1。
 * 参考：https://github.com/waveshareteam/e-Paper/blob/master/Arduino/epd4in2b_V2/epd4in2b_V2.cpp
 */

#include "epd.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"

/* ── 硬件引脚 ─────────────────────────────────────────────────────── */

#define PIN_EPD_SCK   4
#define PIN_EPD_MOSI  5
#define PIN_EPD_DC    7
#define PIN_EPD_CS    15
#define PIN_EPD_RST   6
#define PIN_EPD_BUSY  16

#define EPD_SPI_HOST      SPI2_HOST
#define EPD_SPI_HZ        (2 * 1000 * 1000)
#define EPD_SPI_MAX_TX    16384
#define EPD_FILL_CHUNK    4096
#define EPD_BUSY_TIMEOUT_MS  60000

static const char *TAG = "epd";

static spi_device_handle_t s_spi;
static SemaphoreHandle_t   s_epd_mutex;
static epd_panel_t         s_panel = EPD_PANEL_42_BWR;

/** 已为当前面板跑过完整上电初始化序列（避免每次刷屏都硬件复位） */
static bool s_panel_seq_done;

/** epd_init() 完全结束后再为 true，供 HTTP 与其它任务判断 */
static bool s_epd_ready;

/**
 * NVS 键 busy_idle：0 = 空闲时 BUSY 为低；1 = 空闲时为高。
 * 未配置时：SSD1619 4.2 默认 0（与 Waveshare 官方一致）；5.83 UC8179 默认 1。
 */
static int s_busy_idle_level = -1;

static int busy_idle_effective(void);

/* ── 面板规格 ─────────────────────────────────────────────────────── */

static const struct {
    int w, h;
    bool has_red;
    const char *name;
} s_specs[] = {
    [EPD_PANEL_42_BWR]  = { 400, 300, true, "SSD1619 4.2\" BWR"  },
    [EPD_PANEL_583_BWR] = { 648, 480, true, "UC8179 5.83\" BWR" },
};

int  epd_width(void)       { return s_specs[s_panel].w; }
int  epd_height(void)      { return s_specs[s_panel].h; }
int  epd_plane_bytes(void) { return s_specs[s_panel].w * s_specs[s_panel].h / 8; }
bool epd_has_red(void)     { return s_specs[s_panel].has_red; }
epd_panel_t epd_get_panel(void) { return s_panel; }

esp_err_t epd_set_panel(epd_panel_t panel)
{
    if (panel != EPD_PANEL_42_BWR && panel != EPD_PANEL_583_BWR)
        return ESP_ERR_INVALID_ARG;

    if (s_epd_mutex)
        xSemaphoreTake(s_epd_mutex, portMAX_DELAY);

    if (s_panel != panel) {
        s_panel_seq_done = false;
        ESP_LOGW(TAG, "Panel changed to %s, will re-init on next display",
                 s_specs[panel].name);
    }

    s_panel = panel;

    if (s_epd_mutex)
        xSemaphoreGive(s_epd_mutex);

    nvs_handle_t h;
    if (nvs_open("epd", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "panel", (uint8_t)panel);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Panel -> %s (%dx%d)", s_specs[panel].name,
             s_specs[panel].w, s_specs[panel].h);
    return ESP_OK;
}

int epd_busy_idle_level(void)
{
    return busy_idle_effective();
}

esp_err_t epd_set_busy_idle(int level)
{
    if (level != 0 && level != 1)
        return ESP_ERR_INVALID_ARG;

    s_busy_idle_level = level;

    nvs_handle_t h;
    esp_err_t e = nvs_open("epd", NVS_READWRITE, &h);
    if (e != ESP_OK)
        return e;
    e = nvs_set_u8(h, "busy_idle", (uint8_t)level);
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK)
        ESP_LOGI(TAG, "busy_idle -> %d (0=idle low, 1=idle high)", level);
    return e;
}

static void nvs_load_panel(void)
{
    nvs_handle_t h;
    if (nvs_open("epd", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "panel", &v) == ESP_OK) {
            if (v == EPD_PANEL_42_BWR || v == EPD_PANEL_583_BWR)
                s_panel = (epd_panel_t)v;
        }
        uint8_t bi;
        if (nvs_get_u8(h, "busy_idle", &bi) == ESP_OK && bi <= 1)
            s_busy_idle_level = (int)bi;
        nvs_close(h);
    }
}

static int busy_idle_effective(void)
{
    if (s_busy_idle_level == 0 || s_busy_idle_level == 1)
        return s_busy_idle_level;
    /* Waveshare epd4in2b_V2：新 SSD1619 在 ReadBusy 里 while(busy==1) 即忙为高、就绪为低 */
    if (s_panel == EPD_PANEL_42_BWR)
        return 0;
    return 1;
}

/* ── 延时 / SPI 底层 ──────────────────────────────────────────────── */

static inline void delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static esp_err_t spi_tx(const void *data, size_t len_bytes)
{
    spi_transaction_t t = {0};
    t.length = (uint32_t)(len_bytes * 8);
    t.tx_buffer = data;
    return spi_device_transmit(s_spi, &t);
}

static void epd_cmd(uint8_t cmd)
{
    gpio_set_level(PIN_EPD_DC, 0);
    esp_err_t e = spi_tx(&cmd, 1);
    if (e != ESP_OK) ESP_LOGE(TAG, "cmd 0x%02X spi err %s", cmd, esp_err_to_name(e));
}

static void epd_dat(uint8_t data)
{
    gpio_set_level(PIN_EPD_DC, 1);
    esp_err_t e = spi_tx(&data, 1);
    if (e != ESP_OK) ESP_LOGE(TAG, "dat spi err %s", esp_err_to_name(e));
}

static void epd_bulk(const uint8_t *data, size_t len)
{
    gpio_set_level(PIN_EPD_DC, 1);

    bool src_is_dma = esp_ptr_dma_capable(data);
    uint8_t *bounce = NULL;
    if (!src_is_dma) {
        bounce = heap_caps_malloc(EPD_FILL_CHUNK, MALLOC_CAP_DMA);
        if (!bounce) {
            ESP_LOGE(TAG, "DMA bounce alloc failed");
            return;
        }
    }

    size_t offset = 0;
    while (offset < len) {
        size_t n = (len - offset > EPD_FILL_CHUNK) ? EPD_FILL_CHUNK : (len - offset);
        const uint8_t *tx_ptr;
        if (bounce) {
            memcpy(bounce, data + offset, n);
            tx_ptr = bounce;
        } else {
            tx_ptr = data + offset;
        }
        esp_err_t e = spi_tx(tx_ptr, n);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "bulk spi err %s", esp_err_to_name(e));
            break;
        }
        offset += n;
    }
    free(bounce);
}

static void epd_fill(uint8_t value, size_t len)
{
    uint8_t *buf = heap_caps_malloc(EPD_FILL_CHUNK, MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE(TAG, "epd_fill: DMA malloc failed");
        return;
    }
    memset(buf, value, EPD_FILL_CHUNK);
    gpio_set_level(PIN_EPD_DC, 1);
    size_t offset = 0;
    while (offset < len) {
        size_t n = (len - offset > EPD_FILL_CHUNK) ? EPD_FILL_CHUNK : (len - offset);
        esp_err_t e = spi_tx(buf, n);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "fill spi err %s", esp_err_to_name(e));
            break;
        }
        offset += n;
    }
    free(buf);
}

/* ── 复位 / BUSY ─────────────────────────────────────────────────── */

static void epd_hw_reset(void)
{
    gpio_set_level(PIN_EPD_RST, 1);
    delay_ms(20);
    gpio_set_level(PIN_EPD_RST, 0);
    delay_ms(40);
    gpio_set_level(PIN_EPD_RST, 1);
    delay_ms(200);
}

/**
 * 等待 BUSY 释放。idle 电平见 busy_idle_effective()。
 * UC8179：轮询前可发 0x71；SSD1619 一般直接读 GPIO。
 *
 * after_update_cmd：发完 0x20（SSD1619）/0x12（UC8179 等）刷新命令后须置 true。
 * 否则 BUSY 可能仍短暂保持“空闲”电平（上拉），会误判为已就绪（仅 ~10ms 就返回）。
 */
static void epd_wait_ready(bool after_update_cmd)
{
    delay_ms(10);
    const bool is_uc8179 = (s_panel == EPD_PANEL_583_BWR);
    const int idle = busy_idle_effective();

    if (is_uc8179)
        epd_cmd(0x71);

    /* 阶段 1：等到离开空闲态（进入忙），避免刷新命令刚发出时仍读到空闲 */
    if (after_update_cmd) {
        int phase1 = 0;
        const int phase1_max_ms = 800;
        while (gpio_get_level(PIN_EPD_BUSY) == idle && phase1 < phase1_max_ms) {
            delay_ms(20);
            phase1 += 20;
            if (is_uc8179)
                epd_cmd(0x71);
        }
        if (phase1 >= phase1_max_ms)
            ESP_LOGW(TAG, "BUSY: no busy phase after update (still idle %d ms). "
                          "Check GPIO4 / NVS epd/busy_idle (0=Waveshare idle-low).",
                     phase1_max_ms);
    }

    int waited = 0;
    bool timed_out = false;
    while (gpio_get_level(PIN_EPD_BUSY) != idle) {
        if (is_uc8179)
            epd_cmd(0x71);
        delay_ms(200);
        waited += 200;
        if (waited >= EPD_BUSY_TIMEOUT_MS) {
            ESP_LOGW(TAG, "BUSY timeout %d ms (expect idle=%d, gpio=%d). "
                          "Try NVS key busy_idle=0 or 1 on namespace 'epd'.",
                     waited, idle, (int)gpio_get_level(PIN_EPD_BUSY));
            timed_out = true;
            break;
        }
    }
    if (!timed_out && waited > 200)
        ESP_LOGI(TAG, "ready after %d ms", waited);
}

/* ═══════════════════════════════════════════════════════════════════
 * SSD1619 — 4.2" 400×300
 * ═══════════════════════════════════════════════════════════════════ */

static void ssd1619_cursor_home(void)
{
    epd_cmd(0x4E);
    epd_dat(0x00);
    epd_cmd(0x4F);
    epd_dat(0x00);
    epd_dat(0x00);
}

static void ssd1619_init_sequence(void)
{
    epd_hw_reset();
    ESP_LOGI(TAG, "SSD1619: BUSY gpio after reset = %d (idle expect %d)",
             (int)gpio_get_level(PIN_EPD_BUSY), busy_idle_effective());
    if (busy_idle_effective() == 1 && gpio_get_level(PIN_EPD_BUSY) == 0)
        ESP_LOGW(TAG, "BUSY low while idle=1 — set NVS epd/busy_idle=0 or use default for 4.2\".");
    epd_wait_ready(false);

    epd_cmd(0x12);
    epd_wait_ready(false);

    epd_cmd(0x74);
    epd_dat(0x54);
    epd_cmd(0x7E);
    epd_dat(0x3B);

    epd_cmd(0x01);
    epd_dat(0x2B);
    epd_dat(0x01);
    epd_dat(0x00);

    epd_cmd(0x11);
    epd_dat(0x03);

    epd_cmd(0x44);
    epd_dat(0x00);
    epd_dat(0x31);

    epd_cmd(0x45);
    epd_dat(0x00);
    epd_dat(0x00);
    epd_dat(0x2B);
    epd_dat(0x01);

    /* BorderWaveform：Waveshare EPD_4in2b_V2_Init_new 用 0x05，减轻屏体边缘红框；0x03 易偏红 */
    epd_cmd(0x3C);
    epd_dat(0x05);
    epd_cmd(0x2C);
    epd_dat(0x55);

    epd_cmd(0x03);
    epd_dat(0x15);
    epd_cmd(0x04);
    epd_dat(0x41);
    epd_dat(0xA8);
    epd_dat(0x32);

    epd_cmd(0x3A);
    epd_dat(0x2C);
    epd_cmd(0x3B);
    epd_dat(0x0B);

    ssd1619_cursor_home();

    ESP_LOGI(TAG, "SSD1619 init OK");
}

static void ssd1619_refresh(void)
{
    /* 每帧刷新前再写边框，避免部分序列下边缘仍按红边驱动 */
    epd_cmd(0x3C);
    epd_dat(0x05);

    /* Waveshare DisplayFrame(new): 0x22 0xF7 再 0x20；与 epd4in2b_V2 一致 */
    epd_cmd(0x22);
    epd_dat(0xF7);
    delay_ms(2);
    epd_cmd(0x20);

    TickType_t t0 = xTaskGetTickCount();
    epd_wait_ready(true);
    uint32_t elapsed_ms = pdTICKS_TO_MS(xTaskGetTickCount() - t0);

    /* BUSY 未接或恒为高时：wait_ready 几毫秒内就“就绪”，全刷实际未完成，屏不更新 */
    if (elapsed_ms < 250) {
        ESP_LOGW(TAG, "SSD1619: refresh BUSY only %u ms — likely BUSY floating/wrong. "
                      "Waiting 14s for full e-paper update (check GPIO4 BUSY).",
                 (unsigned)elapsed_ms);
        delay_ms(14000);
    } else {
        delay_ms(500);
    }
}

/*
 * Fast BW-only LUT for SSD1619 (based on Waveshare EPD_4in2_V2 fast waveform).
 * Uses fewer drive phases → refresh in ~1-2s with acceptable ghosting.
 * Structure: 5 VS groups × 12 bytes + 9 TP groups × 5 bytes = 105 bytes.
 */
static const uint8_t SSD1619_LUT_FAST[105] = {
    /* VS group 0 (VCOM) */
    0x80, 0x48, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* VS group 1 (WW) */
    0x40, 0x48, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* VS group 2 (BW) */
    0x80, 0x48, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* VS group 3 (WB) */
    0x40, 0x48, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* VS group 4 (BB) */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* TP (timing) */
    0x0A, 0x00, 0x00, 0x00, 0x00,
    0x08, 0x01, 0x00, 0x08, 0x01,
    0x00, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
};

static void ssd1619_refresh_fast(void)
{
    /* Load custom fast LUT */
    epd_cmd(0x32);
    epd_bulk(SSD1619_LUT_FAST, sizeof(SSD1619_LUT_FAST));

    /* Border waveform for fast mode */
    epd_cmd(0x3C);
    epd_dat(0x80);

    /*
     * Display Update Control 2:
     * 0xC7 = Enable CLK + Enable CP + (skip temp & LUT reload) +
     *        Display + Disable CP + Disable CLK
     * Bit4=0 → use register LUT (our fast table), not OTP.
     */
    epd_cmd(0x22);
    epd_dat(0xC7);
    delay_ms(2);
    epd_cmd(0x20);

    TickType_t t0 = xTaskGetTickCount();
    epd_wait_ready(true);
    uint32_t elapsed_ms = pdTICKS_TO_MS(xTaskGetTickCount() - t0);

    if (elapsed_ms < 100) {
        ESP_LOGW(TAG, "fast refresh BUSY only %u ms, waiting 2s safety",
                 (unsigned)elapsed_ms);
        delay_ms(2000);
    } else {
        delay_ms(200);
    }
    ESP_LOGI(TAG, "fast refresh done in %u ms", (unsigned)elapsed_ms);
}

static void ssd1619_write_bw_then_red(const uint8_t *bw, const uint8_t *red, int pb)
{
    ssd1619_cursor_home();
    epd_cmd(0x24);
    epd_bulk(bw, (size_t)pb);

    ssd1619_cursor_home();
    epd_cmd(0x26);
    epd_bulk(red, (size_t)pb);

    ssd1619_refresh();
}

static void ssd1619_fill_bw_red(uint8_t bw_byte, uint8_t red_byte, int pb)
{
    ssd1619_cursor_home();
    epd_cmd(0x24);
    epd_fill(bw_byte, (size_t)pb);

    ssd1619_cursor_home();
    epd_cmd(0x26);
    epd_fill(red_byte, (size_t)pb);

    ssd1619_refresh();
}

/* ═══════════════════════════════════════════════════════════════════
 * UC8179 — 5.83" 648×480 BWR（参考 Waveshare 5.83 BWR）
 * ═══════════════════════════════════════════════════════════════════ */

static void uc8179_init_sequence(void)
{
    epd_hw_reset();
    epd_wait_ready(false);

    epd_cmd(0x01);
    epd_dat(0x07);
    epd_dat(0x07);
    epd_dat(0x3F);
    epd_dat(0x3F);

    epd_cmd(0x04);
    delay_ms(100);
    epd_wait_ready(false);

    epd_cmd(0x00);
    epd_dat(0x0F);

    epd_cmd(0x61);
    epd_dat(0x02);
    epd_dat(0x88);
    epd_dat(0x01);
    epd_dat(0xE0);

    epd_cmd(0x15);
    epd_dat(0x00);
    epd_cmd(0x50);
    epd_dat(0x11);
    epd_dat(0x07);
    epd_cmd(0x60);
    epd_dat(0x22);

    ESP_LOGI(TAG, "UC8179 init OK (BWR)");
}

static void uc8179_refresh(void)
{
    epd_cmd(0x12);
    delay_ms(200);
    epd_wait_ready(true);
}

static void uc8179_write_dtm12(const uint8_t *dtm1, const uint8_t *dtm2, int pb)
{
    epd_cmd(0x10);
    delay_ms(2);
    epd_bulk(dtm1, (size_t)pb);
    delay_ms(2);

    epd_cmd(0x13);
    delay_ms(2);
    epd_bulk(dtm2, (size_t)pb);
    delay_ms(2);

    uc8179_refresh();
}

static void uc8179_fill_dtm12(uint8_t dtm1, uint8_t dtm2, int pb)
{
    epd_cmd(0x10);
    delay_ms(2);
    epd_fill(dtm1, (size_t)pb);
    delay_ms(2);

    epd_cmd(0x13);
    delay_ms(2);
    epd_fill(dtm2, (size_t)pb);
    delay_ms(2);

    uc8179_refresh();
}

/* ── 完整初始化（每面板一次；换面板或 NVS 覆盖后需重跑） ───────────── */

static void panel_run_full_init(void)
{
    if (s_panel == EPD_PANEL_42_BWR)
        ssd1619_init_sequence();
    else
        uc8179_init_sequence();
    s_panel_seq_done = true;
}

static void panel_ensure_ready(void)
{
    if (!s_panel_seq_done)
        panel_run_full_init();
}

/** 调用方已持有 s_epd_mutex */
static void epd_clear_to_white_locked(void)
{
    panel_ensure_ready();
    int pb = epd_plane_bytes();
    if (s_panel == EPD_PANEL_42_BWR)
        ssd1619_fill_bw_red(0xFF, 0x00, pb);
    else
        uc8179_fill_dtm12(0xFF, 0x00, pb);
}

bool epd_is_ready(void)
{
    return s_epd_ready;
}

/* ── 对外 API ─────────────────────────────────────────────────────── */

esp_err_t epd_init(void)
{
    s_epd_ready = false;

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_EPD_DC) | (1ULL << PIN_EPD_CS) | (1ULL << PIN_EPD_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out));

    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_EPD_BUSY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1,
    };
    ESP_ERROR_CHECK(gpio_config(&in));

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_EPD_SCK,
        .mosi_io_num = PIN_EPD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_SPI_MAX_TX,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(EPD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = EPD_SPI_HZ,
        .mode = 0,
        .spics_io_num = PIN_EPD_CS,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(EPD_SPI_HOST, &devcfg, &s_spi));

    s_epd_mutex = xSemaphoreCreateMutex();
    assert(s_epd_mutex);

    nvs_load_panel();

    ESP_LOGI(TAG, "EPD start: %s %dx%d, BUSY idle_level=%d (4.2\" default 0=Waveshare; NVS busy_idle=1 if GDE idle-high)",
             s_specs[s_panel].name, epd_width(), epd_height(), busy_idle_effective());

    panel_run_full_init();
    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);
    epd_clear_to_white_locked();
    xSemaphoreGive(s_epd_mutex);

    s_epd_ready = true;
    return ESP_OK;
}

esp_err_t epd_clear_screen(void)
{
    if (!s_epd_mutex || !s_epd_ready)
        return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);
    epd_clear_to_white_locked();
    xSemaphoreGive(s_epd_mutex);
    return ESP_OK;
}

esp_err_t epd_display_test_pattern(void)
{
    if (!s_epd_mutex || !s_epd_ready)
        return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);

    int W = epd_width();
    int H = epd_height();
    int pb = epd_plane_bytes();
    int rb = W / 8;

    uint8_t *buf = heap_caps_malloc((size_t)pb, MALLOC_CAP_DMA);
    if (!buf) {
        xSemaphoreGive(s_epd_mutex);
        return ESP_ERR_NO_MEM;
    }
    memset(buf, 0xFF, (size_t)pb);

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (y < 2 || y >= H - 2 || x < 2 || x >= W - 2)
                buf[y * rb + (x >> 3)] &= ~(0x80 >> (x & 7));
        }
    }

    int mid_x = W / 2, mid_y = H / 2;
    for (int y = 20; y < mid_y; y++) {
        if ((y / 10) & 1) continue;
        for (int x = 20; x < mid_x - 20; x++)
            buf[y * rb + (x >> 3)] &= ~(0x80 >> (x & 7));
    }

    panel_ensure_ready();

    if (s_panel == EPD_PANEL_42_BWR) {
        uint8_t *red = heap_caps_calloc(1, (size_t)pb, MALLOC_CAP_DMA);
        if (!red) {
            free(buf);
            xSemaphoreGive(s_epd_mutex);
            return ESP_ERR_NO_MEM;
        }
        ssd1619_write_bw_then_red(buf, red, pb);
        free(red);
    } else {
        uint8_t *red = heap_caps_calloc(1, (size_t)pb, MALLOC_CAP_DMA);
        if (!red) {
            free(buf);
            xSemaphoreGive(s_epd_mutex);
            return ESP_ERR_NO_MEM;
        }
        uc8179_write_dtm12(buf, red, pb);
        free(red);
    }

    free(buf);
    xSemaphoreGive(s_epd_mutex);
    ESP_LOGI(TAG, "test pattern done");
    return ESP_OK;
}

esp_err_t epd_display_from_file(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    if (!s_epd_mutex || !s_epd_ready)
        return ESP_ERR_INVALID_STATE;

    int pb = epd_plane_bytes();

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "open %s failed", path);
        return ESP_FAIL;
    }

    uint8_t *buf = heap_caps_malloc((size_t)pb, MALLOC_CAP_DMA);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t r1 = fread(buf, 1, (size_t)pb, f);
    if (r1 != (size_t)pb) {
        ESP_LOGE(TAG, "file too small: %u / %u", (unsigned)r1, (unsigned)pb);
        free(buf);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);
    panel_ensure_ready();

    if (s_panel == EPD_PANEL_42_BWR) {
        ssd1619_cursor_home();
        epd_cmd(0x24);
        epd_bulk(buf, (size_t)pb);

        size_t r2 = fread(buf, 1, (size_t)pb, f);
        if (r2 != (size_t)pb)
            memset(buf, 0x00, (size_t)pb);

        ssd1619_cursor_home();
        epd_cmd(0x26);
        epd_bulk(buf, (size_t)pb);

        fclose(f); f = NULL;
        free(buf); buf = NULL;
        ssd1619_refresh();
    } else {
        epd_cmd(0x10);
        delay_ms(2);
        epd_bulk(buf, (size_t)pb);
        delay_ms(2);

        if (epd_has_red()) {
            size_t r2 = fread(buf, 1, (size_t)pb, f);
            if (r2 != (size_t)pb) {
                ESP_LOGW(TAG, "red plane short, zero fill");
                memset(buf, 0x00, (size_t)pb);
            }
        } else {
            memset(buf, 0x00, (size_t)pb);
        }
        epd_cmd(0x13);
        delay_ms(2);
        epd_bulk(buf, (size_t)pb);
        delay_ms(2);

        fclose(f); f = NULL;
        free(buf); buf = NULL;
        uc8179_refresh();
    }

    xSemaphoreGive(s_epd_mutex);
    if (f) fclose(f);
    free(buf);

    ESP_LOGI(TAG, "display from %s", path);
    return ESP_OK;
}

esp_err_t epd_display_from_buffer(const fb_t *fb)
{
    if (!fb) return ESP_ERR_INVALID_ARG;
    if (!s_epd_mutex || !s_epd_ready)
        return ESP_ERR_INVALID_STATE;

    int pb = epd_plane_bytes();

    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);
    panel_ensure_ready();

    if (s_panel == EPD_PANEL_42_BWR) {
        /* Black plane */
        ssd1619_cursor_home();
        epd_cmd(0x24);
        epd_bulk(fb->black, (size_t)pb);

        /* Red plane */
        ssd1619_cursor_home();
        epd_cmd(0x26);
        epd_bulk(fb->red, (size_t)pb);
        ssd1619_refresh();
    } else {
        epd_cmd(0x10);
        delay_ms(2);
        epd_bulk(fb->black, (size_t)pb);
        delay_ms(2);

        epd_cmd(0x13);
        delay_ms(2);
        epd_bulk(fb->red, (size_t)pb);
        delay_ms(2);
        uc8179_refresh();
    }

    xSemaphoreGive(s_epd_mutex);

    ESP_LOGI(TAG, "display from buffer");
    return ESP_OK;
}

esp_err_t epd_display_fb_free(fb_t *fb)
{
    if (!fb) return ESP_ERR_INVALID_ARG;
    if (!s_epd_mutex || !s_epd_ready) {
        fb_destroy(fb);
        return ESP_ERR_INVALID_STATE;
    }

    int pb = epd_plane_bytes();

    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);
    panel_ensure_ready();

    if (s_panel == EPD_PANEL_42_BWR) {
        ssd1619_cursor_home();
        epd_cmd(0x24);
        epd_bulk(fb->black, (size_t)pb);

        ssd1619_cursor_home();
        epd_cmd(0x26);
        epd_bulk(fb->red, (size_t)pb);

        fb_destroy(fb);
        ssd1619_refresh();
    } else {
        epd_cmd(0x10);
        delay_ms(2);
        epd_bulk(fb->black, (size_t)pb);
        delay_ms(2);

        epd_cmd(0x13);
        delay_ms(2);
        epd_bulk(fb->red, (size_t)pb);
        delay_ms(2);

        fb_destroy(fb);
        uc8179_refresh();
    }

    xSemaphoreGive(s_epd_mutex);

    ESP_LOGI(TAG, "display from buffer");
    return ESP_OK;
}

esp_err_t epd_display_from_buffer_fast(const fb_t *fb)
{
    if (!fb) return ESP_ERR_INVALID_ARG;
    if (!s_epd_mutex || !s_epd_ready)
        return ESP_ERR_INVALID_STATE;

    if (s_panel != EPD_PANEL_42_BWR) {
        ESP_LOGW(TAG, "fast refresh only for SSD1619, falling back to full");
        return epd_display_from_buffer(fb);
    }

    int pb = epd_plane_bytes();

    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);
    panel_ensure_ready();

    /* BW plane only — skip red for speed */
    ssd1619_cursor_home();
    epd_cmd(0x24);
    epd_bulk(fb->black, (size_t)pb);

    /* Clear red plane to avoid stale red artifacts */
    ssd1619_cursor_home();
    epd_cmd(0x26);
    epd_fill(0x00, (size_t)pb);

    ssd1619_refresh_fast();

    xSemaphoreGive(s_epd_mutex);

    ESP_LOGI(TAG, "display from buffer (fast)");
    return ESP_OK;
}
