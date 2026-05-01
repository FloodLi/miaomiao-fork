#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include "fb_render.h"

typedef enum {
    EPD_PANEL_42_BWR        = 0,  /* 4.2" 400x300 BW+Red, SSD1619 */
    EPD_PANEL_583_BWR       = 1,  /* 5.83" 648x480 BW+Red, UC8179 */

    /* Waveshare reference drivers ported from E-Paper_ESP32_Driver_Board_Code. */
    EPD_PANEL_WS_29_BW      = 2,  /* 2.9" 128x296 BW V2 */
    EPD_PANEL_WS_29_BWR     = 3,  /* 2.9" 128x296 BW+Red B V4 */
    EPD_PANEL_WS_266_BW     = 4,  /* 2.66" 152x296 BW */
    EPD_PANEL_WS_266_BWR    = 5,  /* 2.66" 152x296 BW+Red */
    EPD_PANEL_WS_27_BW      = 6,  /* 2.7" 176x264 BW V2 */
    EPD_PANEL_WS_27_BWR     = 7,  /* 2.7" 176x264 BW+Red B V2 */
    EPD_PANEL_WS_426_BW     = 8,  /* 4.26" 800x480 BW */
    EPD_PANEL_WS_583_BW     = 9,  /* 5.83" 648x480 BW V2 */
    EPD_PANEL_WS_583_BWR    = 10, /* 5.83" 648x480 BW+Red B V2 */
    EPD_PANEL_WS_75_BW      = 11, /* 7.5" 800x480 BW V2 */
    EPD_PANEL_WS_75_BWR     = 12, /* 7.5" 800x480 BW+Red B V2 */
    EPD_PANEL_WS_42_BWR_UC8176 = 13, /* 4.2" 400x300 BW+Red, UC8176 */

    EPD_PANEL_COUNT
} epd_panel_t;

/**
 * 从 NVS 读取面板类型 / busy_idle（不写 SPI、不初始化墨水屏）。
 * 在 epd_init() 之前调用，便于 HTTP 已启动后 /panel_config 与网页使用正确分辨率；
 * 换屏后若 NVS 仍是小屏，先起网再改配置可避免长时间卡在 BUSY 等待。
 */
void         epd_load_panel_from_nvs(void);

esp_err_t    epd_init(void);
/** false 直至 epd_init() 成功返回；HTTP 等应在刷图前检查 */
bool         epd_is_ready(void);
/** Wait until no EPD transfer/refresh function is holding the display mutex. */
bool         epd_wait_idle(uint32_t timeout_ms);
esp_err_t    epd_display_from_file(const char *path);
esp_err_t    epd_display_from_buffer(const fb_t *fb);
/** 同上，但 SPI 传输完成后立即释放 fb，再等待 EPD 刷新——节省 18s 内存占用 */
esp_err_t    epd_display_fb_free(fb_t *fb);
/**
 * Fast BW-only refresh using a custom short LUT (~1-2s instead of ~10s).
 * Only the black plane is driven; red is left unchanged.
 * May produce slight ghosting — follow with a full refresh to clean up.
 * Currently only implemented for SSD1619 (4.2"); other panels fall back to full.
 */
esp_err_t    epd_display_from_buffer_fast(const fb_t *fb);
esp_err_t    epd_clear_screen(void);
esp_err_t    epd_display_test_pattern(void);

epd_panel_t  epd_get_panel(void);
esp_err_t    epd_set_panel(epd_panel_t panel);
esp_err_t    epd_save_panel_config(epd_panel_t panel, int busy_idle);

/** 当前 BUSY 空闲电平：0=空闲时低，1=空闲时高（与 NVS epd/busy_idle 一致） */
int          epd_busy_idle_level(void);
/** 指定面板的默认 BUSY 空闲电平；非法面板返回 -1。 */
int          epd_panel_default_busy_idle(epd_panel_t panel);
esp_err_t    epd_set_busy_idle(int level); /* 0 或 1，写入 NVS 并立即生效 */

int          epd_width(void);
int          epd_height(void);
int          epd_plane_bytes(void);
bool         epd_has_red(void);
