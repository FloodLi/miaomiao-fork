/**
 * EPD 驱动 — 主测 SSD1619 / UC8179，并接入 ATC/Solum 面板配置
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
#include <stdatomic.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "power_mgr.h"
#include "display_policy.h"
#include "ui_theme.h"

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
#define EPD_BUSY_UPDATE_TIMEOUT_MS  12000
#define EPD_UC8179_75_BUSY_UPDATE_TIMEOUT_MS  35000
#define EPD_JD796XX_BUSY_UPDATE_TIMEOUT_MS  120000
#define EPD_SSD1667_102_BUSY_UPDATE_TIMEOUT_MS  120000
#define EPD_SSD2677_97_BUSY_UPDATE_TIMEOUT_MS  120000
#define EPD_ATC29_BUSY_TIMEOUT_MS  2500
#define EPD_ATC29_SAFE_REFRESH_MS  12000
#define EPD_E6_BUSY_TIMEOUT_MS     180000
#define HINK29_REFRESH_MS          15000
#define EPD_BUSY_FALLBACK_REFRESH_MS  14000
#define HINK29_LOGICAL_W 128
#define HINK29_LOGICAL_H 296
#define SSD16XX_213_RAM_W     128
#define SSD16XX_213_ACTIVE_W  122
#define SSD16XX_213_H         250
#define SSD16XX_213_X_OFFSET  8
#define EPD_REPAIR_MAX_CYCLES      8
#define UC8176_EXPERIMENTAL_REFRESH_LIMIT  8
#define UC8179_75_PARTIAL_REFRESH_LIMIT     8
#define UC8253_PARTIAL_REFRESH_LIMIT       5
#define JD79686A_PARTIAL_REFRESH_LIMIT     8

static const char *TAG = "epd";

static spi_device_handle_t s_spi;
static bool               s_spi_bus_initialized;
static SemaphoreHandle_t   s_epd_mutex;
static epd_panel_t         s_panel = EPD_PANEL_42_BWR;

/** 已为当前面板跑过完整上电初始化序列（避免每次刷屏都硬件复位） */
static bool s_panel_seq_done;

/** epd_init() 完全结束后再为 true，供 HTTP 与其它任务判断 */
static bool s_epd_ready;

static atomic_bool s_force_full_refresh_next = ATOMIC_VAR_INIT(false);

static uint8_t    *s_previous_bw;
static int         s_previous_bw_bytes;
static epd_panel_t s_previous_bw_panel = EPD_PANEL_COUNT;
static bool        s_previous_bw_valid;
static unsigned    s_experimental_refresh_count;
static unsigned    s_uc8179_partial_refresh_count;
static unsigned    s_uc8253_partial_refresh_count;
static unsigned    s_jd79686a_partial_refresh_count;
static bool        s_uc8176_register_lut_active;
static bool        s_uc8179_register_lut_active;
static uint64_t    s_uc8179_red_signature;
static bool        s_uc8179_red_signature_valid;
static uint64_t    s_jd79686a_red_signature;
static bool        s_jd79686a_red_signature_valid;
/** JD79686A 上一帧红平面内容（仅供网页局刷试验按钮组包用，见 jd79686a_prev_red_store） */
static uint8_t    *s_jd79686a_prev_red;
static int         s_jd79686a_prev_red_bytes;
static bool        s_last_wait_saw_busy_phase = true;

/**
 * NVS 键 busy_idle：0 = 空闲时 BUSY 为低；1 = 空闲时为高。
 * 未配置时：SSD1619 4.2 默认 0（与 Waveshare 官方一致）；5.83 UC8179 默认 1。
 */
static int s_busy_idle_level = -1;

static int busy_idle_effective(void);

/* ── 面板规格 / 回调 ─────────────────────────────────────────────── */

typedef enum {
    EPD_CMD_DATA_24_26,          /* black:0x24, red:0x26, red bit 1 = red */
    EPD_CMD_DATA_24_26_SSD16XX,  /* SSD16xx 0x24/0x26; SSD1683 BWR uses red bit 1 = red */
    EPD_CMD_DATA_10_13,          /* black:0x10, red:0x13, red bit 1 = red */
    EPD_CMD_DATA_10_92_13,       /* black:0x10, partial-out marker, red:0x13 */
    EPD_CMD_DATA_10_13_INV_RED,  /* black:0x10, red:0x13, red bit 0 = red */
    EPD_CMD_DATA_UC81XX_REF,     /* UC8176/UC8179 reference path with partial window */
    EPD_CMD_DATA_UC8179_75_BWR,  /* GDEY075Z08: black 0x10, inverted red 0x13 */
    EPD_CMD_DATA_10_13_DUMMY,    /* dummy data byte, black:0x10, red:0x13 */
    EPD_CMD_DATA_10_13_INV_BLACK_DUMMY, /* dummy byte, inverted black on 0x10 */
    EPD_CMD_DATA_DUAL_SSD,       /* split 792x272 frame over two SSD controllers */
    EPD_CMD_DATA_UC8159_INTERLEAVED, /* 4 bits/pixel UC8159 stream on 0x10, two pixels/byte */
    EPD_CMD_DATA_JD796XX_2BPP,   /* JD79665/668 native 2-bit-per-pixel stream on 0x10 */
    EPD_CMD_DATA_SSD2677_2BPP,   /* SSD2677/2667 9.7" BWRY 2-bit-per-pixel stream on 0x10 */
    EPD_CMD_DATA_E6_3IN7_6C,     /* E6 3.7" six-color 2-bit compact stream, two refresh passes */
    EPD_CMD_DATA_UC8253_BW,      /* UC8253 old/new BW planes on 0x10/0x13 */
    EPD_CMD_DATA_JD79686A_BWR,     /* JD79686A: BW->0x10, Red->0x13 */
} epd_data_mode_t;

typedef struct epd_panel_desc {
    int w;
    int h;
    bool has_red;
    int default_busy_idle;
    bool busy_probe_0x71;
    epd_data_mode_t data_mode;
    const char *name;
    esp_err_t (*init)(void);
    esp_err_t (*refresh)(void);
    esp_err_t (*cursor_home)(void);
    uint16_t x_offset;
    bool mirror_v;
    bool has_yellow;
    /* FULL is always available; optional modes stay disabled unless whitelisted. */
    uint32_t optional_refresh_caps;
} epd_panel_desc_t;

static esp_err_t ssd1619_init_sequence(void);
static esp_err_t ssd1619_refresh(void);
static esp_err_t ssd1619_cursor_home(void);
static esp_err_t uc8179_init_sequence(void);
static esp_err_t uc8179_refresh(void);
static esp_err_t ws_426_bw_init(void);
static esp_err_t ws_583_bwr_init(void);
static esp_err_t atc_unissd_init(void);
static esp_err_t hink_ssd1619_29_init(void);
static esp_err_t hink_ssd1619_29_refresh(void);
static esp_err_t ssd16xx_213_bw_init(void);
static esp_err_t ssd16xx_213_bw_refresh(void);
static esp_err_t gd7965_uc8179_75_bw_init(void);
static esp_err_t atc_unissd_97_init(void);
static esp_err_t atc_ucvar29_init(void);
static esp_err_t atc_ucvar43_init(void);
static esp_err_t atc_dualssd_init(void);
static esp_err_t atc_uc8159_init(void);
static esp_err_t atc_uc8179_init(void);
static esp_err_t ref_uc81xx_init(void);
static esp_err_t ref_uc8179_75_bwr_init(void);
static esp_err_t ref_uc8159_init(void);
static esp_err_t ref_ssd1619_init(void);
static esp_err_t ref_ssd1683_init(void);
static esp_err_t fpc194_ssd1683_init(void);
static esp_err_t ref_ssd1677_init(void);
static esp_err_t ref_ssd1667_102_init(void);
static esp_err_t ref_ssd2677_97_init(void);
static esp_err_t ref_ssd2677_97_sleep(void);
static esp_err_t e6_37_init(void);
static esp_err_t uc8253_init(void);
static esp_err_t ref_jd79665_init(void);
static esp_err_t ref_jd79668_init(void);
static esp_err_t jd79686a_583_bwr_init(void);
static esp_err_t jd79686a_583_bwr_refresh(void);
static esp_err_t refresh_22_f7_20(void);
static esp_err_t refresh_12(void);
static esp_err_t refresh_power_on_12(void);
static esp_err_t ref_uc81xx_refresh(void);
static esp_err_t ref_uc8179_75_bwr_refresh(void);
static esp_err_t ref_ssd16xx_refresh(void);
static esp_err_t ref_ssd1683_refresh(void);
static esp_err_t fpc194_ssd1683_refresh(void);
static esp_err_t ref_ssd1667_102_refresh(void);
static esp_err_t ref_ssd2677_97_refresh(void);
static esp_err_t e6_37_refresh(void);
static esp_err_t uc8253_refresh(void);
static esp_err_t ref_jd796xx_refresh(void);
static esp_err_t refresh_atc_dualssd(void);
static esp_err_t panel_ensure_ready(void);
static esp_err_t panel_run_full_init(void);
static esp_err_t atc_unissd_cursor_home(void);
static esp_err_t hink_ssd1619_29_cursor_home(void);
static esp_err_t ssd16xx_213_bw_cursor_home(void);
static esp_err_t atc_unissd_97_cursor_home(void);
static esp_err_t ref_uc81xx_cursor_home(void);
static esp_err_t ref_ssd1619_cursor_home(void);
static esp_err_t ref_ssd1683_cursor_home(void);
static esp_err_t fpc194_ssd1683_cursor_home(void);
static esp_err_t ref_ssd1677_cursor_home(void);
static esp_err_t ref_ssd1667_102_cursor_home(void);
static esp_err_t ref_ssd2677_97_cursor_home(void);
static esp_err_t ref_jd796xx_cursor_home(void);
static esp_err_t ws_426_bw_cursor_home(void);
static esp_err_t cursor_none(void);

static const epd_panel_desc_t s_specs[EPD_PANEL_COUNT] = {
    [EPD_PANEL_42_BWR]     = { 400, 300, true,  0, false, EPD_CMD_DATA_24_26,         "SSD1619 4.2\" BWR",         ssd1619_init_sequence, ssd1619_refresh, cursor_none },
    [EPD_PANEL_583_BWR]    = { 648, 480, true,  1, true,  EPD_CMD_DATA_10_13,         "UC8179 5.83\" BWR",         uc8179_init_sequence,  uc8179_refresh,  cursor_none },
    /* Waveshare EPD_4in26 full-refresh sequence; optional modes await this panel's real-screen validation. */
    [EPD_PANEL_WS_426_BW]  = { 800, 480, false, 0, false, EPD_CMD_DATA_24_26,         "P426010 4.26\" BW",          ws_426_bw_init,        refresh_22_f7_20, ws_426_bw_cursor_home, .optional_refresh_caps = 0 },
    [EPD_PANEL_WS_583_BWR] = { 648, 480, true,  1, true,  EPD_CMD_DATA_10_13,         "Waveshare 5.83\" BWR B V2", ws_583_bwr_init,      refresh_12,       cursor_none },
    [EPD_PANEL_ATC_SSD1619_16_BWR] = { 200, 200, true,  0, false, EPD_CMD_DATA_24_26, "ATC/Solum 1.6\" BWR SSD1619", atc_unissd_init, refresh_22_f7_20, atc_unissd_cursor_home, 0, true },
    [EPD_PANEL_ATC_SSD1619_22_BWR] = { 160, 296, true,  0, false, EPD_CMD_DATA_24_26, "ATC/Solum 2.2\" BWR SSD1619", atc_unissd_init, refresh_22_f7_20, atc_unissd_cursor_home, 8, false },
    [EPD_PANEL_ATC_SSD1619_26_BWR] = { 184, 360, true,  0, false, EPD_CMD_DATA_24_26, "ATC/Solum 2.6\" BWR SSD1619", atc_unissd_init, refresh_22_f7_20, atc_unissd_cursor_home, 8, false },
    [EPD_PANEL_HINK_SSD1619_29_BWR] = { HINK29_LOGICAL_W, HINK29_LOGICAL_H, true,  0, false, EPD_CMD_DATA_24_26, "HINK-E029A14-A1 2.9\" BWR SSD1619", hink_ssd1619_29_init, hink_ssd1619_29_refresh, hink_ssd1619_29_cursor_home, 8, false },
    [EPD_PANEL_ATC_UC8151_29_BWR]  = { 168, 384, true,  1, false, EPD_CMD_DATA_10_13_INV_BLACK_DUMMY, "ATC/Solum 2.9\" BWR UC8151", atc_ucvar29_init, refresh_power_on_12, cursor_none },
    [EPD_PANEL_ATC_UCVAR43_BWR]    = { 152, 522, true,  1, false, EPD_CMD_DATA_10_13_DUMMY, "ATC/Solum 4.3\" BWR UC", atc_ucvar43_init, refresh_power_on_12, cursor_none },
    [EPD_PANEL_ATC_DUALSSD_585_BWR] = { 792, 272, true, 0, false, EPD_CMD_DATA_DUAL_SSD, "ATC/Solum 5.85\" BWR dual SSD", atc_dualssd_init, refresh_atc_dualssd, cursor_none },
    [EPD_PANEL_ATC_DUALSSD_585_BW]  = { 792, 272, false, 0, false, EPD_CMD_DATA_DUAL_SSD, "ATC/Solum 5.85\" BW dual SSD", atc_dualssd_init, refresh_atc_dualssd, cursor_none },
    [EPD_PANEL_ATC_UC8159_60_BWR]  = { 600, 448, true,  1, false, EPD_CMD_DATA_UC8159_INTERLEAVED, "ATC/Solum 6.0\" BWR UC8159", atc_uc8159_init, refresh_12, cursor_none },
    [EPD_PANEL_ATC_UC8179_74_BWR]  = { 800, 480, true,  1, false, EPD_CMD_DATA_10_13, "ATC/Solum 7.4\" BWR UC8179", atc_uc8179_init, refresh_power_on_12, cursor_none },
    [EPD_PANEL_ATC_SSD_97_BWR]     = { 960, 672, true,  0, false, EPD_CMD_DATA_24_26, "ATC/Solum 9.7\" BWR SSD", atc_unissd_97_init, refresh_22_f7_20, atc_unissd_97_cursor_home },
    [EPD_PANEL_ATC_PEGHOOK_13_BWR] = { 144, 200, true,  0, false, EPD_CMD_DATA_24_26, "ATC/Solum 1.3\" peghook BWR SSD", atc_unissd_init, refresh_22_f7_20, atc_unissd_cursor_home, 8, false },
    [EPD_PANEL_REF_UC8176_42_BW]      = { 400, 300, false, 1, false, EPD_CMD_DATA_UC81XX_REF, "UC8176 4.2\" BW", ref_uc81xx_init, ref_uc81xx_refresh, ref_uc81xx_cursor_home, .optional_refresh_caps = EPD_REFRESH_CAP_FAST_BW | EPD_REFRESH_CAP_PARTIAL_BW },
    [EPD_PANEL_REF_UC8176_42_BWR]     = { 400, 300, true,  1, false, EPD_CMD_DATA_UC81XX_REF, "UC 4.2\" BWR reference", ref_uc81xx_init, ref_uc81xx_refresh, ref_uc81xx_cursor_home },
    [EPD_PANEL_REF_UC8179_75_BW]      = { 800, 480, false, 1, false, EPD_CMD_DATA_UC81XX_REF, "UC8179 7.5\" BW", ref_uc81xx_init, ref_uc81xx_refresh, ref_uc81xx_cursor_home },
    [EPD_PANEL_REF_UC8179_75_BWR]     = { 800, 480, true,  1, true,  EPD_CMD_DATA_UC8179_75_BWR, "GDEY075Z08 7.5\" HD BWR UC8179", ref_uc8179_75_bwr_init, ref_uc8179_75_bwr_refresh, cursor_none, .optional_refresh_caps = EPD_REFRESH_CAP_PARTIAL_COLOR },
    /* EI0583T5PCZ09230H label maps to the WF0583T5PCZ09230HU59 600x448 BW family.
     * The controller is not printed on the label, so keep this UC8159-compatible
     * bring-up path full-refresh-only. Real-panel logs confirm BUSY is active-low
     * and returns high when the controller is ready. */
    [EPD_PANEL_VISIONPEAK_EI0583T5PCZ_BW] = { 600, 448, false, 1, false, EPD_CMD_DATA_UC8159_INTERLEAVED, "Vision Peak EI0583T5PCZ 5.83\" BW", ref_uc8159_init, ref_uc81xx_refresh, ref_uc81xx_cursor_home, .optional_refresh_caps = 0 },
    [EPD_PANEL_REF_UC8159_75_LOW_BWR] = { 640, 384, true,  1, false, EPD_CMD_DATA_UC8159_INTERLEAVED, "UC8159 7.5\" low BWR", ref_uc8159_init, ref_uc81xx_refresh, ref_uc81xx_cursor_home },
    [EPD_PANEL_REF_SSD1677_75_HD_BW]  = { 880, 528, false, 0, false, EPD_CMD_DATA_24_26_SSD16XX, "SSD1677 7.5\" HD BW", ref_ssd1677_init, ref_ssd16xx_refresh, ref_ssd1677_cursor_home },
    [EPD_PANEL_REF_SSD1677_75_HD_BWR] = { 880, 528, true,  0, false, EPD_CMD_DATA_24_26_SSD16XX, "SSD1677 7.5\" HD BWR", ref_ssd1677_init, ref_ssd16xx_refresh, ref_ssd1677_cursor_home },
    [EPD_PANEL_REF_JD79665_75_BWRY]   = { 800, 480, true,  1, false, EPD_CMD_DATA_JD796XX_2BPP, "JD79665 7.5\" BWRY", ref_jd79665_init, ref_jd796xx_refresh, ref_jd796xx_cursor_home, .has_yellow = true },
    [EPD_PANEL_REF_SSD1619_42_BW]     = { 400, 300, false, 0, false, EPD_CMD_DATA_24_26_SSD16XX, "SSD1619A 4.2\" BW reference", ref_ssd1619_init, ref_ssd16xx_refresh, ref_ssd1619_cursor_home },
    [EPD_PANEL_REF_SSD1619_42_BWR]    = { 400, 300, true,  0, false, EPD_CMD_DATA_24_26_SSD16XX, "SSD1619 4.2\" BWR", ref_ssd1619_init, ref_ssd16xx_refresh, ref_ssd1619_cursor_home },
    [EPD_PANEL_REF_JD79668_42_BWRY]   = { 400, 300, true,  1, false, EPD_CMD_DATA_JD796XX_2BPP, "JD79668 4.2\" BWRY", ref_jd79668_init, ref_jd796xx_refresh, ref_jd796xx_cursor_home, .has_yellow = true },
    [EPD_PANEL_REF_SSD1683_42_BWR]    = { 400, 300, true,  0, false, EPD_CMD_DATA_24_26_SSD16XX, "SSD1683 4.2\" BWR", ref_ssd1683_init, ref_ssd1683_refresh, ref_ssd1683_cursor_home },
    [EPD_PANEL_REF_SSD1683_42_LEGACY] = { 400, 300, true,  0, false, EPD_CMD_DATA_24_26_SSD16XX, "SSD1683 4.2\" BWR", ref_ssd1683_init, ref_ssd1683_refresh, ref_ssd1683_cursor_home },
    [EPD_PANEL_REF_SSD1683_42_BW]     = { 400, 300, false, 0, false, EPD_CMD_DATA_24_26_SSD16XX, "SSD1683 4.2\" BW", ref_ssd1683_init, ref_ssd1683_refresh, ref_ssd1683_cursor_home },
    [EPD_PANEL_FPC194_SSD1683_42_BWR] = { 400, 300, true,  0, false, EPD_CMD_DATA_24_26_SSD16XX, "FPC-194 4.2\" BWR SSD1683", fpc194_ssd1683_init, fpc194_ssd1683_refresh, fpc194_ssd1683_cursor_home },
    [EPD_PANEL_REF_SSD1667_102_BWR]   = { 960, 640, true,  0, false, EPD_CMD_DATA_24_26_SSD16XX, "SSD1667 10.2\" BWR", ref_ssd1667_102_init, ref_ssd1667_102_refresh, ref_ssd1667_102_cursor_home },
    [EPD_PANEL_E6_37_6C]              = { 792, 528, true,  1, false, EPD_CMD_DATA_E6_3IN7_6C, "E6 3.7\" six-color", e6_37_init, e6_37_refresh, cursor_none, .has_yellow = true },
    [EPD_PANEL_REF_SSD2677_97_BWRY]   = { 960, 672, true,  1, false, EPD_CMD_DATA_SSD2677_2BPP, "SSD2677 9.7\" BWRY", ref_ssd2677_97_init, ref_ssd2677_97_refresh, ref_ssd2677_97_cursor_home, .has_yellow = true },
    [EPD_PANEL_GDEY037T03_UC8253_BW]  = { 416, 240, false, 1, false, EPD_CMD_DATA_UC8253_BW, "GDEY037T03 3.7\" BW UC8253", uc8253_init, uc8253_refresh, cursor_none, .optional_refresh_caps = EPD_REFRESH_CAP_FAST_BW | EPD_REFRESH_CAP_PARTIAL_BW },
    /* Keep optional refresh disabled until a matching 640x384 BW waveform is verified. */
    [EPD_PANEL_REF_UC8159_75_LOW_BW]  = { 640, 384, false, 0, false, EPD_CMD_DATA_UC8159_INTERLEAVED, "UC8159 7.5\" low BW", ref_uc8159_init, ref_uc81xx_refresh, ref_uc81xx_cursor_home, .optional_refresh_caps = 0 },
    /* 122 active columns in a byte-aligned 128x250 SSD16xx controller RAM. */
    [EPD_PANEL_SSD16XX_213_BW]       = { SSD16XX_213_RAM_W, SSD16XX_213_H, false, 0, false, EPD_CMD_DATA_24_26, "SSD16xx 2.13\" BW 128x250", ssd16xx_213_bw_init, ssd16xx_213_bw_refresh, ssd16xx_213_bw_cursor_home, SSD16XX_213_X_OFFSET, false, .optional_refresh_caps = 0 },
    /* HT075A04 label: controller compatibility is unverified; keep optional refresh isolated. */
    [EPD_PANEL_GD7965_UC8179_75_BW]  = { 800, 480, false, 1, true, EPD_CMD_DATA_UC81XX_REF, "GD7965/UC8179 7.5\" BW 800x480", gd7965_uc8179_75_bw_init, ref_uc81xx_refresh, ref_uc81xx_cursor_home, .optional_refresh_caps = 0 },
    /* 局刷已验证（三段式寄存器 LUT，实测约0.6~1s vs 全刷18.8s），对时钟开放；
     * 每 JD79686A_PARTIAL_REFRESH_LIMIT 次局刷自动做一次维护全刷抗残影。 */
    [EPD_PANEL_JD79686A_583_BWR]     = { 648, 480, true,  1, true, EPD_CMD_DATA_JD79686A_BWR, "JD79686A 5.83\" BWR", jd79686a_583_bwr_init, jd79686a_583_bwr_refresh, cursor_none, .optional_refresh_caps = EPD_REFRESH_CAP_PARTIAL_COLOR },

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

static epd_panel_t panel_normalize(epd_panel_t panel)
{
    return panel == EPD_PANEL_REF_SSD1683_42_LEGACY
               ? EPD_PANEL_REF_SSD1683_42_BWR
               : panel;
}

int  epd_width(void)       { return panel_desc()->w; }
int  epd_height(void)      { return panel_desc()->h; }
int  epd_plane_bytes(void) { return panel_desc()->w * panel_desc()->h / 8; }
bool epd_has_red(void)     { return panel_desc()->has_red; }
bool epd_has_yellow(void)  { return panel_desc()->has_yellow; }
epd_panel_t epd_get_panel(void) { return s_panel; }

uint32_t epd_refresh_capabilities(void)
{
    return EPD_REFRESH_CAP_FULL | panel_desc()->optional_refresh_caps;
}

bool epd_refresh_mode_supported(epd_refresh_mode_t mode)
{
    const epd_panel_desc_t *d = panel_desc();
    const bool color = d->has_red || d->has_yellow;
    switch (mode) {
    case EPD_REFRESH_FULL:
        return true;
    case EPD_REFRESH_FAST:
        return (d->optional_refresh_caps &
                (color ? EPD_REFRESH_CAP_FAST_COLOR : EPD_REFRESH_CAP_FAST_BW)) != 0;
    case EPD_REFRESH_PARTIAL:
        return (d->optional_refresh_caps &
                (color ? EPD_REFRESH_CAP_PARTIAL_COLOR : EPD_REFRESH_CAP_PARTIAL_BW)) != 0;
    default:
        return false;
    }
}

const char *epd_refresh_mode_name(epd_refresh_mode_t mode)
{
    switch (mode) {
    case EPD_REFRESH_FULL: return "full";
    case EPD_REFRESH_FAST: return "fast";
    case EPD_REFRESH_PARTIAL: return "partial";
    default: return "invalid";
    }
}

static void previous_bw_invalidate(void)
{
    s_previous_bw_valid = false;
    s_experimental_refresh_count = 0;
    s_uc8179_partial_refresh_count = 0;
    s_uc8253_partial_refresh_count = 0;
    s_jd79686a_partial_refresh_count = 0;
    s_uc8179_red_signature_valid = false;
    s_jd79686a_red_signature_valid = false;
}

/* All callers hold the display mutex. No bus traffic is safe after an error. */
static esp_err_t epd_failure(esp_err_t err)
{
    if (err != ESP_OK) {
        previous_bw_invalidate();
        s_panel_seq_done = false;
        atomic_store(&s_force_full_refresh_next, true);
    }
    return err;
}

#define EPD_TRY(call) do { \
    epd_err = (call); \
    if (epd_err != ESP_OK) goto epd_fail; \
} while (0)

static void previous_bw_release(void)
{
    free(s_previous_bw);
    s_previous_bw = NULL;
    s_previous_bw_bytes = 0;
    s_previous_bw_panel = EPD_PANEL_COUNT;
    free(s_jd79686a_prev_red);
    s_jd79686a_prev_red = NULL;
    s_jd79686a_prev_red_bytes = 0;
    previous_bw_invalidate();
}

static bool previous_bw_store(const uint8_t *data, int bytes)
{
    if (!data || bytes <= 0)
        return false;

    if (!s_previous_bw || s_previous_bw_bytes != bytes ||
        s_previous_bw_panel != s_panel) {
        previous_bw_release();
        s_previous_bw = heap_caps_malloc((size_t)bytes,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_previous_bw)
            s_previous_bw = heap_caps_malloc((size_t)bytes, MALLOC_CAP_8BIT);
        if (!s_previous_bw) {
            ESP_LOGW(TAG, "previous-frame cache allocation failed: %d bytes", bytes);
            return false;
        }
        s_previous_bw_bytes = bytes;
        s_previous_bw_panel = s_panel;
    }

    memcpy(s_previous_bw, data, (size_t)bytes);
    s_previous_bw_valid = true;
    return true;
}

static bool previous_bw_fill(uint8_t value, int bytes)
{
    if (bytes <= 0)
        return false;

    if (!s_previous_bw || s_previous_bw_bytes != bytes ||
        s_previous_bw_panel != s_panel) {
        previous_bw_release();
        s_previous_bw = heap_caps_malloc((size_t)bytes,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_previous_bw)
            s_previous_bw = heap_caps_malloc((size_t)bytes, MALLOC_CAP_8BIT);
        if (!s_previous_bw) {
            ESP_LOGW(TAG, "previous-frame cache allocation failed: %d bytes", bytes);
            return false;
        }
        s_previous_bw_bytes = bytes;
        s_previous_bw_panel = s_panel;
    }

    memset(s_previous_bw, value, (size_t)bytes);
    s_previous_bw_valid = true;
    return true;
}

static bool previous_bw_matches_current_panel(void)
{
    return s_previous_bw_valid && s_previous_bw &&
           s_previous_bw_panel == s_panel &&
           s_previous_bw_bytes == epd_plane_bytes();
}

/**
 * JD79686A 上一帧红平面内容。红基线只存签名（校验用），但网页局刷试验按钮
 * 组包时必须提供真实红平面——传 NULL 会把 SRAM 红平面清零，窗口内红色会消失。
 * 真实刷屏路径直接用 fb->red，不依赖这里。
 */
static void jd79686a_prev_red_store(const uint8_t *red, uint8_t fill, int bytes)
{
    if (bytes <= 0)
        return;
    if (!s_jd79686a_prev_red || s_jd79686a_prev_red_bytes != bytes) {
        free(s_jd79686a_prev_red);
        s_jd79686a_prev_red = heap_caps_malloc((size_t)bytes,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_jd79686a_prev_red)
            s_jd79686a_prev_red = heap_caps_malloc((size_t)bytes, MALLOC_CAP_8BIT);
        if (!s_jd79686a_prev_red) {
            s_jd79686a_prev_red_bytes = 0;
            return;
        }
        s_jd79686a_prev_red_bytes = bytes;
    }
    if (red)
        memcpy(s_jd79686a_prev_red, red, (size_t)bytes);
    else
        memset(s_jd79686a_prev_red, fill, (size_t)bytes);
}

static uint64_t plane_signature(const uint8_t *data, int bytes, uint8_t fill)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (int i = 0; i < bytes; i++) {
        hash ^= data ? data[i] : fill;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool panel_is_atc_29_flexible(void)
{
    return s_panel == EPD_PANEL_HINK_SSD1619_29_BWR ||
           s_panel == EPD_PANEL_ATC_UC8151_29_BWR;
}

static bool panel_is_hink_29(void)
{
    return s_panel == EPD_PANEL_HINK_SSD1619_29_BWR;
}

static bool panel_is_ssd16xx_213_bw(void)
{
    return s_panel == EPD_PANEL_SSD16XX_213_BW;
}

static bool panel_is_gd7965_uc8179_75_bw(void)
{
    return s_panel == EPD_PANEL_GD7965_UC8179_75_BW;
}

static bool panel_is_jd79686a_583(void)
{
    return s_panel == EPD_PANEL_JD79686A_583_BWR;
}

static bool panel_is_visionpeak_ei0583_bw(void)
{
    return s_panel == EPD_PANEL_VISIONPEAK_EI0583T5PCZ_BW;
}

static bool panel_uses_hink_ssd1619_wire_format(void)
{
    return panel_is_hink_29() || panel_is_ssd16xx_213_bw();
}

static bool panel_is_ws426_bw(void)
{
    return s_panel == EPD_PANEL_WS_426_BW;
}

static bool panel_is_ref_ssd1683_bwr(void)
{
    return s_panel == EPD_PANEL_REF_SSD1683_42_BWR ||
           s_panel == EPD_PANEL_REF_SSD1683_42_LEGACY ||
           s_panel == EPD_PANEL_FPC194_SSD1683_42_BWR;
}

static bool panel_is_ref_uc42_bwr(void)
{
    return s_panel == EPD_PANEL_REF_UC8176_42_BWR;
}

static bool panel_is_jd796xx(void)
{
    return panel_desc()->data_mode == EPD_CMD_DATA_JD796XX_2BPP ||
	       panel_desc()->data_mode == EPD_CMD_DATA_JD79686A_BWR;
}

static bool panel_is_jd79668_42(void)
{
    return s_panel == EPD_PANEL_REF_JD79668_42_BWRY;
}

static bool panel_is_ssd1667_102(void)
{
    return s_panel == EPD_PANEL_REF_SSD1667_102_BWR;
}

static bool panel_is_ssd2677_97(void)
{
    return s_panel == EPD_PANEL_REF_SSD2677_97_BWRY;
}

static bool panel_is_uc8253_37(void)
{
    return s_panel == EPD_PANEL_GDEY037T03_UC8253_BW;
}

static bool panel_is_uc8179_75_bwr(void)
{
    return s_panel == EPD_PANEL_REF_UC8179_75_BWR;
}

static bool panel_uses_bwry_2bpp(void)
{
    return panel_desc()->data_mode == EPD_CMD_DATA_JD796XX_2BPP ||
           panel_desc()->data_mode == EPD_CMD_DATA_SSD2677_2BPP;
}

static bool panel_is_42_400x300(void)
{
    return s_panel == EPD_PANEL_42_BWR ||
           s_panel == EPD_PANEL_REF_UC8176_42_BW ||
           s_panel == EPD_PANEL_REF_UC8176_42_BWR ||
           s_panel == EPD_PANEL_REF_SSD1619_42_BW ||
           s_panel == EPD_PANEL_REF_SSD1619_42_BWR ||
           s_panel == EPD_PANEL_REF_SSD1683_42_BWR ||
           s_panel == EPD_PANEL_REF_SSD1683_42_LEGACY ||
           s_panel == EPD_PANEL_REF_SSD1683_42_BW ||
           s_panel == EPD_PANEL_FPC194_SSD1683_42_BWR;
}

static bool panel_uses_noninverted_ssd16xx_red_plane(void)
{
    return panel_is_ref_ssd1683_bwr() ||
           s_panel == EPD_PANEL_REF_SSD1667_102_BWR;
}

static bool panel_uses_inverted_ssd16xx_black_plane(void)
{
    return s_panel == EPD_PANEL_REF_SSD1667_102_BWR;
}

static bool panel_needs_inverted_black_plane(void)
{
    return panel_uses_hink_ssd1619_wire_format();
}

esp_err_t epd_save_panel_config(epd_panel_t panel, int busy_idle)
{
    panel = panel_normalize(panel);
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
        bool applied_now = (panel == s_panel);
        if (applied_now)
            s_busy_idle_level = busy_idle;
        ESP_LOGI(TAG, "Panel config saved%s: %s, busy_idle=%d",
                 applied_now ? " and applied" : " for next boot",
                 s_specs[panel].name, busy_idle);
    }
    return e;
}

