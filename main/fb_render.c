#include "fb_render.h"
#include "font_data.h"
#include "epd.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

static const char *TAG = "fb";

/* ── lifecycle ────────────────────────────────────────────────────── */

fb_t *fb_create(void)
{
    int W  = epd_width();
    int H  = epd_height();
    int pb = epd_plane_bytes();

    fb_t *fb = calloc(1, sizeof(fb_t));
    if (!fb) return NULL;

    fb->width       = W;
    fb->height      = H;
    fb->plane_bytes = pb;
    fb->row_bytes   = W / 8;

    /* Allocate both planes as one block to avoid fragmentation */
    uint8_t *buf = heap_caps_malloc((size_t)pb * 2, MALLOC_CAP_DMA);
    if (!buf) {
        buf = malloc((size_t)pb * 2);
        if (!buf) {
            free(fb);
            ESP_LOGE(TAG, "Failed to allocate frame buffer (2x%d bytes), "
                     "free heap: %lu, largest block: %lu",
                     pb,
                     (unsigned long)esp_get_free_heap_size(),
                     (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            return NULL;
        }
        ESP_LOGW(TAG, "DMA alloc failed, using regular heap (2x%d bytes)", pb);
    }
    fb->black = buf;
    fb->red   = buf + pb;

    fb_clear(fb);
    return fb;
}

void fb_destroy(fb_t *fb)
{
    if (!fb) return;
    free(fb->black);
    free(fb);
}

void fb_clear(fb_t *fb)
{
    memset(fb->black, 0xFF, fb->plane_bytes);
    memset(fb->red,   0x00, fb->plane_bytes);
}

/* ── primitives ───────────────────────────────────────────────────── */

void fb_pixel(fb_t *fb, int x, int y, fb_color_t c)
{
    if (x < 0 || x >= fb->width || y < 0 || y >= fb->height) return;
    int byte_idx = y * fb->row_bytes + x / 8;
    uint8_t mask = 0x80 >> (x % 8);

    switch (c) {
    case COLOR_RED:
        if (epd_has_red()) {
            fb->red[byte_idx] |= mask;
            fb->black[byte_idx] &= ~mask;
            return;
        }
        /* BW panels: red maps to black */
        __attribute__((fallthrough));
    case COLOR_BLACK:
        fb->black[byte_idx] &= ~mask;
        break;
    default:
        fb->black[byte_idx] |= mask;
        fb->red[byte_idx] &= ~mask;
        break;
    }
}

void fb_hline(fb_t *fb, int x, int y, int w, fb_color_t c)
{
    for (int i = 0; i < w; i++) fb_pixel(fb, x + i, y, c);
}

void fb_vline(fb_t *fb, int x, int y, int h, fb_color_t c)
{
    for (int i = 0; i < h; i++) fb_pixel(fb, x, y + i, c);
}

void fb_rect(fb_t *fb, int x, int y, int w, int h, fb_color_t c)
{
    fb_hline(fb, x, y, w, c);
    fb_hline(fb, x, y + h - 1, w, c);
    fb_vline(fb, x, y, h, c);
    fb_vline(fb, x + w - 1, y, h, c);
}

void fb_fill_rect(fb_t *fb, int x, int y, int w, int h, fb_color_t c)
{
    for (int j = 0; j < h; j++)
        fb_hline(fb, x, y + j, w, c);
}

void fb_bitmap(fb_t *fb, int x, int y, int w, int h,
               const uint8_t *data, fb_color_t c)
{
    int stride = (w + 7) / 8;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            if (data[j * stride + i / 8] & (0x80 >> (i % 8)))
                fb_pixel(fb, x + i, y + j, c);
        }
    }
}

/* ── text ─────────────────────────────────────────────────────────── */

int fb_char(fb_t *fb, int x, int y, char ch, fb_color_t c)
{
    int idx = (unsigned char)ch - 32;
    if (idx < 0 || idx >= 95) idx = 0;
    fb_bitmap(fb, x, y, 8, 16, font_ascii[idx], c);
    return 8;
}

int fb_string(fb_t *fb, int x, int y, const char *s, fb_color_t c)
{
    int cx = x;
    while (*s) {
        fb_char(fb, cx, y, *s, c);
        cx += 8;
        s++;
    }
    return cx - x;
}

static const uint8_t *find_zh_glyph(uint32_t cp)
{
    for (int i = 0; i < FONT_ZH_COUNT; i++) {
        if (font_zh[i].cp == cp) return font_zh[i].bmp;
    }
    return NULL;
}

static int decode_utf8(const char **pp)
{
    const uint8_t *p = (const uint8_t *)*pp;
    int cp;
    if (p[0] < 0x80) {
        cp = p[0]; *pp += 1;
    } else if ((p[0] & 0xE0) == 0xC0) {
        cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        *pp += 2;
    } else if ((p[0] & 0xF0) == 0xE0) {
        cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        *pp += 3;
    } else if ((p[0] & 0xF8) == 0xF0) {
        cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) |
             ((p[2] & 0x3F) << 6)  | (p[3] & 0x3F);
        *pp += 4;
    } else {
        *pp += 1;
        return '?';
    }
    return cp;
}

