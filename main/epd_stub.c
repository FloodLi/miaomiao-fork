/**
 * EPD 驱动 — 主测 SSD1619 / UC8179，并接入多款 Waveshare 面板配置
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
#include <sys/stat.h>

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

/* ── 面板规格 / 回调 ─────────────────────────────────────────────── */

typedef enum {
    EPD_CMD_DATA_24_26,          /* black:0x24, red:0x26, red bit 1 = red */
    EPD_CMD_DATA_10_13,          /* black:0x10, red:0x13, red bit 1 = red */
    EPD_CMD_DATA_10_92_13,       /* black:0x10, 0x92, red:0x13, red bit 1 = red */
    EPD_CMD_DATA_10_13_INV_BW,   /* black:0x10, second pass is inverted BW */
    EPD_CMD_DATA_10_13_INV_RED,  /* black:0x10, red:0x13, red bit 0 = red */
} epd_data_mode_t;

typedef struct epd_panel_desc {
    int w;
    int h;
    bool has_red;
    int default_busy_idle;
    bool busy_probe_0x71;
    epd_data_mode_t data_mode;
    const char *name;
    void (*init)(void);
    void (*refresh)(void);
    void (*cursor_home)(void);
} epd_panel_desc_t;

static void ssd1619_init_sequence(void);
static void ssd1619_refresh(void);
static void ssd1619_cursor_home(void);
static void uc8179_init_sequence(void);
static void uc8179_refresh(void);
static void ws_29_bw_init(void);
static void ws_29_bwr_init(void);
static void ws_266_bw_init(void);
static void ws_266_bwr_init(void);
static void ws_27_bw_init(void);
static void ws_27_bwr_init(void);
static void ws_426_bw_init(void);
static void ws_583_bw_init(void);
static void ws_583_bwr_init(void);
static void ws_75_bw_init(void);
static void ws_75_bwr_init(void);
static void ws_42_uc8176_bwr_init(void);
static void refresh_0x20(void);
static void refresh_22_f7_20(void);
static void refresh_12(void);
static void cursor_x8_y16_home(void);
static void cursor_x8_y9_home(void);
static void cursor_x8_y9_x1_home(void);
static void cursor_x16_y16_home(void);
static void cursor_none(void);

static const epd_panel_desc_t s_specs[EPD_PANEL_COUNT] = {
    [EPD_PANEL_42_BWR]     = { 400, 300, true,  0, false, EPD_CMD_DATA_24_26,         "SSD1619 4.2\" BWR",         ssd1619_init_sequence, ssd1619_refresh, cursor_none },
    [EPD_PANEL_583_BWR]    = { 648, 480, true,  1, true,  EPD_CMD_DATA_10_13,         "UC8179 5.83\" BWR",         uc8179_init_sequence,  uc8179_refresh,  cursor_none },
    [EPD_PANEL_WS_29_BW]   = { 128, 296, false, 0, false, EPD_CMD_DATA_24_26,         "Waveshare 2.9\" BW V2",     ws_29_bw_init,        refresh_22_f7_20, cursor_x8_y16_home },
    [EPD_PANEL_WS_29_BWR]  = { 128, 296, true,  0, false, EPD_CMD_DATA_24_26,         "Waveshare 2.9\" BWR B V4",  ws_29_bwr_init,       refresh_22_f7_20, cursor_x8_y16_home },
    [EPD_PANEL_WS_266_BW]  = { 152, 296, false, 0, false, EPD_CMD_DATA_24_26,         "Waveshare 2.66\" BW",       ws_266_bw_init,       refresh_0x20,     cursor_x8_y9_x1_home },
    [EPD_PANEL_WS_266_BWR] = { 152, 296, true,  0, false, EPD_CMD_DATA_24_26,         "Waveshare 2.66\" BWR",      ws_266_bwr_init,      refresh_0x20,     cursor_x8_y9_home },
    [EPD_PANEL_WS_27_BW]   = { 176, 264, false, 0, false, EPD_CMD_DATA_24_26,         "Waveshare 2.7\" BW V2",     ws_27_bw_init,        refresh_22_f7_20, cursor_x8_y16_home },
    [EPD_PANEL_WS_27_BWR]  = { 176, 264, true,  0, false, EPD_CMD_DATA_24_26,         "Waveshare 2.7\" BWR B V2",  ws_27_bwr_init,       refresh_0x20,     cursor_x8_y16_home },
    [EPD_PANEL_WS_426_BW]  = { 800, 480, false, 0, false, EPD_CMD_DATA_24_26,         "Waveshare 4.26\" BW",       ws_426_bw_init,       refresh_22_f7_20, cursor_x16_y16_home },
    [EPD_PANEL_WS_583_BW]  = { 648, 480, false, 1, false, EPD_CMD_DATA_10_13,         "Waveshare 5.83\" BW V2",    ws_583_bw_init,       refresh_12,       cursor_none },
    [EPD_PANEL_WS_583_BWR] = { 648, 480, true,  1, true,  EPD_CMD_DATA_10_13,         "Waveshare 5.83\" BWR B V2", ws_583_bwr_init,      refresh_12,       cursor_none },
    [EPD_PANEL_WS_75_BW]   = { 800, 480, false, 1, false, EPD_CMD_DATA_10_13_INV_BW,  "Waveshare 7.5\" BW V2",     ws_75_bw_init,        refresh_12,       cursor_none },
    [EPD_PANEL_WS_75_BWR]  = { 800, 480, true,  1, true,  EPD_CMD_DATA_10_92_13,      "Waveshare 7.5\" BWR B V2",  ws_75_bwr_init,       refresh_12,       cursor_none },
    [EPD_PANEL_WS_42_BWR_UC8176] = { 400, 300, true, 1, false, EPD_CMD_DATA_10_13_INV_RED, "Waveshare 4.2\" BWR UC8176", ws_42_uc8176_bwr_init, refresh_12, cursor_none },
};

