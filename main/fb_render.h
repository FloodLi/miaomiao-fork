#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#define FB_MAX_WIDTH   648
#define FB_MAX_HEIGHT  480
#define FB_MAX_PLANE_BYTES  ((FB_MAX_WIDTH / 8) * FB_MAX_HEIGHT)

typedef enum {
    COLOR_WHITE = 0,
    COLOR_BLACK = 1,
    COLOR_RED   = 2,
} fb_color_t;

typedef struct {
    uint8_t *black;
    uint8_t *red;
    int      width;
    int      height;
    int      plane_bytes;
    int      row_bytes;
    bool     planes_split; /* true: black/red 是两次独立 malloc，需分别 free */
    bool     uses_reserved;/* true: 使用 fb_reserve_planes_early() 预留块，fb_destroy 不 free 平面 */
} fb_t;

/**
 * 在 WiFi/HTTP 等大块分配之前调用（需已 epd_load_panel_from_nvs），
 * 按当前面板尺寸预留 2×plane 连续 RAM，减轻碎片导致 5.83" 无法分配 2×38880。
 */
void        fb_reserve_planes_early(void);
/** 切换面板类型时释放预留（由 epd_set_panel 调用） */
void        fb_release_reserved_planes(void);
void        fb_raw_file_lock(void);
void        fb_raw_file_unlock(void);

fb_t       *fb_create(void);
void        fb_destroy(fb_t *fb);
void        fb_clear(fb_t *fb);

void        fb_pixel(fb_t *fb, int x, int y, fb_color_t c);
void        fb_hline(fb_t *fb, int x, int y, int w, fb_color_t c);
void        fb_vline(fb_t *fb, int x, int y, int h, fb_color_t c);
void        fb_rect(fb_t *fb, int x, int y, int w, int h, fb_color_t c);
void        fb_fill_rect(fb_t *fb, int x, int y, int w, int h, fb_color_t c);

void        fb_bitmap(fb_t *fb, int x, int y, int w, int h,
                      const uint8_t *data, fb_color_t c);

int         fb_char(fb_t *fb, int x, int y, char ch, fb_color_t c);
int         fb_string(fb_t *fb, int x, int y, const char *s, fb_color_t c);
int         fb_utf8(fb_t *fb, int x, int y, const char *s, fb_color_t c);

int         fb_char_2x(fb_t *fb, int x, int y, char ch, fb_color_t c);
int         fb_utf8_2x(fb_t *fb, int x, int y, const char *s, fb_color_t c);
int         fb_utf8_scaled(fb_t *fb, int x, int y, const char *s,
                           fb_color_t c, int scale);
/** 同上，但超出 max_w 像素宽度则截断（不画半个字） */
int         fb_utf8_scaled_maxw(fb_t *fb, int x, int y, const char *s,
                                fb_color_t c, int scale, int max_w);

esp_err_t   fb_export(const fb_t *fb, const char *path);
esp_err_t   fb_import(fb_t *fb, const char *path);