int fb_utf8(fb_t *fb, int x, int y, const char *s, fb_color_t c)
{
    int cx = x;
    while (*s) {
        int cp = decode_utf8(&s);
        if (cp < 0x80) {
            fb_char(fb, cx, y, (char)cp, c);
            cx += 8;
        } else {
            const uint8_t *glyph = find_zh_glyph((uint32_t)cp);
            if (glyph)
                fb_bitmap(fb, cx, y, 16, 16, glyph, c);
            cx += 16;
        }
    }
    return cx - x;
}

/* ── scaled text (any integer scale 1..N) ────────────────────────── */

static void fb_bitmap_sc(fb_t *fb, int x, int y, int w, int h,
                          const uint8_t *data, fb_color_t c, int sc)
{
    int stride = (w + 7) / 8;
    int bold = (sc >= 2) ? 1 : 0;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            if (data[j * stride + i / 8] & (0x80 >> (i % 8))) {
                int px = x + i * sc;
                int py = y + j * sc;
                for (int dy = 0; dy < sc + bold; dy++)
                    for (int dx = 0; dx < sc + bold; dx++)
                        fb_pixel(fb, px + dx, py + dy, c);
            }
        }
    }
}

int fb_char_2x(fb_t *fb, int x, int y, char ch, fb_color_t c)
{
    int idx = (unsigned char)ch - 32;
    if (idx < 0 || idx >= 95) idx = 0;
    fb_bitmap_sc(fb, x, y, 8, 16, font_ascii[idx], c, 2);
    return 16;
}

int fb_utf8_2x(fb_t *fb, int x, int y, const char *s, fb_color_t c)
{
    return fb_utf8_scaled(fb, x, y, s, c, 2);
}

int fb_utf8_scaled(fb_t *fb, int x, int y, const char *s,
                   fb_color_t c, int scale)
{
    if (scale < 1) scale = 1;
    int cx = x;
    while (*s) {
        int cp = decode_utf8(&s);
        if (cp < 0x80) {
            int idx = cp - 32;
            if (idx < 0 || idx >= 95) idx = 0;
            fb_bitmap_sc(fb, cx, y, 8, 16, font_ascii[idx], c, scale);
            cx += 8 * scale;
        } else {
            const uint8_t *glyph = find_zh_glyph((uint32_t)cp);
            if (glyph)
                fb_bitmap_sc(fb, cx, y, 16, 16, glyph, c, scale);
            cx += 16 * scale;
        }
    }
    return cx - x;
}

int fb_utf8_scaled_maxw(fb_t *fb, int x, int y, const char *s,
                        fb_color_t c, int scale, int max_w)
{
    if (scale < 1) scale = 1;
    if (max_w <= 0) return 0;
    int cx = x;
    const int lim = x + max_w;
    while (*s && cx < lim) {
        const char *before = s;
        int cp = decode_utf8(&s);
        int adv = (cp < 0x80) ? (8 * scale) : (16 * scale);
        if (cx + adv > lim) {
            s = before;
            break;
        }
        if (cp < 0x80) {
            int idx = cp - 32;
            if (idx < 0 || idx >= 95) idx = 0;
            fb_bitmap_sc(fb, cx, y, 8, 16, font_ascii[idx], c, scale);
        } else {
            const uint8_t *glyph = find_zh_glyph((uint32_t)cp);
            if (glyph)
                fb_bitmap_sc(fb, cx, y, 16, 16, glyph, c, scale);
        }
        cx += adv;
    }
    return cx - x;
}

/* ── export ───────────────────────────────────────────────────────── */

esp_err_t fb_export(const fb_t *fb, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot create %s", path);
        return ESP_FAIL;
    }
    bool ok = (fwrite(fb->black, 1, fb->plane_bytes, f) == fb->plane_bytes);
    if (ok && epd_has_red())
        ok = (fwrite(fb->red, 1, fb->plane_bytes, f) == fb->plane_bytes);
    fclose(f);
    if (!ok) {
        ESP_LOGE(TAG, "fb_export write failed (disk full?)");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t fb_import(fb_t *fb, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return ESP_FAIL;
    }
    bool ok = (fread(fb->black, 1, fb->plane_bytes, f) == fb->plane_bytes);
    if (ok && epd_has_red())
        ok = (fread(fb->red, 1, fb->plane_bytes, f) == fb->plane_bytes);
    fclose(f);
    if (!ok) {
        ESP_LOGE(TAG, "fb_import read failed (file truncated?)");
        return ESP_FAIL;
    }
    return ESP_OK;
}