static bool panel_valid(epd_panel_t panel)
{
    return panel >= 0 && panel < EPD_PANEL_COUNT && s_specs[panel].name != NULL;
}

static const epd_panel_desc_t *panel_desc(void)
{
    if (!panel_valid(s_panel))
        s_panel = EPD_PANEL_42_BWR;
    return &s_specs[s_panel];
}

int  epd_width(void)       { return panel_desc()->w; }
int  epd_height(void)      { return panel_desc()->h; }
int  epd_plane_bytes(void) { return panel_desc()->w * panel_desc()->h / 8; }
bool epd_has_red(void)     { return panel_desc()->has_red; }
epd_panel_t epd_get_panel(void) { return s_panel; }

esp_err_t epd_save_panel_config(epd_panel_t panel, int busy_idle)
{
    if (!panel_valid(panel))
        return ESP_ERR_INVALID_ARG;
    if (busy_idle != 0 && busy_idle != 1)
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t e = nvs_open("epd", NVS_READWRITE, &h);
    if (e != ESP_OK)
        return e;
    e = nvs_set_u8(h, "panel", (uint8_t)panel);
    if (e == ESP_OK)
        e = nvs_set_u8(h, "busy_idle", (uint8_t)busy_idle);
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK) {
        ESP_LOGI(TAG, "Panel config saved for next boot: %s, busy_idle=%d",
                 s_specs[panel].name, busy_idle);
    }
    return e;
}