esp_err_t epd_get_saved_panel_config(epd_panel_t *panel, int *busy_idle)
{
    if (!panel && !busy_idle)
        return ESP_ERR_INVALID_ARG;

    epd_panel_t saved_panel = s_panel;
    int saved_busy = busy_idle_effective();

    nvs_handle_t h;
    esp_err_t e = nvs_open("epd", NVS_READONLY, &h);
    if (e == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "panel", &v) == ESP_OK && panel_valid((epd_panel_t)v)) {
            saved_panel = panel_normalize((epd_panel_t)v);
            saved_busy = s_specs[saved_panel].default_busy_idle;
        }
        uint8_t bi;
        if (nvs_get_u8(h, "busy_idle", &bi) == ESP_OK && bi <= 1)
            saved_busy = (int)bi;
        nvs_close(h);
    } else if (e != ESP_ERR_NVS_NOT_FOUND) {
        return e;
    }

    if (panel)
        *panel = saved_panel;
    if (busy_idle)
        *busy_idle = saved_busy;
    return ESP_OK;
}

esp_err_t epd_set_panel(epd_panel_t panel)
{
    panel = panel_normalize(panel);
    if (!panel_valid(panel))
        return ESP_ERR_INVALID_ARG;

    if (s_epd_mutex)
        xSemaphoreTake(s_epd_mutex, portMAX_DELAY);

    if (s_panel != panel) {
        fb_release_reserved_planes();
        previous_bw_release();
        s_uc8176_register_lut_active = false;
        s_uc8179_register_lut_active = false;
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

const char *epd_panel_name(epd_panel_t panel)
{
    return panel_valid(panel) ? s_specs[panel_normalize(panel)].name : "Unknown panel";
}

bool epd_panel_image_profile(epd_panel_t panel, epd_image_profile_t *out)
{
    if (!out || !panel_valid(panel))
        return false;
    const epd_panel_desc_t *d = &s_specs[panel_normalize(panel)];
    const char *type = d->has_yellow ? (d->has_red ? "bwry" : "bwy") :
                       d->has_red ? "bwr" : "bw";
    const char *id = d->has_yellow && d->has_red ? "coverage-four-v1" :
                     d->has_red || d->has_yellow ? "coverage-three-v1" : "mono-v1";
    if (panel == EPD_PANEL_REF_JD79665_75_BWRY)
        id = "jd79665-v1";
    else if (panel == EPD_PANEL_REF_JD79668_42_BWRY)
        id = "jd79668-v1";
    else if (panel == EPD_PANEL_REF_SSD2677_97_BWRY)
        id = "ssd2677-v1";
    else if (panel == EPD_PANEL_E6_37_6C) {
        type = "e6";
        id = "e6-v1";
    }
    else if (panel == EPD_PANEL_JD79686A_583_BWR) {
        type = "bwr";
        id = "jd79686a-v1";
    }
    *out = (epd_image_profile_t){d->w, d->h, id, type, d->has_red, d->has_yellow};
    return true;
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
    s_busy_idle_level = -1;
    nvs_handle_t h;
    if (nvs_open("epd", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "panel", &v) == ESP_OK) {
            if (panel_valid((epd_panel_t)v)) {
                s_panel = panel_normalize((epd_panel_t)v);
            } else {
                ESP_LOGW(TAG, "Saved panel id %u is no longer supported; using default panel", (unsigned)v);
            }
        }
        uint8_t bi;
        if (nvs_get_u8(h, "busy_idle", &bi) == ESP_OK && bi <= 1)
            s_busy_idle_level = (int)bi;
        nvs_close(h);
    }
}

void epd_load_panel_from_nvs(void)
{
    nvs_load_panel();
    if (s_panel == EPD_PANEL_REF_JD79665_75_BWRY && busy_idle_effective() == 0) {
        ESP_LOGW(TAG, "JD79665 reference flow uses BUSY idle-high; migrating busy_idle -> 1");
        (void)epd_set_busy_idle(1);
    }
    if (panel_is_jd79668_42() && busy_idle_effective() == 0) {
        ESP_LOGW(TAG, "JD79668 4.2 datasheet uses BUSY_N idle-high; migrating busy_idle -> 1");
        (void)epd_set_busy_idle(1);
    }
    if (s_panel == EPD_PANEL_REF_SSD2677_97_BWRY && busy_idle_effective() == 0) {
        ESP_LOGW(TAG, "SSD2677 9.7 reference flow uses BUSY idle-high; migrating busy_idle -> 1");
        (void)epd_set_busy_idle(1);
    }
    if (panel_is_uc8253_37() && busy_idle_effective() == 0) {
        ESP_LOGW(TAG, "GDEY037T03 UC8253 uses BUSY idle-high; migrating busy_idle -> 1");
        (void)epd_set_busy_idle(1);
    }
    if (panel_is_uc8179_75_bwr() && busy_idle_effective() == 0) {
        ESP_LOGW(TAG, "GDEY075Z08 UC8179 uses BUSY idle-high; migrating busy_idle -> 1");
        (void)epd_set_busy_idle(1);
    }
    if (panel_is_gd7965_uc8179_75_bw() && busy_idle_effective() == 0) {
        ESP_LOGW(TAG, "GD7965/UC8179 7.5 BW candidate uses BUSY idle-high; migrating busy_idle -> 1");
        (void)epd_set_busy_idle(1);
    }
    if (panel_is_ws426_bw() && busy_idle_effective() != 0) {
        ESP_LOGW(TAG, "P426010 4.26 BW uses BUSY idle-low; restoring busy_idle -> 0");
        (void)epd_set_busy_idle(0);
    }
    if (panel_is_ssd16xx_213_bw() && busy_idle_effective() != 0) {
        ESP_LOGW(TAG, "SSD16xx 2.13 128x250 uses BUSY idle-low; restoring busy_idle -> 0");
        (void)epd_set_busy_idle(0);
    }
    if (s_panel == EPD_PANEL_REF_UC8159_75_LOW_BW && busy_idle_effective() != 0) {
        ESP_LOGW(TAG, "UC8159 7.5 low-resolution BW hardware uses BUSY idle-low; restoring busy_idle -> 0");
        (void)epd_set_busy_idle(0);
    }
    if (s_panel == EPD_PANEL_42_BWR && busy_idle_effective() == 1) {
        ESP_LOGW(TAG, "4.2 SSD1619 init needs BUSY idle-low on this board; migrating busy_idle -> 0");
        (void)epd_set_busy_idle(0);
    }
    if (panel_is_jd79686a_583() && busy_idle_effective() == 0) {
        ESP_LOGW(TAG, "JD79686A 5.83 BWR uses BUSY idle-high; migrating busy_idle -> 1");
        (void)epd_set_busy_idle(1);
    }
}

static int busy_idle_effective(void)
{
    const epd_panel_desc_t *d = panel_desc();
    if (d->data_mode == EPD_CMD_DATA_E6_3IN7_6C)
        return d->default_busy_idle; /* E6 reference driver uses BUSY ready = HIGH. */
    if (s_busy_idle_level == 0 || s_busy_idle_level == 1)
        return s_busy_idle_level;
    return d->default_busy_idle;
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

static esp_err_t epd_cmd(uint8_t cmd)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(gpio_set_level(PIN_EPD_DC, 0));
    EPD_TRY(spi_tx(&cmd, 1));

    return ESP_OK;

epd_fail:
    ESP_LOGE(TAG, "cmd 0x%02X failed: %s", cmd, esp_err_to_name(epd_err));
    return epd_failure(epd_err);
}

static esp_err_t epd_dat(uint8_t data)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(gpio_set_level(PIN_EPD_DC, 1));
    EPD_TRY(spi_tx(&data, 1));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t epd_bulk(const uint8_t *data, size_t len)
{
    esp_err_t epd_err = ESP_OK;
    uint8_t *bounce = NULL;

    EPD_TRY(gpio_set_level(PIN_EPD_DC, 1));

    bool src_is_dma = esp_ptr_dma_capable(data);
    bounce = NULL;
    if (!src_is_dma) {
        bounce = heap_caps_malloc(EPD_FILL_CHUNK, MALLOC_CAP_DMA);
        if (!bounce) {
            ESP_LOGE(TAG, "DMA bounce alloc failed");
            return epd_failure(ESP_ERR_NO_MEM);
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
        EPD_TRY(spi_tx(tx_ptr, n));
        offset += n;
    }
    free(bounce);
    bounce = NULL;

    return ESP_OK;

epd_fail:
    free(bounce);
    return epd_failure(epd_err);
}

static esp_err_t epd_fill(uint8_t value, size_t len)
{
    esp_err_t epd_err = ESP_OK;
    uint8_t *buf = NULL;

    buf = heap_caps_malloc(EPD_FILL_CHUNK, MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE(TAG, "epd_fill: DMA malloc failed");
        return epd_failure(ESP_ERR_NO_MEM);
    }
    memset(buf, value, EPD_FILL_CHUNK);
    EPD_TRY(gpio_set_level(PIN_EPD_DC, 1));
    size_t offset = 0;
    while (offset < len) {
        size_t n = (len - offset > EPD_FILL_CHUNK) ? EPD_FILL_CHUNK : (len - offset);
        EPD_TRY(spi_tx(buf, n));
        offset += n;
    }
    free(buf);
    buf = NULL;

    return ESP_OK;

epd_fail:
    free(buf);
    return epd_failure(epd_err);
}

/* ── 复位 / BUSY ─────────────────────────────────────────────────── */

static esp_err_t epd_hw_reset(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(gpio_set_level(PIN_EPD_RST, 1));
    delay_ms(20);
    EPD_TRY(gpio_set_level(PIN_EPD_RST, 0));
    delay_ms(40);
    EPD_TRY(gpio_set_level(PIN_EPD_RST, 1));
    delay_ms(200);

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t epd_hw_reset_waveshare_v2(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(gpio_set_level(PIN_EPD_RST, 1));
    delay_ms(200);
    EPD_TRY(gpio_set_level(PIN_EPD_RST, 0));
    delay_ms(2);
    EPD_TRY(gpio_set_level(PIN_EPD_RST, 1));
    delay_ms(200);

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

/**
 * 等待 BUSY 释放。idle 电平见 busy_idle_effective()。
 * UC8179：轮询前可发 0x71；SSD1619 一般直接读 GPIO。
 *
 * after_update_cmd：发完 0x20（SSD1619）/0x12（UC8179 等）刷新命令后须置 true。
 * 否则 BUSY 可能仍短暂保持“空闲”电平（上拉），会误判为已就绪（仅 ~10ms 就返回）。
 */
static esp_err_t epd_wait_ready(const char *stage, bool after_update_cmd)
{
    esp_err_t epd_err = ESP_OK;
    int64_t start_us = esp_timer_get_time();
    s_last_wait_saw_busy_phase = false;
    const bool probe_0x71 = panel_desc()->busy_probe_0x71;
    int idle = busy_idle_effective();
    const bool flexible_busy = panel_is_atc_29_flexible();
    const int timeout_ms = flexible_busy ? EPD_ATC29_BUSY_TIMEOUT_MS :
                           (after_update_cmd && (panel_is_uc8179_75_bwr() ||
                                                 panel_is_gd7965_uc8179_75_bw())
                                ? EPD_UC8179_75_BUSY_UPDATE_TIMEOUT_MS
                                : (after_update_cmd && panel_is_jd796xx()
                                ? EPD_JD796XX_BUSY_UPDATE_TIMEOUT_MS
                                : (after_update_cmd && panel_is_ssd1667_102()
                                       ? EPD_SSD1667_102_BUSY_UPDATE_TIMEOUT_MS
                                       : (after_update_cmd && panel_is_ssd2677_97()
                                              ? EPD_SSD2677_97_BUSY_UPDATE_TIMEOUT_MS
                                              : (after_update_cmd ? EPD_BUSY_UPDATE_TIMEOUT_MS
                                                                  : EPD_BUSY_TIMEOUT_MS)))));

    if (probe_0x71)
        EPD_TRY(epd_cmd(0x71));

    /* 阶段 1：等到离开空闲态（进入忙），避免刷新命令刚发出时仍读到空闲 */
    if (strcmp(stage, "refresh") == 0) {
        int phase1 = 0;
        const int phase1_max_ms = 800;
        while (gpio_get_level(PIN_EPD_BUSY) == idle && phase1 < phase1_max_ms) {
            delay_ms(20);
            phase1 += 20;
            if (probe_0x71)
                EPD_TRY(epd_cmd(0x71));
        }
        if (gpio_get_level(PIN_EPD_BUSY) == idle) {
            epd_err = ESP_ERR_TIMEOUT;
            goto epd_fail;
        }
        s_last_wait_saw_busy_phase = true;
    }

    int waited = 0;
    while (gpio_get_level(PIN_EPD_BUSY) != idle) {
        s_last_wait_saw_busy_phase = true;
        if (probe_0x71)
            EPD_TRY(epd_cmd(0x71));
        delay_ms(200);
        waited += 200;
        if (waited >= timeout_ms && gpio_get_level(PIN_EPD_BUSY) != idle) {
            epd_err = ESP_ERR_TIMEOUT;
            goto epd_fail;
        }
    }
epd_fail:
    ESP_LOGI(TAG, "BUSY panel=%s stage=%s GPIO%d=%d idle=%d phase=%d duration=%lld ms result=%s",
             panel_desc()->name, stage, PIN_EPD_BUSY,
             gpio_get_level(PIN_EPD_BUSY), idle, s_last_wait_saw_busy_phase,
             (long long)((esp_timer_get_time() - start_us) / 1000),
             esp_err_to_name(epd_err));
    return epd_failure(epd_err);
}

/* ── Waveshare-style helpers ───────────────────────────────────────── */

static esp_err_t cmd1(uint8_t c, uint8_t v1)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_cmd(c));
    EPD_TRY(epd_dat(v1));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t cmd2(uint8_t c, uint8_t v1, uint8_t v2)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_cmd(c));
    EPD_TRY(epd_dat(v1));
    EPD_TRY(epd_dat(v2));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t cmd3(uint8_t c, uint8_t v1, uint8_t v2, uint8_t v3)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_cmd(c));
    EPD_TRY(epd_dat(v1));
    EPD_TRY(epd_dat(v2));
    EPD_TRY(epd_dat(v3));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t cmd4(uint8_t c, uint8_t v1, uint8_t v2, uint8_t v3, uint8_t v4)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_cmd(c));
    EPD_TRY(epd_dat(v1));
    EPD_TRY(epd_dat(v2));
    EPD_TRY(epd_dat(v3));
    EPD_TRY(epd_dat(v4));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t cmd5(uint8_t c, uint8_t v1, uint8_t v2, uint8_t v3, uint8_t v4, uint8_t v5)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_cmd(c));
    EPD_TRY(epd_dat(v1));
    EPD_TRY(epd_dat(v2));
    EPD_TRY(epd_dat(v3));
    EPD_TRY(epd_dat(v4));
    EPD_TRY(epd_dat(v5));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t cursor_none(void)
{


    return ESP_OK;
}