esp_err_t epd_set_panel(epd_panel_t panel)
{
    if (!panel_valid(panel))
        return ESP_ERR_INVALID_ARG;

    if (s_epd_mutex)
        xSemaphoreTake(s_epd_mutex, portMAX_DELAY);

    if (s_panel != panel) {
        fb_release_reserved_planes();
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

int epd_panel_default_busy_idle(epd_panel_t panel)
{
    if (!panel_valid(panel))
        return -1;
    return s_specs[panel].default_busy_idle;
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
        if (nvs_get_u8(h, "panel", &v) == ESP_OK && panel_valid((epd_panel_t)v))
            s_panel = (epd_panel_t)v;
        uint8_t bi;
        if (nvs_get_u8(h, "busy_idle", &bi) == ESP_OK && bi <= 1)
            s_busy_idle_level = (int)bi;
        nvs_close(h);
    }
}

void epd_load_panel_from_nvs(void)
{
    nvs_load_panel();
}

static int busy_idle_effective(void)
{
    if (s_busy_idle_level == 0 || s_busy_idle_level == 1)
        return s_busy_idle_level;
    return panel_desc()->default_busy_idle;
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
    const bool probe_0x71 = panel_desc()->busy_probe_0x71;
    const int idle = busy_idle_effective();

    if (probe_0x71)
        epd_cmd(0x71);

    /* 阶段 1：等到离开空闲态（进入忙），避免刷新命令刚发出时仍读到空闲 */
    if (after_update_cmd) {
        int phase1 = 0;
        const int phase1_max_ms = 800;
        while (gpio_get_level(PIN_EPD_BUSY) == idle && phase1 < phase1_max_ms) {
            delay_ms(20);
            phase1 += 20;
            if (probe_0x71)
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
        if (probe_0x71)
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

/* ── Waveshare-style helpers ───────────────────────────────────────── */

static void cmd1(uint8_t c, uint8_t v1)
{
    epd_cmd(c);
    epd_dat(v1);
}

static void cmd2(uint8_t c, uint8_t v1, uint8_t v2)
{
    epd_cmd(c);
    epd_dat(v1);
    epd_dat(v2);
}

static void cmd3(uint8_t c, uint8_t v1, uint8_t v2, uint8_t v3)
{
    epd_cmd(c);
    epd_dat(v1);
    epd_dat(v2);
    epd_dat(v3);
}

static void cmd4(uint8_t c, uint8_t v1, uint8_t v2, uint8_t v3, uint8_t v4)
{
    epd_cmd(c);
    epd_dat(v1);
    epd_dat(v2);
    epd_dat(v3);
    epd_dat(v4);
}

static void cmd5(uint8_t c, uint8_t v1, uint8_t v2, uint8_t v3, uint8_t v4, uint8_t v5)
{
    epd_cmd(c);
    epd_dat(v1);
    epd_dat(v2);
    epd_dat(v3);
    epd_dat(v4);
    epd_dat(v5);
}

static void cursor_none(void)
{
}

static void cursor_x8_y16_home(void)
{
    epd_cmd(0x4E);
    epd_dat(0x00);
    epd_cmd(0x4F);
    epd_dat(0x00);
    epd_dat(0x00);
}

static void cursor_x8_y9_home(void)
{
    epd_cmd(0x4E);
    epd_dat(0x00);
    epd_cmd(0x4F);
    epd_dat(0x00);
    epd_dat(0x00);
}

static void cursor_x8_y9_x1_home(void)
{
    epd_cmd(0x4E);
    epd_dat(0x01);
    epd_cmd(0x4F);
    epd_dat(0x00);
    epd_dat(0x00);
}

static void cursor_x16_y16_home(void)
{
    epd_cmd(0x4E);
    epd_dat(0x00);
    epd_dat(0x00);
    epd_cmd(0x4F);
    epd_dat(0x00);
    epd_dat(0x00);
}

static void refresh_0x20(void)
{
    epd_cmd(0x20);
    epd_wait_ready(true);
    delay_ms(200);
}

static void refresh_22_f7_20(void)
{
    cmd1(0x22, 0xF7);
    epd_cmd(0x20);
    epd_wait_ready(true);
    delay_ms(200);
}

static void refresh_12(void)
{
    epd_cmd(0x12);
    delay_ms(100);
    epd_wait_ready(true);
    delay_ms(200);
}

static void panel_write_plane(uint8_t cmd, const uint8_t *data, uint8_t fill, bool invert, int pb)
{
    epd_cmd(cmd);
    if (data && !invert) {
        epd_bulk(data, (size_t)pb);
        return;
    }
    if (!data) {
        epd_fill(fill, (size_t)pb);
        return;
    }

    uint8_t *buf = heap_caps_malloc(EPD_FILL_CHUNK, MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE(TAG, "invert plane: DMA malloc failed");
        return;
    }
    size_t offset = 0;
    while (offset < (size_t)pb) {
        size_t n = ((size_t)pb - offset > EPD_FILL_CHUNK) ? EPD_FILL_CHUNK : ((size_t)pb - offset);
        for (size_t i = 0; i < n; i++)
            buf[i] = (uint8_t)~data[offset + i];
        epd_bulk(buf, n);
        offset += n;
    }
    free(buf);
}

static void panel_write_bw_red_data(const uint8_t *bw, const uint8_t *red, int pb)
{
    const epd_panel_desc_t *d = panel_desc();
    if (d->cursor_home)
        d->cursor_home();

    switch (d->data_mode) {
    case EPD_CMD_DATA_24_26:
        panel_write_plane(0x24, bw, 0xFF, false, pb);
        if (d->has_red) {
            if (d->cursor_home)
                d->cursor_home();
            panel_write_plane(0x26, red, 0x00, false, pb);
        }
        break;
    case EPD_CMD_DATA_10_13:
        panel_write_plane(0x10, bw, 0xFF, false, pb);
        panel_write_plane(0x13, d->has_red ? red : bw, d->has_red ? 0x00 : 0xFF, false, pb);
        break;
    case EPD_CMD_DATA_10_92_13:
        panel_write_plane(0x10, bw, 0xFF, false, pb);
        epd_cmd(0x92);
        panel_write_plane(0x13, red, 0x00, false, pb);
        break;
    case EPD_CMD_DATA_10_13_INV_BW:
        panel_write_plane(0x10, bw, 0xFF, false, pb);
        panel_write_plane(0x13, bw, 0x00, true, pb);
        break;
    case EPD_CMD_DATA_10_13_INV_RED:
        panel_write_plane(0x10, bw, 0xFF, false, pb);
        panel_write_plane(0x13, red, 0xFF, true, pb);
        break;
    }
}

static void panel_write_bw_red(const uint8_t *bw, const uint8_t *red, int pb)
{
    panel_write_bw_red_data(bw, red, pb);
    panel_desc()->refresh();
}

static esp_err_t file_send_plane(FILE *f, int pb, bool invert)
{
    uint8_t *buf = heap_caps_malloc(EPD_FILL_CHUNK, MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE(TAG, "file plane DMA malloc failed");
        return ESP_ERR_NO_MEM;
    }

    gpio_set_level(PIN_EPD_DC, 1);
    size_t sent = 0;
    esp_err_t ret = ESP_OK;
    while (sent < (size_t)pb) {
        size_t n = ((size_t)pb - sent > EPD_FILL_CHUNK) ? EPD_FILL_CHUNK : ((size_t)pb - sent);
        size_t got = fread(buf, 1, n, f);
        if (got != n) {
            ESP_LOGE(TAG, "file plane short: %u / %u", (unsigned)got, (unsigned)n);
            ret = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (invert) {
            for (size_t i = 0; i < n; i++)
                buf[i] = (uint8_t)~buf[i];
        }
        ret = spi_tx(buf, n);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "file plane spi err %s", esp_err_to_name(ret));
            break;
        }
        sent += n;
    }

    free(buf);
    return ret;
}

static esp_err_t panel_write_bw_red_file(FILE *f, int pb, bool has_red_file)
{
    const epd_panel_desc_t *d = panel_desc();
    esp_err_t ret = ESP_OK;
    if (d->cursor_home)
        d->cursor_home();

    switch (d->data_mode) {
    case EPD_CMD_DATA_24_26:
        epd_cmd(0x24);
        ret = file_send_plane(f, pb, false);
        if (ret != ESP_OK)
            return ret;
        if (d->has_red) {
            if (d->cursor_home)
                d->cursor_home();
            epd_cmd(0x26);
            ret = has_red_file ? file_send_plane(f, pb, false)
                               : (epd_fill(0x00, (size_t)pb), ESP_OK);
        }
        break;
    case EPD_CMD_DATA_10_13:
        epd_cmd(0x10);
        ret = file_send_plane(f, pb, false);
        if (ret != ESP_OK)
            return ret;
        epd_cmd(0x13);
        if (d->has_red) {
            ret = has_red_file ? file_send_plane(f, pb, false)
                               : (epd_fill(0x00, (size_t)pb), ESP_OK);
        } else {
            if (fseek(f, 0, SEEK_SET) != 0)
                return ESP_FAIL;
            ret = file_send_plane(f, pb, false);
        }
        break;
    case EPD_CMD_DATA_10_92_13:
        epd_cmd(0x10);
        ret = file_send_plane(f, pb, false);
        if (ret != ESP_OK)
            return ret;
        epd_cmd(0x92);
        epd_cmd(0x13);
        ret = has_red_file ? file_send_plane(f, pb, false)
                           : (epd_fill(0x00, (size_t)pb), ESP_OK);
        break;
    case EPD_CMD_DATA_10_13_INV_BW:
        epd_cmd(0x10);
        ret = file_send_plane(f, pb, false);
        if (ret != ESP_OK)
            return ret;
        if (fseek(f, 0, SEEK_SET) != 0)
            return ESP_FAIL;
        epd_cmd(0x13);
        ret = file_send_plane(f, pb, true);
        break;
    case EPD_CMD_DATA_10_13_INV_RED:
        epd_cmd(0x10);
        ret = file_send_plane(f, pb, false);
        if (ret != ESP_OK)
            return ret;
        epd_cmd(0x13);
        if (has_red_file)
            ret = file_send_plane(f, pb, true);
        else
            ret = (epd_fill(0xFF, (size_t)pb), ESP_OK);
        break;
    }

    if (ret == ESP_OK)
        d->refresh();
    return ret;
}

static void panel_fill_bw_red(uint8_t bw_byte, uint8_t red_byte, int pb)
{
    const epd_panel_desc_t *d = panel_desc();
    if (d->cursor_home)
        d->cursor_home();

    switch (d->data_mode) {
    case EPD_CMD_DATA_24_26:
        panel_write_plane(0x24, NULL, bw_byte, false, pb);
        if (d->has_red) {
            if (d->cursor_home)
                d->cursor_home();
            panel_write_plane(0x26, NULL, red_byte, false, pb);
        }
        break;
    case EPD_CMD_DATA_10_13:
        panel_write_plane(0x10, NULL, bw_byte, false, pb);
        panel_write_plane(0x13, NULL, d->has_red ? red_byte : bw_byte, false, pb);
        break;
    case EPD_CMD_DATA_10_92_13:
        panel_write_plane(0x10, NULL, bw_byte, false, pb);
        epd_cmd(0x92);
        panel_write_plane(0x13, NULL, red_byte, false, pb);
        break;
    case EPD_CMD_DATA_10_13_INV_BW:
        panel_write_plane(0x10, NULL, bw_byte, false, pb);
        panel_write_plane(0x13, NULL, (uint8_t)~bw_byte, false, pb);
        break;
    case EPD_CMD_DATA_10_13_INV_RED:
        panel_write_plane(0x10, NULL, bw_byte, false, pb);
        panel_write_plane(0x13, NULL, (uint8_t)~red_byte, false, pb);
        break;
    }

    d->refresh();
}

static void ws_common_2in9_bw_address(void)
{
    cmd3(0x01, 0x27, 0x01, 0x00);
    cmd1(0x11, 0x03);
    cmd2(0x44, 0x00, 0x0F);
    cmd4(0x45, 0x00, 0x00, 0x27, 0x01);
    cmd2(0x21, 0x00, 0x80);
    cursor_x8_y16_home();
}

static void ws_common_2in9_bwr_address(void)
{
    ws_common_2in9_bw_address();
    cmd1(0x3C, 0x05);
    cmd1(0x18, 0x80);
}

static void ws_29_bw_init(void)
{
    epd_hw_reset();
    epd_wait_ready(false);
    epd_cmd(0x12);
    epd_wait_ready(false);
    ws_common_2in9_bw_address();
    ESP_LOGI(TAG, "Waveshare 2.9 BW V2 init OK");
}

static void ws_29_bwr_init(void)
{
    epd_hw_reset();
    epd_wait_ready(false);
    epd_cmd(0x12);
    epd_wait_ready(false);
    ws_common_2in9_bwr_address();
    ESP_LOGI(TAG, "Waveshare 2.9 BWR B V4 init OK");
}

static void ws_266_bw_init(void)
{
    epd_hw_reset();
    epd_wait_ready(false);
    epd_cmd(0x12);
    epd_wait_ready(false);
    cmd1(0x11, 0x03);
    cmd2(0x44, 0x01, 0x13);
    cmd4(0x45, 0x00, 0x00, 0x28, 0x01);
    cursor_x8_y9_x1_home();
    ESP_LOGI(TAG, "Waveshare 2.66 BW init OK");
}

static void ws_266_bwr_init(void)
{
    epd_hw_reset();
    epd_wait_ready(false);
    epd_cmd(0x12);
    epd_wait_ready(false);
    cmd1(0x11, 0x03);
    cmd2(0x44, 0x00, 0x12);
    cmd4(0x45, 0x00, 0x00, 0x27, 0x01);
    cmd2(0x21, 0x00, 0x80);
    cursor_x8_y9_home();
    ESP_LOGI(TAG, "Waveshare 2.66 BWR init OK");
}

static void ws_27_bw_init(void)
{
    epd_hw_reset();
    epd_wait_ready(false);
    epd_cmd(0x12);
    epd_wait_ready(false);
    cmd4(0x45, 0x00, 0x00, 0x07, 0x01);
    cmd2(0x4F, 0x00, 0x00);
    cmd1(0x11, 0x03);
    ESP_LOGI(TAG, "Waveshare 2.7 BW V2 init OK");
}

static void ws_27_bwr_init(void)
{
    epd_hw_reset();
    epd_wait_ready(false);
    epd_cmd(0x12);
    epd_wait_ready(false);
    cmd3(0x00, 0x27, 0x01, 0x00);
    cmd1(0x11, 0x03);
    cmd2(0x44, 0x00, 0x15);
    cmd4(0x45, 0x00, 0x00, 0x07, 0x01);
    cursor_x8_y16_home();
    ESP_LOGI(TAG, "Waveshare 2.7 BWR B V2 init OK");
}

static void ws_426_bw_init(void)
{
    epd_hw_reset();
    delay_ms(100);
    epd_wait_ready(false);
    epd_cmd(0x12);
    epd_wait_ready(false);
    cmd1(0x18, 0x80);
    cmd5(0x0C, 0xAE, 0xC7, 0xC3, 0xC0, 0x80);
    cmd3(0x01, 0xDF, 0x01, 0x02);
    cmd1(0x3C, 0x01);
    cmd1(0x11, 0x01);
    epd_cmd(0x44);
    epd_dat(0x00); epd_dat(0x00); epd_dat(0x1F); epd_dat(0x03);
    epd_cmd(0x45);
    epd_dat(0xDF); epd_dat(0x01); epd_dat(0x00); epd_dat(0x00);
    cursor_x16_y16_home();
    ESP_LOGI(TAG, "Waveshare 4.26 BW init OK");
}

static void ws_583_bw_init(void)
{
    epd_hw_reset();
    cmd1(0x00, 0x1F);
    cmd2(0x50, 0x21, 0x07);
    epd_cmd(0x04);
    delay_ms(100);
    epd_wait_ready(false);
    ESP_LOGI(TAG, "Waveshare 5.83 BW V2 init OK");
}

static void ws_583_bwr_init(void)
{
    epd_hw_reset();
    cmd4(0x01, 0x07, 0x07, 0x3F, 0x3F);
    epd_cmd(0x04);
    delay_ms(100);
    epd_wait_ready(false);
    cmd1(0x00, 0x0F);
    cmd4(0x61, 0x02, 0x88, 0x01, 0xE0);
    cmd1(0x15, 0x00);
    cmd2(0x50, 0x11, 0x07);
    cmd1(0x60, 0x22);
    ESP_LOGI(TAG, "Waveshare 5.83 BWR B V2 init OK");
}

static void ws_75_bw_init(void)
{
    epd_hw_reset();
    cmd4(0x01, 0x07, 0x07, 0x3F, 0x3F);
    cmd4(0x06, 0x17, 0x17, 0x28, 0x17);
    epd_cmd(0x04);
    delay_ms(100);
    epd_wait_ready(false);
    cmd1(0x00, 0x1F);
    cmd4(0x61, 0x03, 0x20, 0x01, 0xE0);
    cmd1(0x15, 0x00);
    cmd2(0x50, 0x10, 0x07);
    cmd1(0x60, 0x22);
    ESP_LOGI(TAG, "Waveshare 7.5 BW V2 init OK");
}

static void ws_75_bwr_init(void)
{
    epd_hw_reset();
    cmd4(0x01, 0x07, 0x07, 0x3F, 0x3F);
    epd_cmd(0x04);
    delay_ms(100);
    epd_wait_ready(false);
    cmd1(0x00, 0x0F);
    cmd4(0x61, 0x03, 0x20, 0x01, 0xE0);
    cmd1(0x15, 0x00);
    cmd2(0x50, 0x11, 0x07);
    cmd1(0x60, 0x22);
    epd_cmd(0x65);
    epd_dat(0x00); epd_dat(0x00); epd_dat(0x00); epd_dat(0x00);
    ESP_LOGI(TAG, "Waveshare 7.5 BWR B V2 init OK");
}

static void ws_42_uc8176_bwr_init(void)
{
    epd_hw_reset();
    cmd3(0x06, 0x17, 0x17, 0x17);
    epd_cmd(0x04);
    epd_wait_ready(false);
    cmd1(0x00, 0x0F);
    ESP_LOGI(TAG, "Waveshare 4.2 BWR UC8176 init OK");
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



/* ── 完整初始化（每面板一次；换面板或 NVS 覆盖后需重跑） ───────────── */

static void panel_run_full_init(void)
{
    panel_desc()->init();
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
    panel_fill_bw_red(0xFF, 0x00, epd_plane_bytes());
}

bool epd_is_ready(void)
{
    return s_epd_ready;
}

bool epd_wait_idle(uint32_t timeout_ms)
{
    if (!s_epd_mutex || !s_epd_ready)
        return false;
    if (xSemaphoreTake(s_epd_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
        return false;
    xSemaphoreGive(s_epd_mutex);
    return true;
}

static bool fb_matches_current_panel(const fb_t *fb)
{
    return fb &&
           fb->width == epd_width() &&
           fb->height == epd_height() &&
           fb->plane_bytes == epd_plane_bytes() &&
           fb->row_bytes == epd_width() / 8;
}

/* ── 对外 API ─────────────────────────────────────────────────────── */

esp_err_t epd_init(void)
{
    s_epd_ready = false;

    if (!s_epd_mutex) {
        s_epd_mutex = xSemaphoreCreateMutex();
        if (!s_epd_mutex)
            return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_EPD_DC) | (1ULL << PIN_EPD_CS) | (1ULL << PIN_EPD_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&out);
    if (err != ESP_OK)
        goto fail;

    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_EPD_BUSY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1,
    };
    err = gpio_config(&in);
    if (err != ESP_OK)
        goto fail;

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_EPD_SCK,
        .mosi_io_num = PIN_EPD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_SPI_MAX_TX,
    };
    err = spi_bus_initialize(EPD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK)
        goto fail;

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = EPD_SPI_HZ,
        .mode = 0,
        .spics_io_num = PIN_EPD_CS,
        .queue_size = 1,
    };
    err = spi_bus_add_device(EPD_SPI_HOST, &devcfg, &s_spi);
    if (err != ESP_OK)
        goto fail;

    nvs_load_panel();

    ESP_LOGI(TAG, "EPD start: %s %dx%d, BUSY idle_level=%d (4.2\" default 0=Waveshare; NVS busy_idle=1 if GDE idle-high)",
              s_specs[s_panel].name, epd_width(), epd_height(), busy_idle_effective());

    panel_run_full_init();
    epd_clear_to_white_locked();

    s_epd_ready = true;
    xSemaphoreGive(s_epd_mutex);

    return ESP_OK;

fail:
    ESP_LOGE(TAG, "EPD init failed: %s", esp_err_to_name(err));
    xSemaphoreGive(s_epd_mutex);
    return err;
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

    uint8_t *red = heap_caps_calloc(1, (size_t)pb, MALLOC_CAP_DMA);
    if (!red) {
        free(buf);
        xSemaphoreGive(s_epd_mutex);
        return ESP_ERR_NO_MEM;
    }
    for (int y = mid_y + 20; y < H - 20; y++) {
        for (int x = mid_x + 20; x < W - 20; x++) {
            size_t off = (size_t)y * rb + (size_t)(x >> 3);
            uint8_t bit = (uint8_t)(0x80u >> (x & 7));
            red[off] |= bit;
            buf[off] |= bit;
        }
    }

    panel_ensure_ready();

    panel_write_bw_red(buf, red, pb);

    free(red);
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

    struct stat st;
    bool have_size = (stat(path, &st) == 0);
    if (have_size && st.st_size < pb) {
        ESP_LOGE(TAG, "file too small: %ld / %u", (long)st.st_size, (unsigned)pb);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    bool has_red_file = have_size && st.st_size >= (off_t)pb * 2;
    if (epd_has_red() && !has_red_file)
        ESP_LOGW(TAG, "red plane missing, zero fill");

    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);
    if (pb != epd_plane_bytes()) {
        ESP_LOGE(TAG, "file/panel mismatch: file plane=%d current=%d",
                 pb, epd_plane_bytes());
        xSemaphoreGive(s_epd_mutex);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    panel_ensure_ready();
    esp_err_t ret = panel_write_bw_red_file(f, pb, has_red_file);
    xSemaphoreGive(s_epd_mutex);
    fclose(f);
    if (ret != ESP_OK)
        return ret;

    ESP_LOGI(TAG, "display from %s", path);
    return ESP_OK;
}

esp_err_t epd_display_from_buffer(const fb_t *fb)
{
    if (!fb) return ESP_ERR_INVALID_ARG;
    if (!s_epd_mutex || !s_epd_ready)
        return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);
    if (!fb_matches_current_panel(fb)) {
        ESP_LOGE(TAG, "fb/panel mismatch: fb=%dx%d/%d current=%dx%d/%d",
                 fb->width, fb->height, fb->plane_bytes,
                 epd_width(), epd_height(), epd_plane_bytes());
        xSemaphoreGive(s_epd_mutex);
        return ESP_ERR_INVALID_SIZE;
    }
    int pb = epd_plane_bytes();
    panel_ensure_ready();

    panel_write_bw_red(fb->black, fb->red, pb);

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

    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);
    if (!fb_matches_current_panel(fb)) {
        ESP_LOGE(TAG, "fb/panel mismatch: fb=%dx%d/%d current=%dx%d/%d",
                 fb->width, fb->height, fb->plane_bytes,
                 epd_width(), epd_height(), epd_plane_bytes());
        xSemaphoreGive(s_epd_mutex);
        fb_destroy(fb);
        return ESP_ERR_INVALID_SIZE;
    }
    int pb = epd_plane_bytes();
    panel_ensure_ready();

    panel_write_bw_red_data(fb->black, fb->red, pb);
    fb_destroy(fb);
    panel_desc()->refresh();

    xSemaphoreGive(s_epd_mutex);

    ESP_LOGI(TAG, "display from buffer");
    return ESP_OK;
}

esp_err_t epd_display_from_buffer_fast(const fb_t *fb)
{
    if (!fb) return ESP_ERR_INVALID_ARG;
    if (!s_epd_mutex || !s_epd_ready)
        return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);
    if (!fb_matches_current_panel(fb)) {
        ESP_LOGE(TAG, "fb/panel mismatch: fb=%dx%d/%d current=%dx%d/%d",
                 fb->width, fb->height, fb->plane_bytes,
                 epd_width(), epd_height(), epd_plane_bytes());
        xSemaphoreGive(s_epd_mutex);
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_panel != EPD_PANEL_42_BWR) {
        xSemaphoreGive(s_epd_mutex);
        ESP_LOGW(TAG, "fast refresh only for SSD1619, falling back to full");
        return epd_display_from_buffer(fb);
    }

    int pb = epd_plane_bytes();
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