static esp_err_t ws_426_bw_cursor_home(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(cmd2(0x4E, 0x00, 0x00));
    EPD_TRY(cmd2(0x4F, 0x00, 0x00));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t atc_unissd_cursor_home(void)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    uint16_t y = d->mirror_v ? 0u : (uint16_t)(d->h - 1u);
    EPD_TRY(epd_cmd(0x4E));
    EPD_TRY(epd_dat((uint8_t)(d->x_offset / 8u)));
    EPD_TRY(epd_cmd(0x4F));
    EPD_TRY(epd_dat((uint8_t)y));
    EPD_TRY(epd_dat((uint8_t)(y >> 8)));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t hink_ssd1619_29_cursor_home(void)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    uint16_t y = (uint16_t)(HINK29_LOGICAL_H - 1u);
    EPD_TRY(epd_cmd(0x4E));
    EPD_TRY(epd_dat((uint8_t)(d->x_offset / 8u)));
    EPD_TRY(epd_cmd(0x4F));
    EPD_TRY(epd_dat((uint8_t)y));
    EPD_TRY(epd_dat((uint8_t)(y >> 8)));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t atc_unissd_97_cursor_home(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_cmd(0x4E));
    EPD_TRY(epd_dat(0xBF));
    EPD_TRY(epd_dat(0x03));
    EPD_TRY(epd_cmd(0x4F));
    EPD_TRY(epd_dat(0x00));
    EPD_TRY(epd_dat(0x00));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_ssd2677_97_full_window(void)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    EPD_TRY(cmd4(0x61, (uint8_t)(d->w >> 8), (uint8_t)d->w,
         (uint8_t)(d->h >> 8), (uint8_t)d->h));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_ssd2677_97_cursor_home(void)
{
    return ESP_OK;
}

static esp_err_t ref_ssd1667_102_full_window(void)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    uint16_t x0 = 0;
    uint16_t x1 = (uint16_t)(d->w - 1);
    uint16_t y0 = 0;
    uint16_t y1 = (uint16_t)(d->h - 1);

    EPD_TRY(cmd1(0x11, 0x03));
    EPD_TRY(cmd4(0x44, (uint8_t)(x0 & 0xFF), (uint8_t)(x0 >> 8),
         (uint8_t)(x1 & 0xFF), (uint8_t)(x1 >> 8)));
    EPD_TRY(cmd4(0x45, (uint8_t)(y0 & 0xFF), (uint8_t)(y0 >> 8),
         (uint8_t)(y1 & 0xFF), (uint8_t)(y1 >> 8)));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_ssd1667_102_cursor_home(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_cmd(0x4E));
    EPD_TRY(epd_dat(0x00));
    EPD_TRY(epd_dat(0x00));
    EPD_TRY(epd_cmd(0x4F));
    EPD_TRY(epd_dat(0x00));
    EPD_TRY(epd_dat(0x00));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_send_tres(void)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    EPD_TRY(cmd4(0x61, (uint8_t)(d->w >> 8), (uint8_t)d->w,
         (uint8_t)(d->h >> 8), (uint8_t)d->h));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_uc81xx_set_window_scan(int x, int y, int w, int h,
                                      uint8_t scan_control)
{
    esp_err_t epd_err = ESP_OK;
    int xe = (x + w - 1) | 0x07;
    int ye = y + h - 1;
    x &= ~0x07;

    EPD_TRY(epd_cmd(0x90));
    EPD_TRY(epd_dat((uint8_t)(x >> 8)));
    EPD_TRY(epd_dat((uint8_t)x));
    EPD_TRY(epd_dat((uint8_t)(xe >> 8)));
    EPD_TRY(epd_dat((uint8_t)xe));
    EPD_TRY(epd_dat((uint8_t)(y >> 8)));
    EPD_TRY(epd_dat((uint8_t)y));
    EPD_TRY(epd_dat((uint8_t)(ye >> 8)));
    EPD_TRY(epd_dat((uint8_t)ye));
    EPD_TRY(epd_dat(scan_control));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_uc81xx_set_window(int x, int y, int w, int h)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(ref_uc81xx_set_window_scan(x, y, w, h, 0x00));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_uc81xx_full_window(void)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    EPD_TRY(ref_uc81xx_set_window(0, 0, d->w, d->h));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_ssd1619_full_window(void)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    uint16_t x0 = 0;
    uint16_t x1 = (uint16_t)(d->w - 1);
    uint16_t y0 = 0;
    uint16_t y1 = (uint16_t)(d->h - 1);

    EPD_TRY(cmd1(0x11, 0x03));
    EPD_TRY(cmd2(0x44, (uint8_t)(x0 / 8u), (uint8_t)(x1 / 8u)));
    EPD_TRY(cmd4(0x45, (uint8_t)y0, (uint8_t)(y0 >> 8),
         (uint8_t)y1, (uint8_t)(y1 >> 8)));
    EPD_TRY(cmd1(0x4E, (uint8_t)(x0 / 8u)));
    EPD_TRY(cmd2(0x4F, (uint8_t)y0, (uint8_t)(y0 >> 8)));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_ssd1683_set_window(int x, int y, int w, int h)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    int x0 = x & ~0x07;
    int x1 = (x + w - 1) | 0x07;
    int y0 = y;
    int y1 = y + h - 1;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= d->w) x1 = d->w - 1;
    if (y1 >= d->h) y1 = d->h - 1;

    EPD_TRY(cmd1(0x11, 0x03));
    EPD_TRY(cmd2(0x44, (uint8_t)(x0 / 8), (uint8_t)(x1 / 8)));
    EPD_TRY(cmd4(0x45, (uint8_t)y0, (uint8_t)(y0 >> 8),
         (uint8_t)y1, (uint8_t)(y1 >> 8)));
    EPD_TRY(cmd1(0x4E, (uint8_t)(x0 / 8)));
    EPD_TRY(cmd2(0x4F, (uint8_t)y0, (uint8_t)(y0 >> 8)));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_ssd1683_full_window(void)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    EPD_TRY(ref_ssd1683_set_window(0, 0, d->w, d->h));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_ssd1683_apply_full_refresh_config(void)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    uint16_t y1 = (uint16_t)(d->h - 1);

    EPD_TRY(cmd3(0x01, (uint8_t)(y1 & 0xFF), (uint8_t)(y1 >> 8), 0x00));
    EPD_TRY(cmd1(0x3C, 0x05));
    EPD_TRY(cmd1(0x18, 0x80));
    EPD_TRY(ref_ssd1683_full_window());

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_ssd1677_full_window(void)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    uint16_t x0 = 0;
    uint16_t x1 = (uint16_t)(d->w - 1);
    uint16_t y0 = 0;
    uint16_t y1 = (uint16_t)(d->h - 1);

    EPD_TRY(cmd1(0x11, 0x03));
    EPD_TRY(cmd4(0x44, (uint8_t)x0, (uint8_t)(x0 >> 8),
         (uint8_t)x1, (uint8_t)(x1 >> 8)));
    EPD_TRY(cmd4(0x45, (uint8_t)y0, (uint8_t)(y0 >> 8),
         (uint8_t)y1, (uint8_t)(y1 >> 8)));
    EPD_TRY(cmd2(0x4E, (uint8_t)x0, (uint8_t)(x0 >> 8)));
    EPD_TRY(cmd2(0x4F, (uint8_t)y0, (uint8_t)(y0 >> 8)));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_jd796xx_full_window(void)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    uint16_t x0 = 0;
    uint16_t x1 = (uint16_t)(d->w - 1);
    uint16_t y0 = 0;
    uint16_t y1 = (uint16_t)(d->h - 1);

    EPD_TRY(epd_cmd(0x83));
    EPD_TRY(epd_dat((uint8_t)(x0 >> 8)));
    EPD_TRY(epd_dat((uint8_t)x0));
    EPD_TRY(epd_dat((uint8_t)(x1 >> 8)));
    EPD_TRY(epd_dat((uint8_t)x1));
    EPD_TRY(epd_dat((uint8_t)(y0 >> 8)));
    EPD_TRY(epd_dat((uint8_t)y0));
    EPD_TRY(epd_dat((uint8_t)(y1 >> 8)));
    EPD_TRY(epd_dat((uint8_t)y1));
    EPD_TRY(epd_dat(0x01));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_uc81xx_cursor_home(void)
{
    return ESP_OK;
}

static esp_err_t ref_ssd1619_cursor_home(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(ref_ssd1619_full_window());

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_ssd1683_cursor_home(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(ref_ssd1683_full_window());

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t fpc194_ssd1683_cursor_home(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(ref_ssd1683_full_window());

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_ssd1677_cursor_home(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(ref_ssd1677_full_window());

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_jd796xx_cursor_home(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(ref_jd796xx_full_window());

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t refresh_22_f7_20(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(cmd1(0x22, 0xF7));
    EPD_TRY(epd_cmd(0x20));
    EPD_TRY(epd_wait_ready("refresh", true));
    delay_ms(panel_is_atc_29_flexible() ? EPD_ATC29_SAFE_REFRESH_MS : 200);

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t hink_ssd1619_29_refresh(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(cmd1(0x22, 0xCF));
    EPD_TRY(epd_cmd(0x20));
    EPD_TRY(epd_wait_ready("refresh", true));
    /* Preserve the panel's conservative settle interval after verified BUSY. */
    delay_ms(HINK29_REFRESH_MS);

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ssd16xx_213_bw_refresh(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(cmd1(0x22, 0xCF));
    EPD_TRY(epd_cmd(0x20));
    EPD_TRY(epd_wait_ready("refresh", true));
    bool busy_ok = true;
    delay_ms(200);
    if (!busy_ok)
        ESP_LOGW(TAG, "SSD16xx 2.13 128x250 full refresh used BUSY fallback");

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t refresh_12(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_cmd(0x12));
    delay_ms(100);
    EPD_TRY(epd_wait_ready("refresh", true));
    delay_ms(200);

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t refresh_power_on_12(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_cmd(0x04));
    delay_ms(20);
    EPD_TRY(epd_wait_ready("power-on", false));
    EPD_TRY(epd_cmd(0x12));
    delay_ms(100);
    EPD_TRY(epd_wait_ready("refresh", true));
    delay_ms(panel_is_atc_29_flexible() ? EPD_ATC29_SAFE_REFRESH_MS : 200);

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_uc81xx_refresh(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_cmd(0x04));
    delay_ms(20);
    EPD_TRY(epd_wait_ready("power-on", false));
    EPD_TRY(epd_cmd(0x91));
    EPD_TRY(ref_uc81xx_full_window());
    EPD_TRY(epd_cmd(0x12));
    delay_ms(100);
    EPD_TRY(epd_wait_ready("refresh", true));
    EPD_TRY(epd_cmd(0x92));
    EPD_TRY(epd_cmd(0x02));
    delay_ms(100);
    EPD_TRY(epd_wait_ready("poweroff", true));
    bool power_off_ok = true;

    if (panel_is_ref_uc42_bwr()) {
        /* The tested UC 4.2 BWR panel retained 15-20V after refresh when only
         * Power Off was sent. Deep sleep forces the booster/VCOM section off;
         * the next display must reset and initialize the controller again. */
        EPD_TRY(cmd1(0x07, 0xA5));
        delay_ms(100);
        s_panel_seq_done = false;
        ESP_LOGI(TAG, "UC 4.2 BWR power down: power_off=%s, deep sleep entered",
                 power_off_ok ? "ready" : "fallback");
    } else {
        delay_ms(200);
    }

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_uc8179_75_bwr_refresh(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(cmd1(0x00, 0x0F));
    EPD_TRY(cmd2(0x50, 0x13, 0x07));
    EPD_TRY(cmd1(0xE0, 0x00)); /* Use the controller's internal temperature sensor. */
    EPD_TRY(cmd1(0x41, 0x00));

    EPD_TRY(epd_cmd(0x04));
    delay_ms(20);
    EPD_TRY(epd_wait_ready("power-on", false));

    TickType_t t0 = xTaskGetTickCount();
    EPD_TRY(epd_cmd(0x12));
    delay_ms(100);
    EPD_TRY(epd_wait_ready("refresh", true));
    bool refresh_ok = true;
    uint32_t elapsed_ms = pdTICKS_TO_MS(xTaskGetTickCount() - t0);

    EPD_TRY(epd_cmd(0x02));
    delay_ms(100);
    EPD_TRY(epd_wait_ready("poweroff", false));
    bool power_off_ok = true;
    ESP_LOGI(TAG,
             "GDEY075Z08 UC8179 full refresh: %lu ms, refresh=%s, power-off=%s",
             (unsigned long)elapsed_ms,
             refresh_ok ? "ready" : "fallback",
             power_off_ok ? "ready" : "fallback");

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

/*
 * UC8176/GDEW042T2 register LUT from Waveshare epd4in2.cpp (2017).
 * Copyright (C) Waveshare
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
static const uint8_t s_uc8176_lut_vcom[] = {
    0x00, 0x08, 0x08, 0x00, 0x00, 0x02,
    0x00, 0x0F, 0x0F, 0x00, 0x00, 0x01,
    0x00, 0x08, 0x08, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t s_uc8176_lut_ww[] = {
    0x50, 0x08, 0x08, 0x00, 0x00, 0x02,
    0x90, 0x0F, 0x0F, 0x00, 0x00, 0x01,
    0xA0, 0x08, 0x08, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t s_uc8176_lut_bw[] = {
    0x50, 0x08, 0x08, 0x00, 0x00, 0x02,
    0x90, 0x0F, 0x0F, 0x00, 0x00, 0x01,
    0xA0, 0x08, 0x08, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t s_uc8176_lut_wb[] = {
    0xA0, 0x08, 0x08, 0x00, 0x00, 0x02,
    0x90, 0x0F, 0x0F, 0x00, 0x00, 0x01,
    0x50, 0x08, 0x08, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t s_uc8176_lut_bb[] = {
    0x20, 0x08, 0x08, 0x00, 0x00, 0x02,
    0x90, 0x0F, 0x0F, 0x00, 0x00, 0x01,
    0x10, 0x08, 0x08, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* GDEY075Z08 UC8179 black/white differential waveform, 42 bytes per bank. */
static const uint8_t s_uc8179_75_lut_neutral[42] = {
    0x00, 0x03, 0x05, 0x00, 0x00, 0x02,
    0x00, 0x05, 0x05, 0x00, 0x00, 0x01,
    0x00, 0x05, 0x05, 0x00, 0x00, 0x01,
    0x00, 0x05, 0x05, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t s_uc8179_75_lut_bw[42] = {
    0x20, 0x03, 0x05, 0x00, 0x00, 0x02,
    0x20, 0x05, 0x05, 0x00, 0x00, 0x01,
    0x00, 0x05, 0x05, 0x00, 0x00, 0x01,
    0x20, 0x05, 0x05, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t s_uc8179_75_lut_wb[42] = {
    0x40, 0x03, 0x05, 0x00, 0x00, 0x02,
    0x40, 0x05, 0x05, 0x00, 0x00, 0x01,
    0x00, 0x05, 0x05, 0x00, 0x00, 0x01,
    0x40, 0x05, 0x05, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t s_uc8179_75_lut_bd[42] = {0};

static esp_err_t uc8179_75_load_bw_partial_lut(void)
{
    esp_err_t epd_err = ESP_OK;
    if (s_uc8179_register_lut_active)
        return ESP_OK;

    EPD_TRY(cmd1(0x00, 0x1F));
    EPD_TRY(cmd1(0x00, 0x3F));
    EPD_TRY(cmd1(0x82, 0x30));
    EPD_TRY(cmd2(0x50, 0xA0, 0x07));
    EPD_TRY(epd_cmd(0x20)); EPD_TRY(epd_bulk(s_uc8179_75_lut_neutral, sizeof(s_uc8179_75_lut_neutral)));
    EPD_TRY(epd_cmd(0x21)); EPD_TRY(epd_bulk(s_uc8179_75_lut_neutral, sizeof(s_uc8179_75_lut_neutral)));
    EPD_TRY(epd_cmd(0x22)); EPD_TRY(epd_bulk(s_uc8179_75_lut_bw, sizeof(s_uc8179_75_lut_bw)));
    EPD_TRY(epd_cmd(0x23)); EPD_TRY(epd_bulk(s_uc8179_75_lut_wb, sizeof(s_uc8179_75_lut_wb)));
    EPD_TRY(epd_cmd(0x24)); EPD_TRY(epd_bulk(s_uc8179_75_lut_neutral, sizeof(s_uc8179_75_lut_neutral)));
    EPD_TRY(epd_cmd(0x25)); EPD_TRY(epd_bulk(s_uc8179_75_lut_bd, sizeof(s_uc8179_75_lut_bd)));
    s_uc8179_register_lut_active = true;
    ESP_LOGI(TAG, "GDEY075Z08 UC8179 BW partial LUT loaded (0x20..0x25)");

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

typedef struct {
    int x;
    int y;
    int w;
    int h;
} epd_dirty_rect_t;

static esp_err_t uc8176_load_stable_register_lut(void)
{
    esp_err_t epd_err = ESP_OK;
    if (s_uc8176_register_lut_active)
        return ESP_OK;

    EPD_TRY(cmd4(0x01, 0x03, 0x00, 0x2B, 0x2B));
    EPD_TRY(cmd3(0x06, 0x17, 0x17, 0x17));
    EPD_TRY(cmd1(0x00, 0x3F));
    EPD_TRY(cmd1(0x30, 0x3C));
    EPD_TRY(ref_send_tres());
    EPD_TRY(cmd1(0x82, 0x12));
    EPD_TRY(cmd1(0x50, 0x97));

    EPD_TRY(epd_cmd(0x20)); EPD_TRY(epd_bulk(s_uc8176_lut_vcom, sizeof(s_uc8176_lut_vcom)));
    EPD_TRY(epd_cmd(0x21)); EPD_TRY(epd_bulk(s_uc8176_lut_ww, sizeof(s_uc8176_lut_ww)));
    EPD_TRY(epd_cmd(0x22)); EPD_TRY(epd_bulk(s_uc8176_lut_bw, sizeof(s_uc8176_lut_bw)));
    EPD_TRY(epd_cmd(0x23)); EPD_TRY(epd_bulk(s_uc8176_lut_wb, sizeof(s_uc8176_lut_wb)));
    EPD_TRY(epd_cmd(0x24)); EPD_TRY(epd_bulk(s_uc8176_lut_bb, sizeof(s_uc8176_lut_bb)));
    s_uc8176_register_lut_active = true;
    ESP_LOGI(TAG, "UC8176 experimental GDEW042T2 register LUT loaded");

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static bool epd_find_dirty_rect(const uint8_t *old_data,
                                const uint8_t *new_data,
                                epd_dirty_rect_t *rect)
{
    const int rb = epd_width() / 8;
    const int height = epd_height();
    int min_xb = rb;
    int max_xb = -1;
    int min_y = height;
    int max_y = -1;

    for (int y = 0; y < height; y++) {
        const size_t row = (size_t)y * (size_t)rb;
        for (int xb = 0; xb < rb; xb++) {
            if (old_data[row + (size_t)xb] == new_data[row + (size_t)xb])
                continue;
            if (xb < min_xb) min_xb = xb;
            if (xb > max_xb) max_xb = xb;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    }

    if (max_xb < min_xb || max_y < min_y)
        return false;

    rect->x = min_xb * 8;
    rect->y = min_y;
    rect->w = (max_xb - min_xb + 1) * 8;
    rect->h = max_y - min_y + 1;
    return true;
}

static bool plane_region_is_clear(const uint8_t *data,
                                  const epd_dirty_rect_t *rect)
{
    if (!data)
        return true;
    const int rb = epd_width() / 8;
    const int x_byte = rect->x / 8;
    const int row_bytes = rect->w / 8;
    for (int y = 0; y < rect->h; y++) {
        size_t offset = (size_t)(rect->y + y) * (size_t)rb + (size_t)x_byte;
        for (int i = 0; i < row_bytes; i++) {
            if (data[offset + (size_t)i] != 0x00)
                return false;
        }
    }
    return true;
}

static esp_err_t uc81xx_write_region(uint8_t command, const uint8_t *data,
                                     const epd_dirty_rect_t *rect)
{
    esp_err_t epd_err = ESP_OK;
    uint8_t *row = NULL;

    const int rb = epd_width() / 8;
    const int row_bytes = rect->w / 8;
    const int x_byte = rect->x / 8;

    EPD_TRY(epd_cmd(0x91));
    EPD_TRY(ref_uc81xx_set_window_scan(rect->x, rect->y, rect->w, rect->h, 0x01));
    EPD_TRY(epd_cmd(command));

    if (rect->x == 0 && rect->w == epd_width()) {
        EPD_TRY(epd_bulk(data + (size_t)rect->y * (size_t)rb,
                 (size_t)rect->h * (size_t)rb));
        EPD_TRY(epd_cmd(0x92));
        return ESP_OK;
    }

    row = heap_caps_malloc((size_t)row_bytes, MALLOC_CAP_DMA);
    if (!row) {
        return epd_failure(ESP_ERR_NO_MEM);
    }

    EPD_TRY(gpio_set_level(PIN_EPD_DC, 1));
    esp_err_t err = ESP_OK;
    for (int y = 0; y < rect->h; y++) {
        size_t offset = (size_t)(rect->y + y) * (size_t)rb + (size_t)x_byte;
        memcpy(row, data + offset, (size_t)row_bytes);
        EPD_TRY(err = spi_tx(row, (size_t)row_bytes));
        if (err != ESP_OK)
            break;
    }
    free(row);
    row = NULL;
    EPD_TRY(epd_cmd(0x92));
    return epd_failure(err);

epd_fail:
    free(row);
    return epd_failure(epd_err);
}

static esp_err_t uc8176_refresh_region(const uint8_t *old_data,
                                       const uint8_t *new_data,
                                       const epd_dirty_rect_t *rect)
{
    esp_err_t epd_err = ESP_OK;
    int64_t start_us = esp_timer_get_time();
    EPD_TRY(uc8176_load_stable_register_lut());

    EPD_TRY(epd_cmd(0x04));
    delay_ms(20);
    EPD_TRY(epd_wait_ready("power-on", false));

    esp_err_t err;

    EPD_TRY(err = uc81xx_write_region(0x10, old_data, rect));
    if (err == ESP_OK)
        EPD_TRY(err = uc81xx_write_region(0x13, new_data, rect));

    bool update_ready = false;
    if (err == ESP_OK) {
        EPD_TRY(epd_cmd(0x91));
        EPD_TRY(ref_uc81xx_set_window_scan(rect->x, rect->y, rect->w, rect->h, 0x01));
        EPD_TRY(epd_cmd(0x12));
        delay_ms(100);
        EPD_TRY(epd_wait_ready("refresh", true));
    update_ready = true;
        EPD_TRY(epd_cmd(0x92));
    }

    EPD_TRY(epd_cmd(0x02));
    delay_ms(100);
    EPD_TRY(epd_wait_ready("poweroff", true));
    bool power_off_ready = true;
    delay_ms(100);
    ESP_LOGI(TAG,
             "UC8176 experimental refresh done: update_ready=%d power_off=%s elapsed=%lld ms",
             update_ready ? 1 : 0, power_off_ready ? "ready" : "fallback",
             (long long)((esp_timer_get_time() - start_us) / 1000LL));
    return epd_failure(err);

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t uc8176_display_experimental(const uint8_t *new_data,
                                              epd_refresh_mode_t mode,
                                              bool *did_refresh)
{
    esp_err_t epd_err = ESP_OK;
    epd_dirty_rect_t dirty;
    *did_refresh = false;
    if (!epd_find_dirty_rect(s_previous_bw, new_data, &dirty)) {
        ESP_LOGI(TAG, "UC8176 refresh requested=%s actual=skip dirty=none count=%u/%u",
                 epd_refresh_mode_name(mode), s_experimental_refresh_count,
                 UC8176_EXPERIMENTAL_REFRESH_LIMIT);
        return ESP_OK;
    }

    epd_dirty_rect_t update = dirty;
    if (mode == EPD_REFRESH_FAST) {
        update.x = 0;
        update.y = 0;
        update.w = epd_width();
        update.h = epd_height();
    }

    ESP_LOGI(TAG,
             "UC8176 refresh requested=%s actual=%s dirty=%d,%d %dx%d update=%d,%d %dx%d count=%u/%u",
             epd_refresh_mode_name(mode), epd_refresh_mode_name(mode),
             dirty.x, dirty.y, dirty.w, dirty.h,
             update.x, update.y, update.w, update.h,
             s_experimental_refresh_count + 1,
             UC8176_EXPERIMENTAL_REFRESH_LIMIT);

    esp_err_t err;

    EPD_TRY(err = uc8176_refresh_region(s_previous_bw, new_data, &update));
    if (err != ESP_OK) {
        previous_bw_invalidate();
        return epd_failure(err);
    }

    if (previous_bw_store(new_data, epd_plane_bytes()))
        s_experimental_refresh_count++;
    else
        previous_bw_invalidate();
    *did_refresh = true;
    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t uc8179_75_write_bw_region(uint8_t command,
                                           const uint8_t *data,
                                           const epd_dirty_rect_t *rect)
{
    esp_err_t epd_err = ESP_OK;
    uint8_t *row = NULL;

    const int rb = epd_width() / 8;
    const int row_bytes = rect->w / 8;
    const int x_byte = rect->x / 8;

    EPD_TRY(epd_cmd(0x91));
    EPD_TRY(ref_uc81xx_set_window_scan(rect->x, rect->y, rect->w, rect->h, 0x00));
    EPD_TRY(epd_cmd(command));

    if (rect->x == 0 && rect->w == epd_width()) {
        EPD_TRY(epd_bulk(data + (size_t)rect->y * (size_t)rb,
                 (size_t)rect->h * (size_t)rb));
        EPD_TRY(epd_cmd(0x92));
        return ESP_OK;
    }

    row = heap_caps_malloc((size_t)row_bytes, MALLOC_CAP_DMA);
    if (!row) {
        return epd_failure(ESP_ERR_NO_MEM);
    }

    EPD_TRY(gpio_set_level(PIN_EPD_DC, 1));
    esp_err_t err = ESP_OK;
    for (int y = 0; y < rect->h; y++) {
        size_t offset = (size_t)(rect->y + y) * (size_t)rb + (size_t)x_byte;
        memcpy(row, data + offset, (size_t)row_bytes);
        EPD_TRY(err = spi_tx(row, (size_t)row_bytes));
        if (err != ESP_OK)
            break;
    }
    free(row);
    row = NULL;
    EPD_TRY(epd_cmd(0x92));
    return epd_failure(err);

epd_fail:
    free(row);
    return epd_failure(epd_err);
}

static esp_err_t uc8179_75_refresh_bw_region(const uint8_t *old_data,
                                             const uint8_t *new_data,
                                             const epd_dirty_rect_t *rect)
{
    esp_err_t epd_err = ESP_OK;
    int64_t start_us = esp_timer_get_time();
    esp_err_t err;
    EPD_TRY(err = uc8179_75_write_bw_region(0x13, old_data, rect));
    if (err == ESP_OK)
        EPD_TRY(err = uc8179_75_write_bw_region(0x10, new_data, rect));
    if (err != ESP_OK)
        return epd_failure(err);

    EPD_TRY(uc8179_75_load_bw_partial_lut());
    EPD_TRY(epd_cmd(0x04));
    delay_ms(20);
    EPD_TRY(epd_wait_ready("power-on", false));

    EPD_TRY(epd_cmd(0x91));
    EPD_TRY(ref_uc81xx_set_window_scan(rect->x, rect->y, rect->w, rect->h, 0x00));
    EPD_TRY(epd_cmd(0x12));
    delay_ms(100);
    EPD_TRY(epd_wait_ready("refresh", true));
    bool update_ready = true;
    EPD_TRY(epd_cmd(0x92));

    EPD_TRY(epd_cmd(0x02));
    delay_ms(100);
    EPD_TRY(epd_wait_ready("poweroff", false));
    bool power_off_ready = true;
    ESP_LOGI(TAG,
             "GDEY075Z08 UC8179 BW partial done: window=%d,%d %dx%d update=%s power-off=%s elapsed=%lld ms",
             rect->x, rect->y, rect->w, rect->h,
             update_ready ? "ready" : "fallback",
             power_off_ready ? "ready" : "fallback",
             (long long)((esp_timer_get_time() - start_us) / 1000LL));
    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t uc8179_75_display_partial(const uint8_t *new_data,
                                           bool *did_refresh)
{
    esp_err_t epd_err = ESP_OK;
    epd_dirty_rect_t dirty;
    *did_refresh = false;
    if (!epd_find_dirty_rect(s_previous_bw, new_data, &dirty)) {
        ESP_LOGI(TAG, "GDEY075Z08 UC8179 partial skipped: dirty=none count=%u/%u",
                 s_uc8179_partial_refresh_count,
                 UC8179_75_PARTIAL_REFRESH_LIMIT);
        return ESP_OK;
    }

    ESP_LOGI(TAG,
             "GDEY075Z08 UC8179 partial: dirty=%d,%d %dx%d count=%u/%u",
             dirty.x, dirty.y, dirty.w, dirty.h,
             s_uc8179_partial_refresh_count + 1,
             UC8179_75_PARTIAL_REFRESH_LIMIT);
    esp_err_t err;
    EPD_TRY(err = uc8179_75_refresh_bw_region(s_previous_bw, new_data, &dirty));
    if (err != ESP_OK) {
        previous_bw_invalidate();
        return epd_failure(err);
    }

    if (previous_bw_store(new_data, epd_plane_bytes())) {
        s_uc8179_partial_refresh_count++;
    } else {
        previous_bw_invalidate();
    }
    *did_refresh = true;
    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_ssd16xx_refresh(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(cmd2(0x21, panel_desc()->has_red ? 0x80 : 0x40, 0x00));
    EPD_TRY(cmd1(0x22, 0xF7));
    EPD_TRY(epd_cmd(0x20));
    EPD_TRY(epd_wait_ready("refresh", true));
    if (s_panel == EPD_PANEL_REF_SSD1619_42_BW || s_panel == EPD_PANEL_REF_SSD1619_42_BWR)
        EPD_TRY(ref_ssd1619_full_window());
    else
        EPD_TRY(ref_ssd1677_full_window());
    EPD_TRY(cmd1(0x22, 0x83));
    EPD_TRY(epd_cmd(0x20));
    delay_ms(200);

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_ssd1683_update_once(uint8_t ctrl2, const char *label)
{
    esp_err_t epd_err = ESP_OK;
    ESP_LOGI(TAG, "SSD1683 %s start: ctrl2=0x%02X", label, ctrl2);
    EPD_TRY(epd_cmd(0x22));
    EPD_TRY(epd_dat(ctrl2));
    delay_ms(2);
    EPD_TRY(epd_cmd(0x20));

    TickType_t t0 = xTaskGetTickCount();
    EPD_TRY(epd_wait_ready("refresh", true));
    bool busy_ok = true;
    uint32_t elapsed_ms = pdTICKS_TO_MS(xTaskGetTickCount() - t0);

    if (!busy_ok) {
        ESP_LOGW(TAG, "SSD1683 %s BUSY invalid after %u ms, using fixed wait fallback",
                 label, (unsigned)elapsed_ms);
    } else if (elapsed_ms < 100) {
        ESP_LOGW(TAG, "SSD1683 %s BUSY only %u ms, waiting 2s safety",
                 label, (unsigned)elapsed_ms);
        delay_ms(2000);
    } else {
        delay_ms(200);
    }
    ESP_LOGI(TAG, "SSD1683 %s done in %u ms", label, (unsigned)elapsed_ms);
    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_ssd1683_update(uint8_t ctrl2, const char *label)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(ref_ssd1683_apply_full_refresh_config());
    { EPD_TRY(ref_ssd1683_update_once(ctrl2, label)); return ESP_OK; };

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_ssd1683_refresh(void)
{
    esp_err_t epd_err = ESP_OK;
    if (!panel_desc()->has_red) {
        EPD_TRY(cmd2(0x21, 0x40, 0x00));
        EPD_TRY(ref_ssd1683_update(0xF7, "BW full refresh"));
        EPD_TRY(ref_ssd1683_full_window());
        EPD_TRY(cmd1(0x22, 0x83));
        EPD_TRY(epd_cmd(0x20));
        delay_ms(200);
        return ESP_OK;
    }
    EPD_TRY(ref_ssd1683_update(0xF7, "full refresh"));
    EPD_TRY(ref_ssd1683_full_window());

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t fpc194_ssd1683_refresh(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(ref_ssd1683_full_window());
    EPD_TRY(ref_ssd1683_update_once(0xF7, "FPC-194 full refresh"));
    EPD_TRY(ref_ssd1683_full_window());

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_jd796xx_refresh(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(ref_jd796xx_full_window());
    EPD_TRY(epd_cmd(0x12));
    EPD_TRY(epd_dat(0x00));
    delay_ms(100);
    EPD_TRY(epd_wait_ready("refresh", true));
    delay_ms(200);

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t refresh_atc_dualssd(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_cmd(0x20));
    EPD_TRY(epd_wait_ready("refresh", true));
    delay_ms(200);

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t panel_write_plane(uint8_t cmd, const uint8_t *data, uint8_t fill, bool invert, int pb)
{
    esp_err_t epd_err = ESP_OK;
    uint8_t *buf = NULL;

    EPD_TRY(epd_cmd(cmd));
    if (data && !invert) {
        EPD_TRY(epd_bulk(data, (size_t)pb));
        return ESP_OK;
    }
    if (!data) {
        EPD_TRY(epd_fill(fill, (size_t)pb));
        return ESP_OK;
    }

    buf = heap_caps_malloc(EPD_FILL_CHUNK, MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE(TAG, "invert plane: DMA malloc failed");
        return epd_failure(ESP_ERR_NO_MEM);
    }
    size_t offset = 0;
    while (offset < (size_t)pb) {
        size_t n = ((size_t)pb - offset > EPD_FILL_CHUNK) ? EPD_FILL_CHUNK : ((size_t)pb - offset);
        for (size_t i = 0; i < n; i++)
            buf[i] = (uint8_t)~data[offset + i];
        EPD_TRY(epd_bulk(buf, n));
        offset += n;
    }
    free(buf);
    buf = NULL;

    return ESP_OK;

epd_fail:
    free(buf);
    return epd_failure(epd_err);
}

static esp_err_t hink_ssd1619_write_plane(uint8_t cmd, const uint8_t *data,
                                     uint8_t fill, bool invert, int pb)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    int rb = d->w / 8;
    if ((d->w & 7) != 0 ||
        !panel_uses_hink_ssd1619_wire_format() ||
        rb <= 0 || rb > 64 || pb != rb * d->h) {
        ESP_LOGE(TAG, "HINK-compatible plane geometry invalid: rb=%d pb=%d panel=%dx%d",
                 rb, pb, d->w, d->h);
        return epd_failure(ESP_ERR_INVALID_SIZE);
    }

    uint8_t line[64];
    EPD_TRY(epd_cmd(cmd));
    for (int y = 0; y < d->h; y++) {
        const size_t row = (size_t)y * (size_t)rb;
        for (int x = 0; x < rb; x++) {
            uint8_t v = data ? data[row + (size_t)x] : fill;
            if (invert)
                v = (uint8_t)~v;
            line[x] = v;
        }
        EPD_TRY(epd_bulk(line, (size_t)rb));
    }

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t hink_ssd1619_file_plane(uint8_t cmd, FILE *f, int pb, bool has_data,
                                         bool invert, uint8_t fill)
{
    esp_err_t epd_err = ESP_OK;
    uint8_t *plane = NULL;

    plane = NULL;
    if (has_data) {
        plane = heap_caps_malloc((size_t)pb, MALLOC_CAP_8BIT);
        if (!plane) {
            ESP_LOGE(TAG, "HINK file plane malloc failed: %d", pb);
            return epd_failure(ESP_ERR_NO_MEM);
        }
        if (fread(plane, 1, (size_t)pb, f) != (size_t)pb) {
            free(plane);
            plane = NULL;
            ESP_LOGE(TAG, "HINK file plane short read");
            return epd_failure(ESP_ERR_INVALID_SIZE);
        }
    }
    EPD_TRY(hink_ssd1619_write_plane(cmd, plane, fill, invert, pb));
    free(plane);
    plane = NULL;
    return ESP_OK;

epd_fail:
    free(plane);
    return epd_failure(epd_err);
}

static uint8_t plane_byte_or_fill(const uint8_t *data, size_t index, uint8_t fill)
{
    return data ? data[index] : fill;
}

static bool plane_bit_set(uint8_t byte, int bit)
{
    return (byte & (uint8_t)(0x80u >> bit)) != 0;
}

static esp_err_t panel_write_dualssd_plane(uint8_t cmd, const uint8_t *data,
                                      uint8_t fill, bool right_half)
{
    esp_err_t epd_err = ESP_OK;
    int rb = epd_width() / 8;
    int len = rb / 2 + 1;
    int start = right_half ? rb / 2 : 0;
    int H = epd_height();

    if (len <= 0 || len > 128) {
        ESP_LOGE(TAG, "dual SSD unsupported row split: rb=%d len=%d", rb, len);
        return epd_failure(ESP_ERR_INVALID_SIZE);
    }

    uint8_t line[128];
    EPD_TRY(epd_cmd(cmd));
    for (int y = 0; y < H; y++) {
        const size_t row = (size_t)y * (size_t)rb;
        for (int i = 0; i < len; i++)
            line[i] = plane_byte_or_fill(data, row + (size_t)start + (size_t)i, fill);
        EPD_TRY(epd_bulk(line, (size_t)len));
    }

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t panel_write_dualssd_data(const uint8_t *bw, const uint8_t *red,
                                     uint8_t bw_fill, uint8_t red_fill)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();

    EPD_TRY(panel_write_dualssd_plane(0x24, bw, bw_fill, true));
    if (d->has_red)
        EPD_TRY(panel_write_dualssd_plane(0x26, red, red_fill, true));
    EPD_TRY(panel_write_dualssd_plane(0xA4, bw, bw_fill, false));
    if (d->has_red)
        EPD_TRY(panel_write_dualssd_plane(0xA6, red, red_fill, false));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static void uc8159_interleave_color(uint8_t *dst, uint8_t bw, uint8_t red)
{
    for (int pair = 0; pair < 4; pair++) {
        int bit0 = pair * 2;
        int bit1 = bit0 + 1;
        uint8_t out = 0;
        bool r0 = plane_bit_set(red, bit0);
        bool b0 = !plane_bit_set(bw, bit0);
        bool r1 = plane_bit_set(red, bit1);
        bool b1 = !plane_bit_set(bw, bit1);

        out |= (uint8_t)((r0 ? 0x04 : (b0 ? 0x00 : 0x03)) << 4);
        out |= (uint8_t)(r1 ? 0x04 : (b1 ? 0x00 : 0x03));
        *dst++ = out;
    }
}

static esp_err_t panel_write_uc8159_interleaved(const uint8_t *bw, const uint8_t *red,
                                           uint8_t bw_fill, uint8_t red_fill)
{
    esp_err_t epd_err = ESP_OK;
    uint8_t *line = NULL;

    const bool use_red = panel_desc()->has_red;
    int rb = epd_width() / 8;
    int H = epd_height();
    line = heap_caps_malloc((size_t)rb * 4u, MALLOC_CAP_DMA);
    if (!line) {
        ESP_LOGE(TAG, "UC8159 line DMA malloc failed");
        return epd_failure(ESP_ERR_NO_MEM);
    }

    EPD_TRY(epd_cmd(0x10));
    for (int y = 0; y < H; y++) {
        const size_t row = (size_t)y * (size_t)rb;
        for (int x = 0; x < rb; x++) {
            uint8_t b = plane_byte_or_fill(bw, row + (size_t)x, bw_fill);
            uint8_t r = use_red
                            ? plane_byte_or_fill(red, row + (size_t)x, red_fill)
                            : 0x00;
            uc8159_interleave_color(line + (size_t)x * 4u, b, r);
        }
        EPD_TRY(epd_bulk(line, (size_t)rb * 4u));
    }
    free(line);
    line = NULL;
    EPD_TRY(epd_cmd(0x11));

    return ESP_OK;

epd_fail:
    free(line);
    return epd_failure(epd_err);
}

static uint8_t jd796xx_pack4(uint8_t bw, uint8_t red, uint8_t yellow, int start_bit)
{
    uint8_t out = 0;
    for (int i = 0; i < 4; i++) {
        int bit = start_bit + i;
        uint8_t px = 0x01; /* white, matching the reference clear pattern 0x55 */
        if (!plane_bit_set(bw, bit))
            px = 0x00;     /* black */
        else if (plane_bit_set(yellow, bit))
            px = 0x02;     /* yellow */
        else if (plane_bit_set(red, bit))
            px = 0x03;     /* red */
        out = (uint8_t)((out << 2) | px);
    }
    return out;
}

static esp_err_t panel_write_bwry_2bpp_cmd(uint8_t data_cmd,
                                      const uint8_t *bw, const uint8_t *red,
                                      const uint8_t *yellow,
                                      uint8_t bw_fill, uint8_t red_fill,
                                      uint8_t yellow_fill)
{
    esp_err_t epd_err = ESP_OK;
    uint8_t *line = NULL;

    int rb = epd_width() / 8;
    int H = epd_height();
    line = heap_caps_malloc((size_t)rb * 2u, MALLOC_CAP_DMA);
    if (!line) {
        ESP_LOGE(TAG, "BWRY 2bpp line DMA malloc failed");
        return epd_failure(ESP_ERR_NO_MEM);
    }

    EPD_TRY(epd_cmd(data_cmd));
    for (int y = 0; y < H; y++) {
        const size_t row = (size_t)y * (size_t)rb;
        for (int x = 0; x < rb; x++) {
            uint8_t b = plane_byte_or_fill(bw, row + (size_t)x, bw_fill);
            uint8_t r = plane_byte_or_fill(red, row + (size_t)x, red_fill);
            uint8_t yel = plane_byte_or_fill(yellow, row + (size_t)x, yellow_fill);
            line[(size_t)x * 2u] = jd796xx_pack4(b, r, yel, 0);
            line[(size_t)x * 2u + 1u] = jd796xx_pack4(b, r, yel, 4);
        }
        EPD_TRY(epd_bulk(line, (size_t)rb * 2u));
    }
    free(line);
    line = NULL;

    return ESP_OK;

epd_fail:
    free(line);
    return epd_failure(epd_err);
}

static esp_err_t panel_write_jd796xx_2bpp(const uint8_t *bw, const uint8_t *red,
                                     const uint8_t *yellow,
                                     uint8_t bw_fill, uint8_t red_fill,
                                     uint8_t yellow_fill)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(panel_write_bwry_2bpp_cmd(0x10, bw, red, yellow,
                              bw_fill, red_fill, yellow_fill));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t panel_write_ssd2677_2bpp(const uint8_t *bw, const uint8_t *red,
                                     const uint8_t *yellow,
                                     uint8_t bw_fill, uint8_t red_fill,
                                     uint8_t yellow_fill)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(panel_write_bwry_2bpp_cmd(0x10, bw, red, yellow,
                              bw_fill, red_fill, yellow_fill));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static const uint8_t e6_37_color_map_pass1[16] = {
    1, 1, 2, 3, 0, 1, 0, 1,
    1, 1, 1, 1, 1, 1, 1, 1
};

static const uint8_t e6_37_color_map_pass2[16] = {
    0, 1, 1, 3, 1, 2, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1
};

static void e6_37_log_elapsed(const char *stage, int64_t start_us)
{
    ESP_LOGI(TAG, "E6 3.7: %s done in %lld ms",
             stage ? stage : "?", (long long)((esp_timer_get_time() - start_us) / 1000LL));
}

static esp_err_t e6_37_wait_ready(const char *stage)
{
    const int idle = busy_idle_effective();
    int elapsed = 0;
    int64_t start_us = esp_timer_get_time();
    bool refresh = strcmp(stage, "refresh") == 0;
    s_last_wait_saw_busy_phase = false;

    while (refresh && gpio_get_level(PIN_EPD_BUSY) == idle && elapsed < 800) {
        delay_ms(10);
        elapsed += 10;
    }
    bool missing_phase = refresh && gpio_get_level(PIN_EPD_BUSY) == idle;
    elapsed = 0;

    while (gpio_get_level(PIN_EPD_BUSY) != idle && elapsed < EPD_E6_BUSY_TIMEOUT_MS) {
        s_last_wait_saw_busy_phase = true;
        delay_ms(10);
        elapsed += 10;
    }

    esp_err_t err = missing_phase || gpio_get_level(PIN_EPD_BUSY) != idle
                        ? ESP_ERR_TIMEOUT : ESP_OK;
    ESP_LOGI(TAG, "BUSY panel=%s stage=%s detail=%s GPIO%d=%d idle=%d phase=%d duration=%lld ms result=%s",
             panel_desc()->name, refresh ? "refresh" : (strcmp(stage, "sleep") == 0 ? "poweroff" : "init"),
             stage, PIN_EPD_BUSY,
             gpio_get_level(PIN_EPD_BUSY), idle, s_last_wait_saw_busy_phase,
             (long long)((esp_timer_get_time() - start_us) / 1000), esp_err_to_name(err));
    return epd_failure(err);
}

static esp_err_t e6_37_hw_reset(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(gpio_set_level(PIN_EPD_RST, 1));
    delay_ms(20);
    EPD_TRY(gpio_set_level(PIN_EPD_RST, 0));
    delay_ms(100);
    EPD_TRY(gpio_set_level(PIN_EPD_RST, 1));
    delay_ms(100);
    EPD_TRY(e6_37_wait_ready("reset"));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t e6_37_set_temperature_waveform(uint8_t value)
{
    esp_err_t epd_err = ESP_OK;
    int64_t start_us = esp_timer_get_time();
    ESP_LOGI(TAG, "E6 3.7: set temperature waveform %u begin", (unsigned)value);
    EPD_TRY(cmd1(0xE0, 0x02));
    EPD_TRY(cmd1(0xE6, value));
    EPD_TRY(e6_37_wait_ready("temperature waveform"));
    e6_37_log_elapsed("set temperature waveform", start_us);

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t e6_37_refresh_once(const char *stage)
{
    esp_err_t epd_err = ESP_OK;
    int64_t start_us = esp_timer_get_time();
    ESP_LOGI(TAG, "E6 3.7: %s refresh begin", stage ? stage : "?");
    EPD_TRY(cmd1(0x17, 0xA5));
    EPD_TRY(e6_37_wait_ready("refresh"));
    ESP_LOGI(TAG, "E6 3.7: %s refresh done in %lld ms",
             stage ? stage : "?",
             (long long)((esp_timer_get_time() - start_us) / 1000LL));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t e6_37_sleep(void)
{
    esp_err_t epd_err = ESP_OK;
    int64_t start_us = esp_timer_get_time();
    ESP_LOGI(TAG, "E6 3.7: sleep begin");
    EPD_TRY(e6_37_wait_ready("sleep"));
    EPD_TRY(cmd1(0x07, 0xA5));
    s_panel_seq_done = false;
    e6_37_log_elapsed("sleep", start_us);

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static uint8_t e6_37_pixel_code(uint8_t bw, uint8_t red, uint8_t yellow,
                                uint8_t blue, uint8_t green, int bit)
{
    if (!plane_bit_set(bw, bit))
        return 0; /* black */
    if (plane_bit_set(yellow, bit))
        return 2; /* yellow */
    if (plane_bit_set(red, bit))
        return 3; /* red */
    if (plane_bit_set(blue, bit))
        return 5; /* logical blue: physical E6 code pair that renders blue */
    if (plane_bit_set(green, bit))
        return 4; /* logical green: physical E6 code pair that renders green */
    return 1;     /* white */
}

static uint8_t e6_37_pack4(uint8_t bw, uint8_t red, uint8_t yellow,
                           uint8_t blue, uint8_t green,
                           int start_bit, const uint8_t color_map[16])
{
    uint8_t out = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t src = e6_37_pixel_code(bw, red, yellow, blue, green,
                                       start_bit + i);
        uint8_t px = color_map[src & 0x0F] & 0x03;
        out = (uint8_t)((out << 2) | px);
    }
    return out;
}

static esp_err_t e6_37_write_pass_from_planes(const uint8_t *bw, const uint8_t *red,
                                         const uint8_t *yellow,
                                         const uint8_t *blue,
                                         const uint8_t *green,
                                         uint8_t bw_fill, uint8_t red_fill,
                                         uint8_t yellow_fill,
                                         uint8_t blue_fill,
                                         uint8_t green_fill,
                                         const uint8_t color_map[16],
                                         const char *stage)
{
    esp_err_t epd_err = ESP_OK;
    uint8_t *line = NULL;

    int rb = epd_width() / 8;
    int H = epd_height();
    int64_t start_us = esp_timer_get_time();
    line = heap_caps_malloc((size_t)rb * 2u, MALLOC_CAP_DMA);
    if (!line) {
        ESP_LOGE(TAG, "E6 3.7 line DMA malloc failed");
        return epd_failure(ESP_ERR_NO_MEM);
    }

    ESP_LOGI(TAG, "E6 3.7: %s data write begin (%dx%d, rb=%d)",
             stage ? stage : "?", epd_width(), H, rb);
    EPD_TRY(epd_cmd(0x10));
    for (int y = 0; y < H; y++) {
        const size_t row = (size_t)y * (size_t)rb;
        for (int x = 0; x < rb; x++) {
            uint8_t b = plane_byte_or_fill(bw, row + (size_t)x, bw_fill);
            uint8_t r = plane_byte_or_fill(red, row + (size_t)x, red_fill);
            uint8_t yel = plane_byte_or_fill(yellow, row + (size_t)x, yellow_fill);
            uint8_t blu = plane_byte_or_fill(blue, row + (size_t)x, blue_fill);
            uint8_t grn = plane_byte_or_fill(green, row + (size_t)x, green_fill);
            line[(size_t)x * 2u] = e6_37_pack4(b, r, yel, blu, grn, 0, color_map);
            line[(size_t)x * 2u + 1u] = e6_37_pack4(b, r, yel, blu, grn, 4, color_map);
        }
        EPD_TRY(epd_bulk(line, (size_t)rb * 2u));
        if (((y + 1) % (H / 4 ? H / 4 : 1)) == 0 || y + 1 == H) {
            ESP_LOGI(TAG, "E6 3.7: %s data row %d/%d after %lld ms",
                     stage ? stage : "?", y + 1, H,
                     (long long)((esp_timer_get_time() - start_us) / 1000LL));
        }
    }

    free(line);
    line = NULL;
    ESP_LOGI(TAG, "E6 3.7: %s data write done in %lld ms",
             stage ? stage : "?",
             (long long)((esp_timer_get_time() - start_us) / 1000LL));

    return ESP_OK;

epd_fail:
    free(line);
    return epd_failure(epd_err);
}

static esp_err_t panel_write_e6_37_6c(const uint8_t *bw, const uint8_t *red,
                                 const uint8_t *yellow,
                                 const uint8_t *blue,
                                 const uint8_t *green,
                                 uint8_t bw_fill, uint8_t red_fill,
                                 uint8_t yellow_fill,
                                 uint8_t blue_fill,
                                 uint8_t green_fill)
{
    esp_err_t epd_err = ESP_OK;
    int64_t start_us = esp_timer_get_time();
    ESP_LOGI(TAG, "E6 3.7: framebuffer display begin");
    EPD_TRY(e6_37_set_temperature_waveform(32));
    EPD_TRY(e6_37_write_pass_from_planes(bw, red, yellow, blue, green,
                                 bw_fill, red_fill, yellow_fill,
                                 blue_fill, green_fill,
                                 e6_37_color_map_pass1, "pass1"));
    EPD_TRY(e6_37_refresh_once("pass1"));

    EPD_TRY(e6_37_set_temperature_waveform(37));
    EPD_TRY(e6_37_write_pass_from_planes(bw, red, yellow, blue, green,
                                 bw_fill, red_fill, yellow_fill,
                                 blue_fill, green_fill,
                                 e6_37_color_map_pass2, "pass2"));
    EPD_TRY(e6_37_refresh_once("pass2"));
    EPD_TRY(e6_37_sleep());
    ESP_LOGI(TAG, "E6 3.7: framebuffer display done in %lld ms",
             (long long)((esp_timer_get_time() - start_us) / 1000LL));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t panel_write_bw_red_data(const uint8_t *bw, const uint8_t *red, int pb)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    if (d->cursor_home)
        EPD_TRY(d->cursor_home());

    switch (d->data_mode) {
    case EPD_CMD_DATA_24_26:
        if (panel_uses_hink_ssd1619_wire_format()) {
            if (d->has_red) {
                EPD_TRY(hink_ssd1619_write_plane(0x26, red, 0x00, false, pb));
                if (d->cursor_home)
                    EPD_TRY(d->cursor_home());
            }
            EPD_TRY(hink_ssd1619_write_plane(0x24, bw, 0xFF, true, pb));
            break;
        }
        EPD_TRY(panel_write_plane(0x24, bw, 0xFF, panel_needs_inverted_black_plane(), pb));
        if (d->has_red) {
            if (d->cursor_home)
                EPD_TRY(d->cursor_home());
            EPD_TRY(panel_write_plane(0x26, red, 0x00, false, pb));
        }
        break;
    case EPD_CMD_DATA_24_26_SSD16XX:
        EPD_TRY(panel_write_plane(0x24, bw, 0xFF,
                          panel_uses_inverted_ssd16xx_black_plane(), pb));
        if (d->has_red) {
            if (d->cursor_home)
                EPD_TRY(d->cursor_home());
            if (panel_uses_noninverted_ssd16xx_red_plane()) {
                /* Waveshare sends ~ryimage because its source image uses 0=red;
                   our framebuffer red plane is already 1=red. */
                EPD_TRY(panel_write_plane(0x26, red, 0x00, false, pb));
            } else {
                EPD_TRY(panel_write_plane(0x26, red, 0xFF, true, pb));
            }
        } else {
            if (d->cursor_home)
                EPD_TRY(d->cursor_home());
            EPD_TRY(panel_write_plane(0x26, NULL, 0xFF, false, pb));
        }
        break;
    case EPD_CMD_DATA_10_13:
        EPD_TRY(panel_write_plane(0x10, bw, 0xFF, false, pb));
        EPD_TRY(panel_write_plane(0x13, d->has_red ? red : bw, d->has_red ? 0x00 : 0xFF, false, pb));
        break;
    case EPD_CMD_DATA_10_92_13:
    case EPD_CMD_DATA_UC81XX_REF:
        EPD_TRY(epd_cmd(0x91));
        EPD_TRY(ref_uc81xx_full_window());
        EPD_TRY(panel_write_plane(0x10, d->has_red ? bw : NULL, 0xFF, false, pb));
        EPD_TRY(epd_cmd(0x92));
        EPD_TRY(epd_cmd(0x91));
        EPD_TRY(ref_uc81xx_full_window());
        EPD_TRY(panel_write_plane(0x13, d->has_red ? red : bw, d->has_red ? 0xFF : 0xFF, d->has_red, pb));
        EPD_TRY(epd_cmd(0x92));
        break;
    case EPD_CMD_DATA_UC8179_75_BWR:
        EPD_TRY(ref_uc81xx_full_window());
        EPD_TRY(panel_write_plane(0x10, bw, 0xFF, false, pb));
        EPD_TRY(ref_uc81xx_full_window());
        EPD_TRY(panel_write_plane(0x13, red, 0xFF, true, pb));
        break;
    case EPD_CMD_DATA_JD79686A_BWR:
        /* JD79686A: BW->0x10(DTM1), Red->0x13(DTM2) */
        EPD_TRY(panel_write_plane(0x10, bw, 0xFF, false, pb));  /* DTM1: BW */
        EPD_TRY(panel_write_plane(0x13, red, 0x00, false, pb));  /* DTM2: Red */
        break;
    case EPD_CMD_DATA_10_13_INV_RED:
        EPD_TRY(panel_write_plane(0x10, bw, 0xFF, false, pb));
        EPD_TRY(panel_write_plane(0x13, red, 0xFF, true, pb));
        break;
    case EPD_CMD_DATA_10_13_DUMMY:
        EPD_TRY(epd_dat(0x00));
        EPD_TRY(panel_write_plane(0x10, bw, 0xFF, false, pb));
        EPD_TRY(panel_write_plane(0x13, d->has_red ? red : NULL, d->has_red ? 0x00 : 0xFF, false, pb));
        break;
    case EPD_CMD_DATA_10_13_INV_BLACK_DUMMY:
        EPD_TRY(epd_dat(0x00));
        EPD_TRY(panel_write_plane(0x10, bw, 0xFF, true, pb));
        EPD_TRY(panel_write_plane(0x13, red, 0x00, false, pb));
        break;
    case EPD_CMD_DATA_DUAL_SSD:
        EPD_TRY(panel_write_dualssd_data(bw, red, 0xFF, 0x00));
        break;
    case EPD_CMD_DATA_UC8159_INTERLEAVED:
        EPD_TRY(panel_write_uc8159_interleaved(bw, red, 0xFF, 0x00));
        break;
    case EPD_CMD_DATA_JD796XX_2BPP:
        EPD_TRY(panel_write_jd796xx_2bpp(bw, red, NULL, 0xFF, 0x00, 0x00));
        break;
    case EPD_CMD_DATA_SSD2677_2BPP:
        EPD_TRY(panel_write_ssd2677_2bpp(bw, red, NULL, 0xFF, 0x00, 0x00));
        break;
    case EPD_CMD_DATA_E6_3IN7_6C:
        EPD_TRY(panel_write_e6_37_6c(bw, red, NULL, NULL, NULL,
                             0xFF, 0x00, 0x00, 0x00, 0x00));
        break;
    case EPD_CMD_DATA_UC8253_BW:
        EPD_TRY(panel_write_plane(0x10,
                          previous_bw_matches_current_panel() ? s_previous_bw : NULL,
                          0xFF, false, pb));
        EPD_TRY(panel_write_plane(0x13, bw, 0xFF, false, pb));
        break;
    }

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ssd16xx_213_bw_cursor_home(void)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    uint16_t y = (uint16_t)(SSD16XX_213_H - 1u);
    EPD_TRY(epd_cmd(0x4E));
    EPD_TRY(epd_dat((uint8_t)(d->x_offset / 8u)));
    EPD_TRY(epd_cmd(0x4F));
    EPD_TRY(epd_dat((uint8_t)y));
    EPD_TRY(epd_dat((uint8_t)(y >> 8)));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t panel_write_bw_red(const uint8_t *bw, const uint8_t *red, int pb)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(panel_ensure_ready());
    EPD_TRY(panel_write_bw_red_data(bw, red, pb));
    EPD_TRY(panel_desc()->refresh());
    if (panel_is_uc8253_37()) {
        if (previous_bw_store(bw, pb))
            s_uc8253_partial_refresh_count = 0;
        else
            previous_bw_invalidate();
    }
    if (panel_is_uc8179_75_bwr()) {
        if (previous_bw_store(bw, pb)) {
            s_uc8179_red_signature = plane_signature(red, pb, 0x00);
            s_uc8179_red_signature_valid = true;
            s_uc8179_partial_refresh_count = 0;
        } else {
            previous_bw_invalidate();
        }
    }
    if (panel_is_jd79686a_583()) {
        if (previous_bw_store(bw, pb)) {
            s_jd79686a_red_signature = plane_signature(red, pb, 0x00);
            s_jd79686a_red_signature_valid = true;
            s_jd79686a_partial_refresh_count = 0;
            jd79686a_prev_red_store(red, 0x00, pb);
        } else {
            previous_bw_invalidate();
        }
    }

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t file_send_plane(FILE *f, int pb, bool invert)
{
    esp_err_t epd_err = ESP_OK;
    uint8_t *buf = NULL;

    buf = heap_caps_malloc(EPD_FILL_CHUNK, MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE(TAG, "file plane DMA malloc failed");
        return epd_failure(ESP_ERR_NO_MEM);
    }

    EPD_TRY(gpio_set_level(PIN_EPD_DC, 1));
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
        EPD_TRY(ret = spi_tx(buf, n));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "file plane spi err %s", esp_err_to_name(ret));
            break;
        }
        sent += n;
    }

    free(buf);
    buf = NULL;
    return epd_failure(ret);

epd_fail:
    free(buf);
    return epd_failure(epd_err);
}

static esp_err_t file_read_row(FILE *f, long base, int row, int rb,
                               uint8_t *dst, uint8_t fill)
{
    if (base < 0) {
        memset(dst, fill, (size_t)rb);
        return ESP_OK;
    }
    if (fseek(f, base + (long)row * rb, SEEK_SET) != 0)
        return ESP_FAIL;
    if (fread(dst, 1, (size_t)rb, f) != (size_t)rb)
        return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

static esp_err_t file_send_dualssd_plane(FILE *f, long base, int rb, int H,
                                         uint8_t fill, uint8_t cmd,
                                         bool right_half)
{
    esp_err_t epd_err = ESP_OK;
    uint8_t *row = NULL;
    uint8_t *out = NULL;

    int len = rb / 2 + 1;
    int start = right_half ? rb / 2 : 0;
    row = heap_caps_malloc((size_t)rb, MALLOC_CAP_8BIT);
    out = heap_caps_malloc((size_t)len, MALLOC_CAP_DMA);
    if (!row || !out) {
        free(row);
        row = NULL;
        free(out);
        out = NULL;
        return epd_failure(ESP_ERR_NO_MEM);
    }

    EPD_TRY(epd_cmd(cmd));
    esp_err_t ret = ESP_OK;
    for (int y = 0; y < H; y++) {
        EPD_TRY(ret = file_read_row(f, base, y, rb, row, fill));
        if (ret != ESP_OK)
            break;
        memcpy(out, row + start, (size_t)len);
        EPD_TRY(epd_bulk(out, (size_t)len));
    }

    free(row);
    row = NULL;
    free(out);
    out = NULL;
    return epd_failure(ret);

epd_fail:
    free(row);
    free(out);
    return epd_failure(epd_err);
}

static esp_err_t file_send_dualssd(FILE *f, int pb, bool has_red_file)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    int rb = d->w / 8;
    long red_base = has_red_file ? (long)pb : -1;
    esp_err_t ret;
    EPD_TRY(ret = file_send_dualssd_plane(f, 0, rb, d->h, 0xFF, 0x24, true));
    if (ret == ESP_OK && d->has_red)
        EPD_TRY(ret = file_send_dualssd_plane(f, red_base, rb, d->h, 0x00, 0x26, true));
    if (ret == ESP_OK)
        EPD_TRY(ret = file_send_dualssd_plane(f, 0, rb, d->h, 0xFF, 0xA4, false));
    if (ret == ESP_OK && d->has_red)
        EPD_TRY(ret = file_send_dualssd_plane(f, red_base, rb, d->h, 0x00, 0xA6, false));
    return epd_failure(ret);

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t file_send_uc8159_interleaved(FILE *f, int pb, bool has_red_file)
{
    esp_err_t epd_err = ESP_OK;
    uint8_t *bw = NULL;
    uint8_t *red = NULL;
    uint8_t *out = NULL;

    int rb = epd_width() / 8;
    int H = epd_height();
    bw = heap_caps_malloc((size_t)rb, MALLOC_CAP_8BIT);
    red = heap_caps_malloc((size_t)rb, MALLOC_CAP_8BIT);
    out = heap_caps_malloc((size_t)rb * 4u, MALLOC_CAP_DMA);
    if (!bw || !red || !out) {
        free(bw);
        bw = NULL;
        free(red);
        red = NULL;
        free(out);
        out = NULL;
        return epd_failure(ESP_ERR_NO_MEM);
    }

    const bool use_red = panel_desc()->has_red && has_red_file;
    long red_base = use_red ? (long)pb : -1;
    EPD_TRY(epd_cmd(0x10));
    esp_err_t ret = ESP_OK;
    for (int y = 0; y < H; y++) {
        EPD_TRY(ret = file_read_row(f, 0, y, rb, bw, 0xFF));
        if (ret != ESP_OK)
            break;
        EPD_TRY(ret = file_read_row(f, red_base, y, rb, red, 0x00));
        if (ret != ESP_OK)
            break;
        for (int x = 0; x < rb; x++)
            uc8159_interleave_color(out + (size_t)x * 4u, bw[x], red[x]);
        EPD_TRY(epd_bulk(out, (size_t)rb * 4u));
    }
    EPD_TRY(epd_cmd(0x11));

    free(bw);
    bw = NULL;
    free(red);
    red = NULL;
    free(out);
    out = NULL;
    return epd_failure(ret);

epd_fail:
    free(bw);
    free(red);
    free(out);
    return epd_failure(epd_err);
}

static esp_err_t file_send_bwry_2bpp_cmd(FILE *f, int pb, bool has_red_file,
                                         bool has_yellow_file,
                                         uint8_t data_cmd)
{
    esp_err_t epd_err = ESP_OK;
    uint8_t *bw = NULL;
    uint8_t *red = NULL;
    uint8_t *yellow = NULL;
    uint8_t *out = NULL;

    int rb = epd_width() / 8;
    int H = epd_height();
    bw = heap_caps_malloc((size_t)rb, MALLOC_CAP_8BIT);
    red = heap_caps_malloc((size_t)rb, MALLOC_CAP_8BIT);
    yellow = heap_caps_malloc((size_t)rb, MALLOC_CAP_8BIT);
    out = heap_caps_malloc((size_t)rb * 2u, MALLOC_CAP_DMA);
    if (!bw || !red || !yellow || !out) {
        free(bw);
        bw = NULL;
        free(red);
        red = NULL;
        free(yellow);
        yellow = NULL;
        free(out);
        out = NULL;
        return epd_failure(ESP_ERR_NO_MEM);
    }

    long red_base = has_red_file ? (long)pb : -1;
    long yellow_base = has_yellow_file ? (long)pb * 2L : -1;
    EPD_TRY(epd_cmd(data_cmd));
    esp_err_t ret = ESP_OK;
    for (int y = 0; y < H; y++) {
        EPD_TRY(ret = file_read_row(f, 0, y, rb, bw, 0xFF));
        if (ret != ESP_OK)
            break;
        EPD_TRY(ret = file_read_row(f, red_base, y, rb, red, 0x00));
        if (ret != ESP_OK)
            break;
        EPD_TRY(ret = file_read_row(f, yellow_base, y, rb, yellow, 0x00));
        if (ret != ESP_OK)
            break;
        for (int x = 0; x < rb; x++) {
            out[(size_t)x * 2u] = jd796xx_pack4(bw[x], red[x], yellow[x], 0);
            out[(size_t)x * 2u + 1u] = jd796xx_pack4(bw[x], red[x], yellow[x], 4);
        }
        EPD_TRY(epd_bulk(out, (size_t)rb * 2u));
    }

    free(bw);
    bw = NULL;
    free(red);
    red = NULL;
    free(yellow);
    yellow = NULL;
    free(out);
    out = NULL;
    return epd_failure(ret);

epd_fail:
    free(bw);
    free(red);
    free(yellow);
    free(out);
    return epd_failure(epd_err);
}

static esp_err_t file_send_jd796xx_2bpp(FILE *f, int pb, bool has_red_file,
                                        bool has_yellow_file)
{
    esp_err_t epd_err = ESP_OK;
    { EPD_TRY(file_send_bwry_2bpp_cmd(f, pb, has_red_file, has_yellow_file, 0x10)); return ESP_OK; };

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t file_send_ssd2677_2bpp(FILE *f, int pb, bool has_red_file,
                                        bool has_yellow_file)
{
    esp_err_t epd_err = ESP_OK;
    { EPD_TRY(file_send_bwry_2bpp_cmd(f, pb, has_red_file, has_yellow_file, 0x10)); return ESP_OK; };

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t e6_37_write_pass_from_file(FILE *f, int pb, bool has_red_file,
                                            bool has_yellow_file,
                                            bool has_blue_file,
                                            bool has_green_file,
                                            const uint8_t color_map[16],
                                            const char *stage)
{
    esp_err_t epd_err = ESP_OK;
    uint8_t *bw = NULL;
    uint8_t *red = NULL;
    uint8_t *yellow = NULL;
    uint8_t *blue = NULL;
    uint8_t *green = NULL;
    uint8_t *out = NULL;

    int rb = epd_width() / 8;
    int H = epd_height();
    int64_t start_us = esp_timer_get_time();
    bw = heap_caps_malloc((size_t)rb, MALLOC_CAP_8BIT);
    red = heap_caps_malloc((size_t)rb, MALLOC_CAP_8BIT);
    yellow = heap_caps_malloc((size_t)rb, MALLOC_CAP_8BIT);
    blue = heap_caps_malloc((size_t)rb, MALLOC_CAP_8BIT);
    green = heap_caps_malloc((size_t)rb, MALLOC_CAP_8BIT);
    out = heap_caps_malloc((size_t)rb * 2u, MALLOC_CAP_DMA);
    if (!bw || !red || !yellow || !blue || !green || !out) {
        free(bw);
        bw = NULL;
        free(red);
        red = NULL;
        free(yellow);
        yellow = NULL;
        free(blue);
        blue = NULL;
        free(green);
        green = NULL;
        free(out);
        out = NULL;
        return epd_failure(ESP_ERR_NO_MEM);
    }

    long red_base = has_red_file ? (long)pb : -1;
    long yellow_base = has_yellow_file ? (long)pb * 2L : -1;
    long blue_base = has_blue_file ? (long)pb * 3L : -1;
    long green_base = has_green_file ? (long)pb * 4L : -1;
    ESP_LOGI(TAG, "E6 3.7: %s file data write begin (%dx%d, rb=%d, planes R/Y/B/G=%d/%d/%d/%d)",
             stage ? stage : "?", epd_width(), H, rb,
             has_red_file ? 1 : 0, has_yellow_file ? 1 : 0,
             has_blue_file ? 1 : 0, has_green_file ? 1 : 0);
    EPD_TRY(epd_cmd(0x10));
    esp_err_t ret = ESP_OK;
    for (int y = 0; y < H; y++) {
        EPD_TRY(ret = file_read_row(f, 0, y, rb, bw, 0xFF));
        if (ret != ESP_OK)
            break;
        EPD_TRY(ret = file_read_row(f, red_base, y, rb, red, 0x00));
        if (ret != ESP_OK)
            break;
        EPD_TRY(ret = file_read_row(f, yellow_base, y, rb, yellow, 0x00));
        if (ret != ESP_OK)
            break;
        EPD_TRY(ret = file_read_row(f, blue_base, y, rb, blue, 0x00));
        if (ret != ESP_OK)
            break;
        EPD_TRY(ret = file_read_row(f, green_base, y, rb, green, 0x00));
        if (ret != ESP_OK)
            break;
        for (int x = 0; x < rb; x++) {
            out[(size_t)x * 2u] = e6_37_pack4(bw[x], red[x], yellow[x],
                                              blue[x], green[x], 0, color_map);
            out[(size_t)x * 2u + 1u] = e6_37_pack4(bw[x], red[x], yellow[x],
                                                   blue[x], green[x], 4, color_map);
        }
        EPD_TRY(epd_bulk(out, (size_t)rb * 2u));
        if (((y + 1) % (H / 4 ? H / 4 : 1)) == 0 || y + 1 == H) {
            ESP_LOGI(TAG, "E6 3.7: %s file data row %d/%d after %lld ms",
                     stage ? stage : "?", y + 1, H,
                     (long long)((esp_timer_get_time() - start_us) / 1000LL));
        }
    }

    free(bw);
    bw = NULL;
    free(red);
    red = NULL;
    free(yellow);
    yellow = NULL;
    free(blue);
    blue = NULL;
    free(green);
    green = NULL;
    free(out);
    out = NULL;
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "E6 3.7: %s file data write done in %lld ms",
                 stage ? stage : "?",
                 (long long)((esp_timer_get_time() - start_us) / 1000LL));
    } else {
        ESP_LOGW(TAG, "E6 3.7: %s file data write failed after %lld ms: %s",
                 stage ? stage : "?",
                 (long long)((esp_timer_get_time() - start_us) / 1000LL),
                 esp_err_to_name(ret));
    }
    return epd_failure(ret);

epd_fail:
    free(bw);
    free(red);
    free(yellow);
    free(blue);
    free(green);
    free(out);
    return epd_failure(epd_err);
}

static esp_err_t file_send_e6_37_6c(FILE *f, int pb, bool has_red_file,
                                    bool has_yellow_file,
                                    bool has_blue_file,
                                    bool has_green_file)
{
    esp_err_t epd_err = ESP_OK;
    int64_t start_us = esp_timer_get_time();
    ESP_LOGI(TAG, "E6 3.7: file display begin (pb=%d)", pb);
    EPD_TRY(e6_37_set_temperature_waveform(32));
    esp_err_t ret;
    EPD_TRY(ret = e6_37_write_pass_from_file(f, pb, has_red_file, has_yellow_file,
                                               has_blue_file, has_green_file,
                                               e6_37_color_map_pass1, "pass1"));
    if (ret != ESP_OK)
        return epd_failure(ret);
    EPD_TRY(e6_37_refresh_once("pass1"));

    EPD_TRY(e6_37_set_temperature_waveform(37));
    EPD_TRY(ret = e6_37_write_pass_from_file(f, pb, has_red_file, has_yellow_file,
                                     has_blue_file, has_green_file,
                                     e6_37_color_map_pass2, "pass2"));
    if (ret != ESP_OK)
        return epd_failure(ret);
    EPD_TRY(e6_37_refresh_once("pass2"));
    EPD_TRY(e6_37_sleep());
    ESP_LOGI(TAG, "E6 3.7: file display done in %lld ms",
             (long long)((esp_timer_get_time() - start_us) / 1000LL));
    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t panel_write_bw_red_file(FILE *f, int pb, bool has_red_file,
                                         bool has_yellow_file,
                                         bool has_blue_file,
                                         bool has_green_file)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    esp_err_t ret = ESP_OK;
    if (d->cursor_home)
        EPD_TRY(d->cursor_home());

    switch (d->data_mode) {
    case EPD_CMD_DATA_24_26:
        if (panel_uses_hink_ssd1619_wire_format()) {
            if (has_red_file && fseek(f, (long)pb, SEEK_SET) != 0)
                return epd_failure(ESP_FAIL);
            EPD_TRY(ret = hink_ssd1619_file_plane(0x26, f, pb, has_red_file, false, 0x00));
            if (ret != ESP_OK)
                return epd_failure(ret);
            if (fseek(f, 0, SEEK_SET) != 0)
                return epd_failure(ESP_FAIL);
            if (d->cursor_home)
                EPD_TRY(d->cursor_home());
            EPD_TRY(ret = hink_ssd1619_file_plane(0x24, f, pb, true, true, 0xFF));
        } else {
            EPD_TRY(epd_cmd(0x24));
            EPD_TRY(ret = file_send_plane(f, pb, panel_needs_inverted_black_plane()));
            if (ret != ESP_OK)
                return epd_failure(ret);
            if (d->has_red) {
                if (d->cursor_home)
                    EPD_TRY(d->cursor_home());
                EPD_TRY(epd_cmd(0x26));
                EPD_TRY(ret = has_red_file ? file_send_plane(f, pb, false) : epd_fill(0x00, (size_t)pb));
            }
        }
        break;
    case EPD_CMD_DATA_24_26_SSD16XX:
        EPD_TRY(epd_cmd(0x24));
        EPD_TRY(ret = file_send_plane(f, pb, panel_uses_inverted_ssd16xx_black_plane()));
        if (ret != ESP_OK)
            return epd_failure(ret);
        if (d->cursor_home)
            EPD_TRY(d->cursor_home());
        EPD_TRY(epd_cmd(0x26));
        if (d->has_red) {
            if (panel_uses_noninverted_ssd16xx_red_plane())
                EPD_TRY(ret = has_red_file ? file_send_plane(f, pb, false) : epd_fill(0x00, (size_t)pb));
            else
                EPD_TRY(ret = has_red_file ? file_send_plane(f, pb, true) : epd_fill(0xFF, (size_t)pb));
        } else {
            if (fseek(f, 0, SEEK_SET) != 0)
                return epd_failure(ESP_FAIL);
            EPD_TRY(epd_fill(0xFF, (size_t)pb));
            ret = ESP_OK;
        }
        break;
    case EPD_CMD_DATA_10_13:
        EPD_TRY(epd_cmd(0x10));
        EPD_TRY(ret = file_send_plane(f, pb, false));
        if (ret != ESP_OK)
            return epd_failure(ret);
        EPD_TRY(epd_cmd(0x13));
        if (d->has_red) {
            EPD_TRY(ret = has_red_file ? file_send_plane(f, pb, false) : epd_fill(0x00, (size_t)pb));
        } else {
            if (fseek(f, 0, SEEK_SET) != 0)
                return epd_failure(ESP_FAIL);
            EPD_TRY(ret = file_send_plane(f, pb, false));
        }
        break;
    case EPD_CMD_DATA_10_92_13:
    case EPD_CMD_DATA_UC81XX_REF:
        EPD_TRY(epd_cmd(0x91));
        EPD_TRY(ref_uc81xx_full_window());
        EPD_TRY(epd_cmd(0x10));
        if (d->has_red) {
            EPD_TRY(ret = file_send_plane(f, pb, false));
            if (ret != ESP_OK)
                return epd_failure(ret);
        } else {
            EPD_TRY(epd_fill(0xFF, (size_t)pb));
        }
        EPD_TRY(epd_cmd(0x92));
        EPD_TRY(epd_cmd(0x91));
        EPD_TRY(ref_uc81xx_full_window());
        EPD_TRY(epd_cmd(0x13));
        if (d->has_red) {
            EPD_TRY(ret = has_red_file ? file_send_plane(f, pb, true) : epd_fill(0xFF, (size_t)pb));
        } else {
            if (fseek(f, 0, SEEK_SET) != 0)
                return epd_failure(ESP_FAIL);
            EPD_TRY(ret = file_send_plane(f, pb, false));
        }
        EPD_TRY(epd_cmd(0x92));
        break;
    case EPD_CMD_DATA_UC8179_75_BWR:
        EPD_TRY(ref_uc81xx_full_window());
        EPD_TRY(epd_cmd(0x10));
        EPD_TRY(ret = file_send_plane(f, pb, false));
        if (ret != ESP_OK)
            return epd_failure(ret);
        EPD_TRY(ref_uc81xx_full_window());
        EPD_TRY(epd_cmd(0x13));
        EPD_TRY(ret = has_red_file ? file_send_plane(f, pb, true) : epd_fill(0xFF, (size_t)pb));
        break;
    case EPD_CMD_DATA_JD79686A_BWR:
        /* JD79686A: BW->0x10(DTM1), Red->0x13(DTM2) */
        EPD_TRY(epd_cmd(0x10));               /* DTM1: 写BW数据 */
        EPD_TRY(ret = file_send_plane(f, pb, false));
        if (ret != ESP_OK)
            return epd_failure(ret);
        EPD_TRY(epd_cmd(0x13));               /* DTM2: 写Red数据 */
        EPD_TRY(ret = has_red_file ? file_send_plane(f, pb, true) : epd_fill(0x00, (size_t)pb));
        break;
    case EPD_CMD_DATA_10_13_INV_RED:
        EPD_TRY(epd_cmd(0x10));
        EPD_TRY(ret = file_send_plane(f, pb, false));
        if (ret != ESP_OK)
            return epd_failure(ret);
        EPD_TRY(epd_cmd(0x13));
        EPD_TRY(ret = has_red_file ? file_send_plane(f, pb, true) : epd_fill(0xFF, (size_t)pb));
        break;
    case EPD_CMD_DATA_10_13_DUMMY:
        EPD_TRY(epd_dat(0x00));
        EPD_TRY(epd_cmd(0x10));
        EPD_TRY(ret = file_send_plane(f, pb, false));
        if (ret != ESP_OK)
            return epd_failure(ret);
        EPD_TRY(epd_cmd(0x13));
        EPD_TRY(ret = has_red_file ? file_send_plane(f, pb, false) : epd_fill(d->has_red ? 0x00 : 0xFF, (size_t)pb));
        break;
    case EPD_CMD_DATA_10_13_INV_BLACK_DUMMY:
        EPD_TRY(epd_dat(0x00));
        EPD_TRY(epd_cmd(0x10));
        EPD_TRY(ret = file_send_plane(f, pb, true));
        if (ret != ESP_OK)
            return epd_failure(ret);
        EPD_TRY(epd_cmd(0x13));
        EPD_TRY(ret = has_red_file ? file_send_plane(f, pb, false) : epd_fill(0x00, (size_t)pb));
        break;
    case EPD_CMD_DATA_DUAL_SSD:
        EPD_TRY(ret = file_send_dualssd(f, pb, has_red_file));
        break;
    case EPD_CMD_DATA_UC8159_INTERLEAVED:
        EPD_TRY(ret = file_send_uc8159_interleaved(f, pb, has_red_file));
        break;
    case EPD_CMD_DATA_JD796XX_2BPP:
        EPD_TRY(ret = file_send_jd796xx_2bpp(f, pb, has_red_file, has_yellow_file));
        break;
    case EPD_CMD_DATA_SSD2677_2BPP:
        EPD_TRY(ret = file_send_ssd2677_2bpp(f, pb, has_red_file, has_yellow_file));
        break;
    case EPD_CMD_DATA_E6_3IN7_6C:
        EPD_TRY(ret = file_send_e6_37_6c(f, pb, has_red_file, has_yellow_file,
                                 has_blue_file, has_green_file));
        break;
    case EPD_CMD_DATA_UC8253_BW:
        EPD_TRY(epd_cmd(0x10));
        EPD_TRY(epd_fill(0xFF, (size_t)pb));
        if (fseek(f, 0, SEEK_SET) != 0)
            return epd_failure(ESP_FAIL);
        EPD_TRY(epd_cmd(0x13));
        EPD_TRY(ret = file_send_plane(f, pb, false));
        break;
    }

    if (ret == ESP_OK)
        EPD_TRY(d->refresh());
    return epd_failure(ret);

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t panel_fill_bw_red(uint8_t bw_byte, uint8_t red_byte, int pb)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(panel_ensure_ready());
    const epd_panel_desc_t *d = panel_desc();
    if (d->cursor_home)
        EPD_TRY(d->cursor_home());

    switch (d->data_mode) {
    case EPD_CMD_DATA_24_26:
        if (panel_uses_hink_ssd1619_wire_format()) {
            if (d->has_red) {
                EPD_TRY(hink_ssd1619_write_plane(0x26, NULL, red_byte, false, pb));
                if (d->cursor_home)
                    EPD_TRY(d->cursor_home());
            }
            EPD_TRY(hink_ssd1619_write_plane(0x24, NULL, bw_byte, true, pb));
            break;
        }
        EPD_TRY(panel_write_plane(0x24, NULL,
                          panel_needs_inverted_black_plane() ? (uint8_t)~bw_byte : bw_byte,
                          false, pb));
        if (d->has_red) {
            if (d->cursor_home)
                EPD_TRY(d->cursor_home());
            EPD_TRY(panel_write_plane(0x26, NULL, red_byte, false, pb));
        }
        break;
    case EPD_CMD_DATA_24_26_SSD16XX:
        EPD_TRY(panel_write_plane(0x24, NULL,
                          panel_uses_inverted_ssd16xx_black_plane()
                              ? (uint8_t)~bw_byte
                              : bw_byte,
                          false, pb));
        if (d->cursor_home)
            EPD_TRY(d->cursor_home());
        if (d->has_red && panel_uses_noninverted_ssd16xx_red_plane())
            EPD_TRY(panel_write_plane(0x26, NULL, red_byte, false, pb));
        else
            EPD_TRY(panel_write_plane(0x26, NULL, d->has_red ? (uint8_t)~red_byte : 0xFF, false, pb));
        break;
    case EPD_CMD_DATA_10_13:
        EPD_TRY(panel_write_plane(0x10, NULL, bw_byte, false, pb));
        EPD_TRY(panel_write_plane(0x13, NULL, d->has_red ? red_byte : bw_byte, false, pb));
        break;
    case EPD_CMD_DATA_10_92_13:
    case EPD_CMD_DATA_UC81XX_REF:
        EPD_TRY(epd_cmd(0x91));
        EPD_TRY(ref_uc81xx_full_window());
        EPD_TRY(panel_write_plane(0x10, NULL, d->has_red ? bw_byte : 0xFF, false, pb));
        EPD_TRY(epd_cmd(0x92));
        EPD_TRY(epd_cmd(0x91));
        EPD_TRY(ref_uc81xx_full_window());
        EPD_TRY(panel_write_plane(0x13, NULL, d->has_red ? (uint8_t)~red_byte : bw_byte, false, pb));
        EPD_TRY(epd_cmd(0x92));
        break;
    case EPD_CMD_DATA_UC8179_75_BWR:
        EPD_TRY(ref_uc81xx_full_window());
        EPD_TRY(panel_write_plane(0x10, NULL, bw_byte, false, pb));
        EPD_TRY(ref_uc81xx_full_window());
        EPD_TRY(panel_write_plane(0x13, NULL, (uint8_t)~red_byte, false, pb));
        break;
    case EPD_CMD_DATA_JD79686A_BWR:
        EPD_TRY(ref_uc81xx_full_window());
        EPD_TRY(panel_write_plane(0x10, NULL, bw_byte, false, pb));  /* DTM1: BW */
        EPD_TRY(ref_uc81xx_full_window());
        EPD_TRY(panel_write_plane(0x13, NULL, red_byte, false, pb)); /* DTM2: Red */
        break;
    case EPD_CMD_DATA_10_13_INV_RED:
        EPD_TRY(panel_write_plane(0x10, NULL, bw_byte, false, pb));
        EPD_TRY(panel_write_plane(0x13, NULL, (uint8_t)~red_byte, false, pb));
        break;
    case EPD_CMD_DATA_10_13_DUMMY:
        EPD_TRY(epd_dat(0x00));
        EPD_TRY(panel_write_plane(0x10, NULL, bw_byte, false, pb));
        EPD_TRY(panel_write_plane(0x13, NULL, d->has_red ? red_byte : bw_byte, false, pb));
        break;
    case EPD_CMD_DATA_10_13_INV_BLACK_DUMMY:
        EPD_TRY(epd_dat(0x00));
        EPD_TRY(panel_write_plane(0x10, NULL, (uint8_t)~bw_byte, false, pb));
        EPD_TRY(panel_write_plane(0x13, NULL, red_byte, false, pb));
        break;
    case EPD_CMD_DATA_DUAL_SSD:
        EPD_TRY(panel_write_dualssd_data(NULL, NULL, bw_byte, red_byte));
        break;
    case EPD_CMD_DATA_UC8159_INTERLEAVED:
        EPD_TRY(panel_write_uc8159_interleaved(NULL, NULL, bw_byte, red_byte));
        break;
    case EPD_CMD_DATA_JD796XX_2BPP:
        EPD_TRY(panel_write_jd796xx_2bpp(NULL, NULL, NULL, bw_byte, red_byte, 0x00));
        break;
    case EPD_CMD_DATA_SSD2677_2BPP:
        EPD_TRY(panel_write_ssd2677_2bpp(NULL, NULL, NULL, bw_byte, red_byte, 0x00));
        break;
    case EPD_CMD_DATA_E6_3IN7_6C:
        EPD_TRY(panel_write_e6_37_6c(NULL, NULL, NULL, NULL, NULL,
                             bw_byte, red_byte, 0x00, 0x00, 0x00));
        break;
    case EPD_CMD_DATA_UC8253_BW:
        EPD_TRY(panel_write_plane(0x10,
                          previous_bw_matches_current_panel() ? s_previous_bw : NULL,
                          0xFF, false, pb));
        EPD_TRY(panel_write_plane(0x13, NULL, bw_byte, false, pb));
        break;
    }

    EPD_TRY(d->refresh());
    if (panel_is_uc8253_37()) {
        if (previous_bw_fill(bw_byte, pb))
            s_uc8253_partial_refresh_count = 0;
        else
            previous_bw_invalidate();
    }
    if (panel_is_uc8179_75_bwr()) {
        if (previous_bw_fill(bw_byte, pb)) {
            s_uc8179_red_signature = plane_signature(NULL, pb, red_byte);
            s_uc8179_red_signature_valid = true;
            s_uc8179_partial_refresh_count = 0;
        } else {
            previous_bw_invalidate();
        }
    }
    if (panel_is_jd79686a_583()) {
        if (previous_bw_fill(bw_byte, pb)) {
            s_jd79686a_red_signature = plane_signature(NULL, pb, red_byte);
            s_jd79686a_red_signature_valid = true;
            s_jd79686a_partial_refresh_count = 0;
            jd79686a_prev_red_store(NULL, red_byte, pb);
        } else {
            previous_bw_invalidate();
        }
    }

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

/* ATC/Solum tag controller sequences, adapted from
 * atc1441/Tag_FW_nRF52811 tag_fw/src/epd_driver. The nRF project reads
 * tag metadata from UICR; this firmware exposes common tag types manually
 * and keeps the existing ESP-IDF SPI/BUSY/framebuffer layer.
 */

static esp_err_t atc_unissd_init(void)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    uint16_t y0 = 0;
    uint16_t y1 = (uint16_t)d->h;

    EPD_TRY(epd_hw_reset());
    EPD_TRY(epd_cmd(0x12));
    delay_ms(10);

    EPD_TRY(cmd3(0x01, (uint8_t)(d->h & 0xFF), (uint8_t)(d->h >> 8), 0x00));
    if (d->mirror_v) {
        EPD_TRY(cmd1(0x11, 0x03));
        EPD_TRY(cmd4(0x45, (uint8_t)y0, (uint8_t)(y0 >> 8), (uint8_t)y1, (uint8_t)(y1 >> 8)));
    } else {
        EPD_TRY(cmd1(0x11, 0x01));
        EPD_TRY(cmd4(0x45, (uint8_t)y1, (uint8_t)(y1 >> 8), (uint8_t)y0, (uint8_t)(y0 >> 8)));
    }
    EPD_TRY(cmd2(0x44, (uint8_t)(d->x_offset / 8u), (uint8_t)(((d->x_offset + d->w) / 8u) - 1u)));
    EPD_TRY(cmd1(0x3C, 0x05));
    EPD_TRY(cmd1(0x18, 0x80));
    EPD_TRY(cmd2(0x21, d->has_red ? 0x08 : 0x48, 0x00));
    EPD_TRY(atc_unissd_cursor_home());
    if (s_panel == EPD_PANEL_ATC_SSD1619_29_BWR)
        ESP_LOGI(TAG, "%s init OK", d->name);
    else
        ESP_LOGI(TAG, "%s init OK", d->name);

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t hink_ssd1619_29_init(void)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    uint16_t y0 = 0;
    uint16_t y1 = (uint16_t)(HINK29_LOGICAL_H - 1u);
    uint8_t x0 = (uint8_t)(d->x_offset / 8u);
    uint8_t x1 = (uint8_t)(((d->x_offset + HINK29_LOGICAL_W) / 8u) - 1u);

    EPD_TRY(epd_hw_reset());
    EPD_TRY(epd_cmd(0x12));
    delay_ms(10);

    EPD_TRY(cmd1(0x74, 0x54));
    EPD_TRY(cmd1(0x7E, 0x3B));
    EPD_TRY(cmd2(0x2B, 0x04, 0x63));
    EPD_TRY(cmd4(0x0C, 0x8F, 0x8F, 0x8F, 0x3F));
    EPD_TRY(cmd3(0x01, (uint8_t)(y1 & 0xFF), (uint8_t)(y1 >> 8), 0x00));
    EPD_TRY(cmd1(0x11, 0x01));
    EPD_TRY(cmd2(0x44, x0, x1));
    EPD_TRY(cmd4(0x45, (uint8_t)y1, (uint8_t)(y1 >> 8),
         (uint8_t)y0, (uint8_t)(y0 >> 8)));
    EPD_TRY(cmd1(0x3C, 0x01));
    EPD_TRY(cmd1(0x18, 0x80));
    EPD_TRY(cmd2(0x21, 0x08, 0x00));
    EPD_TRY(cmd1(0x22, 0xB1));
    EPD_TRY(epd_cmd(0x20));
    EPD_TRY(epd_wait_ready("init", true));
    EPD_TRY(hink_ssd1619_29_cursor_home());
    ESP_LOGI(TAG, "%s init OK (%dx%d, x window=%u..%u)",
             panel_desc()->name, HINK29_LOGICAL_W, HINK29_LOGICAL_H,
             (unsigned)x0, (unsigned)x1);

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ssd16xx_213_bw_init(void)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    uint16_t y0 = 0;
    uint16_t y1 = (uint16_t)(SSD16XX_213_H - 1u);
    uint8_t x0 = (uint8_t)(d->x_offset / 8u);
    uint8_t x1 = (uint8_t)(((d->x_offset + SSD16XX_213_RAM_W) / 8u) - 1u);

    EPD_TRY(epd_hw_reset());
    EPD_TRY(epd_cmd(0x12));
    delay_ms(10);

    /* Preserve the HINK/SSD16xx sequence already proven to update this panel. */
    EPD_TRY(cmd1(0x74, 0x54));
    EPD_TRY(cmd1(0x7E, 0x3B));
    EPD_TRY(cmd2(0x2B, 0x04, 0x63));
    EPD_TRY(cmd4(0x0C, 0x8F, 0x8F, 0x8F, 0x3F));
    EPD_TRY(cmd3(0x01, (uint8_t)(y1 & 0xFF), (uint8_t)(y1 >> 8), 0x00));
    EPD_TRY(cmd1(0x11, 0x01));
    EPD_TRY(cmd2(0x44, x0, x1));
    EPD_TRY(cmd4(0x45, (uint8_t)y1, (uint8_t)(y1 >> 8),
         (uint8_t)y0, (uint8_t)(y0 >> 8)));
    EPD_TRY(cmd1(0x3C, 0x01));
    EPD_TRY(cmd1(0x18, 0x80));
    EPD_TRY(cmd2(0x21, 0x08, 0x00));
    EPD_TRY(cmd1(0x22, 0xB1));
    EPD_TRY(epd_cmd(0x20));
    EPD_TRY(epd_wait_ready("init", true));
    EPD_TRY(ssd16xx_213_bw_cursor_home());
    ESP_LOGI(TAG,
             "%s init OK (RAM=%dx%d active=%dx%d x window=%u..%u)",
             d->name, SSD16XX_213_RAM_W, SSD16XX_213_H,
             SSD16XX_213_ACTIVE_W, SSD16XX_213_H,
             (unsigned)x0, (unsigned)x1);

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t atc_unissd_97_init(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_hw_reset());
    EPD_TRY(cmd1(0x46, 0xF7));
    delay_ms(15);
    EPD_TRY(cmd1(0x47, 0xF7));
    delay_ms(15);
    EPD_TRY(cmd5(0x0C, 0xAE, 0xC7, 0xC3, 0xC0, 0x80));
    EPD_TRY(cmd3(0x01, 0x9F, 0x02, 0x00));
    EPD_TRY(cmd1(0x11, 0x02));
    EPD_TRY(cmd4(0x44, 0xBF, 0x03, 0x00, 0x00));
    EPD_TRY(cmd4(0x45, 0x00, 0x00, 0x9F, 0x02));
    EPD_TRY(cmd1(0x3C, 0x01));
    EPD_TRY(cmd1(0x18, 0x80));
    EPD_TRY(cmd1(0x22, 0xF7));
    EPD_TRY(cmd2(0x21, 0x08, 0x00));
    EPD_TRY(atc_unissd_97_cursor_home());
    ESP_LOGI(TAG, "ATC/Solum 9.7 SSD init OK");

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_ssd2677_97_init(void)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();

    EPD_TRY(gpio_set_level(PIN_EPD_RST, 1));
    delay_ms(100);
    EPD_TRY(gpio_set_level(PIN_EPD_RST, 0));
    delay_ms(10);
    EPD_TRY(gpio_set_level(PIN_EPD_RST, 1));
    delay_ms(10);
    ESP_LOGI(TAG, "SSD2677 9.7: BUSY gpio after reset = %d (idle expect %d)",
             (int)gpio_get_level(PIN_EPD_BUSY), busy_idle_effective());
    EPD_TRY(epd_wait_ready("init", false));

    EPD_TRY(cmd2(0x00, 0x2F, 0x29));

    EPD_TRY(epd_cmd(0x01));
    EPD_TRY(epd_dat(0x07));
    EPD_TRY(epd_dat(0x00));
    EPD_TRY(epd_dat(0x20));
    EPD_TRY(epd_dat(0x1E));
    EPD_TRY(epd_dat(0x78));
    EPD_TRY(epd_dat(0x20));

    EPD_TRY(cmd3(0x03, 0x00, 0x00, 0x00));
    EPD_TRY(cmd4(0x06, 0x0F, 0x8B, 0x9C, 0xC1));
    EPD_TRY(cmd1(0x30, 0x08));
    EPD_TRY(cmd1(0x41, 0x00));
    EPD_TRY(cmd1(0x50, 0x37));
    EPD_TRY(cmd2(0x60, 0x04, 0x02));
    EPD_TRY(ref_ssd2677_97_full_window());
    EPD_TRY(cmd4(0x65, 0x00, 0x00, 0x00, 0x00));
    EPD_TRY(cmd1(0xE7, 0x16));
    EPD_TRY(cmd1(0xE3, 0x65));
    EPD_TRY(cmd1(0xE0, 0x00));

    EPD_TRY(epd_cmd(0x62));
    EPD_TRY(epd_dat(0x77));
    EPD_TRY(epd_dat(0x77));
    EPD_TRY(epd_dat(0x77));
    EPD_TRY(epd_dat(0x5C));
    EPD_TRY(epd_dat(0x9F));
    EPD_TRY(epd_dat(0x8C));
    EPD_TRY(epd_dat(0x77));
    EPD_TRY(epd_dat(0x63));

    EPD_TRY(cmd1(0xE9, 0x01));
    EPD_TRY(epd_cmd(0x04));
    EPD_TRY(epd_wait_ready("power-on", false));

    ESP_LOGI(TAG, "%s init OK (%dx%d BWRY 2bpp, reference sequence)",
             d->name, d->w, d->h);

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_ssd1667_102_init(void)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    uint16_t y1 = (uint16_t)(d->h - 1);

    EPD_TRY(epd_hw_reset());
    ESP_LOGI(TAG, "SSD1667 10.2: BUSY gpio after reset = %d (idle expect %d)",
             (int)gpio_get_level(PIN_EPD_BUSY), busy_idle_effective());
    EPD_TRY(epd_wait_ready("init", false));

    EPD_TRY(epd_cmd(0x12));
    EPD_TRY(epd_wait_ready("init", false));

    EPD_TRY(cmd1(0x46, 0xF7));
    delay_ms(15);
    EPD_TRY(cmd1(0x47, 0xF7));
    delay_ms(15);

    EPD_TRY(cmd1(0x74, 0x54));
    EPD_TRY(cmd1(0x7E, 0x3B));
    EPD_TRY(cmd5(0x0C, 0xAE, 0xC7, 0xC3, 0xC0, 0x80));
    EPD_TRY(cmd3(0x01, (uint8_t)(y1 & 0xFF), (uint8_t)(y1 >> 8), 0x00));
    EPD_TRY(ref_ssd1667_102_full_window());
    EPD_TRY(cmd1(0x3C, 0x05));
    EPD_TRY(cmd1(0x2C, 0x55));
    EPD_TRY(cmd1(0x18, 0x80));
    EPD_TRY(cmd2(0x21, 0x08, 0x00));
    EPD_TRY(ref_ssd1667_102_cursor_home());

    ESP_LOGI(TAG, "%s init OK (%dx%d)", d->name, d->w, d->h);

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t e6_37_init(void)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();

    EPD_TRY(e6_37_hw_reset());

    EPD_TRY(cmd2(0x00, 0x27, 0x29));
    EPD_TRY(cmd2(0x01, 0x07, 0x00));
    EPD_TRY(cmd3(0x03, 0x10, 0x54, 0x44));
    EPD_TRY(cmd4(0x06, 0x25, 0x25, 0x3C, 0x17));

    EPD_TRY(cmd1(0x30, 0x08));
    EPD_TRY(e6_37_wait_ready("pll"));

    EPD_TRY(cmd1(0x41, 0x00));
    EPD_TRY(cmd1(0x50, 0x37));
    EPD_TRY(cmd2(0x62, 0x02, 0x02));

    EPD_TRY(epd_cmd(0x83));
    EPD_TRY(epd_dat(0x00));
    EPD_TRY(epd_dat(0x00));
    EPD_TRY(epd_dat(0x03));
    EPD_TRY(epd_dat(0x17));
    EPD_TRY(epd_dat(0x00));
    EPD_TRY(epd_dat(0x48));
    EPD_TRY(epd_dat(0x02));
    EPD_TRY(epd_dat(0x57));
    EPD_TRY(epd_dat(0x01));
    EPD_TRY(e6_37_wait_ready("window"));

    EPD_TRY(cmd1(0xE7, 0x1C));
    EPD_TRY(cmd1(0xE3, 0x22));
    EPD_TRY(cmd1(0xE9, 0x01));
    EPD_TRY(cmd1(0xE1, 0x02));

    ESP_LOGI(TAG, "%s init OK (%dx%d)", d->name, d->w, d->h);

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t uc8253_init_mode(epd_refresh_mode_t mode)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(gpio_set_level(PIN_EPD_RST, 0));
    delay_ms(10);
    EPD_TRY(gpio_set_level(PIN_EPD_RST, 1));
    delay_ms(10);

    EPD_TRY(epd_cmd(0x04));
    EPD_TRY(epd_wait_ready("power-on", false));

    if (mode == EPD_REFRESH_FAST) {
        EPD_TRY(cmd1(0xE0, 0x02));
        EPD_TRY(cmd1(0xE5, 0x5F));
    } else if (mode == EPD_REFRESH_PARTIAL) {
        EPD_TRY(cmd1(0xE0, 0x02));
        EPD_TRY(cmd1(0xE5, 0x6E));
        EPD_TRY(cmd1(0x50, 0xD7));
    } else {
        EPD_TRY(cmd1(0x50, 0x97));
    }

    ESP_LOGI(TAG, "GDEY037T03 UC8253 init OK (%s, 416x240 BW)",
             epd_refresh_mode_name(mode));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t uc8253_init(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(uc8253_init_mode(EPD_REFRESH_FULL));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

/*
 * P426010-MF1-B / Waveshare 4.26inch e-Paper, 800x480 BW.
 * Source sequence: waveshareteam/e-Paper EPD_4in26.c (full refresh).
 */
static esp_err_t ws_426_bw_init(void)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();

    EPD_TRY(epd_hw_reset());
    delay_ms(100);
    EPD_TRY(epd_wait_ready("init", false));

    EPD_TRY(epd_cmd(0x12));
    EPD_TRY(epd_wait_ready("init", false));

    EPD_TRY(cmd1(0x18, 0x80));
    EPD_TRY(cmd5(0x0C, 0xAE, 0xC7, 0xC3, 0xC0, 0x80));
    EPD_TRY(cmd3(0x01, 0xDF, 0x01, 0x02));
    EPD_TRY(cmd1(0x3C, 0x01));
    EPD_TRY(cmd1(0x11, 0x01));

    EPD_TRY(cmd4(0x44, 0x00, 0x00, 0x1F, 0x03));
    EPD_TRY(cmd4(0x45, 0xDF, 0x01, 0x00, 0x00));
    EPD_TRY(ws_426_bw_cursor_home());

    ESP_LOGI(TAG, "%s init OK (%dx%d BW, full refresh)",
             d->name, d->w, d->h);

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t atc_ucvar29_init(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_hw_reset());
    EPD_TRY(cmd1(0x4D, 0x55));
    EPD_TRY(cmd1(0xF3, 0x0A));
    EPD_TRY(cmd1(0x31, 0x00));
    EPD_TRY(cmd3(0x06, 0xE5, 0x35, 0x3C));
    EPD_TRY(cmd1(0x50, 0x57));
    EPD_TRY(cmd2(0x00, 0x07, 0x09));
    ESP_LOGI(TAG, "ATC/Solum UC8151 2.9 init OK");

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t atc_ucvar43_init(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_hw_reset());
    EPD_TRY(cmd2(0xF8, 0x60, 0x05));
    EPD_TRY(cmd2(0xF8, 0xA1, 0x00));
    EPD_TRY(cmd2(0xF8, 0x73, 0x05));
    EPD_TRY(cmd2(0xF8, 0x7E, 0x31));
    EPD_TRY(cmd2(0xF8, 0xB8, 0x80));
    EPD_TRY(cmd2(0xF8, 0x92, 0x00));
    EPD_TRY(cmd2(0xF8, 0x87, 0x11));
    EPD_TRY(cmd2(0xF8, 0x88, 0x06));
    EPD_TRY(cmd2(0xF8, 0xA8, 0x30));
    EPD_TRY(cmd4(0x61, 0x00, 0x98, 0x02, 0x0A));
    EPD_TRY(cmd3(0x06, 0x57, 0x63, 0x3A));
    EPD_TRY(cmd1(0x50, 0x87));
    ESP_LOGI(TAG, "ATC/Solum UC 4.3 init OK");

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t atc_dualssd_init(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_hw_reset());
    EPD_TRY(epd_cmd(0x12));
    delay_ms(10);
    EPD_TRY(cmd1(0x11, 0x02));
    EPD_TRY(cmd1(0x91, 0x03));
    EPD_TRY(cmd2(0x21, panel_desc()->has_red ? 0x08 : 0x48, 0x10));
    EPD_TRY(cmd2(0x44, 0x31, 0x00));
    EPD_TRY(cmd4(0x45, 0x00, 0x00, 0x0F, 0x01));
    EPD_TRY(cmd1(0x4E, 0x31));
    EPD_TRY(cmd2(0x4F, 0x00, 0x00));
    EPD_TRY(cmd2(0xC4, 0x00, 0x31));
    EPD_TRY(cmd4(0xC5, 0x00, 0x00, 0x0F, 0x01));
    EPD_TRY(cmd1(0xCE, 0x00));
    EPD_TRY(cmd2(0xCF, 0x0F, 0x01));
    EPD_TRY(cmd1(0x3C, 0x01));
    ESP_LOGI(TAG, "%s init OK", panel_desc()->name);

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t atc_uc8159_init(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_hw_reset());
    EPD_TRY(cmd2(0x00, 0xEF, 0x08));
    EPD_TRY(cmd4(0x01, 0x37, 0x00, 0x05, 0x05));
    EPD_TRY(cmd1(0x03, 0x00));
    EPD_TRY(cmd3(0x06, 0xC7, 0xCC, 0x1D));
    EPD_TRY(epd_wait_ready("init", false));
    EPD_TRY(epd_cmd(0x04));
    EPD_TRY(epd_wait_ready("power-on", false));
    EPD_TRY(cmd1(0x13, 0x00));
    EPD_TRY(cmd1(0x30, 0x3C));
    EPD_TRY(cmd1(0x41, 0x00));
    EPD_TRY(cmd1(0x50, 0x77));
    EPD_TRY(cmd1(0x60, 0x22));
    EPD_TRY(cmd4(0x61, 0x02, 0x58, 0x01, 0xC0));
    EPD_TRY(cmd1(0x65, 0x00));
    EPD_TRY(cmd1(0x82, 0x1E));
    ESP_LOGW(TAG, "ATC/Solum UC8159 init uses fallback VCOM/PLL; source driver expects external EEPROM pins not present here");

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t atc_uc8179_init(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_hw_reset());
    EPD_TRY(cmd1(0x00, 0x0F));
    EPD_TRY(cmd2(0x50, 0x30, 0x07));
    EPD_TRY(cmd4(0x61, 0x03, 0x20, 0x01, 0xE0));
    ESP_LOGI(TAG, "ATC/Solum UC8179 7.4 init OK");

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_uc81xx_init(void)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    EPD_TRY(epd_hw_reset());
    EPD_TRY(cmd1(0x00, d->has_red ? 0x0F : 0x1F));
    EPD_TRY(cmd1(0x50, d->has_red ? 0x77 : 0x97));
    EPD_TRY(ref_send_tres());
    EPD_TRY(ref_uc81xx_full_window());
    ESP_LOGI(TAG, "%s init OK", d->name);

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

/* GDEW075T7/GD7965-compatible 800x480 BW OTP full-refresh setup.
 * HT075A04 uses this as an isolated controller candidate pending real-panel proof. */
static esp_err_t gd7965_uc8179_75_bw_init(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_hw_reset());
    EPD_TRY(cmd4(0x01, 0x07, 0x07, 0x3F, 0x3F));
    EPD_TRY(cmd1(0x00, 0x1F)); /* BW waveform from controller OTP. */
    EPD_TRY(ref_send_tres());
    EPD_TRY(cmd1(0x15, 0x00));
    EPD_TRY(cmd2(0x50, 0x29, 0x07)); /* Copy new image to previous image after update. */
    EPD_TRY(cmd1(0x60, 0x22));
    EPD_TRY(ref_uc81xx_full_window());
    ESP_LOGI(TAG, "GD7965/UC8179 7.5 BW candidate init OK (800x480, OTP full refresh)");

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

/* Good Display GDEY075Z08, 800x480 BWR, UC8179 full-refresh setup. */
static esp_err_t ref_uc8179_75_bwr_init(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_hw_reset());
    EPD_TRY(cmd4(0x01, 0x07, 0x07, 0x3F, 0x3F));
    EPD_TRY(cmd4(0x06, 0x17, 0x17, 0x28, 0x17));
    EPD_TRY(cmd1(0x00, 0x0F));
    EPD_TRY(ref_send_tres());
    EPD_TRY(cmd1(0x15, 0x00));
    EPD_TRY(cmd2(0x50, 0x13, 0x07));
    EPD_TRY(cmd1(0x60, 0x22));
    EPD_TRY(ref_uc81xx_full_window());
    ESP_LOGI(TAG, "GDEY075Z08 7.5 HD BWR UC8179 init OK (800x480, full refresh)");

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_uc8159_init(void)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    EPD_TRY(epd_hw_reset());
    EPD_TRY(cmd2(0x01, 0x37, 0x00));
    EPD_TRY(cmd2(0x00, 0xCF, 0x08));
    EPD_TRY(cmd1(0x30, 0x3A));
    EPD_TRY(cmd1(0x82, 0x28));
    EPD_TRY(cmd3(0x06, 0xC7, 0xCC, 0x15));
    EPD_TRY(cmd1(0x50, 0x77));
    EPD_TRY(cmd1(0x60, 0x22));
    EPD_TRY(cmd1(0x65, 0x00));
    EPD_TRY(cmd1(0xE5, 0x03));
    EPD_TRY(ref_send_tres());
    EPD_TRY(ref_uc81xx_full_window());
    ESP_LOGI(TAG, "%s init OK", d->name);

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_ssd1619_init(void)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    EPD_TRY(epd_hw_reset());
    EPD_TRY(epd_cmd(0x12));
    EPD_TRY(epd_wait_ready("init", false));
    EPD_TRY(cmd1(0x3C, 0x01));
    EPD_TRY(cmd1(0x18, 0x80));
    EPD_TRY(ref_ssd1619_full_window());
    ESP_LOGI(TAG, "%s init OK", d->name);

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_ssd1683_init(void)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    EPD_TRY(epd_hw_reset_waveshare_v2());
    EPD_TRY(epd_wait_ready("init", false));

    EPD_TRY(epd_cmd(0x12));
    EPD_TRY(epd_wait_ready("init", false));

    EPD_TRY(cmd1(0x74, 0x54));
    EPD_TRY(cmd1(0x7E, 0x3B));
    EPD_TRY(cmd1(0x2C, 0x55));
    EPD_TRY(cmd1(0x03, 0x15));
    EPD_TRY(cmd3(0x04, 0x41, 0xA8, 0x32));
    EPD_TRY(cmd1(0x3A, 0x2C));
    EPD_TRY(cmd1(0x3B, 0x0B));
    EPD_TRY(ref_ssd1683_apply_full_refresh_config());
    EPD_TRY(epd_wait_ready("init", false));
    ESP_LOGI(TAG, "%s init OK", d->name);

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t fpc194_ssd1683_init(void)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    EPD_TRY(epd_hw_reset_waveshare_v2());
    EPD_TRY(epd_wait_ready("init", false));

    EPD_TRY(epd_cmd(0x12));
    delay_ms(10);

    EPD_TRY(ref_ssd1683_apply_full_refresh_config());
    EPD_TRY(epd_wait_ready("init", false));
    ESP_LOGI(TAG, "%s init OK (OTP waveform reference)", d->name);

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_ssd1677_init(void)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    EPD_TRY(epd_hw_reset());
    EPD_TRY(epd_cmd(0x12));
    EPD_TRY(epd_wait_ready("init", false));
    EPD_TRY(cmd1(0x3C, 0x01));
    EPD_TRY(cmd1(0x18, 0x80));
    EPD_TRY(ref_ssd1677_full_window());
    ESP_LOGI(TAG, "%s init OK", d->name);

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_jd79665_init(void)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    EPD_TRY(epd_hw_reset());
    delay_ms(50);

    EPD_TRY(cmd1(0x4D, 0x78));
    EPD_TRY(cmd2(0x00, 0x2F, 0x29));
    EPD_TRY(cmd4(0x06, 0x0F, 0x8B, 0x93, 0xA1));
    EPD_TRY(cmd1(0x41, 0x00));
    EPD_TRY(cmd1(0x50, 0x37));
    EPD_TRY(cmd2(0x60, 0x02, 0x02));
    EPD_TRY(ref_send_tres());
    EPD_TRY(epd_cmd(0x62));
    EPD_TRY(epd_dat(0x98)); EPD_TRY(epd_dat(0x98)); EPD_TRY(epd_dat(0x98)); EPD_TRY(epd_dat(0x75));
    EPD_TRY(epd_dat(0xCA)); EPD_TRY(epd_dat(0xB2)); EPD_TRY(epd_dat(0x98)); EPD_TRY(epd_dat(0x7E));
    EPD_TRY(cmd4(0x65, 0x00, 0x00, 0x00, 0x00));
    EPD_TRY(cmd1(0xE7, 0x1C));
    EPD_TRY(cmd1(0xE3, 0x00));
    EPD_TRY(cmd1(0xE9, 0x01));
    EPD_TRY(cmd1(0x30, 0x08));
    EPD_TRY(ref_jd796xx_full_window());
    EPD_TRY(epd_cmd(0x04));
    EPD_TRY(epd_wait_ready("power-on", false));
    ESP_LOGI(TAG, "%s init OK (JD79665 800x480 BWRY 2bpp)", d->name);

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_jd79668_init(void)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    EPD_TRY(epd_hw_reset());
    ESP_LOGI(TAG, "JD79668 4.2: BUSY gpio after reset = %d (idle expect %d)",
             (int)gpio_get_level(PIN_EPD_BUSY), busy_idle_effective());
    EPD_TRY(epd_wait_ready("init", false));

    EPD_TRY(cmd1(0x4D, 0x78));
    EPD_TRY(cmd2(0x00, 0x0F, 0x29));
    EPD_TRY(epd_cmd(0x06));
    EPD_TRY(epd_dat(0x0D)); EPD_TRY(epd_dat(0x12)); EPD_TRY(epd_dat(0x24)); EPD_TRY(epd_dat(0x25));
    EPD_TRY(epd_dat(0x12)); EPD_TRY(epd_dat(0x29)); EPD_TRY(epd_dat(0x10));
    EPD_TRY(cmd1(0x30, 0x08));
    EPD_TRY(cmd1(0x50, 0x37));
    EPD_TRY(ref_send_tres());
    EPD_TRY(cmd1(0xAE, 0xCF));
    EPD_TRY(cmd1(0xB0, 0x13));
    EPD_TRY(cmd1(0xBD, 0x07));
    EPD_TRY(cmd1(0xBE, 0xFE));
    EPD_TRY(cmd1(0xE9, 0x01));
    EPD_TRY(ref_jd796xx_full_window());
    EPD_TRY(epd_cmd(0x04));
    EPD_TRY(epd_wait_ready("power-on", false));
    ESP_LOGI(TAG, "%s init OK (JD79668 BWRY 2bpp)", d->name);

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

/* ====================================================================
 * JD79686A — 5.83" 648x480 BWR
 * ==================================================================== */

static esp_err_t jd79686a_583_bwr_init(void) {
    esp_err_t epd_err = ESP_OK;

    ESP_LOGI(TAG, "JD79686A Init Start");

    // 1. 硬复位
    EPD_TRY(epd_hw_reset());
    delay_ms(100);
    ESP_LOGI(TAG, "JD79668 4.2: BUSY gpio after reset = %d (idle expect %d)",
             (int)gpio_get_level(PIN_EPD_BUSY), busy_idle_effective());
    epd_wait_ready("init", false);
    ESP_LOGI(TAG, "Reset Done. BUSY: %d", (int)gpio_get_level(PIN_EPD_BUSY));

    // 2. 跳过 TDY 命令
    EPD_TRY(epd_cmd(0x08)); EPD_TRY(epd_dat(0x00));
    EPD_TRY(epd_cmd(0xF8)); EPD_TRY(epd_dat(0x60)); EPD_TRY(epd_dat(0xA5));
    EPD_TRY(epd_cmd(0xF8)); EPD_TRY(epd_dat(0x93)); EPD_TRY(epd_dat(0x18));
    EPD_TRY(epd_cmd(0xF8)); EPD_TRY(epd_dat(0x73)); EPD_TRY(epd_dat(0x05));
    EPD_TRY(epd_cmd(0xF8)); EPD_TRY(epd_dat(0x92)); EPD_TRY(epd_dat(0x08));
    EPD_TRY(epd_cmd(0xF8)); EPD_TRY(epd_dat(0xA8)); EPD_TRY(epd_dat(0x3A));
    EPD_TRY(epd_cmd(0xF8)); EPD_TRY(epd_dat(0x88)); EPD_TRY(epd_dat(0x06));

    // 3. 用户命令
    EPD_TRY(epd_cmd(0x00)); EPD_TRY(epd_dat(0x8F)); // Panel Setting: BWR Mode 0xAF for load lut

    ESP_LOGI(TAG, "User Cmds Done");

    // 4. 设置窗口
    EPD_TRY(ref_uc81xx_full_window()); // 复用 UC8179 的窗口设置函数，因为寄存器地址一样

    ESP_LOGI(TAG, "Init Complete");
	
    return ESP_OK;
	
epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t jd79686a_583_bwr_refresh(void)
{
    esp_err_t epd_err = ESP_OK;

    /* ---- 1. Power On ---- */
    EPD_TRY(epd_cmd(0x04));
    delay_ms(100);
    EPD_TRY(epd_wait_ready("power-on", false));

    /* ---- 2. Display Refresh ---- */
    EPD_TRY(epd_cmd(0x12));
    delay_ms(100);
    EPD_TRY(epd_wait_ready("refresh", true));

    /* ---- 3. Power Off ---- */
    EPD_TRY(epd_cmd(0x02));
    delay_ms(100);
    EPD_TRY(epd_wait_ready("poweroff", false));

    /* ---- 4. Deep Sleep 恢复序列 */
    //EPD_TRY(epd_cmd(0x04));        /* Power On */
    EPD_TRY(epd_cmd(0x07));        /* Deep Sleep */
    EPD_TRY(epd_dat(0xA5));        /* DSLP check code (必须为0xA5) */
    EPD_TRY(epd_wait_ready("sleep", false));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

/* ── JD79686A 局部刷新（0x14/0x15 局刷数据传输 → 0x16 PDRF，寄存器局刷 LUT） ──
 * 依据 JD79686AB_User Guide V1.2：
 *   §3.1.11 R14H(PDTM1) / §3.1.12 R15H(PDTM2) = 局刷数据传输（窗口+数据），
 *   §3.1.13 R16H(PDRF) = 局刷刷新（DFV_EN + 窗口，只刷窗口内像素），
 *   §3.1.14~3.1.18 R20~R24 = LUT 寄存器，2.5.2 = BWR 波形参考表。
 * 真机验证过的约束（缺一不可，全部来自实机日志）：
 *   1) 0x14/0x15 数据按整帧（分辨率字节数）计数才收得满，窗口打包会卡收数态；
 *   2) 0x14/0x15 必须是最后一次 SRAM 写入——之后再写 0x10/0x13 会解除武装；
 *   3) 窗口固定取 (0,0, 全宽, 脏区下沿)：数据落点从窗口原点回绕，偏移为0时
 *      所有落点解释等价，整帧数据流必然正确（否则花屏）；
 *   4) 每次局刷前必须重新 init——F8 A8 3A (r_partial_all_gate_en) 局刷门控
 *      在 POF/深睡后丢失，不 init 直接发局刷 PDRF 永不启动（phase=0 超时）；
 *   5) X/W 8像素对齐（脏区按字节给出天然满足），DFV_EN=0（BWR 固定）。
 * 波形：OTP 只有全刷波形（18.8s 全屏闪），局刷改用寄存器 LUT
 * （PSR REG_EN=1，刷完恢复 8Fh；失败路径由重新 init 恢复）：
 *   纯目标驱动 = 直驱110帧 → 浮空15帧 ≈125帧@50Hz≈2.5s（各 LUT 总帧数对齐），
 *   实测端到端约2.9s（全刷18.8s，仍快6倍）。
 *   调试记录（正式运行反馈）：
 *   - LUTR（红色目标）必须全浮空零驱动：用 level00 保持会每分钟叠加同向
 *     偏置把红色逐渐漂白；
 *   - 不要加“反向前置”相位：LUTW 反向=先打黑再打回白，打不回就留灰底；
 *   - 驱动时长是首拍残影的决定因素：维护全刷（OTP）定影的深黑很硬，
 *     直驱50帧首拍仍有残影、第二拍才干净（旧图变成自家波形打的松状态）。
 *     统一加长到110帧，首拍与稳态同一波形（每分钟1.2s vs 2.5s 无实际差别，
 *     简单可靠优先）。
 * LUT 字节格式（3.1.14~3.1.18）：每状态6字节 =
 *   电平字节(4×2bit，相位顺序 D7:D6→D1:D0) + 4×帧数 + 整状态重复次数；
 *   电平：00=VCM_DC保持、01=VSH+(黑向)、10=VSL+(白向)、11=浮空；0帧=不动作；
 *   R26H=0F → VCOM/Red/B2W 各10状态 → 每表下发60字节。
 * PDRF 不启动时回退全刷；每 JD79686A_PARTIAL_REFRESH_LIMIT 次局刷做维护全刷。
 * ------------------------------------------------------------------ */

/* 局刷 LUT（纯目标驱动+浮空，见上）。LUTR=全浮空：红色像素零驱动防渐淡。
 * 总帧数对齐125：LUTW/LUTB=110驱动+15浮空，LUTC/LUTR=125。 */
static const uint8_t s_jd79686a_lut_lutc[60] = { 0x00, 125, 0, 0, 0, 1 };
static const uint8_t s_jd79686a_lut_lutr[60] = { 0xFF, 125, 0, 0, 0, 1 };
static const uint8_t s_jd79686a_lut_lutw[60] = { 0xBF, 110, 15, 0, 0, 1 };
static const uint8_t s_jd79686a_lut_lutb[60] = { 0x7F, 110, 15, 0, 0, 1 };

/** 窗口参数：X/Y/W/L 各 10bit，分两个参数字节下发（低 3 位补 0 = 8 像素对齐） */
static esp_err_t jd79686a_window_params(const epd_dirty_rect_t *rect, bool dfv_en)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_dat((uint8_t)((dfv_en ? 0x80 : 0x00) | ((rect->x >> 8) & 0x03))));
    EPD_TRY(epd_dat((uint8_t)(rect->x & 0xF8)));
    EPD_TRY(epd_dat((uint8_t)((rect->y >> 8) & 0x03)));
    EPD_TRY(epd_dat((uint8_t)(rect->y & 0xFF)));
    EPD_TRY(epd_dat((uint8_t)((rect->w >> 8) & 0x03)));
    EPD_TRY(epd_dat((uint8_t)(rect->w & 0xF8)));
    EPD_TRY(epd_dat((uint8_t)((rect->h >> 8) & 0x03)));
    EPD_TRY(epd_dat((uint8_t)(rect->h & 0xFF)));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

/**
 * 一次局部刷新：0x14/0x15 带窗口 + 整帧正确数据（武装与数据一步完成，
 * 且是最后一次 SRAM 写入）→ Power ON → PDRF(0x16)+同一窗口 → Power OFF/DSLP。
 * 窗口必须是 (0,0,全宽,…)（见上方落点说明）。
 * 返回 ESP_ERR_TIMEOUT 且 s_last_wait_saw_busy_phase==false 表示
 * PDRF 从未启动（BUSY 没有拉低）。
 */
static esp_err_t jd79686a_partial_attempt(const uint8_t *bw, const uint8_t *red,
                                          const epd_dirty_rect_t *win)
{
    esp_err_t epd_err = ESP_OK;
    const size_t plane = (size_t)epd_plane_bytes();

    /* 0. 重新初始化（hw reset + 初始代码）。真机证据：PDRF 唯一一次启动发生
     *    在“刚 init 过”的控制器上（init 含 F8 A8 3A r_partial_all_gate_en 局刷
     *    门控）；全刷的 POF/深睡之后该状态丢失，直接发局刷四次全部 phase=0 超时。 */
    EPD_TRY(panel_run_full_init());

    /* 0.5 切到寄存器局刷 LUT（PSR REG_EN=1）：纯目标驱动
     *     直驱110帧 → 浮空15帧（≈125帧@50Hz≈2.5s，各 LUT 总帧数对齐），
     *     LUTR 红色 LUT 全浮空零驱动；端到端约2.9s（OTP 全刷18.8s）。 */
    EPD_TRY(epd_cmd(0x00));
    EPD_TRY(epd_dat(0xAF)); /* BWR + use register LUT */
    EPD_TRY(epd_cmd(0x20));
    EPD_TRY(epd_bulk(s_jd79686a_lut_lutc, sizeof(s_jd79686a_lut_lutc)));
    EPD_TRY(epd_cmd(0x22));
    EPD_TRY(epd_bulk(s_jd79686a_lut_lutr, sizeof(s_jd79686a_lut_lutr)));
    EPD_TRY(epd_cmd(0x23));
    EPD_TRY(epd_bulk(s_jd79686a_lut_lutw, sizeof(s_jd79686a_lut_lutw)));
    EPD_TRY(epd_cmd(0x24));
    EPD_TRY(epd_bulk(s_jd79686a_lut_lutb, sizeof(s_jd79686a_lut_lutb)));

    /* 1. PDTM1(0x14)：窗口 + 整帧 B/W 数据 */
    EPD_TRY(epd_cmd(0x14));
    EPD_TRY(jd79686a_window_params(win, false));
    if (bw)
        EPD_TRY(epd_bulk(bw, plane));
    else
        EPD_TRY(epd_fill(0xFF, plane));

    /* 2. PDTM2(0x15)：窗口 + 整帧 RED 数据 */
    EPD_TRY(epd_cmd(0x15));
    EPD_TRY(jd79686a_window_params(win, false));
    if (red)
        EPD_TRY(epd_bulk(red, plane));
    else
        EPD_TRY(epd_fill(0x00, plane));

    /* 3. Power ON（与全刷相同：数据先于上电） */
    EPD_TRY(epd_cmd(0x04));
    delay_ms(100);
    EPD_TRY(epd_wait_ready("power-on", false));

    /* 4. PDRF(0x16)：DFV_EN=0 + 同一窗口。命令后立即等 BUSY——局刷可能只有
     *    几十毫秒，固定延时会整个错过 BUSY 脉冲而误判失败。 */
    EPD_TRY(epd_cmd(0x16));
    EPD_TRY(jd79686a_window_params(win, false));
    EPD_TRY(epd_wait_ready("refresh", true));

    /* 恢复 OTP 波形（PSR REG_EN=0）：下一次全刷必须用 OTP。
     * 失败路径无需在此恢复——epd_failure 会清 s_panel_seq_done，
     * 下一次显示重新 init 时会把 PSR 写回 8Fh。 */
    EPD_TRY(epd_cmd(0x00));
    EPD_TRY(epd_dat(0x8F));

    /* 5. Power OFF + Deep Sleep（与全刷收尾一致） */
    EPD_TRY(epd_cmd(0x02));
    delay_ms(100);
    EPD_TRY(epd_wait_ready("poweroff", false));
    EPD_TRY(epd_cmd(0x07));
    EPD_TRY(epd_dat(0xA5));
    EPD_TRY(epd_wait_ready("sleep", false));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

/**
 * 局部刷新脏区（调用方已确认红平面基线未变，窗口外像素不受影响）。
 * PDRF 未启动时回退全刷，保证画面正确。
 */
static esp_err_t jd79686a_display_partial(const uint8_t *bw, const uint8_t *red,
                                          bool *did_refresh)
{
    esp_err_t epd_err = ESP_OK;
    epd_dirty_rect_t dirty;
    *did_refresh = false;

    if (!epd_find_dirty_rect(s_previous_bw, bw, &dirty)) {
        ESP_LOGI(TAG, "JD79686A partial skipped: dirty=none count=%u/%u",
                 s_jd79686a_partial_refresh_count, JD79686A_PARTIAL_REFRESH_LIMIT);
        return ESP_OK;
    }

    /* 实际局刷窗口：从屏幕顶部开始的全宽条带，下沿盖住脏区。
     * 线性偏移为0 —— 无论控制器把0x14/0x15 的数据落点解释为整帧、
     * 还是从窗口原点回绕，整帧数据流都会得到正确的 SRAM 内容（不花屏）。 */
    epd_dirty_rect_t win = { .x = 0, .y = 0, .w = epd_width(),
                             .h = dirty.y + dirty.h };

    ESP_LOGI(TAG,
             "JD79686A partial: dirty=%d,%d %dx%d window=%d,%d %dx%d count=%u/%u",
             dirty.x, dirty.y, dirty.w, dirty.h,
             win.x, win.y, win.w, win.h,
             s_jd79686a_partial_refresh_count + 1, JD79686A_PARTIAL_REFRESH_LIMIT);
    int64_t start_us = esp_timer_get_time();

    esp_err_t err = jd79686a_partial_attempt(bw, red, &win);
    if (err == ESP_ERR_TIMEOUT && !s_last_wait_saw_busy_phase) {
        ESP_LOGW(TAG, "JD79686A partial PDRF did not start; falling back to full refresh");
        EPD_TRY(panel_write_bw_red(bw, red, epd_plane_bytes()));
        *did_refresh = true;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        epd_err = err;
        goto epd_fail;
    }

    ESP_LOGI(TAG,
             "JD79686A partial done: dirty=%d,%d %dx%d window=%dx%d elapsed=%lld ms",
             dirty.x, dirty.y, dirty.w, dirty.h, win.w, win.h,
             (long long)((esp_timer_get_time() - start_us) / 1000LL));

    if (previous_bw_store(bw, epd_plane_bytes())) {
        jd79686a_prev_red_store(red, 0x00, epd_plane_bytes());
        s_jd79686a_partial_refresh_count++;
    } else {
        previous_bw_invalidate();
    }
    *did_refresh = true;
    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

/**
 * 网页触发的 JD79686A 局刷试验：以当前屏内容（上一帧基线）为底，把画面中央
 * 一块像素按字节取反后走一次局刷路径——绕开时钟任务的 force_full/模式门控，
 * 随时可验证 PDRF 是否启动、闪烁范围与残影。
 * 前置：已有全刷基线（开机全刷或任意一次成功刷新之后即可用）。
 * 红平面按"窗口内清红"写入（没有存储上一帧红平面）；试验窗口选在中央数字区，
 * 正常时钟画面该区域无红色内容，若有红色也会在下一次刷新按基线自愈。
 */
esp_err_t epd_jd79686a_partial_test(epd_refresh_test_result_t *result)
{
    uint8_t *bw = NULL;
    bool did_refresh = false;

    if (!s_epd_mutex || !s_epd_ready)
        return ESP_ERR_INVALID_STATE;
    if (!panel_is_jd79686a_583())
        return ESP_ERR_NOT_SUPPORTED;

    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);

    if (!previous_bw_matches_current_panel() || !s_jd79686a_red_signature_valid) {
        xSemaphoreGive(s_epd_mutex);
        ESP_LOGW(TAG, "JD79686A partial test: no full-refresh baseline yet");
        return ESP_ERR_INVALID_STATE;
    }

    const int box_w = 104;
    const int box_h = 64;
    const int box_x = ((epd_width() / 2 - box_w / 2) / 8) * 8; /* 对齐到字节 */
    const int box_y = epd_height() / 2 - box_h / 2;
    if (result) {
        /* 实际局刷窗口是从顶部开始的全宽条带（见 jd79686a_display_partial） */
        *result = (epd_refresh_test_result_t){
            .baseline_ms = 0,
            .candidate_ms = 0,
            .baseline_busy_ok = true,
            .candidate_busy_ok = false,
            .x = 0, .y = 0, .w = epd_width(), .h = box_y + box_h,
        };
    }

    bw = heap_caps_malloc((size_t)epd_plane_bytes(),
                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bw)
        bw = heap_caps_malloc((size_t)epd_plane_bytes(), MALLOC_CAP_8BIT);
    if (!bw) {
        xSemaphoreGive(s_epd_mutex);
        return ESP_ERR_NO_MEM;
    }
    memcpy(bw, s_previous_bw, (size_t)epd_plane_bytes());
    /* 用上一帧红平面组包：传 NULL 会把 SRAM 红平面清零，窗口内红色会消失
     *（实验C的现象就是这么来的；真实刷屏路径传 fb->red 不受影响）。 */
    const uint8_t *red = (s_jd79686a_prev_red &&
                          s_jd79686a_prev_red_bytes == epd_plane_bytes())
                             ? s_jd79686a_prev_red
                             : NULL;

    const int rb = epd_width() / 8;
    ESP_LOGI(TAG, "JD79686A partial test: invert %d,%d %dx%d on current baseline",
             box_x, box_y, box_w, box_h);
    int64_t start_us = esp_timer_get_time();
    for (int y = box_y; y < box_y + box_h; y++) {
        uint8_t *row = bw + (size_t)y * (size_t)rb + (size_t)(box_x / 8);
        for (int i = 0; i < box_w / 8; i++)
            row[i] = (uint8_t)~row[i];
    }

    esp_err_t err = jd79686a_display_partial(bw, red, &did_refresh);
    if (result)
        result->candidate_ms =
            (uint32_t)((esp_timer_get_time() - start_us) / 1000);
    free(bw);
    bw = NULL;
    xSemaphoreGive(s_epd_mutex);

    if (result)
        result->candidate_busy_ok = did_refresh && err == ESP_OK;
    if (err != ESP_OK)
        return err;
    if (!did_refresh) {
        ESP_LOGW(TAG, "JD79686A partial test: dirty region was skipped");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "JD79686A partial test done: window=%d,%d %dx%d elapsed=%lu ms",
             box_x, box_y, box_w, box_h,
             (unsigned long)(result ? result->candidate_ms : 0));
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════
 * SSD1619 — 4.2" 400×300
 * ═══════════════════════════════════════════════════════════════════ */

static esp_err_t ssd1619_cursor_home(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_cmd(0x4E));
    EPD_TRY(epd_dat(0x00));
    EPD_TRY(epd_cmd(0x4F));
    EPD_TRY(epd_dat(0x00));
    EPD_TRY(epd_dat(0x00));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ssd1619_init_sequence(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_hw_reset());
    ESP_LOGI(TAG, "SSD1619: BUSY gpio after reset = %d (idle expect %d)",
             (int)gpio_get_level(PIN_EPD_BUSY), busy_idle_effective());
    if (busy_idle_effective() == 1 && gpio_get_level(PIN_EPD_BUSY) == 0)
        ESP_LOGW(TAG, "BUSY low while idle=1 — set NVS epd/busy_idle=0 or use default for 4.2\".");
    EPD_TRY(epd_wait_ready("init", false));

    EPD_TRY(epd_cmd(0x12));
    EPD_TRY(epd_wait_ready("init", false));

    EPD_TRY(epd_cmd(0x74));
    EPD_TRY(epd_dat(0x54));
    EPD_TRY(epd_cmd(0x7E));
    EPD_TRY(epd_dat(0x3B));

    EPD_TRY(epd_cmd(0x01));
    EPD_TRY(epd_dat(0x2B));
    EPD_TRY(epd_dat(0x01));
    EPD_TRY(epd_dat(0x00));

    EPD_TRY(epd_cmd(0x11));
    EPD_TRY(epd_dat(0x03));

    EPD_TRY(epd_cmd(0x44));
    EPD_TRY(epd_dat(0x00));
    EPD_TRY(epd_dat(0x31));

    EPD_TRY(epd_cmd(0x45));
    EPD_TRY(epd_dat(0x00));
    EPD_TRY(epd_dat(0x00));
    EPD_TRY(epd_dat(0x2B));
    EPD_TRY(epd_dat(0x01));

    /* BorderWaveform：Waveshare EPD_4in2b_V2_Init_new 用 0x05，减轻屏体边缘红框；0x03 易偏红 */
    EPD_TRY(epd_cmd(0x3C));
    EPD_TRY(epd_dat(0x05));
    EPD_TRY(epd_cmd(0x2C));
    EPD_TRY(epd_dat(0x55));

    EPD_TRY(epd_cmd(0x03));
    EPD_TRY(epd_dat(0x15));
    EPD_TRY(epd_cmd(0x04));
    EPD_TRY(epd_dat(0x41));
    EPD_TRY(epd_dat(0xA8));
    EPD_TRY(epd_dat(0x32));

    EPD_TRY(epd_cmd(0x3A));
    EPD_TRY(epd_dat(0x2C));
    EPD_TRY(epd_cmd(0x3B));
    EPD_TRY(epd_dat(0x0B));

    EPD_TRY(ssd1619_cursor_home());

    ESP_LOGI(TAG, "SSD1619 init OK");

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ssd1619_refresh(void)
{
    esp_err_t epd_err = ESP_OK;
    /* 每帧刷新前再写边框，避免部分序列下边缘仍按红边驱动 */
    EPD_TRY(epd_cmd(0x3C));
    EPD_TRY(epd_dat(0x05));

    /* Waveshare DisplayFrame(new): 0x22 0xF7 再 0x20；与 epd4in2b_V2 一致 */
    EPD_TRY(epd_cmd(0x22));
    EPD_TRY(epd_dat(0xF7));
    delay_ms(2);
    EPD_TRY(epd_cmd(0x20));

    TickType_t t0 = xTaskGetTickCount();
    EPD_TRY(epd_wait_ready("refresh", true));
    bool busy_ok = true;
    uint32_t elapsed_ms = pdTICKS_TO_MS(xTaskGetTickCount() - t0);

    /* BUSY 未接或恒为高时：wait_ready 几毫秒内就“就绪”，全刷实际未完成，屏不更新 */
    if (!busy_ok) {
        ESP_LOGW(TAG, "SSD1619: refresh BUSY invalid after %u ms, used fallback wait",
                 (unsigned)elapsed_ms);
    } else if (elapsed_ms < 250) {
        ESP_LOGW(TAG, "SSD1619: refresh BUSY only %u ms — likely BUSY floating/wrong. "
                      "Using fixed %d ms full-refresh wait (check GPIO%d BUSY).",
                 (unsigned)elapsed_ms, EPD_BUSY_FALLBACK_REFRESH_MS, PIN_EPD_BUSY);
        delay_ms(EPD_BUSY_FALLBACK_REFRESH_MS);
    } else {
        delay_ms(500);
    }

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

/* ═══════════════════════════════════════════════════════════════════
 * UC8179 — 5.83" 648×480 BWR（参考 Waveshare 5.83 BWR）
 * ═══════════════════════════════════════════════════════════════════ */

static esp_err_t ref_ssd1667_102_refresh(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_cmd(0x3C));
    EPD_TRY(epd_dat(0x05));

    EPD_TRY(epd_cmd(0x22));
    EPD_TRY(epd_dat(0xF7));
    delay_ms(2);
    EPD_TRY(epd_cmd(0x20));

    TickType_t t0 = xTaskGetTickCount();
    EPD_TRY(epd_wait_ready("refresh", true));
    bool busy_ok = true;
    uint32_t elapsed_ms = pdTICKS_TO_MS(xTaskGetTickCount() - t0);

    if (!busy_ok) {
        ESP_LOGW(TAG, "SSD1667 10.2: refresh BUSY invalid after %u ms, used fallback wait",
                 (unsigned)elapsed_ms);
    } else if (elapsed_ms < 250) {
        ESP_LOGW(TAG, "SSD1667 10.2: refresh BUSY only %u ms, using fixed %d ms wait",
                 (unsigned)elapsed_ms, EPD_BUSY_FALLBACK_REFRESH_MS);
        delay_ms(EPD_BUSY_FALLBACK_REFRESH_MS);
    } else {
        delay_ms(500);
    }

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_ssd2677_97_refresh(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_cmd(0x12));
    EPD_TRY(epd_dat(0x00));

    TickType_t t0 = xTaskGetTickCount();
    EPD_TRY(epd_wait_ready("refresh", true));
    bool busy_ok = true;
    uint32_t elapsed_ms = pdTICKS_TO_MS(xTaskGetTickCount() - t0);

    if (!busy_ok) {
        ESP_LOGW(TAG, "SSD2677 9.7: refresh BUSY invalid after %u ms, used fallback wait",
                 (unsigned)elapsed_ms);
    } else if (elapsed_ms < 250) {
        ESP_LOGW(TAG, "SSD2677 9.7: refresh BUSY only %u ms, using fixed %d ms wait",
                 (unsigned)elapsed_ms, EPD_BUSY_FALLBACK_REFRESH_MS);
        delay_ms(EPD_BUSY_FALLBACK_REFRESH_MS);
    } else {
        delay_ms(500);
    }
    EPD_TRY(ref_ssd2677_97_sleep());

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ref_ssd2677_97_sleep(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_cmd(0x02));
    EPD_TRY(epd_dat(0x00));
    EPD_TRY(epd_wait_ready("poweroff", false));
    EPD_TRY(cmd1(0x07, 0xA5));
    s_panel_seq_done = false;

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t e6_37_refresh(void)
{

    /* E6 3.7 performs two complete data+refresh passes inside the writer. */

    return ESP_OK;
}

static esp_err_t uc8253_refresh(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_cmd(0x12));
    delay_ms(1);
    EPD_TRY(epd_wait_ready("refresh", true));

    EPD_TRY(epd_cmd(0x02));
    EPD_TRY(epd_wait_ready("poweroff", true));
    EPD_TRY(cmd1(0x07, 0xA5));
    s_panel_seq_done = false;

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t uc8179_init_sequence(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_hw_reset());
    EPD_TRY(epd_wait_ready("init", false));

    EPD_TRY(epd_cmd(0x01));
    EPD_TRY(epd_dat(0x07));
    EPD_TRY(epd_dat(0x07));
    EPD_TRY(epd_dat(0x3F));
    EPD_TRY(epd_dat(0x3F));

    EPD_TRY(epd_cmd(0x04));
    delay_ms(100);
    EPD_TRY(epd_wait_ready("power-on", false));

    EPD_TRY(epd_cmd(0x00));
    EPD_TRY(epd_dat(0x0F));

    EPD_TRY(epd_cmd(0x61));
    EPD_TRY(epd_dat(0x02));
    EPD_TRY(epd_dat(0x88));
    EPD_TRY(epd_dat(0x01));
    EPD_TRY(epd_dat(0xE0));

    EPD_TRY(epd_cmd(0x15));
    EPD_TRY(epd_dat(0x00));
    EPD_TRY(epd_cmd(0x50));
    EPD_TRY(epd_dat(0x11));
    EPD_TRY(epd_dat(0x07));
    EPD_TRY(epd_cmd(0x60));
    EPD_TRY(epd_dat(0x22));

    ESP_LOGI(TAG, "UC8179 init OK (BWR)");

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t uc8179_refresh(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_cmd(0x12));
    delay_ms(200);
    EPD_TRY(epd_wait_ready("refresh", true));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ws_583_bwr_init(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(epd_hw_reset());
    EPD_TRY(cmd4(0x01, 0x07, 0x07, 0x3F, 0x3F));
    EPD_TRY(epd_cmd(0x04));
    delay_ms(100);
    EPD_TRY(epd_wait_ready("power-on", false));
    EPD_TRY(cmd1(0x00, 0x0F));
    EPD_TRY(cmd4(0x61, 0x02, 0x88, 0x01, 0xE0));
    EPD_TRY(cmd1(0x15, 0x00));
    EPD_TRY(cmd2(0x50, 0x11, 0x07));
    EPD_TRY(cmd1(0x60, 0x22));
    ESP_LOGI(TAG, "Waveshare 5.83 BWR B V2 init OK");

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}



/* ── 完整初始化（每面板一次；换面板或 NVS 覆盖后需重跑） ───────────── */

static esp_err_t panel_run_full_init(void)
{
    esp_err_t epd_err = ESP_OK;
    EPD_TRY(panel_desc()->init());
    s_uc8176_register_lut_active = false;
    s_uc8179_register_lut_active = false;
    s_panel_seq_done = true;

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static bool consume_force_full_refresh_request(void)
{
    return atomic_exchange(&s_force_full_refresh_next, false);
}

static void prepare_for_forced_full_refresh(void)
{
    previous_bw_invalidate();
    s_panel_seq_done = false;
}

void epd_request_full_refresh_next(void)
{
    atomic_store(&s_force_full_refresh_next, true);
}

static esp_err_t panel_ensure_ready(void)
{
    esp_err_t epd_err = ESP_OK;
    if (!s_panel_seq_done)
        EPD_TRY(panel_run_full_init());

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

/** 调用方已持有 s_epd_mutex */
static esp_err_t epd_clear_to_white_locked(void)
{
    esp_err_t epd_err = ESP_OK;
    int pb = epd_plane_bytes();
    EPD_TRY(panel_ensure_ready());
    EPD_TRY(panel_fill_bw_red(0xFF, 0x00, pb));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
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
    esp_err_t epd_err = ESP_OK;
    bool epd_locked = false;

    if (!s_epd_mutex) {
        s_epd_mutex = xSemaphoreCreateMutex();
        if (!s_epd_mutex)
            return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);
    epd_locked = true;
    s_epd_ready = false;
    s_panel_seq_done = false;
    previous_bw_release();
    s_uc8176_register_lut_active = false;
    s_uc8179_register_lut_active = false;

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_EPD_DC) | (1ULL << PIN_EPD_CS) | (1ULL << PIN_EPD_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    EPD_TRY(gpio_config(&out));

    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_EPD_BUSY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1,
    };
    EPD_TRY(gpio_config(&in));

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_EPD_SCK,
        .mosi_io_num = PIN_EPD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_SPI_MAX_TX,
    };
    if (!s_spi_bus_initialized) {
        EPD_TRY(spi_bus_initialize(EPD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
        s_spi_bus_initialized = true;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = EPD_SPI_HZ,
        .mode = 0,
        .spics_io_num = PIN_EPD_CS,
        .queue_size = 1,
    };
    if (!s_spi) {
        EPD_TRY(spi_bus_add_device(EPD_SPI_HOST, &devcfg, &s_spi));
    }

    nvs_load_panel();
    if (panel_is_jd79668_42() && busy_idle_effective() == 0) {
        ESP_LOGW(TAG, "JD79668 4.2 datasheet uses BUSY_N idle-high; migrating busy_idle -> 1");
        (void)epd_set_busy_idle(1);
    }
    if (panel_is_ssd2677_97() && busy_idle_effective() == 0) {
        ESP_LOGW(TAG, "SSD2677 9.7 reference flow uses BUSY idle-high; migrating busy_idle -> 1");
        (void)epd_set_busy_idle(1);
    }
    if (panel_is_uc8253_37() && busy_idle_effective() == 0) {
        ESP_LOGW(TAG, "GDEY037T03 UC8253 uses BUSY idle-high; migrating busy_idle -> 1");
        (void)epd_set_busy_idle(1);
    }
    if (panel_is_uc8179_75_bwr() && busy_idle_effective() == 0) {
        ESP_LOGW(TAG, "GDEY075Z08 UC8179 uses BUSY idle-high; migrating busy_idle -> 1");
        (void)epd_set_busy_idle(1);
    }
    if (panel_is_gd7965_uc8179_75_bw() && busy_idle_effective() == 0) {
        ESP_LOGW(TAG, "GD7965/UC8179 7.5 BW candidate uses BUSY idle-high; migrating busy_idle -> 1");
        (void)epd_set_busy_idle(1);
    }
    if (panel_is_ws426_bw() && busy_idle_effective() != 0) {
        ESP_LOGW(TAG, "P426010 4.26 BW uses BUSY idle-low; restoring busy_idle -> 0");
        (void)epd_set_busy_idle(0);
    }
    if (panel_is_ssd16xx_213_bw() && busy_idle_effective() != 0) {
        ESP_LOGW(TAG, "SSD16xx 2.13 128x250 uses BUSY idle-low; restoring busy_idle -> 0");
        (void)epd_set_busy_idle(0);
    }
    if (s_panel == EPD_PANEL_REF_UC8159_75_LOW_BW && busy_idle_effective() != 0) {
        ESP_LOGW(TAG, "UC8159 7.5 low-resolution BW hardware uses BUSY idle-low; restoring busy_idle -> 0");
        (void)epd_set_busy_idle(0);
    }

    ESP_LOGI(TAG, "EPD start: %s %dx%d, BUSY idle_level=%d (4.2\" default 0=Waveshare; NVS busy_idle=1 if GDE idle-high)",
              s_specs[s_panel].name, epd_width(), epd_height(), busy_idle_effective());

    EPD_TRY(panel_run_full_init());
    if (panel_desc()->data_mode == EPD_CMD_DATA_E6_3IN7_6C) {
        ESP_LOGI(TAG, "E6 3.7: skip startup white clear; first full content refresh overwrites panel");
    } else if (panel_is_jd796xx()) {
        ESP_LOGI(TAG, "JD796xx: skip startup white clear; first full content refresh overwrites panel");
    } else if (panel_is_ssd1667_102()) {
        ESP_LOGI(TAG, "SSD1667 10.2: skip startup white clear; first full content refresh overwrites panel");
    } else if (panel_is_ssd2677_97()) {
        ESP_LOGI(TAG, "SSD2677 9.7: skip startup white clear; first full content refresh overwrites panel");
    } else if (panel_is_uc8253_37()) {
        ESP_LOGI(TAG, "GDEY037T03 UC8253: skip startup white clear; first full content refresh overwrites panel");
    } else if (panel_is_uc8179_75_bwr()) {
        ESP_LOGI(TAG, "GDEY075Z08 UC8179: skip startup white clear; first full content refresh overwrites panel");
    } else if (panel_is_gd7965_uc8179_75_bw()) {
        ESP_LOGI(TAG, "GD7965/UC8179 7.5 BW candidate: skip startup white clear; welcome/IP page is the first full refresh");
    } else if (panel_is_visionpeak_ei0583_bw()) {
        ESP_LOGI(TAG, "Vision Peak EI0583T5PCZ: skip startup white clear; welcome/IP page is the first full refresh");
    } else if (panel_is_ws426_bw()) {
        ESP_LOGI(TAG, "P426010 4.26 BW: skip startup white clear; welcome/IP page is the first full refresh");
    } else if (panel_is_ssd16xx_213_bw()) {
        ESP_LOGI(TAG, "SSD16xx 2.13 128x250: skip startup white clear; welcome/IP page is the first full refresh");
    } else if (panel_is_42_400x300()) {
        ESP_LOGI(TAG, "4.2 400x300: skip startup white clear; welcome/IP page is the first full refresh");
    } else {
        EPD_TRY(epd_clear_to_white_locked());
    }

    s_epd_ready = true;
    xSemaphoreGive(s_epd_mutex);
    epd_locked = false;

    return ESP_OK;

epd_fail:
    ESP_LOGE(TAG, "EPD init failed: %s", esp_err_to_name(epd_err));
    epd_failure(epd_err);
    if (epd_locked) xSemaphoreGive(s_epd_mutex);
    return epd_err;
}

static esp_err_t epd_clear_screen_internal(const unsigned *epoch)
{
    esp_err_t epd_err = ESP_OK;
    bool epd_locked = false;

    if (!s_epd_mutex || !s_epd_ready)
        return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);
    epd_locked = true;
    if (epoch && !display_policy_epoch_is_current(*epoch)) {
        xSemaphoreGive(s_epd_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    bool force_full = consume_force_full_refresh_request();
    if (force_full || s_uc8176_register_lut_active ||
        s_uc8179_register_lut_active)
        prepare_for_forced_full_refresh();
    if (s_panel == EPD_PANEL_REF_UC8176_42_BW)
        previous_bw_invalidate();
    EPD_TRY(epd_clear_to_white_locked());
    xSemaphoreGive(s_epd_mutex);
    epd_locked = false;
    return ESP_OK;

epd_fail:
    epd_failure(epd_err);
    if (epd_locked) xSemaphoreGive(s_epd_mutex);
    return epd_err;
}

esp_err_t epd_clear_screen(void)
{
    return epd_clear_screen_internal(NULL);
}

esp_err_t epd_clear_screen_epoch(unsigned epoch)
{
    return epd_clear_screen_internal(&epoch);
}

static inline void test_set_black(uint8_t *bw, uint8_t *red, int rb, int x, int y)
{
    if (x < 0 || y < 0 || x >= epd_width() || y >= epd_height())
        return;
    size_t off = (size_t)y * (size_t)rb + (size_t)(x >> 3);
    uint8_t bit = (uint8_t)(0x80u >> (x & 7));
    bw[off] &= (uint8_t)~bit;
    if (red)
        red[off] &= (uint8_t)~bit;
}

static inline void test_set_red(uint8_t *bw, uint8_t *red, int rb, int x, int y)
{
    if (!red || x < 0 || y < 0 || x >= epd_width() || y >= epd_height())
        return;
    size_t off = (size_t)y * (size_t)rb + (size_t)(x >> 3);
    uint8_t bit = (uint8_t)(0x80u >> (x & 7));
    bw[off] |= bit;
    red[off] |= bit;
}

static void test_fill_rect(uint8_t *bw, uint8_t *red, int rb, int x, int y,
                           int w, int h, bool color_red)
{
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            if (color_red)
                test_set_red(bw, red, rb, xx, yy);
            else
                test_set_black(bw, red, rb, xx, yy);
        }
    }
}

static void test_rect(uint8_t *bw, uint8_t *red, int rb, int x, int y,
                      int w, int h, int t, bool color_red)
{
    test_fill_rect(bw, red, rb, x, y, w, t, color_red);
    test_fill_rect(bw, red, rb, x, y + h - t, w, t, color_red);
    test_fill_rect(bw, red, rb, x, y, t, h, color_red);
    test_fill_rect(bw, red, rb, x + w - t, y, t, h, color_red);
}

static void test_checker(uint8_t *bw, uint8_t *red, int rb, int x, int y,
                         int w, int h, int cell, bool color_red)
{
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            if ((((xx / cell) + (yy / cell)) & 1) == 0) {
                if (color_red)
                    test_set_red(bw, red, rb, x + xx, y + yy);
                else
                    test_set_black(bw, red, rb, x + xx, y + yy);
            }
        }
    }
}

static void draw_jd79668_test_pattern(uint8_t *bw, uint8_t *red,
                                      uint8_t *yellow, int pb, int rb,
                                      int W, int H)
{
    fb_t fb = {
        .black = bw,
        .red = red,
        .yellow = yellow,
        .width = W,
        .height = H,
        .plane_bytes = pb,
        .row_bytes = rb,
    };

    fb_rect(&fb, 0, 0, W, H, COLOR_BLACK);
    fb_rect(&fb, 4, 4, W - 8, H - 8, COLOR_BLACK);
    ui_draw_fixed_text(&fb, 16, 12, "JD79668  BWRY  TEST",
                       COLOR_BLACK, 1);

    const int swatch_y = 38;
    const int swatch_h = 48;
    const int swatch_w = 82;
    const int gap = 10;
    const int start_x = (W - (swatch_w * 4 + gap * 3)) / 2;
    const fb_color_t colors[4] = {
        COLOR_BLACK, COLOR_WHITE, COLOR_RED, COLOR_YELLOW,
    };
    const char *labels[4] = { "BLACK", "WHITE", "RED", "YELLOW" };
    for (int i = 0; i < 4; i++) {
        int x = start_x + i * (swatch_w + gap);
        fb_fill_rect(&fb, x, swatch_y, swatch_w, swatch_h, colors[i]);
        fb_rect(&fb, x, swatch_y, swatch_w, swatch_h, COLOR_BLACK);
        fb_color_t text_color = (i == 0) ? COLOR_WHITE : COLOR_BLACK;
        int tw = ui_fixed_text_width(&fb, labels[i], 1);
        ui_draw_fixed_text(&fb, x + (swatch_w - tw) / 2,
                           swatch_y + 17, labels[i], text_color, 1);
    }

    ui_draw_fixed_text(&fb, 18, 101, "1/2/3PX LINES", COLOR_BLACK, 1);
    const fb_color_t line_colors[3] = {
        COLOR_BLACK, COLOR_RED, COLOR_YELLOW,
    };
    for (int c = 0; c < 3; c++) {
        int y = 124 + c * 21;
        for (int t = 1; t <= 3; t++)
            fb_fill_rect(&fb, 18, y + (t - 1) * 5, 158, t,
                         line_colors[c]);
    }

    ui_draw_fixed_text(&fb, 214, 101, "OVERWRITE", COLOR_BLACK, 1);
    fb_fill_rect(&fb, 214, 123, 150, 76, COLOR_RED);
    fb_fill_rect(&fb, 244, 139, 120, 60, COLOR_YELLOW);
    fb_fill_rect(&fb, 274, 155, 90, 44, COLOR_BLACK);
    fb_fill_rect(&fb, 304, 171, 60, 28, COLOR_WHITE);
    fb_rect(&fb, 214, 123, 150, 76, COLOR_BLACK);

    ui_draw_fixed_text(&fb, 18, 211, "TEXT", COLOR_BLACK, 1);
    ui_draw_fixed_text(&fb, 76, 211, "BLACK", COLOR_BLACK, 1);
    ui_draw_fixed_text(&fb, 151, 211, "RED", COLOR_RED, 1);
    ui_draw_fixed_text(&fb, 211, 211, "YELLOW", COLOR_YELLOW, 1);

    fb_fill_rect(&fb, 18, 239, 104, 27, COLOR_BLACK);
    fb_fill_rect(&fb, 148, 239, 104, 27, COLOR_RED);
    fb_fill_rect(&fb, 278, 239, 104, 27, COLOR_YELLOW);
    ui_draw_fixed_text(&fb, 18, H - 24,
                       "BLACK / WHITE / RED / YELLOW", COLOR_BLACK, 1);
}

esp_err_t epd_display_test_pattern(void)
{
    esp_err_t epd_err = ESP_OK;
    bool epd_locked = false;
    uint8_t *buf = NULL;
    uint8_t *red = NULL;
    uint8_t *yellow = NULL;

    if (!s_epd_mutex || !s_epd_ready)
        return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);
    epd_locked = true;

    int W = epd_width();
    int H = epd_height();
    int pb = epd_plane_bytes();
    int rb = W / 8;

    buf = heap_caps_malloc((size_t)pb, MALLOC_CAP_DMA);
    if (!buf) {
        xSemaphoreGive(s_epd_mutex);
        epd_locked = false;
        return ESP_ERR_NO_MEM;
    }
    memset(buf, 0xFF, (size_t)pb);

    red = heap_caps_calloc(1, (size_t)pb, MALLOC_CAP_DMA);
    yellow = epd_has_yellow() ? heap_caps_calloc(1, (size_t)pb, MALLOC_CAP_DMA) : NULL;
    if (!red || (epd_has_yellow() && !yellow)) {
        free(red);
        red = NULL;
        free(yellow);
        yellow = NULL;
        free(buf);
        buf = NULL;
        xSemaphoreGive(s_epd_mutex);
        epd_locked = false;
        return ESP_ERR_NO_MEM;
    }

    if (s_panel == EPD_PANEL_REF_JD79668_42_BWRY) {
        draw_jd79668_test_pattern(buf, red, yellow, pb, rb, W, H);
    } else {
    int mid_x = W / 2, mid_y = H / 2;
    test_rect(buf, red, rb, 0, 0, W, H, 1, false);
    test_rect(buf, red, rb, 4, 4, W - 8, H - 8, 2, false);
    if (epd_has_red())
        test_rect(buf, red, rb, 10, 10, W - 20, H - 20, 1, true);

    int corner = W < 300 ? 44 : 58;
    test_checker(buf, red, rb, 14, 14, corner, corner, 2, false);
    test_checker(buf, red, rb, W - 14 - corner, 14, corner, corner, 4, false);
    test_checker(buf, red, rb, 14, H - 14 - corner, corner, corner, 6, false);
    test_checker(buf, red, rb, W - 14 - corner, H - 14 - corner, corner, corner, 8, false);

    for (int i = 0; i < 8; i++) {
        int x = 28 + i * 10;
        test_fill_rect(buf, red, rb, x, mid_y - 46, i + 1, 92, false);
    }
    for (int i = 0; i < 8; i++) {
        int y = mid_y + 8 + i * 8;
        test_fill_rect(buf, red, rb, 24, y, W / 2 - 48, i + 1, false);
    }

    test_rect(buf, red, rb, mid_x - 52, mid_y - 52, 104, 104, 1, false);
    test_rect(buf, red, rb, mid_x - 42, mid_y - 42, 84, 84, 2, false);
    test_fill_rect(buf, red, rb, mid_x - 28, mid_y - 28, 56, 56, false);
    test_checker(buf, red, rb, mid_x - 24, mid_y - 24, 48, 48, 4, false);

    if (epd_has_red()) {
        test_fill_rect(buf, red, rb, W - 96, mid_y - 46, 66, 28, true);
        test_rect(buf, red, rb, W - 96, mid_y - 8, 66, 54, 2, true);
        for (int i = 0; i < 5; i++)
            test_fill_rect(buf, red, rb, W - 92 + i * 12, mid_y + 58, i + 1, 48, true);
    }

    const char *label = "FPC-194 SSD1683 EDGE TEST";
    fb_t label_fb = {
        .black = buf,
        .red = red,
        .width = W,
        .height = H,
        .plane_bytes = pb,
        .row_bytes = rb,
    };
    int label_w = ui_fixed_text_width(&label_fb, label, 1);
    ui_draw_fixed_text(&label_fb, (W - label_w) / 2, H - 30, label,
                       COLOR_BLACK, 1);
    ui_draw_fixed_text(&label_fb, 18, mid_y - 66, "1-8px", COLOR_BLACK, 1);
    if (epd_has_red())
        ui_draw_fixed_text(&label_fb, W - 98, mid_y - 66, "RED",
                           COLOR_RED, 1);

    if (yellow) {
        for (int y = mid_y + 20; y < H - 20; y++) {
            for (int x = 20; x < mid_x - 20; x++) {
                size_t off = (size_t)y * rb + (size_t)(x >> 3);
                uint8_t bit = (uint8_t)(0x80u >> (x & 7));
                yellow[off] |= bit;
                buf[off] |= bit;
            }
        }
    }
    }

    bool force_full = consume_force_full_refresh_request();
    if (force_full || s_uc8176_register_lut_active ||
        s_uc8179_register_lut_active)
        prepare_for_forced_full_refresh();
    if (s_panel == EPD_PANEL_REF_UC8176_42_BW)
        previous_bw_invalidate();
    EPD_TRY(panel_ensure_ready());

    if (epd_has_yellow() && panel_uses_bwry_2bpp()) {
        if (panel_desc()->data_mode == EPD_CMD_DATA_SSD2677_2BPP)
            EPD_TRY(panel_write_ssd2677_2bpp(buf, red, yellow, 0xFF, 0x00, 0x00));
        else
            EPD_TRY(panel_write_jd796xx_2bpp(buf, red, yellow, 0xFF, 0x00, 0x00));
        EPD_TRY(panel_desc()->refresh());
    } else if (epd_has_yellow() && panel_desc()->data_mode == EPD_CMD_DATA_E6_3IN7_6C) {
        EPD_TRY(panel_write_e6_37_6c(buf, red, yellow, NULL, NULL,
                             0xFF, 0x00, 0x00, 0x00, 0x00));
        EPD_TRY(panel_desc()->refresh());
    } else {
        EPD_TRY(panel_write_bw_red(buf, red, pb));
    }

    free(yellow);
    yellow = NULL;
    free(red);
    red = NULL;
    free(buf);
    buf = NULL;
    xSemaphoreGive(s_epd_mutex);
    epd_locked = false;
    ESP_LOGI(TAG, "test pattern done");
    return ESP_OK;

epd_fail:
    free(buf);
    free(red);
    free(yellow);
    epd_failure(epd_err);
    if (epd_locked) xSemaphoreGive(s_epd_mutex);
    return epd_err;
}

static void draw_bw_refresh_test_pattern(uint8_t *bw,
                                         epd_refresh_mode_t mode,
                                         int target_x, int target_y,
                                         int target_w, int target_h,
                                         const char *title)
{
    const int W = epd_width();
    const int H = epd_height();
    const int pb = epd_plane_bytes();
    const int rb = W / 8;
    memset(bw, 0xFF, (size_t)pb);

    fb_t fb = {
        .black = bw,
        .width = W,
        .height = H,
        .plane_bytes = pb,
        .row_bytes = rb,
    };
    fb_rect(&fb, 0, 0, W, H, COLOR_BLACK);
    fb_rect(&fb, 5, 5, W - 10, H - 10, COLOR_BLACK);
    ui_draw_fixed_text(&fb, 20, 16, title, COLOR_BLACK, 1);

    if (mode == EPD_REFRESH_FULL) {
        test_checker(bw, NULL, rb, 24, 52, 112, 80, 8, false);
        test_checker(bw, NULL, rb, W - 136, 52, 112, 80, 16, false);
        fb_fill_rect(&fb, target_x, target_y, target_w, target_h, COLOR_BLACK);
        int tw = ui_fixed_text_width(&fb, "BASELINE FULL", 1);
        ui_draw_fixed_text(&fb, target_x + (target_w - tw) / 2,
                           target_y + target_h / 2 - 6,
                           "BASELINE FULL", COLOR_WHITE, 1);
        ui_draw_fixed_text(&fb, 20, H - 30,
                           "NEXT: WATCH FLASH AREA AND TIME", COLOR_BLACK, 1);
        return;
    }

    if (mode == EPD_REFRESH_FAST) {
        fb_fill_rect(&fb, 18, 50, W - 36, 54, COLOR_BLACK);
        int tw = ui_fixed_text_width(&fb, "FULL WINDOW CANDIDATE", 1);
        ui_draw_fixed_text(&fb, (W - tw) / 2, 70,
                           "FULL WINDOW CANDIDATE", COLOR_WHITE, 1);
        test_checker(bw, NULL, rb, 28, 126, W - 56, 170, 20, false);
        fb_fill_rect(&fb, 70, 174, W - 140, 74, COLOR_WHITE);
        fb_rect(&fb, 70, 174, W - 140, 74, COLOR_BLACK);
        tw = ui_fixed_text_width(&fb, "FAST? COMPARE ELAPSED TIME", 1);
        ui_draw_fixed_text(&fb, (W - tw) / 2, 204,
                           "FAST? COMPARE ELAPSED TIME", COLOR_BLACK, 1);
        return;
    }

    test_checker(bw, NULL, rb, target_x, target_y,
                 target_w, target_h, 16, false);
    fb_fill_rect(&fb, target_x + 42, target_y + 46,
                 target_w - 84, 52, COLOR_WHITE);
    fb_rect(&fb, target_x + 42, target_y + 46,
            target_w - 84, 52, COLOR_BLACK);
    int tw = ui_fixed_text_width(&fb, "PARTIAL WINDOW", 1);
    ui_draw_fixed_text(&fb, target_x + (target_w - tw) / 2,
                       target_y + 66, "PARTIAL WINDOW", COLOR_BLACK, 1);
}

static bool uc8159_bw_render_regression_check(void)
{
    enum { TEST_W = 96, TEST_H = 32, TEST_RB = TEST_W / 8 };
    uint8_t bw[TEST_RB * TEST_H];
    memset(bw, 0x00, sizeof(bw));

    fb_t fb = {
        .black = bw,
        .width = TEST_W,
        .height = TEST_H,
        .plane_bytes = (int)sizeof(bw),
        .row_bytes = TEST_RB,
    };
    ui_draw_fixed_text(&fb, 4, 8, "WHITE", COLOR_WHITE, 1);
    for (size_t i = 0; i < sizeof(bw); i++) {
        if (bw[i] != 0x00)
            return true;
    }
    return false;
}

static esp_err_t uc8159_write_test_region(const uint8_t *bw,
                                           int x, int y, int w, int h)
{
    esp_err_t epd_err = ESP_OK;
    uint8_t *line = NULL;

    const int W = epd_width();
    const int H = epd_height();
    if (!bw || x < 0 || y < 0 || w <= 0 || h <= 0 ||
        x + w > W || y + h > H || (x & 7) != 0 || (w & 7) != 0)
        return epd_failure(ESP_ERR_INVALID_ARG);

    const int panel_rb = W / 8;
    const int region_rb = w / 8;
    line = heap_caps_malloc((size_t)region_rb * 4u, MALLOC_CAP_DMA);
    if (!line)
        return epd_failure(ESP_ERR_NO_MEM);

    EPD_TRY(epd_cmd(0x10));
    esp_err_t err = ESP_OK;
    for (int row = 0; row < h; row++) {
        size_t src = (size_t)(y + row) * (size_t)panel_rb + (size_t)(x / 8);
        for (int col = 0; col < region_rb; col++)
            uc8159_interleave_color(line + (size_t)col * 4u,
                                    bw[src + (size_t)col], 0x00);
        EPD_TRY(gpio_set_level(PIN_EPD_DC, 1));
        EPD_TRY(err = spi_tx(line, (size_t)region_rb * 4u));
        if (err != ESP_OK)
            break;
    }
    EPD_TRY(epd_cmd(0x11));
    free(line);
    line = NULL;
    return epd_failure(err);

epd_fail:
    free(line);
    return epd_failure(epd_err);
}

static esp_err_t uc8159_refresh_test_window(const uint8_t *bw,
                                             int x, int y, int w, int h,
                                             uint8_t scan_control,
                                             bool sleep_after,
                                             uint32_t *elapsed_ms,
                                             bool *busy_ok)
{
    esp_err_t epd_err = ESP_OK;
    int64_t started = esp_timer_get_time();

    EPD_TRY(epd_cmd(0x04));
    delay_ms(20);
    EPD_TRY(epd_wait_ready("power-on", false));

    EPD_TRY(epd_cmd(0x91));
    EPD_TRY(ref_uc81xx_set_window_scan(x, y, w, h, scan_control));
    EPD_TRY(uc8159_write_test_region(bw, x, y, w, h));
    EPD_TRY(epd_cmd(0x12));
    delay_ms(100);
    EPD_TRY(epd_wait_ready("refresh", true));
    EPD_TRY(epd_cmd(0x92));

    EPD_TRY(epd_cmd(0x02));
    delay_ms(100);
    EPD_TRY(epd_wait_ready("poweroff", true));
    if (sleep_after) {
        EPD_TRY(cmd1(0x07, 0xA5));
        delay_ms(100);
    } else {
        delay_ms(200);
    }

epd_fail:
    if (elapsed_ms)
        *elapsed_ms = (uint32_t)((esp_timer_get_time() - started) / 1000LL);
    if (busy_ok)
        *busy_ok = epd_err == ESP_OK;
    return epd_failure(epd_err);
}

esp_err_t epd_uc8159_refresh_test(epd_refresh_mode_t candidate,
                                  epd_uc8159_test_result_t *result)
{
    esp_err_t epd_err = ESP_OK;
    bool epd_locked = false;
    uint8_t *baseline = NULL;
    uint8_t *changed = NULL;

    if (!result || (candidate != EPD_REFRESH_FAST &&
                    candidate != EPD_REFRESH_PARTIAL))
        return ESP_ERR_INVALID_ARG;
    if (!s_epd_mutex || !s_epd_ready)
        return ESP_ERR_INVALID_STATE;
    if (s_panel != EPD_PANEL_REF_UC8159_75_LOW_BW)
        return ESP_ERR_NOT_SUPPORTED;
    if (busy_idle_effective() != 0) {
        ESP_LOGE(TAG, "UC8159 refresh experiment requires tested BUSY idle-low (busy_idle=0)");
        return ESP_ERR_INVALID_STATE;
    }
    if (!uc8159_bw_render_regression_check()) {
        ESP_LOGE(TAG, "UC8159 BW-only white-text render regression check failed");
        return ESP_FAIL;
    }

    memset(result, 0, sizeof(*result));
    result->x = candidate == EPD_REFRESH_PARTIAL ? 160 : 0;
    result->y = candidate == EPD_REFRESH_PARTIAL ? 120 : 0;
    result->w = candidate == EPD_REFRESH_PARTIAL ? 320 : epd_width();
    result->h = candidate == EPD_REFRESH_PARTIAL ? 144 : epd_height();

    const int pb = epd_plane_bytes();
    baseline = heap_caps_malloc((size_t)pb, MALLOC_CAP_8BIT);
    changed = heap_caps_malloc((size_t)pb, MALLOC_CAP_8BIT);
    if (!baseline || !changed) {
        free(baseline);
        baseline = NULL;
        free(changed);
        changed = NULL;
        return ESP_ERR_NO_MEM;
    }

    draw_bw_refresh_test_pattern(baseline, EPD_REFRESH_FULL,
                                 result->x, result->y,
                                 result->w, result->h,
                                 "UC8159 640x384 REFRESH TEST");
    draw_bw_refresh_test_pattern(changed, candidate,
                                 result->x, result->y,
                                 result->w, result->h,
                                 "UC8159 640x384 REFRESH TEST");

    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);
    epd_locked = true;
    previous_bw_invalidate();
    EPD_TRY(panel_run_full_init());

    ESP_LOGW(TAG,
             "UC8159 refresh experiment start: candidate=%s baseline=full candidate_rect=%d,%d %dx%d; no dedicated fast/partial LUT loaded",
             epd_refresh_mode_name(candidate), result->x, result->y,
             result->w, result->h);
    esp_err_t err;
    EPD_TRY(err = uc8159_refresh_test_window(
        baseline, 0, 0, epd_width(), epd_height(), 0x00, false,
        &result->baseline_ms, &result->baseline_busy_ok));
    if (err == ESP_OK) {
        delay_ms(1200);
        EPD_TRY(err = uc8159_refresh_test_window(
            changed, result->x, result->y, result->w, result->h, 0x01, true,
            &result->candidate_ms, &result->candidate_busy_ok));
    } else {
        EPD_TRY(cmd1(0x07, 0xA5));
    }

    s_panel_seq_done = false;
    previous_bw_invalidate();
    if (err == ESP_OK)
        power_mgr_note_epd_refresh_complete();
    xSemaphoreGive(s_epd_mutex);
    epd_locked = false;

    free(changed);
    changed = NULL;
    free(baseline);
    baseline = NULL;
    ESP_LOGW(TAG,
             "UC8159 refresh experiment done: candidate=%s baseline=%lu ms busy=%d candidate=%lu ms busy=%d err=%s",
             epd_refresh_mode_name(candidate),
             (unsigned long)result->baseline_ms,
             result->baseline_busy_ok ? 1 : 0,
             (unsigned long)result->candidate_ms,
             result->candidate_busy_ok ? 1 : 0,
             esp_err_to_name(err));
    return err;

epd_fail:
    free(baseline);
    free(changed);
    epd_failure(epd_err);
    if (epd_locked) xSemaphoreGive(s_epd_mutex);
    return epd_err;
}

static esp_err_t gd7965_uc8179_75_load_partial_lut(void)
{
    esp_err_t epd_err = ESP_OK;
    /* GxEPD2 GDEW075T7 balanced-charge partial waveform, padded to 42 bytes. */
    static const uint8_t lut_prefix[6][6] = {
        { 0x00, 30, 5, 30, 5, 1 },
        { 0x00, 30, 5, 30, 5, 1 },
        { 0x5A, 30, 5, 30, 5, 1 },
        { 0x84, 30, 5, 30, 5, 1 },
        { 0x00, 30, 5, 30, 5, 1 },
        { 0x00, 30, 5, 30, 5, 1 },
    };

    for (uint8_t bank = 0; bank < 6; bank++) {
        EPD_TRY(epd_cmd((uint8_t)(0x20 + bank)));
        EPD_TRY(epd_bulk(lut_prefix[bank], sizeof(lut_prefix[bank])));
        EPD_TRY(epd_fill(0x00, 42u - sizeof(lut_prefix[bank])));
    }

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t gd7965_uc8179_75_partial_init(void)
{
    esp_err_t epd_err = ESP_OK;
    /* Preserve controller RAM copied by the baseline full refresh. */
    EPD_TRY(cmd4(0x01, 0x07, 0x07, 0x3F, 0x3F));
    EPD_TRY(cmd1(0x00, 0x1F));
    EPD_TRY(ref_send_tres());
    EPD_TRY(cmd1(0x15, 0x00));
    EPD_TRY(cmd2(0x50, 0x29, 0x07));
    EPD_TRY(cmd1(0x60, 0x22));

    EPD_TRY(cmd1(0x00, 0x3F)); /* Use waveform registers instead of OTP. */
    EPD_TRY(cmd1(0x82, 0x26));
    EPD_TRY(cmd2(0x50, 0x39, 0x07));
    EPD_TRY(gd7965_uc8179_75_load_partial_lut());
    ESP_LOGW(TAG, "GD7965/UC8179 7.5 BW candidate partial LUT loaded; HT075A04 compatibility is unverified");

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t gd7965_uc8179_75_refresh_candidate(
    const uint8_t *bw, int x, int y, int w, int h,
    uint32_t *elapsed_ms, bool *busy_ok)
{
    esp_err_t epd_err = ESP_OK;
    epd_dirty_rect_t rect = { .x = x, .y = y, .w = w, .h = h };
    int64_t started = esp_timer_get_time();

    EPD_TRY(gd7965_uc8179_75_partial_init());
    EPD_TRY(epd_cmd(0x04));
    delay_ms(20);
    EPD_TRY(epd_wait_ready("power-on", false));
    EPD_TRY(uc81xx_write_region(0x13, bw, &rect));
    EPD_TRY(epd_cmd(0x91));
    EPD_TRY(ref_uc81xx_set_window_scan(x, y, w, h, 0x01));
    EPD_TRY(epd_cmd(0x12));
    delay_ms(100);
    EPD_TRY(epd_wait_ready("refresh", true));
    EPD_TRY(epd_cmd(0x92));

    EPD_TRY(epd_cmd(0x02));
    delay_ms(100);
    EPD_TRY(epd_wait_ready("poweroff", true));
    EPD_TRY(cmd1(0x07, 0xA5));
    delay_ms(100);

epd_fail:
    if (elapsed_ms)
        *elapsed_ms = (uint32_t)((esp_timer_get_time() - started) / 1000LL);
    if (busy_ok)
        *busy_ok = epd_err == ESP_OK;
    return epd_failure(epd_err);
}

esp_err_t epd_gd7965_75_refresh_test(epd_refresh_mode_t candidate,
                                     epd_gd7965_75_test_result_t *result)
{
    esp_err_t epd_err = ESP_OK;
    bool epd_locked = false;
    uint8_t *baseline = NULL;
    uint8_t *changed = NULL;

    if (!result || (candidate != EPD_REFRESH_FAST &&
                    candidate != EPD_REFRESH_PARTIAL))
        return ESP_ERR_INVALID_ARG;
    if (!s_epd_mutex || !s_epd_ready)
        return ESP_ERR_INVALID_STATE;
    if (!panel_is_gd7965_uc8179_75_bw())
        return ESP_ERR_NOT_SUPPORTED;
    if (busy_idle_effective() != 1) {
        ESP_LOGE(TAG, "GD7965/UC8179 7.5 BW refresh experiment requires busy_idle=1");
        return ESP_ERR_INVALID_STATE;
    }

    memset(result, 0, sizeof(*result));
    result->x = candidate == EPD_REFRESH_PARTIAL ? 200 : 0;
    result->y = candidate == EPD_REFRESH_PARTIAL ? 150 : 0;
    result->w = candidate == EPD_REFRESH_PARTIAL ? 400 : epd_width();
    result->h = candidate == EPD_REFRESH_PARTIAL ? 180 : epd_height();

    const int pb = epd_plane_bytes();
    baseline = heap_caps_malloc((size_t)pb, MALLOC_CAP_8BIT);
    changed = heap_caps_malloc((size_t)pb, MALLOC_CAP_8BIT);
    if (!baseline || !changed) {
        free(baseline);
        baseline = NULL;
        free(changed);
        changed = NULL;
        return ESP_ERR_NO_MEM;
    }

    draw_bw_refresh_test_pattern(baseline, EPD_REFRESH_FULL,
                                 result->x, result->y,
                                 result->w, result->h,
                                 "GD7965 UC8179 800x480 TEST");
    draw_bw_refresh_test_pattern(changed, candidate,
                                 result->x, result->y,
                                 result->w, result->h,
                                 "GD7965 UC8179 800x480 TEST");

    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);
    epd_locked = true;
    previous_bw_invalidate();
    EPD_TRY(panel_run_full_init());

    ESP_LOGW(TAG,
             "GD7965/UC8179 7.5 BW refresh experiment start: candidate=%s baseline=OTP-full candidate_rect=%d,%d %dx%d; partial LUT=GxEPD2 GDEW075T7",
             epd_refresh_mode_name(candidate), result->x, result->y,
             result->w, result->h);
    int64_t baseline_started = esp_timer_get_time();
    EPD_TRY(panel_write_bw_red_data(baseline, NULL, pb));
    EPD_TRY(panel_desc()->refresh());
    result->baseline_ms =
        (uint32_t)((esp_timer_get_time() - baseline_started) / 1000LL);
    result->baseline_busy_ok = true;

    delay_ms(1200);
    esp_err_t err;
    EPD_TRY(err = gd7965_uc8179_75_refresh_candidate(
        changed, result->x, result->y, result->w, result->h,
        &result->candidate_ms, &result->candidate_busy_ok));

    s_panel_seq_done = false;
    previous_bw_invalidate();
    if (err == ESP_OK)
        power_mgr_note_epd_refresh_complete();
    xSemaphoreGive(s_epd_mutex);
    epd_locked = false;

    free(changed);
    changed = NULL;
    free(baseline);
    baseline = NULL;
    ESP_LOGW(TAG,
             "GD7965/UC8179 7.5 BW refresh experiment done: candidate=%s baseline=%lu ms busy=%d candidate=%lu ms busy=%d err=%s",
             epd_refresh_mode_name(candidate),
             (unsigned long)result->baseline_ms,
             result->baseline_busy_ok ? 1 : 0,
             (unsigned long)result->candidate_ms,
             result->candidate_busy_ok ? 1 : 0,
             esp_err_to_name(err));
    return err;

epd_fail:
    free(baseline);
    free(changed);
    epd_failure(epd_err);
    if (epd_locked) xSemaphoreGive(s_epd_mutex);
    return epd_err;
}

static void ssd16xx_213_prepare_refresh_test_patterns(
    uint8_t *baseline, uint8_t *changed, epd_refresh_mode_t candidate,
    int target_x, int target_y, int target_w, int target_h)
{
    const int rb = SSD16XX_213_RAM_W / 8;
    const int pb = rb * SSD16XX_213_H;
    memset(baseline, 0xFF, (size_t)pb);

    fb_t fb = {
        .black = baseline,
        .width = SSD16XX_213_RAM_W,
        .height = SSD16XX_213_H,
        .plane_bytes = pb,
        .row_bytes = rb,
    };
    fb_rect(&fb, 0, 0, SSD16XX_213_ACTIVE_W, SSD16XX_213_H, COLOR_BLACK);
    for (int y = 18; y < 62; y += 8)
        fb_hline(&fb, 8, y, SSD16XX_213_ACTIVE_W - 16, COLOR_BLACK);
    fb_fill_rect(&fb, target_x, target_y, target_w, target_h, COLOR_BLACK);
    fb_rect(&fb, target_x - 3, target_y - 3,
            target_w + 6, target_h + 6, COLOR_BLACK);

    memcpy(changed, baseline, (size_t)pb);
    if (candidate == EPD_REFRESH_FAST) {
        memset(changed, 0xFF, (size_t)pb);
        fb_t changed_fb = fb;
        changed_fb.black = changed;
        fb_rect(&changed_fb, 0, 0, SSD16XX_213_ACTIVE_W, SSD16XX_213_H,
                COLOR_BLACK);
        test_checker(changed, NULL, rb, 8, 8,
                     SSD16XX_213_ACTIVE_W - 16, SSD16XX_213_H - 16,
                     8, false);
    } else {
        test_checker(changed, NULL, rb, target_x, target_y,
                     target_w, target_h, 8, false);
        fb_t changed_fb = fb;
        changed_fb.black = changed;
        fb_rect(&changed_fb, target_x - 3, target_y - 3,
                target_w + 6, target_h + 6, COLOR_BLACK);
    }
}

static esp_err_t ssd16xx_213_set_test_window(int x, int y, int w, int h)
{
    esp_err_t epd_err = ESP_OK;
    const epd_panel_desc_t *d = panel_desc();
    uint8_t x0 = (uint8_t)((d->x_offset + x) / 8);
    uint8_t x1 = (uint8_t)((d->x_offset + x + w - 1) / 8);
    uint16_t y_start = (uint16_t)(SSD16XX_213_H - 1 - y);
    uint16_t y_end = (uint16_t)(SSD16XX_213_H - y - h);

    EPD_TRY(cmd1(0x11, 0x01));
    EPD_TRY(cmd2(0x44, x0, x1));
    EPD_TRY(cmd4(0x45, (uint8_t)y_start, (uint8_t)(y_start >> 8),
         (uint8_t)y_end, (uint8_t)(y_end >> 8)));
    EPD_TRY(cmd1(0x4E, x0));
    EPD_TRY(cmd2(0x4F, (uint8_t)y_start, (uint8_t)(y_start >> 8)));

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t ssd16xx_213_write_test_region(const uint8_t *bw,
                                            int x, int y, int w, int h)
{
    esp_err_t epd_err = ESP_OK;
    uint8_t *line = NULL;

    if (!bw || x < 0 || y < 0 || w <= 0 || h <= 0 ||
        x + w > SSD16XX_213_RAM_W || y + h > SSD16XX_213_H ||
        (x & 7) != 0 || (w & 7) != 0)
        return epd_failure(ESP_ERR_INVALID_ARG);

    const int panel_rb = SSD16XX_213_RAM_W / 8;
    const int region_rb = w / 8;
    line = heap_caps_malloc((size_t)region_rb, MALLOC_CAP_DMA);
    if (!line)
        return epd_failure(ESP_ERR_NO_MEM);

    EPD_TRY(ssd16xx_213_set_test_window(x, y, w, h));
    EPD_TRY(epd_cmd(0x24));
    esp_err_t err = ESP_OK;
    for (int row = 0; row < h; row++) {
        size_t src = (size_t)(y + row) * (size_t)panel_rb +
                     (size_t)(x / 8);
        for (int col = 0; col < region_rb; col++)
            line[col] = (uint8_t)~bw[src + (size_t)col];
        EPD_TRY(gpio_set_level(PIN_EPD_DC, 1));
        EPD_TRY(err = spi_tx(line, (size_t)region_rb));
        if (err != ESP_OK)
            break;
    }
    free(line);
    line = NULL;
    return epd_failure(err);

epd_fail:
    free(line);
    return epd_failure(epd_err);
}

static esp_err_t ssd16xx_213_refresh_test_candidate(
    const uint8_t *bw, epd_refresh_mode_t candidate,
    int x, int y, int w, int h, uint32_t *elapsed_ms, bool *busy_ok)
{
    esp_err_t epd_err = ESP_OK;
    int64_t started = esp_timer_get_time();

    if (candidate == EPD_REFRESH_FAST) {
        EPD_TRY(cmd2(0x1A, 0x64, 0x00));
        EPD_TRY(cmd1(0x22, 0x91));
        EPD_TRY(epd_cmd(0x20));
        EPD_TRY(epd_wait_ready("init", false));
    } else {
        EPD_TRY(gpio_set_level(PIN_EPD_RST, 0));
        delay_ms(1);
        EPD_TRY(gpio_set_level(PIN_EPD_RST, 1));
        delay_ms(2);
        EPD_TRY(cmd1(0x3C, 0x80));
        EPD_TRY(cmd3(0x01, 0xF9, 0x00, 0x00));
    }

    EPD_TRY(ssd16xx_213_write_test_region(bw, x, y, w, h));
    EPD_TRY(cmd1(0x22, candidate == EPD_REFRESH_FAST ? 0xC7 : 0xFF));
    EPD_TRY(epd_cmd(0x20));
    EPD_TRY(epd_wait_ready("refresh", true));

    EPD_TRY(cmd1(0x10, 0x01));
    delay_ms(100);
epd_fail:
    if (elapsed_ms)
        *elapsed_ms = (uint32_t)((esp_timer_get_time() - started) / 1000LL);
    if (busy_ok)
        *busy_ok = epd_err == ESP_OK;
    return epd_failure(epd_err);
}

esp_err_t epd_ssd16xx_213_refresh_test(epd_refresh_mode_t candidate,
                                   epd_ssd16xx_213_test_result_t *result)
{
    esp_err_t epd_err = ESP_OK;
    bool epd_locked = false;
    uint8_t *baseline = NULL;
    uint8_t *changed = NULL;

    if (!result || (candidate != EPD_REFRESH_FAST &&
                    candidate != EPD_REFRESH_PARTIAL))
        return ESP_ERR_INVALID_ARG;
    if (!s_epd_mutex || !s_epd_ready)
        return ESP_ERR_INVALID_STATE;
    if (!panel_is_ssd16xx_213_bw())
        return ESP_ERR_NOT_SUPPORTED;
    if (busy_idle_effective() != 0) {
        ESP_LOGE(TAG, "SSD16xx 2.13 128x250 refresh experiment requires busy_idle=0");
        return ESP_ERR_INVALID_STATE;
    }

    memset(result, 0, sizeof(*result));
    result->x = candidate == EPD_REFRESH_PARTIAL ? 32 : 0;
    result->y = candidate == EPD_REFRESH_PARTIAL ? 85 : 0;
    result->w = candidate == EPD_REFRESH_PARTIAL ? 64 : SSD16XX_213_RAM_W;
    result->h = candidate == EPD_REFRESH_PARTIAL ? 80 : SSD16XX_213_H;

    const int pb = epd_plane_bytes();
    baseline = heap_caps_malloc((size_t)pb, MALLOC_CAP_8BIT);
    changed = heap_caps_malloc((size_t)pb, MALLOC_CAP_8BIT);
    if (!baseline || !changed) {
        free(baseline);
        baseline = NULL;
        free(changed);
        changed = NULL;
        return ESP_ERR_NO_MEM;
    }
    ssd16xx_213_prepare_refresh_test_patterns(
        baseline, changed, candidate,
        candidate == EPD_REFRESH_PARTIAL ? result->x : 32,
        candidate == EPD_REFRESH_PARTIAL ? result->y : 85,
        64, 80);

    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);
    epd_locked = true;
    previous_bw_invalidate();
    EPD_TRY(panel_run_full_init());

    ESP_LOGW(TAG,
             "SSD16xx 2.13 128x250 refresh experiment start: candidate=%s baseline=full candidate_rect=%d,%d %dx%d",
             epd_refresh_mode_name(candidate), result->x, result->y,
             result->w, result->h);
    int64_t baseline_started = esp_timer_get_time();
    EPD_TRY(panel_write_bw_red_data(baseline, NULL, pb));
    EPD_TRY(panel_desc()->refresh());
    result->baseline_ms =
        (uint32_t)((esp_timer_get_time() - baseline_started) / 1000LL);
    result->baseline_busy_ok = true;

    delay_ms(1200);
    esp_err_t err;
    EPD_TRY(err = ssd16xx_213_refresh_test_candidate(
        changed, candidate, result->x, result->y, result->w, result->h,
        &result->candidate_ms, &result->candidate_busy_ok));

    s_panel_seq_done = false;
    previous_bw_invalidate();
    if (err == ESP_OK)
        power_mgr_note_epd_refresh_complete();
    xSemaphoreGive(s_epd_mutex);
    epd_locked = false;

    free(changed);
    changed = NULL;
    free(baseline);
    baseline = NULL;
    ESP_LOGW(TAG,
             "SSD16xx 2.13 128x250 refresh experiment done: candidate=%s baseline=%lu ms busy=%d candidate=%lu ms busy=%d err=%s",
             epd_refresh_mode_name(candidate),
             (unsigned long)result->baseline_ms,
             result->baseline_busy_ok ? 1 : 0,
             (unsigned long)result->candidate_ms,
             result->candidate_busy_ok ? 1 : 0,
             esp_err_to_name(err));
    return err;

epd_fail:
    free(baseline);
    free(changed);
    epd_failure(epd_err);
    if (epd_locked) xSemaphoreGive(s_epd_mutex);
    return epd_err;
}

static void repair_fill_pattern(uint8_t *bw, uint8_t *red, uint8_t *yellow,
                                int variant, bool color_pattern)
{
    const int W = epd_width();
    const int H = epd_height();
    const int rb = W / 8;
    const int pb = epd_plane_bytes();
    memset(bw, 0xFF, (size_t)pb);
    if (red)
        memset(red, 0x00, (size_t)pb);
    if (yellow)
        memset(yellow, 0x00, (size_t)pb);

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            bool active;
            switch (variant & 3) {
            case 0: active = (((x >> 3) + (y >> 3)) & 1) == 0; break; /* 8x8 checker */
            case 1: active = (((x >> 3) + (y >> 3)) & 1) != 0; break;
            case 2: active = ((x >> 2) & 1) == 0; break;              /* vertical stripes */
            default: active = ((y >> 2) & 1) == 0; break;             /* horizontal stripes */
            }
            if (!active)
                continue;
            size_t off = (size_t)y * (size_t)rb + (size_t)(x >> 3);
            uint8_t bit = (uint8_t)(0x80u >> (x & 7));
            if (color_pattern && red && epd_has_red() && ((variant + x / 16 + y / 16) & 1)) {
                red[off] |= bit;
                bw[off] |= bit;
            } else if (color_pattern && yellow && epd_has_yellow()) {
                yellow[off] |= bit;
                bw[off] |= bit;
            } else {
                bw[off] &= (uint8_t)~bit;
                if (red)
                    red[off] &= (uint8_t)~bit;
                if (yellow)
                    yellow[off] &= (uint8_t)~bit;
            }
        }
    }
}

static esp_err_t repair_display_pattern(const uint8_t *bw, const uint8_t *red,
                                   const uint8_t *yellow)
{
    esp_err_t epd_err = ESP_OK;
    if (epd_has_yellow() && panel_uses_bwry_2bpp()) {
        if (panel_desc()->data_mode == EPD_CMD_DATA_SSD2677_2BPP)
            EPD_TRY(panel_write_ssd2677_2bpp(bw, red, yellow, 0xFF, 0x00, 0x00));
        else
            EPD_TRY(panel_write_jd796xx_2bpp(bw, red, yellow, 0xFF, 0x00, 0x00));
        EPD_TRY(panel_desc()->refresh());
    } else if (epd_has_yellow() && panel_desc()->data_mode == EPD_CMD_DATA_E6_3IN7_6C) {
        EPD_TRY(panel_write_e6_37_6c(bw, red, yellow, NULL, NULL,
                             0xFF, 0x00, 0x00, 0x00, 0x00));
        EPD_TRY(panel_desc()->refresh());
    } else {
        EPD_TRY(panel_write_bw_red(bw, red, epd_plane_bytes()));
    }

    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static uint8_t *repair_alloc_plane_buffer(int pb, const char *name)
{
    uint8_t *buf = heap_caps_malloc((size_t)pb, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf)
        buf = heap_caps_malloc((size_t)pb, MALLOC_CAP_8BIT);
    if (!buf)
        ESP_LOGE(TAG, "ghost repair %s buffer malloc failed: %d bytes", name, pb);
    return buf;
}

esp_err_t epd_repair_ghosting(int cycles, int pattern)
{
    esp_err_t epd_err = ESP_OK;
    bool epd_locked = false;
    uint8_t *bw = NULL;
    uint8_t *red = NULL;
    uint8_t *yellow = NULL;

    if (!s_epd_mutex || !s_epd_ready)
        return ESP_ERR_INVALID_STATE;
    if (cycles < 1)
        cycles = 1;
    if (cycles > EPD_REPAIR_MAX_CYCLES)
        cycles = EPD_REPAIR_MAX_CYCLES;
    if (pattern < 0 || pattern > 2)
        pattern = 0;

    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);
    epd_locked = true;

    int pb = epd_plane_bytes();
    bw = NULL;
    red = NULL;
    yellow = NULL;
    if (pattern != 0) {
        bw = repair_alloc_plane_buffer(pb, "bw");
        red = repair_alloc_plane_buffer(pb, "red");
        yellow = epd_has_yellow() ? repair_alloc_plane_buffer(pb, "yellow") : NULL;
        if (!bw || !red || (epd_has_yellow() && !yellow)) {
            free(yellow);
            yellow = NULL;
            free(red);
            red = NULL;
            free(bw);
            bw = NULL;
            xSemaphoreGive(s_epd_mutex);
            epd_locked = false;
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "ghost repair start: cycles=%d, pattern=%d, panel=%s",
             cycles, pattern, panel_desc()->name);
    bool force_full = consume_force_full_refresh_request();
    if (force_full || s_uc8176_register_lut_active ||
        s_uc8179_register_lut_active)
        prepare_for_forced_full_refresh();
    if (s_panel == EPD_PANEL_REF_UC8176_42_BW)
        previous_bw_invalidate();
    EPD_TRY(panel_ensure_ready());

    for (int i = 0; i < cycles; i++) {
        if (pattern == 0 || pattern == 2) {
            ESP_LOGI(TAG, "ghost repair cycle %d/%d: white", i + 1, cycles);
            EPD_TRY(panel_fill_bw_red(0xFF, 0x00, pb));
            delay_ms(300);

            ESP_LOGI(TAG, "ghost repair cycle %d/%d: black", i + 1, cycles);
            EPD_TRY(panel_fill_bw_red(0x00, 0x00, pb));
            delay_ms(300);

            if (epd_has_red() || epd_has_yellow()) {
                ESP_LOGI(TAG, "ghost repair cycle %d/%d: color", i + 1, cycles);
                EPD_TRY(panel_fill_bw_red(0xFF, 0xFF, pb));
                delay_ms(300);
            }
        }

        if (pattern == 1 || pattern == 2) {
            ESP_LOGI(TAG, "ghost repair cycle %d/%d: checker A", i + 1, cycles);
            repair_fill_pattern(bw, red, yellow, i * 2, false);
            EPD_TRY(repair_display_pattern(bw, red, yellow));
            delay_ms(300);

            ESP_LOGI(TAG, "ghost repair cycle %d/%d: checker B", i + 1, cycles);
            repair_fill_pattern(bw, red, yellow, i * 2 + 1, epd_has_red() || epd_has_yellow());
            EPD_TRY(repair_display_pattern(bw, red, yellow));
            delay_ms(300);
        }
    }

    ESP_LOGI(TAG, "ghost repair finish: white");
    EPD_TRY(panel_fill_bw_red(0xFF, 0x00, pb));

    free(yellow);
    yellow = NULL;
    free(red);
    red = NULL;
    free(bw);
    bw = NULL;
    xSemaphoreGive(s_epd_mutex);
    epd_locked = false;
    ESP_LOGI(TAG, "ghost repair done");
    return ESP_OK;

epd_fail:
    free(bw);
    free(red);
    free(yellow);
    epd_failure(epd_err);
    if (epd_locked) xSemaphoreGive(s_epd_mutex);
    return epd_err;
}

static epd_refresh_mode_t resolve_refresh_mode(epd_refresh_mode_t requested,
                                               bool force_full,
                                               const fb_t *fb,
                                               const char **fallback_reason)
{
    *fallback_reason = NULL;
    if (requested < EPD_REFRESH_FULL || requested > EPD_REFRESH_PARTIAL) {
        *fallback_reason = "invalid-request";
        return EPD_REFRESH_FULL;
    }
    if (force_full) {
        *fallback_reason = "forced-full";
        return EPD_REFRESH_FULL;
    }

    epd_refresh_mode_t actual = requested;
    if (requested == EPD_REFRESH_PARTIAL &&
        !epd_refresh_mode_supported(EPD_REFRESH_PARTIAL)) {
        if (epd_refresh_mode_supported(EPD_REFRESH_FAST)) {
            actual = EPD_REFRESH_FAST;
            *fallback_reason = "partial-unsupported";
        } else {
            actual = EPD_REFRESH_FULL;
            *fallback_reason = "mode-unsupported";
        }
    } else if (requested == EPD_REFRESH_FAST &&
               !epd_refresh_mode_supported(EPD_REFRESH_FAST)) {
        actual = EPD_REFRESH_FULL;
        *fallback_reason = "mode-unsupported";
    }

    if (actual == EPD_REFRESH_FULL)
        return actual;
    if (panel_is_uc8253_37()) {
        if (!previous_bw_matches_current_panel()) {
            *fallback_reason = "previous-frame-unavailable";
            return EPD_REFRESH_FULL;
        }
        if (actual == EPD_REFRESH_PARTIAL &&
            s_uc8253_partial_refresh_count >= UC8253_PARTIAL_REFRESH_LIMIT) {
            *fallback_reason = "maintenance-full";
            return EPD_REFRESH_FULL;
        }
        return actual;
    }
    if (panel_is_uc8179_75_bwr()) {
        if (!s_panel_seq_done) {
            *fallback_reason = "controller-uninitialized";
            return EPD_REFRESH_FULL;
        }
        if (!previous_bw_matches_current_panel()) {
            *fallback_reason = "previous-frame-unavailable";
            return EPD_REFRESH_FULL;
        }
        if (!s_uc8179_red_signature_valid || !fb) {
            *fallback_reason = "red-baseline-unavailable";
            return EPD_REFRESH_FULL;
        }
        if (plane_signature(fb->red, epd_plane_bytes(), 0x00) !=
            s_uc8179_red_signature) {
            *fallback_reason = "red-plane-changed";
            return EPD_REFRESH_FULL;
        }
        epd_dirty_rect_t dirty;
        if (epd_find_dirty_rect(s_previous_bw, fb->black, &dirty) &&
            !plane_region_is_clear(fb->red, &dirty)) {
            *fallback_reason = "partial-window-overlaps-red";
            return EPD_REFRESH_FULL;
        }
        if (s_uc8179_partial_refresh_count >= UC8179_75_PARTIAL_REFRESH_LIMIT) {
            *fallback_reason = "maintenance-full";
            return EPD_REFRESH_FULL;
        }
        return actual;
    }
    if (panel_is_jd79686a_583()) {
        if (!s_panel_seq_done) {
            *fallback_reason = "controller-uninitialized";
            return EPD_REFRESH_FULL;
        }
        if (!previous_bw_matches_current_panel()) {
            *fallback_reason = "previous-frame-unavailable";
            return EPD_REFRESH_FULL;
        }
        /* PDRF 只刷窗口：红平面必须与上一帧一致，否则窗口外红色变化会丢 */
        if (!s_jd79686a_red_signature_valid || !fb) {
            *fallback_reason = "red-baseline-unavailable";
            return EPD_REFRESH_FULL;
        }
        if (plane_signature(fb->red, epd_plane_bytes(), 0x00) !=
            s_jd79686a_red_signature) {
            *fallback_reason = "red-plane-changed";
            return EPD_REFRESH_FULL;
        }
        if (s_jd79686a_partial_refresh_count >= JD79686A_PARTIAL_REFRESH_LIMIT) {
            *fallback_reason = "maintenance-full";
            return EPD_REFRESH_FULL;
        }
        return actual;
    }
    if (s_panel != EPD_PANEL_REF_UC8176_42_BW) {
        *fallback_reason = "driver-not-enabled";
        return EPD_REFRESH_FULL;
    }
    if (!s_panel_seq_done) {
        *fallback_reason = "controller-uninitialized";
        return EPD_REFRESH_FULL;
    }
    if (!previous_bw_matches_current_panel()) {
        *fallback_reason = "previous-frame-unavailable";
        return EPD_REFRESH_FULL;
    }
    if (s_experimental_refresh_count >= UC8176_EXPERIMENTAL_REFRESH_LIMIT) {
        *fallback_reason = "maintenance-full";
        return EPD_REFRESH_FULL;
    }
    return actual;
}

static esp_err_t display_framebuffer_locked(const fb_t *fb,
                                            epd_refresh_mode_t requested,
                                            bool force_full,
                                            bool *did_refresh)
{
    esp_err_t epd_err = ESP_OK;
    const char *fallback_reason = NULL;
    epd_refresh_mode_t actual = resolve_refresh_mode(requested, force_full, fb,
                                                     &fallback_reason);
    unsigned refresh_count = panel_is_uc8253_37()
                                 ? s_uc8253_partial_refresh_count
                                 : (panel_is_uc8179_75_bwr()
                                        ? s_uc8179_partial_refresh_count
                                        : (panel_is_jd79686a_583()
                                               ? s_jd79686a_partial_refresh_count
                                               : s_experimental_refresh_count));
    unsigned refresh_limit = panel_is_uc8253_37()
                                 ? UC8253_PARTIAL_REFRESH_LIMIT
                                 : (panel_is_uc8179_75_bwr()
                                        ? UC8179_75_PARTIAL_REFRESH_LIMIT
                                        : (panel_is_jd79686a_583()
                                               ? JD79686A_PARTIAL_REFRESH_LIMIT
                                               : UC8176_EXPERIMENTAL_REFRESH_LIMIT));
    ESP_LOGI(TAG,
             "refresh plan: panel=%s requested=%s actual=%s fallback=%s caps=0x%02X count=%u/%u",
             panel_desc()->name, epd_refresh_mode_name(requested),
             epd_refresh_mode_name(actual),
             fallback_reason ? fallback_reason : "none",
             (unsigned)epd_refresh_capabilities(),
             refresh_count, refresh_limit);

    if (actual != EPD_REFRESH_FULL && panel_is_uc8179_75_bwr())
        { EPD_TRY(uc8179_75_display_partial(fb->black, did_refresh)); return ESP_OK; };
    if (actual != EPD_REFRESH_FULL && panel_is_jd79686a_583())
        { EPD_TRY(jd79686a_display_partial(fb->black, fb->red, did_refresh)); return ESP_OK; };
    if (actual != EPD_REFRESH_FULL && !panel_is_uc8253_37())
        { EPD_TRY(uc8176_display_experimental(fb->black, actual, did_refresh)); return ESP_OK; };

    if (force_full || s_uc8176_register_lut_active ||
        s_uc8179_register_lut_active)
        prepare_for_forced_full_refresh();
    if (s_panel == EPD_PANEL_REF_UC8176_42_BW)
        previous_bw_invalidate();
    if (panel_is_uc8253_37() && actual != EPD_REFRESH_FULL) {
        EPD_TRY(uc8253_init_mode(actual));
        s_panel_seq_done = true;
    } else {
        EPD_TRY(panel_ensure_ready());
    }

    const int pb = epd_plane_bytes();
    if (panel_desc()->data_mode == EPD_CMD_DATA_E6_3IN7_6C) {
        EPD_TRY(panel_write_e6_37_6c(fb->black, fb->red, fb->yellow, fb->blue, fb->green,
                             0xFF, 0x00, 0x00, 0x00, 0x00));
        EPD_TRY(panel_desc()->refresh());
    } else if (panel_uses_bwry_2bpp() && epd_has_yellow()) {
        if (panel_desc()->data_mode == EPD_CMD_DATA_SSD2677_2BPP)
            EPD_TRY(panel_write_ssd2677_2bpp(fb->black, fb->red, fb->yellow,
                                     0xFF, 0x00, 0x00));
        else
            EPD_TRY(panel_write_jd796xx_2bpp(fb->black, fb->red, fb->yellow,
                                     0xFF, 0x00, 0x00));
        EPD_TRY(panel_desc()->refresh());
    } else {
        EPD_TRY(panel_write_bw_red_data(fb->black, fb->red, pb));
        EPD_TRY(panel_desc()->refresh());
    }

    if (panel_is_uc8253_37()) {
        if (previous_bw_store(fb->black, pb)) {
            if (actual == EPD_REFRESH_PARTIAL)
                s_uc8253_partial_refresh_count++;
            else if (actual == EPD_REFRESH_FULL)
                s_uc8253_partial_refresh_count = 0;
        } else {
            previous_bw_invalidate();
        }
    }
    if (panel_is_uc8179_75_bwr()) {
        if (previous_bw_store(fb->black, pb)) {
            s_uc8179_red_signature = plane_signature(fb->red, pb, 0x00);
            s_uc8179_red_signature_valid = true;
            s_uc8179_partial_refresh_count = 0;
            ESP_LOGI(TAG, "GDEY075Z08 UC8179 full baseline: BW partial cache ready");
        } else {
            previous_bw_invalidate();
        }
    }
    if (panel_is_jd79686a_583()) {
        if (previous_bw_store(fb->black, pb)) {
            s_jd79686a_red_signature = plane_signature(fb->red, pb, 0x00);
            s_jd79686a_red_signature_valid = true;
            s_jd79686a_partial_refresh_count = 0;
            jd79686a_prev_red_store(fb->red, 0x00, pb);
            ESP_LOGI(TAG, "JD79686A full baseline: partial cache ready");
        } else {
            previous_bw_invalidate();
        }
    }
    if (s_panel == EPD_PANEL_REF_UC8176_42_BW) {
        (void)previous_bw_store(fb->black, pb);
        s_experimental_refresh_count = 0;
    }
    *did_refresh = true;
    return ESP_OK;

epd_fail:
    return epd_failure(epd_err);
}

static esp_err_t display_buffer_common(fb_t *fb, epd_refresh_mode_t mode,
                                       bool destroy_fb, const unsigned *epoch)
{
    esp_err_t epd_err = ESP_OK;
    bool epd_locked = false;

    if (!fb)
        return ESP_ERR_INVALID_ARG;
    if (!s_epd_mutex || !s_epd_ready) {
        if (destroy_fb)
            fb_destroy(fb);
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);
    epd_locked = true;
    if (epoch && !display_policy_epoch_is_current(*epoch)) {
        if (destroy_fb)
            fb_destroy(fb);
        xSemaphoreGive(s_epd_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (!fb_matches_current_panel(fb)) {
        ESP_LOGE(TAG, "fb/panel mismatch: fb=%dx%d/%d current=%dx%d/%d",
                 fb->width, fb->height, fb->plane_bytes,
                 epd_width(), epd_height(), epd_plane_bytes());
        xSemaphoreGive(s_epd_mutex);
        epd_locked = false;
        if (destroy_fb)
            fb_destroy(fb);
        return ESP_ERR_INVALID_SIZE;
    }

    bool did_refresh = false;
    bool force_full = consume_force_full_refresh_request();
    esp_err_t err;
    EPD_TRY(err = display_framebuffer_locked(fb, mode, force_full, &did_refresh));
    if (destroy_fb)
        fb_destroy(fb);
    if (err == ESP_OK && did_refresh)
        power_mgr_note_epd_refresh_complete();
    xSemaphoreGive(s_epd_mutex);
    epd_locked = false;

    if (err == ESP_OK)
        ESP_LOGI(TAG, "display from buffer requested=%s refreshed=%d",
                 epd_refresh_mode_name(mode), did_refresh ? 1 : 0);
    return err;

epd_fail:
    if (destroy_fb) fb_destroy(fb);
    epd_failure(epd_err);
    if (epd_locked) xSemaphoreGive(s_epd_mutex);
    return epd_err;
}

static esp_err_t display_file_common(const char *path, epd_refresh_mode_t mode,
                                     const unsigned *epoch)
{
    esp_err_t epd_err = ESP_OK;
    bool epd_locked = false;
    fb_t *fb = NULL;
    FILE *f = NULL;

    if (!path)
        return ESP_ERR_INVALID_ARG;
    if (!s_epd_mutex || !s_epd_ready)
        return ESP_ERR_INVALID_STATE;

    if (panel_desc()->data_mode == EPD_CMD_DATA_E6_3IN7_6C ||
        s_panel == EPD_PANEL_REF_UC8176_42_BW || panel_is_uc8253_37() ||
        panel_is_uc8179_75_bwr()) {
        int64_t t0 = esp_timer_get_time();
        fb = fb_create();
        if (!fb)
            return ESP_ERR_NO_MEM;
        esp_err_t err = fb_import(fb, path);
        if (err != ESP_OK) {
            fb_destroy(fb);
            fb = NULL;
            return err;
        }
        /* The consuming API releases fb even on failure. */
        err = display_buffer_common(fb, mode, true, epoch);
        fb = NULL;
        ESP_LOGI(TAG, "framebuffer display from %s finished in %lld ms: %s",
                 path, (long long)((esp_timer_get_time() - t0) / 1000LL),
                 esp_err_to_name(err));
        return err;
    }

    int pb = epd_plane_bytes();
    f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "open %s failed", path);
        return ESP_FAIL;
    }

    struct stat st;
    bool have_size = (stat(path, &st) == 0);
    if (have_size && st.st_size < pb) {
        ESP_LOGE(TAG, "file too small: %ld / %u", (long)st.st_size, (unsigned)pb);
        fclose(f);
        f = NULL;
        return ESP_ERR_INVALID_SIZE;
    }
    bool has_red_file = have_size && st.st_size >= (off_t)pb * 2;
    bool has_yellow_file = have_size && st.st_size >= (off_t)pb * 3;
    bool has_blue_file = have_size && st.st_size >= (off_t)pb * 4;
    bool has_green_file = have_size && st.st_size >= (off_t)pb * 5;
    if (epd_has_red() && !has_red_file)
        ESP_LOGW(TAG, "red plane missing, zero fill");
    if (epd_has_yellow() && !has_yellow_file)
        ESP_LOGW(TAG, "yellow plane missing, zero fill");

    xSemaphoreTake(s_epd_mutex, portMAX_DELAY);
    epd_locked = true;
    if (epoch && !display_policy_epoch_is_current(*epoch)) {
        fclose(f);
        xSemaphoreGive(s_epd_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (pb != epd_plane_bytes()) {
        ESP_LOGE(TAG, "file/panel mismatch: file plane=%d current=%d",
                 pb, epd_plane_bytes());
        xSemaphoreGive(s_epd_mutex);
        epd_locked = false;
        fclose(f);
        f = NULL;
        return ESP_ERR_INVALID_SIZE;
    }

    bool force_full = consume_force_full_refresh_request();
    const char *fallback_reason = NULL;
    epd_refresh_mode_t actual = resolve_refresh_mode(mode, force_full, NULL,
                                                     &fallback_reason);
    if (actual != EPD_REFRESH_FULL) {
        actual = EPD_REFRESH_FULL;
        fallback_reason = "streaming-file-full-only";
    }
    ESP_LOGI(TAG, "refresh plan: panel=%s requested=%s actual=%s fallback=%s caps=0x%02X",
             panel_desc()->name, epd_refresh_mode_name(mode),
             epd_refresh_mode_name(actual),
             fallback_reason ? fallback_reason : "none",
             (unsigned)epd_refresh_capabilities());

    if (force_full)
        prepare_for_forced_full_refresh();
    EPD_TRY(panel_ensure_ready());
    esp_err_t ret;
    EPD_TRY(ret = panel_write_bw_red_file(f, pb, has_red_file, has_yellow_file,
                                            has_blue_file, has_green_file));
    /* 流式文件刷屏不写上一帧缓存：作废局刷基线，下一次局刷自动回退全刷 */
    if (panel_is_jd79686a_583())
        previous_bw_invalidate();
    if (ret == ESP_OK)
        power_mgr_note_epd_refresh_complete();
    xSemaphoreGive(s_epd_mutex);
    epd_locked = false;
    fclose(f);
    f = NULL;
    if (ret != ESP_OK)
        return ret;

    ESP_LOGI(TAG, "display from %s", path);
    return ESP_OK;

epd_fail:
    fb_destroy(fb);
    if (f) fclose(f);
    epd_failure(epd_err);
    if (epd_locked) xSemaphoreGive(s_epd_mutex);
    return epd_err;
}

esp_err_t epd_display_from_file(const char *path)
{
    return epd_display_from_file_mode(path, EPD_REFRESH_FULL);
}

esp_err_t epd_display_from_file_mode(const char *path, epd_refresh_mode_t mode)
{
    return display_file_common(path, mode, NULL);
}

esp_err_t epd_display_from_file_mode_epoch(const char *path, epd_refresh_mode_t mode,
                                         unsigned epoch)
{
    return display_file_common(path, mode, &epoch);
}

esp_err_t epd_display_from_buffer_mode(const fb_t *fb, epd_refresh_mode_t mode)
{
    return display_buffer_common((fb_t *)fb, mode, false, NULL);
}

esp_err_t epd_display_from_buffer(const fb_t *fb)
{
    return epd_display_from_buffer_mode(fb, EPD_REFRESH_FULL);
}

esp_err_t epd_display_fb_free_mode(fb_t *fb, epd_refresh_mode_t mode)
{
    return display_buffer_common(fb, mode, true, NULL);
}

esp_err_t epd_display_fb_free_mode_epoch(fb_t *fb, epd_refresh_mode_t mode,
                                       unsigned epoch)
{
    return display_buffer_common(fb, mode, true, &epoch);
}

esp_err_t epd_display_fb_free(fb_t *fb)
{
    return epd_display_fb_free_mode(fb, EPD_REFRESH_FULL);
}
