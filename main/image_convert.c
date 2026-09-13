#include "image_convert.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "rom/tjpgd.h"
#include "lodepng.h"
#include "epd.h"

static const char *TAG = "img";

#define JPEG_FS_BAND_ROWS       16
#define PNG_MAX_FILE_BYTES      (1024u * 1024u)
#define PNG_MAX_DECODE_PIXELS   (1000u * 1000u)
#define PNG_MAX_SIDE_PIXELS     4096u
#define RAW_WRITE_CHUNK         16384u

typedef struct {
    int x;
    int y;
    int w;
    int h;
} image_fit_rect_t;

static image_fit_rect_t image_fit_contain_rect(int src_w, int src_h,
                                               int dst_w, int dst_h)
{
    image_fit_rect_t r = { 0, 0, dst_w, dst_h };
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return r;

    if ((int64_t)dst_w * src_h <= (int64_t)dst_h * src_w) {
        r.w = dst_w;
        r.h = (int)(((int64_t)dst_w * src_h + src_w / 2) / src_w);
    } else {
        r.h = dst_h;
        r.w = (int)(((int64_t)dst_h * src_w + src_h / 2) / src_h);
    }

    if (r.w < 1) r.w = 1;
    if (r.h < 1) r.h = 1;
    if (r.w > dst_w) r.w = dst_w;
    if (r.h > dst_h) r.h = dst_h;
    r.x = (dst_w - r.w) / 2;
    r.y = (dst_h - r.h) / 2;
    return r;
}

static void log_heap_state(const char *label)
{
    ESP_LOGI(TAG, "%s heap: free8=%lu largest8=%lu internal=%lu/%lu psram=%lu/%lu",
             label,
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static void *img_alloc(size_t size, const char *label)
{
    void *p = NULL;

#ifdef CONFIG_SPIRAM
    if (size >= 4096) {
        p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p)
            return p;
        ESP_LOGW(TAG, "%s PSRAM alloc(%u) failed", label, (unsigned)size);
    }
#endif

    p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    if (!p) {
        ESP_LOGE(TAG, "%s alloc(%u) failed", label, (unsigned)size);
        log_heap_state("alloc failed");
    }
    return p;
}

static void *img_calloc(size_t count, size_t size, const char *label)
{
    if (size != 0 && count > SIZE_MAX / size) {
        ESP_LOGE(TAG, "%s calloc overflow", label);
        return NULL;
    }

    size_t total = count * size;
    void *p = img_alloc(total, label);
    if (p)
        memset(p, 0, total);
    return p;
}

static bool write_all_chunked(FILE *f, const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        size_t n = len - off;
        if (n > RAW_WRITE_CHUNK)
            n = RAW_WRITE_CHUNK;
        if (fwrite(data + off, 1, n, f) != n)
            return false;
        off += n;
    }
    return true;
}

static bool write_raw_planes(const char *out_path, const uint8_t *black,
                             const uint8_t *red, const uint8_t *yellow,
                             const uint8_t *blue, const uint8_t *green,
                             int plane_bytes, bool has_red, bool has_yellow,
                             bool has_blue, bool has_green)
{
    int64_t t0 = esp_timer_get_time();
    FILE *f = fopen(out_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "open out failed: %s", out_path);
        return false;
    }

    bool ok = write_all_chunked(f, black, (size_t)plane_bytes);
    if (ok && has_red)
        ok = write_all_chunked(f, red, (size_t)plane_bytes);
    if (ok && has_yellow)
        ok = write_all_chunked(f, yellow, (size_t)plane_bytes);
    if (ok && has_blue)
        ok = write_all_chunked(f, blue, (size_t)plane_bytes);
    if (ok && has_green)
        ok = write_all_chunked(f, green, (size_t)plane_bytes);
    if (fclose(f) != 0)
        ok = false;

    if (!ok) {
        ESP_LOGE(TAG, "write raw failed: %s", out_path);
        remove(out_path);
    } else {
        int planes = 1 + (has_red ? 1 : 0) + (has_yellow ? 1 : 0) +
                     (has_blue ? 1 : 0) + (has_green ? 1 : 0);
        ESP_LOGI(TAG, "raw write done in %lld ms: %s (%d planes, %u bytes/plane)",
                 (long long)((esp_timer_get_time() - t0) / 1000LL),
                 out_path, planes, (unsigned)plane_bytes);
    }
    return ok;
}

static void flip_plane_rows(uint8_t *plane, int row_bytes, int height)
{
    if (!plane || row_bytes <= 0 || height <= 1)
        return;
    uint8_t *tmp = (uint8_t *)img_alloc((size_t)row_bytes, "row flip temp");
    if (!tmp) {
        ESP_LOGW(TAG, "row flip skipped: temp alloc failed");
        return;
    }
    for (int y = 0; y < height / 2; y++) {
        uint8_t *top = plane + (size_t)y * (size_t)row_bytes;
        uint8_t *bot = plane + (size_t)(height - 1 - y) * (size_t)row_bytes;
        memcpy(tmp, top, (size_t)row_bytes);
        memcpy(top, bot, (size_t)row_bytes);
        memcpy(bot, tmp, (size_t)row_bytes);
    }
    free(tmp);
}

static void compensate_panel_image_orientation(uint8_t *black, uint8_t *red,
                                               uint8_t *yellow,
                                               uint8_t *blue, uint8_t *green,
                                               int plane_bytes,
                                               bool has_red, bool has_yellow,
                                               bool has_blue, bool has_green)
{
    if (epd_get_panel() != EPD_PANEL_HINK_SSD1619_29_BWR)
        return;

    int w = epd_width();
    int h = epd_height();
    int row_bytes = w / 8;
    if (w <= 0 || h <= 0 || row_bytes <= 0 || plane_bytes != row_bytes * h)
        return;

    flip_plane_rows(black, row_bytes, h);
    if (has_red)
        flip_plane_rows(red, row_bytes, h);
    if (has_yellow)
        flip_plane_rows(yellow, row_bytes, h);
    if (has_blue)
        flip_plane_rows(blue, row_bytes, h);
    if (has_green)
        flip_plane_rows(green, row_bytes, h);
    ESP_LOGI(TAG, "HINK 2.9 image orientation compensation: flip Y");
}

/* ── Bayer 8x8 matrix (fallback when F-S allocation fails) ────────── */

static const uint8_t bayer8[8][8] = {
    {   0, 192,  48, 240,  12, 204,  60, 252 },
    { 128,  64, 176, 112, 140,  76, 188, 124 },
    {  32, 224,  16, 208,  44, 236,  28, 220 },
    { 160,  96, 144,  80, 172, 108, 156,  92 },
    {   8, 200,  56, 248,   4, 196,  52, 244 },
    { 136,  72, 184, 120, 132,  68, 180, 116 },
    {  40, 232,  24, 216,  36, 228,  20, 212 },
    { 168, 104, 152,  84, 164, 100, 148,  88 },
};

#define BW_LUMA_THRESHOLD_DEFAULT 96
#define IMAGE_QUANT_GAIN_ONE_Q8   256

typedef struct {
    uint8_t bw_luma_threshold;
    uint16_t fs_error_gain_q8;
    uint16_t red_gain_q8;
    uint16_t yellow_gain_q8;
    uint8_t neutral_chroma_threshold; /* Zero disables the neutral-tone path. */
    uint8_t neutral_luma_threshold;
    uint16_t neutral_fs_error_gain_q8;
} image_quant_profile_t;

static const image_quant_profile_t s_quant_default = {
    .bw_luma_threshold = BW_LUMA_THRESHOLD_DEFAULT,
    .fs_error_gain_q8 = IMAGE_QUANT_GAIN_ONE_Q8,
    .red_gain_q8 = IMAGE_QUANT_GAIN_ONE_Q8,
    .yellow_gain_q8 = IMAGE_QUANT_GAIN_ONE_Q8,
};

static const image_quant_profile_t s_quant_jd79665_75 = {
    .bw_luma_threshold = 104,
    .fs_error_gain_q8 = 192, /* 75%: preserve edges and reduce coarse grain. */
    .red_gain_q8 = 280,      /* 1.09x */
    .yellow_gain_q8 = 288,   /* 1.13x */
    .neutral_chroma_threshold = 20,
    .neutral_luma_threshold = 128,
    .neutral_fs_error_gain_q8 = IMAGE_QUANT_GAIN_ONE_Q8,
};

static const image_quant_profile_t s_quant_jd79668_42 = {
    .bw_luma_threshold = 104,
    .fs_error_gain_q8 = IMAGE_QUANT_GAIN_ONE_Q8,
    .red_gain_q8 = IMAGE_QUANT_GAIN_ONE_Q8,
    .yellow_gain_q8 = IMAGE_QUANT_GAIN_ONE_Q8,
    .neutral_chroma_threshold = 20,
    .neutral_luma_threshold = 128,
    .neutral_fs_error_gain_q8 = IMAGE_QUANT_GAIN_ONE_Q8,
};

static const image_quant_profile_t *image_quant_profile_current(void)
{
    switch (epd_get_panel()) {
    case EPD_PANEL_REF_JD79665_75_BWRY: return &s_quant_jd79665_75;
    case EPD_PANEL_REF_JD79668_42_BWRY: return &s_quant_jd79668_42;
    default: return &s_quant_default;
    }
}

static bool image_quant_source_is_neutral(uint8_t r, uint8_t g, uint8_t b,
                                           const image_quant_profile_t *quant)
{
    int max_c = r; if (g > max_c) max_c = g; if (b > max_c) max_c = b;
    int min_c = r; if (g < min_c) min_c = g; if (b < min_c) min_c = b;
    return quant->neutral_chroma_threshold != 0 &&
           max_c - min_c <= quant->neutral_chroma_threshold;
}

static int image_quant_apply_gain(int strength, uint16_t gain_q8)
{
    int adjusted = (strength * (int)gain_q8 + 128) / 256;
    return adjusted > 255 ? 255 : adjusted;
}

static void image_quant_log_profile(const char *format,
                                    const image_quant_profile_t *profile)
{
    const char *panel_name;
    if (profile == &s_quant_jd79665_75)
        panel_name = "JD79665 ID34 7.5";
    else if (profile == &s_quant_jd79668_42)
        panel_name = "JD79668 ID37 4.2";
    else
        return;
    ESP_LOGI(TAG,
             "%s image profile for %s: chromatic bw=%u fs=%u/256 red=%u/256 yellow=%u/256; "
             "simulated gray (B/W dots, not native grayscale): source chroma<=%u neutral bw=%u "
             "serpentine fs=%u/256, exact B/W preserved; neutral Bayer unbiased",
             panel_name, format, (unsigned)profile->bw_luma_threshold,
             (unsigned)profile->fs_error_gain_q8,
             (unsigned)profile->red_gain_q8,
             (unsigned)profile->yellow_gain_q8,
             (unsigned)profile->neutral_chroma_threshold,
             (unsigned)profile->neutral_luma_threshold,
             (unsigned)profile->neutral_fs_error_gain_q8);
}

static bool red_orange_pixel(uint8_t r, uint8_t g, uint8_t b)
{
    int ri = (int)r, gi = (int)g, bi = (int)b;
    int max_c = ri;
    if (gi > max_c) max_c = gi;
    if (bi > max_c) max_c = bi;
    int min_c = ri;
    if (gi < min_c) min_c = gi;
    if (bi < min_c) min_c = bi;
    int chroma = max_c - min_c;
    if (max_c < 55 || chroma < 22)
        return false;

    int hue = 0;
    if (max_c == ri)
        hue = ((gi - bi) * 60) / chroma;
    else if (max_c == gi)
        hue = 120 + ((bi - ri) * 60) / chroma;
    else
        hue = 240 + ((ri - gi) * 60) / chroma;
    if (hue < 0)
        hue += 360;

    int sat256 = (chroma * 256) / max_c;
    int lum = (ri * 77 + gi * 150 + bi * 29) >> 8;
    bool red_hue = (hue <= 55 || hue >= 305);
    bool orange_hue = (hue <= 72 && ri >= gi + 24 && ri >= bi + 34 &&
                       gi * 100 <= ri * 78 && bi * 100 <= ri * 74);
    bool red_dominant = (ri >= gi + 18) && (ri >= bi + 18);

    return (red_hue || orange_hue) && red_dominant &&
           sat256 >= 55 && lum >= 35 && lum <= 225;
}

static uint8_t red_ink_strength(uint8_t r, uint8_t g, uint8_t b)
{
    int ri = (int)r, gi = (int)g, bi = (int)b;
    int max_c = ri;
    if (gi > max_c) max_c = gi;
    if (bi > max_c) max_c = bi;
    int min_c = ri;
    if (gi < min_c) min_c = gi;
    if (bi < min_c) min_c = bi;
    int chroma = max_c - min_c;
    if (max_c < 55 || chroma < 22 || ri < gi + 14 || ri < bi + 18)
        return 0;

    int hue = 0;
    if (max_c == ri)
        hue = ((gi - bi) * 60) / chroma;
    else if (max_c == gi)
        hue = 120 + ((bi - ri) * 60) / chroma;
    else
        hue = 240 + ((ri - gi) * 60) / chroma;
    if (hue < 0)
        hue += 360;
    if (!(hue <= 72 || hue >= 305))
        return 0;

    int sat256 = (chroma * 256) / max_c;
    int lum = (ri * 77 + gi * 150 + bi * 29) >> 8;
    int dom = ri - (gi > bi ? gi : bi);
    int strength = sat256 + dom * 3 / 2 + lum - 210;

    if (hue > 45 && hue < 90)
        strength -= (hue - 45) * 4;
    if (lum < 70)
        strength -= (70 - lum);
    if (lum > 205)
        strength -= (lum - 205) * 2;

    if (strength < 0)
        return 0;
    if (strength > 245)
        return 245;
    return (uint8_t)strength;
}

static uint8_t yellow_ink_strength(uint8_t r, uint8_t g, uint8_t b)
{
    int ri = (int)r, gi = (int)g, bi = (int)b;
    int max_c = ri;
    if (gi > max_c) max_c = gi;
    if (bi > max_c) max_c = bi;
    int min_c = ri;
    if (gi < min_c) min_c = gi;
    if (bi < min_c) min_c = bi;
    int chroma = max_c - min_c;
    if (max_c < 70 || chroma < 28)
        return 0;

    int hue = 0;
    if (max_c == ri)
        hue = ((gi - bi) * 60) / chroma;
    else if (max_c == gi)
        hue = 120 + ((bi - ri) * 60) / chroma;
    else
        hue = 240 + ((ri - gi) * 60) / chroma;
    if (hue < 0)
        hue += 360;

    bool yellow_hue = (hue >= 38 && hue <= 92);
    bool yellow_channels = ri >= 95 && gi >= 85 &&
                           bi + 24 <= ri && bi + 18 <= gi &&
                           ri + 80 >= gi && gi + 80 >= ri;
    if (!yellow_hue || !yellow_channels)
        return 0;

    int sat256 = (chroma * 256) / max_c;
    int lum = (ri * 77 + gi * 150 + bi * 29) >> 8;
    int yellow_base = ((ri < gi) ? ri : gi) - bi;
    int strength = sat256 + yellow_base + lum - 245;
    if (hue > 78)
        strength -= (hue - 78) * 3; /* avoid green drifting into yellow */
    if (hue < 45)
        strength -= (45 - hue) * 2; /* leave orange/red-orange to red plane */
    if (lum < 80)
        strength -= (80 - lum);

    if (strength < 0)
        return 0;
    if (strength > 245)
        return 245;
    return (uint8_t)strength;
}

/* ── Bayer dithered pixel write (fallback) ────────────────────────── */

static void e6_37_nearest_pixel(uint8_t *black, uint8_t *red,
                                uint8_t *yellow, uint8_t *blue,
                                uint8_t *green, int x, int y, int w,
                                uint8_t r, uint8_t g, uint8_t b);

static inline void dither_pixel_bayer(uint8_t *black, uint8_t *red,
                                      uint8_t *yellow, uint8_t *blue,
                                      uint8_t *green,
                                      int x, int y, int w,
                                      uint8_t r, uint8_t g, uint8_t b,
                                      bool has_red, bool has_yellow,
                                      bool has_blue, bool has_green,
                                      const image_quant_profile_t *profile)
{
    if (has_blue && has_green) {
        e6_37_nearest_pixel(black, red, yellow, blue, green,
                            x, y, w, r, g, b);
        return;
    }

    size_t bp = (size_t)y * (w / 8) + (size_t)(x >> 3);
    uint8_t bit = (uint8_t)(0x80u >> (x & 7));
    uint8_t threshold = bayer8[y & 7][x & 7];

    int ri = r, gi = g, bi = b;
    int max_c = ri; if (gi > max_c) max_c = gi; if (bi > max_c) max_c = bi;
    int min_c = ri; if (gi < min_c) min_c = gi; if (bi < min_c) min_c = bi;
    int chroma = max_c - min_c;
    uint8_t lum = (uint8_t)((ri * 77u + gi * 150u + bi * 29u) >> 8);

    const image_quant_profile_t *quant = profile ? profile : &s_quant_default;
    if (quant->neutral_chroma_threshold) {
        black[bp] |= bit;
        if (has_red) red[bp] &= (uint8_t)~bit;
        if (has_yellow) yellow[bp] &= (uint8_t)~bit;
        if (image_quant_source_is_neutral(r, g, b, quant)) {
            /* Center each Bayer bin; no legacy -40 or chromatic BW bias. */
            if (lum < (int)threshold + 2)
                black[bp] &= (uint8_t)~bit;
            return;
        }
    }
    int yellow_strength = has_yellow
                        ? image_quant_apply_gain(yellow_ink_strength(r, g, b),
                                                 quant->yellow_gain_q8)
                        : 0;
    if (yellow_strength > (int)threshold) {
        yellow[bp] |= bit;
        black[bp] |= bit;
        return;
    }

    int red_strength = has_red
                     ? image_quant_apply_gain(red_ink_strength(r, g, b),
                                              quant->red_gain_q8)
                     : 0;
    if (red_strength > (int)threshold) {
        red[bp] |= bit;
        black[bp] |= bit;
        return;
    }

    if (chroma >= 30 && max_c >= 40) {
        int hue6 = 0;
        if (max_c == ri)      hue6 = ((gi - bi) * 60) / chroma;
        else if (max_c == gi) hue6 = 120 + ((bi - ri) * 60) / chroma;
        else                  hue6 = 240 + ((ri - gi) * 60) / chroma;
        if (hue6 < 0) hue6 += 360;
        int sat256 = (chroma * 256) / max_c;
        if ((hue6 <= 30 || hue6 >= 330) && sat256 > 80) {
            int red_str = (sat256 * max_c) >> 8;
            if (has_red && red_str > (int)threshold) {
                red[bp] |= bit;
                black[bp] |= bit;
            } else if (lum < threshold) {
                black[bp] &= ~bit;
            }
            return;
        }
    }

    int bw_threshold = (threshold > 40) ? ((int)threshold - 40) : 0;
    bw_threshold += (int)quant->bw_luma_threshold - BW_LUMA_THRESHOLD_DEFAULT;
    if (bw_threshold > 255) bw_threshold = 255;
    if ((int)lum < bw_threshold)
        black[bp] &= ~bit;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Red-first dual-plane dithering.
 *
 *  Common 3-color e-paper examples treat black and red as two independent
 *  1-bit planes.  Classify red/orange/brown pixels before black-white
 *  dithering so dark red backgrounds do not collapse into black.
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct { int16_t r, g, b; } fs_err_t;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} e6_rgb_t;

static bool image_panel_is_e6_6c(void)
{
    return epd_get_panel() == EPD_PANEL_E6_37_6C;
}

/* Same measured palette as refdoc/E6_3inch7_wifi_Display:
 * 0 black, 1 white, 2 yellow, 3 red, 4 blue, 5 green.
 */
static const e6_rgb_t e6_37_fs_palette[6] = {
    {   2,   2,   2 },
    { 190, 200, 200 },
    { 205, 202,   0 },
    { 135,  19,   0 },
    {   5,  64, 158 },
    {  39, 102,  60 },
};

static uint8_t e6_37_nearest_color(int r, int g, int b)
{
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;

    uint8_t best = 0;
    int best_err = INT32_MAX;
    for (uint8_t i = 0; i < 6; i++) {
        int dr = r - (int)e6_37_fs_palette[i].r;
        int dg = g - (int)e6_37_fs_palette[i].g;
        int db = b - (int)e6_37_fs_palette[i].b;
        int err = dr * dr + dg * dg + db * db;
        if (err < best_err) {
            best_err = err;
            best = i;
        }
    }
    return best;
}

static void e6_37_set_plane_pixel(uint8_t *black, uint8_t *red,
                                  uint8_t *yellow, uint8_t *blue,
                                  uint8_t *green, size_t bp,
                                  uint8_t bit, uint8_t color)
{
    black[bp] |= bit;
    red[bp] &= (uint8_t)~bit;
    yellow[bp] &= (uint8_t)~bit;
    blue[bp] &= (uint8_t)~bit;
    green[bp] &= (uint8_t)~bit;

    switch (color) {
    case 0:
        black[bp] &= (uint8_t)~bit;
        break;
    case 2:
        yellow[bp] |= bit;
        break;
    case 3:
        red[bp] |= bit;
        break;
    case 4:
        blue[bp] |= bit;
        break;
    case 5:
        green[bp] |= bit;
        break;
    case 1:
    default:
        break;
    }
}

static void e6_37_nearest_pixel(uint8_t *black, uint8_t *red,
                                uint8_t *yellow, uint8_t *blue,
                                uint8_t *green, int x, int y, int w,
                                uint8_t r, uint8_t g, uint8_t b)
{
    size_t bp = (size_t)y * (size_t)(w >> 3) + (size_t)(x >> 3);
    uint8_t bit = (uint8_t)(0x80u >> (x & 7));
    e6_37_set_plane_pixel(black, red, yellow, blue, green, bp, bit,
                          e6_37_nearest_color(r, g, b));
}

typedef enum {
    PQ_WHITE = 0,
    PQ_BLACK,
    PQ_RED,
    PQ_YELLOW,
    PQ_BLUE,
    PQ_GREEN,
    PQ_OTHER,
} prequantized_color_t;

#define IMG_MASK_BLACK  (1u << 0)
#define IMG_MASK_WHITE  (1u << 1)
#define IMG_MASK_RED    (1u << 2)
#define IMG_MASK_YELLOW (1u << 3)
#define IMG_MASK_BLUE   (1u << 4)
#define IMG_MASK_GREEN  (1u << 5)

typedef struct {
    uint32_t key;
    uint8_t cls;
    bool valid;
} pq_cache_entry_t;

typedef struct {
    unsigned black;
    unsigned red;
    unsigned yellow;
    unsigned blue;
    unsigned green;
    unsigned white;
} pq_counts_t;

static void rgba_on_white(uint8_t *r, uint8_t *g, uint8_t *b, uint8_t a)
{
    if (a >= 255)
        return;
    *r = (uint8_t)(((unsigned)*r * a + 255u * (255u - a) + 127u) / 255u);
    *g = (uint8_t)(((unsigned)*g * a + 255u * (255u - a) + 127u) / 255u);
    *b = (uint8_t)(((unsigned)*b * a + 255u * (255u - a) + 127u) / 255u);
}

static prequantized_color_t prequantized_pixel_class(uint8_t r, uint8_t g,
                                                     uint8_t b, uint8_t a,
                                                     bool has_red,
                                                     bool has_yellow,
                                                     bool has_blue,
                                                     bool has_green)
{
    rgba_on_white(&r, &g, &b, a);

    if (r >= 235 && g >= 235 && b >= 235)
        return PQ_WHITE;
    if (r <= 36 && g <= 36 && b <= 36)
        return PQ_BLACK;

    bool red_like = r >= 140 && g <= 110 && b <= 110 &&
                    r >= g + 45 && r >= b + 45;
    if (red_like)
        return has_red ? PQ_RED : PQ_BLACK;

    bool yellow_like = r >= 150 && g >= 120 && b <= 110 &&
                       r >= b + 45 && g >= b + 35;
    if (yellow_like)
        return has_yellow ? PQ_YELLOW : PQ_BLACK;

    bool blue_like = b >= 120 && r <= 110 && g <= 150 &&
                     b >= r + 45 && b >= g + 25;
    if (blue_like)
        return has_blue ? PQ_BLUE : PQ_BLACK;

    bool green_like = g >= 105 && r <= 130 && b <= 150 &&
                      g >= r + 30 && g >= b + 18;
    if (green_like)
        return has_green ? PQ_GREEN : PQ_BLACK;

    return PQ_OTHER;
}

static bool bw_or_color_quantized_pixel(uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                                        bool has_red, bool has_yellow,
                                        bool has_blue, bool has_green,
                                        prequantized_color_t *out)
{
    const image_quant_profile_t *quant = image_quant_profile_current();
    if (quant->neutral_chroma_threshold) {
        uint8_t sr = r, sg = g, sb = b;
        rgba_on_white(&sr, &sg, &sb, a);
        /* Preserve encoded B/W dots, but do not mistake gray tones for dots. */
        if (image_quant_source_is_neutral(sr, sg, sb, quant) &&
            !((sr == 0 && sg == 0 && sb == 0) ||
              (sr == 255 && sg == 255 && sb == 255)))
            return false;
    }
    prequantized_color_t cls = prequantized_pixel_class(r, g, b, a,
                                                        has_red, has_yellow,
                                                        has_blue, has_green);
    if (cls != PQ_OTHER) {
        if (out)
            *out = cls;
        return true;
    }

    rgba_on_white(&r, &g, &b, a);
    int max_c = r;
    if (g > max_c) max_c = g;
    if (b > max_c) max_c = b;
    int min_c = r;
    if (g < min_c) min_c = g;
    if (b < min_c) min_c = b;
    if (max_c - min_c > 12)
        return false;

    int lum = ((int)r * 77 + (int)g * 150 + (int)b * 29) >> 8;
    if (lum <= 72) {
        if (out)
            *out = PQ_BLACK;
        return true;
    }
    if (lum >= 183) {
        if (out)
            *out = PQ_WHITE;
        return true;
    }
    return false;
}

static bool bw_or_color_quantized_pixel_cached(uint8_t r, uint8_t g,
                                               uint8_t b, uint8_t a,
                                               bool has_red,
                                               bool has_yellow,
                                               bool has_blue,
                                               bool has_green,
                                               pq_cache_entry_t *cache,
                                               prequantized_color_t *out)
{
    uint32_t key = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                   ((uint32_t)g << 8) | (uint32_t)b;
    unsigned idx = ((key * 2654435761u) >> 25) & 0x7Fu;
    pq_cache_entry_t *e = &cache[idx];
    if (e->valid && e->key == key) {
        *out = (prequantized_color_t)e->cls;
        return e->cls != PQ_OTHER;
    }

    prequantized_color_t cls = PQ_OTHER;
    bool ok = bw_or_color_quantized_pixel(r, g, b, a, has_red, has_yellow,
                                          has_blue, has_green, &cls);
    e->valid = true;
    e->key = key;
    e->cls = ok ? (uint8_t)cls : (uint8_t)PQ_OTHER;
    if (ok)
        *out = cls;
    return ok;
}

static uint32_t image_panel_color_mask(void)
{
    uint32_t mask = IMG_MASK_BLACK | IMG_MASK_WHITE;
    if (epd_has_red())
        mask |= IMG_MASK_RED;
    if (epd_has_yellow())
        mask |= IMG_MASK_YELLOW;
    if (image_panel_is_e6_6c())
        mask |= IMG_MASK_RED | IMG_MASK_YELLOW | IMG_MASK_BLUE | IMG_MASK_GREEN;
    return mask;
}

static int image_color_mask_count(uint32_t mask)
{
    int n = 0;
    while (mask) {
        n += (int)(mask & 1u);
        mask >>= 1;
    }
    return n;
}

static uint32_t image_color_class_mask(prequantized_color_t cls)
{
    switch (cls) {
    case PQ_BLACK:  return IMG_MASK_BLACK;
    case PQ_WHITE:  return IMG_MASK_WHITE;
    case PQ_RED:    return IMG_MASK_RED;
    case PQ_YELLOW: return IMG_MASK_YELLOW;
    case PQ_BLUE:   return IMG_MASK_BLUE;
    case PQ_GREEN:  return IMG_MASK_GREEN;
    case PQ_OTHER:
    default:
        return 0;
    }
}

esp_err_t image_file_check_panel_compat(const char *input_path,
                                        image_panel_compat_t *out)
{
    if (!input_path || !out)
        return ESP_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));
    out->panel_mask = image_panel_color_mask();
    out->panel_colors = image_color_mask_count(out->panel_mask);
    out->compatible = true;

    uint8_t magic[4] = {0};
    FILE *mf = fopen(input_path, "rb");
    if (!mf)
        return ESP_FAIL;
    size_t mr = fread(magic, 1, sizeof(magic), mf);
    fclose(mf);

    if (mr < 4 || magic[0] != 0x89 || magic[1] != 0x50 ||
        magic[2] != 0x4E || magic[3] != 0x47) {
        return ESP_OK;
    }

    struct stat st;
    if (stat(input_path, &st) != 0 || st.st_size <= 0)
        return ESP_FAIL;
    if (st.st_size > PNG_MAX_FILE_BYTES) {
        ESP_LOGI(TAG, "compat check skipped for large PNG: %s (%ld bytes)",
                 input_path, (long)st.st_size);
        return ESP_OK;
    }

    uint8_t *buf = (uint8_t *)img_alloc((size_t)st.st_size, "compat png buffer");
    if (!buf)
        return ESP_ERR_NO_MEM;

    FILE *pf = fopen(input_path, "rb");
    if (!pf) {
        free(buf);
        return ESP_FAIL;
    }
    size_t got = fread(buf, 1, (size_t)st.st_size, pf);
    fclose(pf);
    if (got != (size_t)st.st_size) {
        free(buf);
        return ESP_FAIL;
    }

    unsigned char *rgba = NULL;
    unsigned pw = 0, ph = 0;
    unsigned perr = lodepng_decode32(&rgba, &pw, &ph, buf, got);
    free(buf);
    if (perr || !rgba) {
        ESP_LOGW(TAG, "compat PNG decode error %u: %s",
                 perr, lodepng_error_text(perr));
        free(rgba);
        return ESP_FAIL;
    }

    uint64_t total64 = (uint64_t)pw * (uint64_t)ph;
    if (pw == 0 || ph == 0 || total64 > PNG_MAX_DECODE_PIXELS) {
        free(rgba);
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t image_mask = 0;
    unsigned total = (unsigned)total64;
    unsigned other = 0;
    unsigned other_limit = total / 1000u;
    if (other_limit < 16u)
        other_limit = 16u;

    for (unsigned i = 0; i < total; i++) {
        const uint8_t *p = rgba + (size_t)i * 4u;
        prequantized_color_t cls = prequantized_pixel_class(p[0], p[1], p[2], p[3],
                                                            true, true, true, true);
        uint32_t bit = image_color_class_mask(cls);
        if (!bit) {
            other++;
            if (other > other_limit) {
                free(rgba);
                ESP_LOGI(TAG,
                         "compat check unknown PNG palette: %s (%ux%u, other>%u)",
                         input_path, pw, ph, other_limit);
                return ESP_OK;
            }
            continue;
        }
        image_mask |= bit;
    }

    free(rgba);
    if (!image_mask)
        image_mask = IMG_MASK_WHITE;

    out->known = true;
    out->image_mask = image_mask;
    out->image_colors = image_color_mask_count(image_mask);
    out->compatible = (image_mask & ~out->panel_mask) == 0;

    ESP_LOGI(TAG,
             "compat PNG palette: %s colors=%d mask=0x%02lx, panel_colors=%d mask=0x%02lx, ok=%d",
             input_path, out->image_colors, (unsigned long)out->image_mask,
             out->panel_colors, (unsigned long)out->panel_mask,
             out->compatible ? 1 : 0);
    return ESP_OK;
}

static esp_err_t png_pack_prequantized_planes(const unsigned char *rgba,
                                              int EW, int EH, int RB,
                                              bool has_red, bool has_yellow,
                                              bool has_blue, bool has_green,
                                              uint8_t *black, uint8_t *red,
                                              uint8_t *yellow, uint8_t *blue,
                                              uint8_t *green,
                                              pq_counts_t *counts)
{
    if (!rgba || !black || (has_red && !red) || (has_yellow && !yellow) ||
        (has_blue && !blue) || (has_green && !green) || !counts)
        return ESP_ERR_INVALID_ARG;

    memset(counts, 0, sizeof(*counts));
    pq_cache_entry_t cache[128] = { 0 };

    for (int y = 0; y < EH; y++) {
        const uint8_t *src = rgba + (size_t)y * (size_t)EW * 4u;
        size_t row = (size_t)y * (size_t)RB;
        for (int bx = 0; bx < RB; bx++) {
            uint8_t black_byte = 0xFF;
            uint8_t red_byte = 0x00;
            uint8_t yellow_byte = 0x00;
            uint8_t blue_byte = 0x00;
            uint8_t green_byte = 0x00;

            for (int bi = 0; bi < 8; bi++, src += 4) {
                uint8_t bit = (uint8_t)(0x80u >> bi);
                prequantized_color_t cls = PQ_WHITE;
                if (!bw_or_color_quantized_pixel_cached(src[0], src[1], src[2], src[3],
                                                        has_red, has_yellow,
                                                        has_blue, has_green,
                                                        cache, &cls)) {
                    return ESP_ERR_NOT_SUPPORTED;
                }
                switch (cls) {
                case PQ_WHITE:
                    counts->white++;
                    break;
                case PQ_BLACK:
                    black_byte &= (uint8_t)~bit;
                    counts->black++;
                    break;
                case PQ_RED:
                    red_byte |= bit;
                    counts->red++;
                    break;
                case PQ_YELLOW:
                    yellow_byte |= bit;
                    counts->yellow++;
                    break;
                case PQ_BLUE:
                    blue_byte |= bit;
                    counts->blue++;
                    break;
                case PQ_GREEN:
                    green_byte |= bit;
                    counts->green++;
                    break;
                case PQ_OTHER:
                default:
                    return ESP_ERR_NOT_SUPPORTED;
                }
            }
            size_t bp = row + (size_t)bx;
            black[bp] = black_byte;
            if (red)
                red[bp] = red_byte;
            if (yellow)
                yellow[bp] = yellow_byte;
            if (blue)
                blue[bp] = blue_byte;
            if (green)
                green[bp] = green_byte;
        }
    }
    return ESP_OK;
}

static esp_err_t png_try_prequantized_passthrough(const unsigned char *rgba,
                                                  unsigned pw, unsigned ph,
                                                  const char *out_path)
{
    const int EW = epd_width();
    const int EH = epd_height();
    const int EC = EW * EH / 8;
    const int RB = EW / 8;
    bool has_red = epd_has_red();
    bool has_yellow = epd_has_yellow();
    bool has_blue = image_panel_is_e6_6c();
    bool has_green = image_panel_is_e6_6c();

    if (!rgba || !out_path || pw != (unsigned)EW || ph != (unsigned)EH)
        return ESP_ERR_NOT_SUPPORTED;

    int64_t t0 = esp_timer_get_time();
    uint8_t *black = (uint8_t *)img_alloc(EC, "png passthrough black");
    uint8_t *red = has_red ? (uint8_t *)img_alloc(EC, "png passthrough red") : NULL;
    uint8_t *yellow = has_yellow ? (uint8_t *)img_alloc(EC, "png passthrough yellow") : NULL;
    uint8_t *blue = has_blue ? (uint8_t *)img_alloc(EC, "png passthrough blue") : NULL;
    uint8_t *green = has_green ? (uint8_t *)img_alloc(EC, "png passthrough green") : NULL;
    if (!black || (has_red && !red) || (has_yellow && !yellow) ||
        (has_blue && !blue) || (has_green && !green)) {
        free(black);
        free(red);
        free(yellow);
        free(blue);
        free(green);
        return ESP_ERR_NO_MEM;
    }

    pq_counts_t counts;
    esp_err_t pack_err = png_pack_prequantized_planes(rgba, EW, EH, RB,
                                                      has_red, has_yellow,
                                                      has_blue, has_green,
                                                      black, red, yellow,
                                                      blue, green, &counts);
    if (pack_err != ESP_OK) {
        free(black);
        free(red);
        free(yellow);
        free(blue);
        free(green);
        return pack_err;
    }
    int64_t t_pack = esp_timer_get_time();
    ESP_LOGI(TAG, "PNG prequantized pack done in %lld ms",
             (long long)((t_pack - t0) / 1000LL));

    compensate_panel_image_orientation(black, red, yellow, blue, green, EC,
                                       has_red, has_yellow, has_blue, has_green);
    if (!write_raw_planes(out_path, black, red, yellow, blue, green, EC,
                          has_red, has_yellow, has_blue, has_green)) {
        free(black);
        free(red);
        free(yellow);
        free(blue);
        free(green);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "PNG prequantized passthrough: %ux%u -> %s (%u bytes/plane, white=%u black=%u red=%u yellow=%u blue=%u green=%u)",
             pw, ph, out_path, (unsigned)EC, counts.white, counts.black,
             counts.red, counts.yellow, counts.blue, counts.green);
    free(black);
    free(red);
    free(yellow);
    free(blue);
    free(green);
    return ESP_OK;
}

static esp_err_t png_try_prequantized_framebuffer(const unsigned char *rgba,
                                                  unsigned pw, unsigned ph,
                                                  fb_t **out_fb)
{
    const int EW = epd_width();
    const int EH = epd_height();
    const int RB = EW / 8;

    if (!rgba || !out_fb || pw != (unsigned)EW || ph != (unsigned)EH)
        return ESP_ERR_NOT_SUPPORTED;

    int64_t t0 = esp_timer_get_time();
    bool has_red = epd_has_red();
    bool has_yellow = epd_has_yellow();
    bool has_blue = image_panel_is_e6_6c();
    bool has_green = image_panel_is_e6_6c();

    fb_t *fb = fb_create();
    if (!fb)
        return ESP_ERR_NO_MEM;
    if ((has_red && !fb->red) || (has_yellow && !fb->yellow) ||
        (has_blue && !fb->blue) || (has_green && !fb->green)) {
        fb_destroy(fb);
        return ESP_ERR_NO_MEM;
    }

    pq_counts_t counts;
    esp_err_t err = png_pack_prequantized_planes(rgba, EW, EH, RB,
                                                 has_red, has_yellow,
                                                 has_blue, has_green,
                                                 fb->black,
                                                 has_red ? fb->red : NULL,
                                                 has_yellow ? fb->yellow : NULL,
                                                 has_blue ? fb->blue : NULL,
                                                 has_green ? fb->green : NULL,
                                                 &counts);
    if (err != ESP_OK) {
        fb_destroy(fb);
        return err;
    }

    ESP_LOGI(TAG,
             "PNG prequantized framebuffer pack done in %lld ms (%ux%u, white=%u black=%u red=%u yellow=%u blue=%u green=%u)",
             (long long)((esp_timer_get_time() - t0) / 1000LL),
             pw, ph, counts.white, counts.black, counts.red, counts.yellow,
             counts.blue, counts.green);
    *out_fb = fb;
    return ESP_OK;
}

static void sample_rgba_on_white(const unsigned char *rgba, unsigned pw,
                                 unsigned ph, int sx, int sy,
                                 uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx >= (int)pw) sx = (int)pw - 1;
    if (sy >= (int)ph) sy = (int)ph - 1;
    const uint8_t *p = rgba + ((size_t)sy * pw + (size_t)sx) * 4u;
    uint8_t a = p[3];
    if (a < 255) {
        *r = (uint8_t)(((unsigned)p[0] * a + 255u * (255u - a) + 127u) / 255u);
        *g = (uint8_t)(((unsigned)p[1] * a + 255u * (255u - a) + 127u) / 255u);
        *b = (uint8_t)(((unsigned)p[2] * a + 255u * (255u - a) + 127u) / 255u);
    } else {
        *r = p[0];
        *g = p[1];
        *b = p[2];
    }
}

static bool png_looks_bw_preprocessed(const unsigned char *rgba,
                                      unsigned pw, unsigned ph)
{
    if (!rgba || pw == 0 || ph == 0)
        return false;

    unsigned total = pw * ph;
    unsigned step = total / 4096u;
    if (step < 1)
        step = 1;

    unsigned samples = 0;
    unsigned bw_like = 0;
    unsigned chroma_like = 0;
    for (unsigned i = 0; i < total; i += step) {
        const uint8_t *p = rgba + (size_t)i * 4u;
        uint8_t r, g, b;
        uint8_t a = p[3];
        if (a < 255) {
            r = (uint8_t)(((unsigned)p[0] * a + 255u * (255u - a) + 127u) / 255u);
            g = (uint8_t)(((unsigned)p[1] * a + 255u * (255u - a) + 127u) / 255u);
            b = (uint8_t)(((unsigned)p[2] * a + 255u * (255u - a) + 127u) / 255u);
        } else {
            r = p[0]; g = p[1]; b = p[2];
        }
        int max_c = r;
        if (g > max_c) max_c = g;
        if (b > max_c) max_c = b;
        int min_c = r;
        if (g < min_c) min_c = g;
        if (b < min_c) min_c = b;
        int lum = ((int)r * 77 + (int)g * 150 + (int)b * 29) >> 8;
        if (max_c - min_c <= 18)
            chroma_like++;
        if (lum <= 48 || lum >= 207)
            bw_like++;
        samples++;
    }

    if (samples == 0)
        return false;
    return chroma_like * 100u >= samples * 96u &&
           bw_like * 100u >= samples * 92u;
}

static esp_err_t png_threshold_bw_passthrough(const unsigned char *rgba,
                                              unsigned pw, unsigned ph,
                                              const char *out_path)
{
    /* Exact prequantized PNGs have already been packed before this heuristic. */
    if (image_quant_profile_current()->neutral_chroma_threshold)
        return ESP_ERR_NOT_SUPPORTED;

    const int EW = epd_width();
    const int EH = epd_height();
    const int EC = EW * EH / 8;
    const int RB = EW / 8;
    bool has_red = epd_has_red();
    bool has_yellow = epd_has_yellow();
    bool has_blue = image_panel_is_e6_6c();
    bool has_green = image_panel_is_e6_6c();

    if (!png_looks_bw_preprocessed(rgba, pw, ph))
        return ESP_ERR_NOT_SUPPORTED;

    uint8_t *black = (uint8_t *)img_alloc(EC, "png bw black");
    uint8_t *red = has_red ? (uint8_t *)img_alloc(EC, "png bw red clear") : NULL;
    uint8_t *yellow = has_yellow ? (uint8_t *)img_alloc(EC, "png bw yellow clear") : NULL;
    uint8_t *blue = has_blue ? (uint8_t *)img_alloc(EC, "png bw blue clear") : NULL;
    uint8_t *green = has_green ? (uint8_t *)img_alloc(EC, "png bw green clear") : NULL;
    if (!black || (has_red && !red) || (has_yellow && !yellow) ||
        (has_blue && !blue) || (has_green && !green)) {
        free(black);
        free(red);
        free(yellow);
        free(blue);
        free(green);
        return ESP_ERR_NO_MEM;
    }

    memset(black, 0xFF, EC);
    if (red)
        memset(red, 0x00, EC);
    if (yellow)
        memset(yellow, 0x00, EC);
    if (blue)
        memset(blue, 0x00, EC);
    if (green)
        memset(green, 0x00, EC);

    image_fit_rect_t fit = image_fit_contain_rect((int)pw, (int)ph, EW, EH);
    for (int ey = fit.y; ey < fit.y + fit.h; ey++) {
        int sy = (ey - fit.y) * (int)ph / fit.h;
        for (int ex = fit.x; ex < fit.x + fit.w; ex++) {
            int sx = (ex - fit.x) * (int)pw / fit.w;
            uint8_t r, g, b;
            sample_rgba_on_white(rgba, pw, ph, sx, sy, &r, &g, &b);
            int lum = ((int)r * 77 + (int)g * 150 + (int)b * 29) >> 8;
            if (lum < 128) {
                size_t bp = (size_t)ey * (size_t)RB + (size_t)(ex >> 3);
                uint8_t bit = (uint8_t)(0x80u >> (ex & 7));
                black[bp] &= (uint8_t)~bit;
            }
        }
    }
    compensate_panel_image_orientation(black, red, yellow, blue, green, EC,
                                       has_red, has_yellow, has_blue, has_green);
    if (!write_raw_planes(out_path, black, red, yellow, blue, green, EC,
                          has_red, has_yellow, has_blue, has_green)) {
        free(black);
        free(red);
        free(yellow);
        free(blue);
        free(green);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG,
             "PNG BW threshold passthrough: %ux%u -> contain %dx%d at %d,%d (%u bytes/plane)",
             pw, ph, fit.w, fit.h, fit.x, fit.y, (unsigned)EC);
    free(black);
    free(red);
    free(yellow);
    free(blue);
    free(green);
    return ESP_OK;
}

/**
 * 允许映射红墨的像素：在拦住真灰（chroma 过低）的前提下尽量放宽。
 * - 色相扇区 + 饱和度
 * - 或 R 为最大通道且明显强于 G/B（暗红、棕红、部分肤色旁物体）
 */
static bool fs_red_eligible(uint8_t r, uint8_t g, uint8_t b)
{
    int ri = (int)r, gi = (int)g, bi = (int)b;
    int max_c = ri;
    if (gi > max_c) max_c = gi;
    if (bi > max_c) max_c = bi;
    int min_c = ri;
    if (gi < min_c) min_c = gi;
    if (bi < min_c) min_c = bi;
    int chroma = max_c - min_c;
    if (chroma < 20 || max_c < 40)
        return false;

    int hue6 = 0;
    if (max_c == ri)
        hue6 = ((gi - bi) * 60) / chroma;
    else if (max_c == gi)
        hue6 = 120 + ((bi - ri) * 60) / chroma;
    else
        hue6 = 240 + ((ri - gi) * 60) / chroma;
    if (hue6 < 0)
        hue6 += 360;

    int sat256 = (chroma * 256) / (max_c > 0 ? max_c : 1);
    bool red_hue = (hue6 <= 55 || hue6 >= 305);
    bool orange_hue = hue6 <= 72 && gi * 100 <= ri * 78 && bi * 100 <= ri * 74;
    bool hue_sat = (red_hue || orange_hue) && sat256 > 55 &&
                   ri >= gi + 16 && ri >= bi + 20;

    bool r_led = (max_c == ri) && ri >= 55 &&
                 ri >= gi + 20 && ri >= bi + 28 &&
                 (gi + bi) * 2 <= ri * 3 && chroma >= 24;

    return hue_sat || r_led;
}

static void fs_dither_row(const uint8_t *rgb, fs_err_t *ec, fs_err_t *en,
                          uint8_t *blk, uint8_t *red_plane,
                          uint8_t *yellow_plane,
                          int y, int w, bool has_red, bool has_yellow,
                          const image_quant_profile_t *profile)
{
    const image_quant_profile_t *quant = profile ? profile : &s_quant_default;
    int rb = w >> 3;
    bool tone_profile = quant->neutral_chroma_threshold != 0;
    int direction = tone_profile && (y & 1) ? -1 : 1;
    /* Q4 residuals retain low-level tones without changing legacy error units. */
    int error_scale = tone_profile ? 16 : 1;
    for (int x = direction > 0 ? 0 : w - 1; x >= 0 && x < w; x += direction) {
        int ex = x + 1;
        uint8_t r0 = rgb[x * 3];
        uint8_t g0 = rgb[x * 3 + 1];
        uint8_t b0 = rgb[x * 3 + 2];
        int lum = ((int)r0 * 77 + (int)g0 * 150 + (int)b0 * 29) >> 8;
        bool neutral = image_quant_source_is_neutral(r0, g0, b0, quant);
        int yv = lum * error_scale + ec[ex].r;
        if (yv < 0) yv = 0;
        if (yv > 255 * error_scale) yv = 255 * error_scale;

        size_t bp = (size_t)y * rb + (x >> 3);
        uint8_t bit = 0x80u >> (x & 7);

        if (tone_profile) {
            blk[bp] |= bit;
            if (has_red) red_plane[bp] &= (uint8_t)~bit;
            if (has_yellow) yellow_plane[bp] &= (uint8_t)~bit;
            /* Absorb incoming error at exact source text/borders, avoiding halos. */
            if (r0 == g0 && g0 == b0 && (r0 == 0 || r0 == 255)) {
                if (r0 == 0) blk[bp] &= (uint8_t)~bit;
                continue;
            }
        }

        int yellow_strength = has_yellow && !neutral
                            ? image_quant_apply_gain(yellow_ink_strength(r0, g0, b0),
                                                     quant->yellow_gain_q8)
                            : 0;
        if (yellow_strength > 0 &&
            yellow_strength > (int)bayer8[y & 7][x & 7]) {
            yellow_plane[bp] |= bit;
            blk[bp] |= bit;
            continue;
        }

        int red_strength = has_red && !neutral
                         ? image_quant_apply_gain(red_ink_strength(r0, g0, b0),
                                                  quant->red_gain_q8)
                         : 0;
        if (red_strength > 0 && (red_orange_pixel(r0, g0, b0) || fs_red_eligible(r0, g0, b0)) &&
            red_strength > (int)bayer8[y & 7][x & 7]) {
            red_plane[bp] |= bit;
            blk[bp] |= bit;
            continue;
        }

        int out = 255 * error_scale;
        int bw_threshold = neutral ? quant->neutral_luma_threshold : quant->bw_luma_threshold;
        if (yv < bw_threshold * error_scale) {
            blk[bp] &= ~bit;
            if (has_red)
                red_plane[bp] &= ~bit;
            if (has_yellow)
                yellow_plane[bp] &= ~bit;
            out = 0;
        }

        int gain = neutral ? quant->neutral_fs_error_gain_q8 : quant->fs_error_gain_q8;
        int16_t err = (int16_t)(((yv - out) * gain) / 256);
        ec[ex + direction].r += err * 7 / 16;
        en[ex - direction].r += err * 3 / 16;
        en[ex].r     += err * 5 / 16;
        en[ex + direction].r += err * 1 / 16;
    }
}

static void e6_37_fs_dither_row(const uint8_t *rgb, fs_err_t *ec, fs_err_t *en,
                                uint8_t *black, uint8_t *red,
                                uint8_t *yellow, uint8_t *blue,
                                uint8_t *green, int y, int w)
{
    int rb = w >> 3;
    for (int x = 0; x < w; x++) {
        int ex = x + 1;
        int r = (int)rgb[x * 3]     + ec[ex].r;
        int g = (int)rgb[x * 3 + 1] + ec[ex].g;
        int b = (int)rgb[x * 3 + 2] + ec[ex].b;
        if (r < 0) r = 0; else if (r > 255) r = 255;
        if (g < 0) g = 0; else if (g > 255) g = 255;
        if (b < 0) b = 0; else if (b > 255) b = 255;
        uint8_t c = e6_37_nearest_color(r, g, b);
        const e6_rgb_t *std = &e6_37_fs_palette[c];

        size_t bp = (size_t)y * (size_t)rb + (size_t)(x >> 3);
        uint8_t bit = (uint8_t)(0x80u >> (x & 7));
        e6_37_set_plane_pixel(black, red, yellow, blue, green, bp, bit, c);

        int er = r - (int)std->r;
        int eg = g - (int)std->g;
        int eb = b - (int)std->b;

        ec[ex + 1].r += er * 7 / 16;
        ec[ex + 1].g += eg * 7 / 16;
        ec[ex + 1].b += eb * 7 / 16;

        en[ex - 1].r += er * 3 / 16;
        en[ex - 1].g += eg * 3 / 16;
        en[ex - 1].b += eb * 3 / 16;

        en[ex].r += er * 5 / 16;
        en[ex].g += eg * 5 / 16;
        en[ex].b += eb * 5 / 16;

        en[ex + 1].r += er * 1 / 16;
        en[ex + 1].g += eg * 1 / 16;
        en[ex + 1].b += eb * 1 / 16;
    }
}

/* ── JPEG decode context ──────────────────────────────────────────── */

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} jpeg_mem_t;

typedef struct {
    uint8_t *black;
    uint8_t *red;
    uint8_t *yellow;
    uint8_t *blue;
    uint8_t *green;
    int w, h;               /* EPD screen size */
    int sw, sh;             /* decoded image size */
    image_fit_rect_t fit;   /* target area for aspect-ratio preserving display */
    /* F-S band state */
    uint8_t  *band;         /* source-resolution band: band_w × band_h × 3 */
    uint8_t  *rgb_row;      /* screen-width row for F-S sampling */
    int       band_w;       /* = sw (decoded width) */
    int       band_h;       /* allocated band height */
    int       band_top;     /* source Y of current band (-1 = none) */
    int       band_rows;    /* actual rows in current band */
    fs_err_t *ec, *en;
    bool      has_red;
    bool      has_yellow;
    bool      has_blue;
    bool      has_green;
    const image_quant_profile_t *quant_profile;
} epd_out_t;

typedef struct {
    jpeg_mem_t mem;
    FILE *fp;
    epd_out_t out;
} jpeg_ctx_t;

/* ── JPEG input callbacks ─────────────────────────────────────────── */

static UINT jpeg_infunc_mem(JDEC *jd, BYTE *buf, UINT len)
{
    jpeg_ctx_t *ctx = (jpeg_ctx_t *)jd->device;
    if (!ctx) return 0;
    jpeg_mem_t *m = &ctx->mem;
    size_t remain = (m->pos < m->len) ? (m->len - m->pos) : 0;
    if (len > remain) len = (UINT)remain;
    if (buf) memcpy(buf, m->data + m->pos, len);
    m->pos += len;
    return len;
}

static UINT jpeg_infunc_file(JDEC *jd, BYTE *buf, UINT len)
{
    jpeg_ctx_t *ctx = (jpeg_ctx_t *)jd->device;
    if (!ctx || !ctx->fp) return 0;
    if (buf) return (UINT)fread(buf, 1, len, ctx->fp);
    if (fseek(ctx->fp, (long)len, SEEK_CUR) == 0) return len;
    return 0;
}

/* ── JPEG output: Bayer (stretch-to-fill) ─────────────────────────── */

static UINT jpeg_outfunc_bayer(JDEC *jd, void *bitmap, JRECT *rect)
{
    (void)jd;
    jpeg_ctx_t *ctx = (jpeg_ctx_t *)jd->device;
    if (!ctx || !bitmap || !rect) return 0;
    epd_out_t *o = &ctx->out;

    const uint8_t *px = (const uint8_t *)bitmap;
    int bw = rect->right  - rect->left + 1;
    int bh = rect->bottom - rect->top  + 1;

    for (int y = 0; y < bh; y++) {
        int src_y = (int)rect->top + y;
        int fit_bottom = o->fit.y + o->fit.h;
        int ey0 = o->fit.y + src_y * o->fit.h / o->sh;
        int ey1 = o->fit.y + (src_y + 1) * o->fit.h / o->sh;
        if (ey0 >= fit_bottom) { px += (size_t)bw * 3; continue; }
        if (ey1 > fit_bottom) ey1 = fit_bottom;
        for (int x = 0; x < bw; x++) {
            int src_x = (int)rect->left + x;
            int fit_right = o->fit.x + o->fit.w;
            int ex0 = o->fit.x + src_x * o->fit.w / o->sw;
            int ex1 = o->fit.x + (src_x + 1) * o->fit.w / o->sw;
            uint8_t r = px[0], g = px[1], b = px[2];
            px += 3;
            if (ex0 >= fit_right) continue;
            if (ex1 > fit_right) ex1 = fit_right;
            for (int ey = ey0; ey < ey1; ey++)
                for (int ex = ex0; ex < ex1; ex++)
                    dither_pixel_bayer(o->black, o->red, o->yellow,
                                       o->blue, o->green, ex, ey,
                                       o->w, r, g, b,
                                       o->has_red, o->has_yellow,
                                       o->has_blue, o->has_green,
                                       o->quant_profile);
        }
    }
    return 1;
}

/* ── JPEG output: red-first F-S (band-buffered, stretch) ──────────── */

static void fs_flush_band(epd_out_t *o)
{
    int src_y0 = o->band_top;
    int src_y1 = src_y0 + o->band_rows;
    int fit_bottom = o->fit.y + o->fit.h;
    int fit_right = o->fit.x + o->fit.w;
    int ey_start = o->fit.y + (src_y0 * o->fit.h + o->sh - 1) / o->sh;
    int ey_end = (src_y1 < o->sh)
               ? o->fit.y + (src_y1 * o->fit.h + o->sh - 1) / o->sh
               : fit_bottom;
    if (ey_start < o->fit.y) ey_start = o->fit.y;
    if (ey_end > fit_bottom) ey_end = fit_bottom;

    for (int ey = ey_start; ey < ey_end; ey++) {
        if (ey < o->fit.y || ey >= fit_bottom) continue;
        int sy = (ey - o->fit.y) * o->sh / o->fit.h;
        int by = sy - src_y0;
        if (by < 0 || by >= o->band_rows) continue;

        for (int ex = 0; ex < o->w; ex++) {
            if (ex < o->fit.x || ex >= fit_right) {
                o->rgb_row[ex * 3]     = 255;
                o->rgb_row[ex * 3 + 1] = 255;
                o->rgb_row[ex * 3 + 2] = 255;
                continue;
            }
            int sx = (ex - o->fit.x) * o->sw / o->fit.w;
            if (sx >= o->sw) sx = o->sw - 1;
            int si = (by * o->band_w + sx) * 3;
            o->rgb_row[ex * 3]     = o->band[si];
            o->rgb_row[ex * 3 + 1] = o->band[si + 1];
            o->rgb_row[ex * 3 + 2] = o->band[si + 2];
        }

        memset(o->en, 0, ((size_t)o->w + 2) * sizeof(fs_err_t));
        if (o->has_blue && o->has_green) {
            e6_37_fs_dither_row(o->rgb_row, o->ec, o->en,
                                o->black, o->red, o->yellow,
                                o->blue, o->green, ey, o->w);
        } else {
            fs_dither_row(o->rgb_row, o->ec, o->en,
                          o->black, o->red, o->yellow,
                          ey, o->w, o->has_red, o->has_yellow,
                          o->quant_profile);
        }
        fs_err_t *tmp = o->ec; o->ec = o->en; o->en = tmp;
    }
}

static UINT jpeg_outfunc_fs(JDEC *jd, void *bitmap, JRECT *rect)
{
    (void)jd;
    jpeg_ctx_t *ctx = (jpeg_ctx_t *)jd->device;
    if (!ctx || !bitmap || !rect) return 0;
    epd_out_t *o = &ctx->out;
    const uint8_t *px = (const uint8_t *)bitmap;

    int bw  = (int)(rect->right  - rect->left + 1);
    int bh  = (int)(rect->bottom - rect->top  + 1);
    int top = (int)rect->top;

    if (bw <= 0 || bh <= 0 || bh > o->band_h) {
        ESP_LOGE(TAG, "JPEG block too large for band: %dx%d (band_h=%d)",
                 bw, bh, o->band_h);
        return 0;
    }

    if (o->band_top < 0) {
        o->band_top  = top;
        o->band_rows = bh;
    } else if (top != o->band_top) {
        fs_flush_band(o);
        o->band_top  = top;
        o->band_rows = bh;
        memset(o->band, 255, (size_t)o->band_h * o->band_w * 3);
    }

    for (int y = 0; y < bh; y++) {
        for (int x = 0; x < bw; x++) {
            int src_x = (int)rect->left + x;
            uint8_t r = px[0], g = px[1], b = px[2];
            px += 3;
            if (src_x < 0 || src_x >= o->sw) continue;
            int idx = (y * o->band_w + src_x) * 3;
            o->band[idx] = r; o->band[idx + 1] = g; o->band[idx + 2] = b;
        }
    }
    return 1;
}

/* ── JPEG scale selection (stretch-to-fill) ───────────────────────── */

static uint8_t choose_scale(uint16_t in_w, uint16_t in_h,
                             int target_w, int target_h,
                             int *out_w, int *out_h)
{
    /*
     * For contain-fit conversion, decode close to the panel pixel count while
     * avoiding tiny intermediate images that would be heavily upscaled.
     */
    uint8_t best_s = 0;
    int best_sw = (int)in_w, best_sh = (int)in_h;
    int32_t best_cost = INT32_MAX;

    for (uint8_t s = 0; s <= 3; s++) {
        int sw = (int)((in_w + ((1u << s) - 1u)) >> s);
        int sh = (int)((in_h + ((1u << s) - 1u)) >> s);
        image_fit_rect_t fit = image_fit_contain_rect(sw, sh, target_w, target_h);
        int32_t dx = fit.w - target_w;
        int32_t dy = fit.h - target_h;
        int32_t cost = dx * dx + dy * dy;
        if (fit.w > sw || fit.h > sh)
            cost = cost * 3 + 100000;
        if (cost < best_cost || (cost == best_cost && s > best_s)) {
            best_cost = cost;
            best_s  = s;
            best_sw = sw;
            best_sh = sh;
        }
    }

    if (out_w) *out_w = best_sw;
    if (out_h) *out_h = best_sh;
    return best_s;
}

/* ── JPEG common decode ───────────────────────────────────────────── */

static esp_err_t jpeg_decode_common(jpeg_ctx_t *ctx, UINT (*infunc)(JDEC*,BYTE*,UINT),
                                     const char *out_path)
{
    const int EW = epd_width();
    const int EH = epd_height();
    const int EC = EW * EH / 8;

    static const UINT POOL_SIZE = 20 * 1024;
    log_heap_state("jpeg begin");

    void *pool = img_alloc(POOL_SIZE, "jpeg pool");
    if (!pool) { ESP_LOGE(TAG, "pool alloc failed"); return ESP_ERR_NO_MEM; }

    JDEC jd = {0};
    jd.device = ctx;
    JRESULT r = jd_prepare(&jd, infunc, pool, POOL_SIZE, ctx);
    if (r != JDR_OK) {
        ESP_LOGE(TAG, "jd_prepare failed: %d", (int)r);
        free(pool);
        return ESP_FAIL;
    }
    if (jd.width == 0 || jd.height == 0) {
        ESP_LOGE(TAG, "invalid JPEG dimensions: %ux%u",
                 (unsigned)jd.width, (unsigned)jd.height);
        free(pool);
        return ESP_ERR_INVALID_SIZE;
    }

    bool has_red = epd_has_red();
    bool has_yellow = epd_has_yellow();
    bool has_blue = image_panel_is_e6_6c();
    bool has_green = image_panel_is_e6_6c();
    const image_quant_profile_t *quant_profile = image_quant_profile_current();
    image_quant_log_profile("JPEG", quant_profile);
    uint8_t *black = (uint8_t *)img_alloc(EC, "jpeg black plane");
    uint8_t *red   = has_red ? (uint8_t *)img_alloc(EC, "jpeg red plane") : NULL;
    uint8_t *yellow = has_yellow ? (uint8_t *)img_alloc(EC, "jpeg yellow plane") : NULL;
    uint8_t *blue = has_blue ? (uint8_t *)img_alloc(EC, "jpeg blue plane") : NULL;
    uint8_t *green = has_green ? (uint8_t *)img_alloc(EC, "jpeg green plane") : NULL;
    if (!black || (has_red && !red) || (has_yellow && !yellow) ||
        (has_blue && !blue) || (has_green && !green)) {
        ESP_LOGE(TAG, "jpeg plane alloc failed: plane=%u bytes, panel=%dx%d red=%d yellow=%d blue=%d green=%d",
                 (unsigned)EC, EW, EH, has_red ? 1 : 0, has_yellow ? 1 : 0,
                 has_blue ? 1 : 0, has_green ? 1 : 0);
        free(pool); free(black); free(red); free(yellow); free(blue); free(green);
        return ESP_ERR_NO_MEM;
    }
    memset(black, 0xFF, EC);
    if (red)
        memset(red,   0x00, EC);
    if (yellow)
        memset(yellow, 0x00, EC);
    if (blue)
        memset(blue, 0x00, EC);
    if (green)
        memset(green, 0x00, EC);

    int decoded_w = 0, decoded_h = 0;
    uint8_t scale = choose_scale(jd.width, jd.height, EW, EH,
                                 &decoded_w, &decoded_h);

    image_fit_rect_t fit = image_fit_contain_rect(decoded_w, decoded_h, EW, EH);
    ESP_LOGI(TAG, "jpeg %ux%u -> 1/%u => %dx%d, contain %dx%d at %d,%d on %dx%d",
             (unsigned)jd.width, (unsigned)jd.height,
             (unsigned)(1u << scale), decoded_w, decoded_h,
             fit.w, fit.h, fit.x, fit.y, EW, EH);

    ctx->out = (epd_out_t){
        .black = black, .red = red, .yellow = yellow,
        .blue = blue, .green = green,
        .w = EW, .h = EH,
        .sw = decoded_w, .sh = decoded_h,
        .fit = fit,
        .has_red = has_red,
        .has_yellow = has_yellow,
        .has_blue = has_blue,
        .has_green = has_green,
        .quant_profile = quant_profile
    };

    /* Try color-first Floyd-Steinberg; fall back to Bayer if OOM */
    int bh = JPEG_FS_BAND_ROWS;
    size_t bsz = (size_t)bh * decoded_w * 3;
    ctx->out.band    = (uint8_t *)img_alloc(bsz, "jpeg fs band");
    ctx->out.rgb_row = (uint8_t *)img_alloc((size_t)EW * 3, "jpeg rgb row");
    ctx->out.ec      = (fs_err_t *)img_calloc((size_t)EW + 2, sizeof(fs_err_t), "jpeg fs err cur");
    ctx->out.en      = (fs_err_t *)img_calloc((size_t)EW + 2, sizeof(fs_err_t), "jpeg fs err next");
    bool use_fs = ctx->out.band && ctx->out.rgb_row &&
                  ctx->out.ec && ctx->out.en;

    if (use_fs) {
        memset(ctx->out.band, 255, bsz);
        ctx->out.band_w    = decoded_w;
        ctx->out.band_h    = bh;
        ctx->out.band_top  = -1;
        ctx->out.band_rows = 0;
        ESP_LOGI(TAG, "%s F-S (band=%u bytes)",
                 has_blue && has_green ? "E6 six-color" :
                 (has_yellow ? "yellow/red-first" : "red-first"),
                 (unsigned)bsz);
    } else {
        free(ctx->out.band); free(ctx->out.rgb_row);
        free(ctx->out.ec);   free(ctx->out.en);
        ctx->out.band = NULL; ctx->out.rgb_row = NULL;
        ctx->out.ec = NULL;   ctx->out.en = NULL;
        ESP_LOGW(TAG, "F-S alloc failed, using Bayer");
    }

    r = jd_decomp(&jd, use_fs ? jpeg_outfunc_fs : jpeg_outfunc_bayer, scale);

    if (use_fs && ctx->out.band_rows > 0)
        fs_flush_band(&ctx->out);

    free(pool);
    free(ctx->out.band); free(ctx->out.rgb_row);
    free(ctx->out.ec);   free(ctx->out.en);

    if (r != JDR_OK) {
        ESP_LOGE(TAG, "jd_decomp failed: %d", (int)r);
        free(black); free(red); free(yellow); free(blue); free(green);
        return ESP_FAIL;
    }

    compensate_panel_image_orientation(black, red, yellow, blue, green, EC,
                                       has_red, has_yellow, has_blue, has_green);
    if (!write_raw_planes(out_path, black, red, yellow, blue, green, EC,
                          has_red, has_yellow, has_blue, has_green)) {
        free(black); free(red); free(yellow); free(blue); free(green);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Converted JPEG -> %s (%u bytes/plane, %s%s, contain)",
             out_path, (unsigned)EC,
             use_fs ? (has_blue && has_green ? "E6 six-color F-S" :
                      (has_yellow ? "yellow/red-first F-S" : "red-first F-S"))
                    : (has_blue && has_green ? "E6 nearest" : "Bayer"),
             has_blue && has_green ? ", E6" : (has_yellow ? ", BWRY" : ""));

    free(black);
    free(red);
    free(yellow);
    free(blue);
    free(green);
    return ESP_OK;
}

/* ── public JPEG API ──────────────────────────────────────────────── */

esp_err_t image_convert_jpeg_to_epd_raw(const uint8_t *jpeg, size_t jpeg_len,
                                         const char *out_path)
{
    if (!jpeg || jpeg_len == 0 || !out_path) return ESP_ERR_INVALID_ARG;
    jpeg_ctx_t ctx = { .mem = { .data = jpeg, .len = jpeg_len, .pos = 0 }, .fp = NULL };
    return jpeg_decode_common(&ctx, jpeg_infunc_mem, out_path);
}

static esp_err_t jpeg_convert_from_file(const char *input_path, const char *out_path)
{
    FILE *fp = fopen(input_path, "rb");
    if (!fp) { ESP_LOGE(TAG, "open input failed: %s", input_path); return ESP_FAIL; }
    jpeg_ctx_t ctx = { .mem = { 0 }, .fp = fp };
    esp_err_t ret = jpeg_decode_common(&ctx, jpeg_infunc_file, out_path);
    fclose(fp);
    return ret;
}

/* ── PNG conversion (stretch-to-fill, red-first F-S) ──────────────── */

static esp_err_t png_decode_to_epd(unsigned char *rgba, unsigned pw, unsigned ph,
                                     const char *out_path)
{
    const int EW = epd_width();
    const int EH = epd_height();
    const int EC = EW * EH / 8;

    log_heap_state("png begin");

    esp_err_t pass = png_try_prequantized_passthrough(rgba, pw, ph, out_path);
    if (pass == ESP_OK) {
        free(rgba);
        return ESP_OK;
    }
    if (pass == ESP_ERR_NO_MEM) {
        free(rgba);
        return pass;
    }

    pass = png_threshold_bw_passthrough(rgba, pw, ph, out_path);
    if (pass == ESP_OK) {
        free(rgba);
        return ESP_OK;
    }
    if (pass == ESP_ERR_NO_MEM) {
        free(rgba);
        return pass;
    }

    bool has_red = epd_has_red();
    bool has_yellow = epd_has_yellow();
    bool has_blue = image_panel_is_e6_6c();
    bool has_green = image_panel_is_e6_6c();
    const image_quant_profile_t *quant_profile = image_quant_profile_current();
    image_quant_log_profile("PNG", quant_profile);
    uint8_t *black = (uint8_t *)img_alloc(EC, "png black plane");
    uint8_t *red   = has_red ? (uint8_t *)img_alloc(EC, "png red plane") : NULL;
    uint8_t *yellow = has_yellow ? (uint8_t *)img_alloc(EC, "png yellow plane") : NULL;
    uint8_t *blue = has_blue ? (uint8_t *)img_alloc(EC, "png blue plane") : NULL;
    uint8_t *green = has_green ? (uint8_t *)img_alloc(EC, "png green plane") : NULL;
    if (!black || (has_red && !red) || (has_yellow && !yellow) ||
        (has_blue && !blue) || (has_green && !green)) {
        ESP_LOGE(TAG, "png plane alloc failed: plane=%u bytes, panel=%dx%d red=%d yellow=%d blue=%d green=%d",
                 (unsigned)EC, EW, EH, has_red ? 1 : 0, has_yellow ? 1 : 0,
                 has_blue ? 1 : 0, has_green ? 1 : 0);
        free(black); free(red); free(yellow); free(blue); free(green); free(rgba);
        return ESP_ERR_NO_MEM;
    }
    memset(black, 0xFF, EC);
    if (red)
        memset(red,   0x00, EC);
    if (yellow)
        memset(yellow, 0x00, EC);
    if (blue)
        memset(blue, 0x00, EC);
    if (green)
        memset(green, 0x00, EC);

    image_fit_rect_t fit = image_fit_contain_rect((int)pw, (int)ph, EW, EH);
    ESP_LOGI(TAG, "PNG %ux%u -> contain %dx%d at %d,%d on %dx%d",
             pw, ph, fit.w, fit.h, fit.x, fit.y, EW, EH);

    uint8_t  *rgb_row = (uint8_t *)img_alloc((size_t)EW * 3, "png rgb row");
    fs_err_t *ec = (fs_err_t *)img_calloc((size_t)EW + 2, sizeof(fs_err_t), "png fs err cur");
    fs_err_t *en = (fs_err_t *)img_calloc((size_t)EW + 2, sizeof(fs_err_t), "png fs err next");
    bool use_fs = rgb_row && ec && en;

    if (use_fs) {
        for (int ey = 0; ey < EH; ey++) {
            int rel_y = ey - fit.y;
            bool in_y = rel_y >= 0 && rel_y < fit.h;
            int sy = in_y ? (rel_y * (int)ph / fit.h) : 0;
            if (sy >= (int)ph) sy = (int)ph - 1;
            for (int ex = 0; ex < EW; ex++) {
                int rel_x = ex - fit.x;
                bool in_x = rel_x >= 0 && rel_x < fit.w;
                uint8_t r = 255, g = 255, b = 255, a = 255;
                if (in_x && in_y) {
                    int sx = rel_x * (int)pw / fit.w;
                    if (sx >= (int)pw) sx = (int)pw - 1;
                    size_t si = ((size_t)sy * pw + (size_t)sx) * 4u;
                    r = rgba[si]; g = rgba[si + 1]; b = rgba[si + 2]; a = rgba[si + 3];
                }
                if (a < 255) {
                    r = (uint8_t)((r * a + 255u * (255u - a)) / 255u);
                    g = (uint8_t)((g * a + 255u * (255u - a)) / 255u);
                    b = (uint8_t)((b * a + 255u * (255u - a)) / 255u);
                }
                rgb_row[ex * 3]     = r;
                rgb_row[ex * 3 + 1] = g;
                rgb_row[ex * 3 + 2] = b;
            }
            memset(en, 0, ((size_t)EW + 2) * sizeof(fs_err_t));
            if (has_blue && has_green) {
                e6_37_fs_dither_row(rgb_row, ec, en,
                                    black, red, yellow, blue, green, ey, EW);
            } else {
                fs_dither_row(rgb_row, ec, en, black, red, yellow, ey, EW,
                              has_red, has_yellow, quant_profile);
            }
            fs_err_t *tmp = ec; ec = en; en = tmp;
        }
    } else {
        free(rgb_row); free(ec); free(en);
        rgb_row = NULL; ec = NULL; en = NULL;
        ESP_LOGW(TAG, "F-S alloc failed, using Bayer");
        for (int ey = 0; ey < EH; ey++) {
            int rel_y = ey - fit.y;
            bool in_y = rel_y >= 0 && rel_y < fit.h;
            int sy = in_y ? (rel_y * (int)ph / fit.h) : 0;
            if (sy >= (int)ph) sy = (int)ph - 1;
            for (int ex = 0; ex < EW; ex++) {
                int rel_x = ex - fit.x;
                bool in_x = rel_x >= 0 && rel_x < fit.w;
                uint8_t r = 255, g = 255, b = 255, a = 255;
                if (in_x && in_y) {
                    int sx = rel_x * (int)pw / fit.w;
                    if (sx >= (int)pw) sx = (int)pw - 1;
                    size_t si = ((size_t)sy * pw + (size_t)sx) * 4u;
                    r = rgba[si]; g = rgba[si + 1]; b = rgba[si + 2]; a = rgba[si + 3];
                }
                if (a < 255) {
                    r = (uint8_t)((r * a + 255u * (255u - a)) / 255u);
                    g = (uint8_t)((g * a + 255u * (255u - a)) / 255u);
                    b = (uint8_t)((b * a + 255u * (255u - a)) / 255u);
                }
                dither_pixel_bayer(black, red, yellow, blue, green,
                                   ex, ey, EW, r, g, b,
                                   has_red, has_yellow, has_blue, has_green,
                                   quant_profile);
            }
        }
    }

    free(rgba);
    free(rgb_row); free(ec); free(en);

    compensate_panel_image_orientation(black, red, yellow, blue, green, EC,
                                       has_red, has_yellow, has_blue, has_green);
    if (!write_raw_planes(out_path, black, red, yellow, blue, green, EC,
                          has_red, has_yellow, has_blue, has_green)) {
        free(black);
        free(red);
        free(yellow);
        free(blue);
        free(green);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Converted PNG -> %s (%u bytes/plane, %s%s, contain)",
             out_path, (unsigned)EC,
             use_fs ? (has_blue && has_green ? "E6 six-color F-S" :
                      (has_yellow ? "yellow/red-first F-S" : "red-first F-S"))
                    : (has_blue && has_green ? "E6 nearest" : "Bayer"),
             has_blue && has_green ? ", E6" : (has_yellow ? ", BWRY" : ""));
    free(black);
    free(red);
    free(yellow);
    free(blue);
    free(green);
    return ESP_OK;
}

esp_err_t image_convert_png_to_epd_raw(const uint8_t *png, size_t png_len,
                                        const char *out_path)
{
    if (!png || png_len == 0 || !out_path) return ESP_ERR_INVALID_ARG;

    if (png_len >= 24) {
        unsigned pw_hdr = ((unsigned)png[16] << 24) | ((unsigned)png[17] << 16) |
                          ((unsigned)png[18] << 8)  | (unsigned)png[19];
        unsigned ph_hdr = ((unsigned)png[20] << 24) | ((unsigned)png[21] << 16) |
                          ((unsigned)png[22] << 8)  | (unsigned)png[23];
        if (pw_hdr == 0 || ph_hdr == 0 ||
            pw_hdr > PNG_MAX_SIDE_PIXELS || ph_hdr > PNG_MAX_SIDE_PIXELS ||
            (uint64_t)pw_hdr * (uint64_t)ph_hdr > PNG_MAX_DECODE_PIXELS) {
            ESP_LOGE(TAG, "PNG dimensions rejected before decode: %ux%u",
                     pw_hdr, ph_hdr);
            return ESP_ERR_INVALID_SIZE;
        }
    }

    unsigned char *rgba = NULL;
    unsigned pw = 0, ph = 0;
    unsigned perr = lodepng_decode32(&rgba, &pw, &ph, png, png_len);
    if (perr || !rgba) {
        ESP_LOGE(TAG, "PNG decode error %u: %s", perr, lodepng_error_text(perr));
        free(rgba);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "PNG decoded: %ux%u (free heap: %lu)", pw, ph,
             (unsigned long)esp_get_free_heap_size());

    return png_decode_to_epd(rgba, pw, ph, out_path);
}

esp_err_t image_convert_to_epd_fb(const uint8_t *data, size_t len, fb_t **out_fb)
{
    if (!data || len < 4 || !out_fb)
        return ESP_ERR_INVALID_ARG;
    *out_fb = NULL;

    if (data[0] != 0x89 || data[1] != 0x50 ||
        data[2] != 0x4E || data[3] != 0x47)
        return ESP_ERR_NOT_SUPPORTED;

    unsigned char *rgba = NULL;
    unsigned pw = 0, ph = 0;
    int64_t t0 = esp_timer_get_time();
    unsigned perr = lodepng_decode32(&rgba, &pw, &ph, data, len);
    if (perr || !rgba) {
        ESP_LOGE(TAG, "PNG framebuffer decode error %u: %s",
                 perr, lodepng_error_text(perr));
        free(rgba);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "PNG decoded for framebuffer in %lld ms: %ux%u",
             (long long)((esp_timer_get_time() - t0) / 1000LL), pw, ph);

    esp_err_t err = png_try_prequantized_framebuffer(rgba, pw, ph, out_fb);
    free(rgba);
    return err;
}

esp_err_t image_convert_file_to_epd_fb(const char *input_path, fb_t **out_fb)
{
    if (!input_path || !out_fb)
        return ESP_ERR_INVALID_ARG;
    *out_fb = NULL;

    uint8_t magic[4] = {0};
    FILE *mf = fopen(input_path, "rb");
    if (!mf) {
        ESP_LOGE(TAG, "open failed: %s", input_path);
        return ESP_FAIL;
    }
    size_t mr = fread(magic, 1, sizeof(magic), mf);
    fclose(mf);
    if (mr < 4 || magic[0] != 0x89 || magic[1] != 0x50 ||
        magic[2] != 0x4E || magic[3] != 0x47)
        return ESP_ERR_NOT_SUPPORTED;

    struct stat st;
    if (stat(input_path, &st) != 0 || st.st_size <= 0)
        return ESP_FAIL;
    if (st.st_size > PNG_MAX_FILE_BYTES) {
        ESP_LOGE(TAG, "PNG too large for framebuffer fast path (%ld bytes, max %u bytes)",
                 (long)st.st_size, (unsigned)PNG_MAX_FILE_BYTES);
        return ESP_ERR_NO_MEM;
    }

    uint8_t *buf = (uint8_t *)img_alloc((size_t)st.st_size, "png fb file buffer");
    if (!buf)
        return ESP_ERR_NO_MEM;
    FILE *pf = fopen(input_path, "rb");
    if (!pf) {
        free(buf);
        return ESP_FAIL;
    }
    size_t got = fread(buf, 1, (size_t)st.st_size, pf);
    fclose(pf);
    if (got != (size_t)st.st_size) {
        free(buf);
        return ESP_FAIL;
    }

    unsigned char *rgba = NULL;
    unsigned pw = 0, ph = 0;
    int64_t t0 = esp_timer_get_time();
    unsigned perr = lodepng_decode32(&rgba, &pw, &ph, buf, got);
    free(buf);
    if (perr || !rgba) {
        ESP_LOGE(TAG, "PNG framebuffer decode error %u: %s",
                 perr, lodepng_error_text(perr));
        free(rgba);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "PNG decoded for framebuffer in %lld ms: %ux%u",
             (long long)((esp_timer_get_time() - t0) / 1000LL), pw, ph);

    esp_err_t err = png_try_prequantized_framebuffer(rgba, pw, ph, out_fb);
    free(rgba);
    return err;
}

/* ── BMP conversion (stretch-to-fill, red-first F-S) ──────────────── */

static inline uint16_t bmp_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t bmp_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static esp_err_t bmp_decode_core(FILE *fp, const uint8_t *mem, size_t mem_len,
                                  const char *out_path)
{
    uint8_t hdr[54];
    if (fp) {
        fseek(fp, 0, SEEK_SET);
        if (fread(hdr, 1, 54, fp) != 54) { ESP_LOGE(TAG, "BMP header too short"); return ESP_FAIL; }
    } else {
        if (mem_len < 54) return ESP_ERR_INVALID_ARG;
        memcpy(hdr, mem, 54);
    }

    if (hdr[0] != 0x42 || hdr[1] != 0x4D) return ESP_ERR_NOT_SUPPORTED;

    uint32_t pixel_off = bmp_le32(hdr + 10);
    uint32_t dib_size  = bmp_le32(hdr + 14);
    if (dib_size < 40) return ESP_ERR_NOT_SUPPORTED;

    int32_t  bmp_w = (int32_t)bmp_le32(hdr + 18);
    int32_t  bmp_h = (int32_t)bmp_le32(hdr + 22);
    uint16_t bpp   = bmp_le16(hdr + 28);
    uint32_t comp  = bmp_le32(hdr + 30);

    if (comp != 0 || (bpp != 24 && bpp != 32)) {
        ESP_LOGE(TAG, "BMP: unsupported (bpp=%u, compression=%lu)", bpp, (unsigned long)comp);
        return ESP_ERR_NOT_SUPPORTED;
    }

    bool bottom_up = (bmp_h > 0);
    int w = (int)bmp_w;
    int h = bottom_up ? (int)bmp_h : (int)(-bmp_h);
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return ESP_ERR_INVALID_ARG;

    const int EW = epd_width();
    const int EH = epd_height();
    const int EC = EW * EH / 8;

    int bpp_bytes  = bpp / 8;
    int row_stride = ((w * bpp_bytes) + 3) & ~3;

    log_heap_state("bmp begin");

    bool has_red_bmp = epd_has_red();
    bool has_yellow_bmp = epd_has_yellow();
    bool has_blue_bmp = image_panel_is_e6_6c();
    bool has_green_bmp = image_panel_is_e6_6c();
    const image_quant_profile_t *quant_profile = image_quant_profile_current();
    image_quant_log_profile("BMP", quant_profile);
    uint8_t *black = (uint8_t *)img_alloc(EC, "bmp black plane");
    uint8_t *red   = has_red_bmp ? (uint8_t *)img_alloc(EC, "bmp red plane") : NULL;
    uint8_t *yellow = has_yellow_bmp ? (uint8_t *)img_alloc(EC, "bmp yellow plane") : NULL;
    uint8_t *blue = has_blue_bmp ? (uint8_t *)img_alloc(EC, "bmp blue plane") : NULL;
    uint8_t *green = has_green_bmp ? (uint8_t *)img_alloc(EC, "bmp green plane") : NULL;
    uint8_t *row   = (uint8_t *)img_alloc((size_t)row_stride, "bmp input row");
    if (!black || (has_red_bmp && !red) || (has_yellow_bmp && !yellow) ||
        (has_blue_bmp && !blue) || (has_green_bmp && !green) || !row) {
        ESP_LOGE(TAG, "bmp alloc failed: plane=%u row=%u panel=%dx%d red=%d yellow=%d blue=%d green=%d",
                 (unsigned)EC, (unsigned)row_stride, EW, EH,
                 has_red_bmp ? 1 : 0, has_yellow_bmp ? 1 : 0,
                 has_blue_bmp ? 1 : 0, has_green_bmp ? 1 : 0);
        free(black); free(red); free(yellow); free(blue); free(green); free(row);
        return ESP_ERR_NO_MEM;
    }
    memset(black, 0xFF, EC);
    if (red)
        memset(red,   0x00, EC);
    if (yellow)
        memset(yellow, 0x00, EC);
    if (blue)
        memset(blue, 0x00, EC);
    if (green)
        memset(green, 0x00, EC);

    image_fit_rect_t fit = image_fit_contain_rect(w, h, EW, EH);
    ESP_LOGI(TAG, "BMP %dx%d %u-bit -> contain %dx%d at %d,%d on %dx%d",
             w, h, (unsigned)bpp, fit.w, fit.h, fit.x, fit.y, EW, EH);

    uint8_t  *rgb_row = (uint8_t *)img_alloc((size_t)EW * 3, "bmp rgb row");
    fs_err_t *ec_b    = (fs_err_t *)img_calloc((size_t)EW + 2, sizeof(fs_err_t), "bmp fs err cur");
    fs_err_t *en_b    = (fs_err_t *)img_calloc((size_t)EW + 2, sizeof(fs_err_t), "bmp fs err next");
    bool use_fs = rgb_row && ec_b && en_b;

    int last_br = -1;

    if (use_fs) {
        for (int ey = 0; ey < EH; ey++) {
            int rel_y = ey - fit.y;
            bool in_y = rel_y >= 0 && rel_y < fit.h;
            int src_y = in_y ? (rel_y * h / fit.h) : 0;
            if (src_y >= h) src_y = h - 1;
            int br = bottom_up ? (h - 1 - src_y) : src_y;
            if (in_y && br != last_br) {
                long off = (long)pixel_off + (long)br * (long)row_stride;
                if (fp) {
                    fseek(fp, off, SEEK_SET);
                    if (fread(row, 1, (size_t)row_stride, fp) != (size_t)row_stride)
                        memset(row, 0xFF, (size_t)row_stride);
                } else if ((size_t)off + (size_t)row_stride <= mem_len) {
                    memcpy(row, mem + off, (size_t)row_stride);
                } else {
                    memset(row, 0xFF, (size_t)row_stride);
                }
                last_br = br;
            }
            for (int ex = 0; ex < EW; ex++) {
                int rel_x = ex - fit.x;
                bool in_x = rel_x >= 0 && rel_x < fit.w;
                if (in_x && in_y) {
                    int src_x = rel_x * w / fit.w;
                    if (src_x >= w) src_x = w - 1;
                    int pi = src_x * bpp_bytes;
                    rgb_row[ex * 3]     = row[pi + 2];
                    rgb_row[ex * 3 + 1] = row[pi + 1];
                    rgb_row[ex * 3 + 2] = row[pi];
                } else {
                    rgb_row[ex * 3]     = 255;
                    rgb_row[ex * 3 + 1] = 255;
                    rgb_row[ex * 3 + 2] = 255;
                }
            }
            memset(en_b, 0, ((size_t)EW + 2) * sizeof(fs_err_t));
            if (has_blue_bmp && has_green_bmp) {
                e6_37_fs_dither_row(rgb_row, ec_b, en_b,
                                    black, red, yellow, blue, green, ey, EW);
            } else {
                fs_dither_row(rgb_row, ec_b, en_b, black, red, yellow, ey, EW,
                              has_red_bmp, has_yellow_bmp, quant_profile);
            }
            fs_err_t *tmp = ec_b; ec_b = en_b; en_b = tmp;
        }
    } else {
        free(rgb_row); free(ec_b); free(en_b);
        rgb_row = NULL; ec_b = NULL; en_b = NULL;
        for (int ey = 0; ey < EH; ey++) {
            int rel_y = ey - fit.y;
            bool in_y = rel_y >= 0 && rel_y < fit.h;
            int src_y = in_y ? (rel_y * h / fit.h) : 0;
            if (src_y >= h) src_y = h - 1;
            int br = bottom_up ? (h - 1 - src_y) : src_y;
            if (in_y && br != last_br) {
                long off = (long)pixel_off + (long)br * (long)row_stride;
                if (fp) {
                    fseek(fp, off, SEEK_SET);
                    if (fread(row, 1, (size_t)row_stride, fp) != (size_t)row_stride) continue;
                } else {
                    if ((size_t)off + (size_t)row_stride > mem_len) continue;
                    memcpy(row, mem + off, (size_t)row_stride);
                }
                last_br = br;
            }
            for (int ex = 0; ex < EW; ex++) {
                int rel_x = ex - fit.x;
                bool in_x = rel_x >= 0 && rel_x < fit.w;
                uint8_t r = 255, g = 255, b = 255;
                if (in_x && in_y) {
                    int src_x = rel_x * w / fit.w;
                    if (src_x >= w) src_x = w - 1;
                    int pi = src_x * bpp_bytes;
                    r = row[pi + 2];
                    g = row[pi + 1];
                    b = row[pi + 0];
                }
                dither_pixel_bayer(black, red, yellow, blue, green,
                                   ex, ey, EW, r, g, b,
                                   has_red_bmp, has_yellow_bmp,
                                   has_blue_bmp, has_green_bmp,
                                   quant_profile);
            }
        }
    }

    free(row);
    free(rgb_row); free(ec_b); free(en_b);

    compensate_panel_image_orientation(black, red, yellow, blue, green, EC,
                                       has_red_bmp, has_yellow_bmp,
                                       has_blue_bmp, has_green_bmp);
    if (!write_raw_planes(out_path, black, red, yellow, blue, green, EC,
                          has_red_bmp, has_yellow_bmp,
                          has_blue_bmp, has_green_bmp)) {
        free(black);
        free(red);
        free(yellow);
        free(blue);
        free(green);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Converted BMP -> %s (%u bytes/plane, %s%s, contain)",
             out_path, (unsigned)EC,
             use_fs ? (has_blue_bmp && has_green_bmp ? "E6 six-color F-S" :
                      (has_yellow_bmp ? "yellow/red-first F-S" : "red-first F-S"))
                    : (has_blue_bmp && has_green_bmp ? "E6 nearest" : "Bayer"),
             has_blue_bmp && has_green_bmp ? ", E6" : (has_yellow_bmp ? ", BWRY" : ""));
    free(black);
    free(red);
    free(yellow);
    free(blue);
    free(green);
    return ESP_OK;
}

esp_err_t image_convert_bmp_to_epd_raw(const uint8_t *bmp, size_t bmp_len,
                                        const char *out_path)
{
    if (!bmp || bmp_len < 54 || !out_path) return ESP_ERR_INVALID_ARG;
    return bmp_decode_core(NULL, bmp, bmp_len, out_path);
}

/* ── unified converter (auto-detect by magic bytes) ───────────────── */

esp_err_t image_convert_to_epd_raw(const uint8_t *data, size_t len,
                                    const char *out_path)
{
    if (!data || len < 4) return ESP_ERR_INVALID_ARG;

    if (data[0] == 0xFF && data[1] == 0xD8)
        return image_convert_jpeg_to_epd_raw(data, len, out_path);
    if (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47)
        return image_convert_png_to_epd_raw(data, len, out_path);
    if (data[0] == 0x42 && data[1] == 0x4D)
        return image_convert_bmp_to_epd_raw(data, len, out_path);

    ESP_LOGE(TAG, "Unknown image format (magic: %02X %02X %02X %02X)",
             data[0], data[1], data[2], data[3]);
    return ESP_ERR_NOT_SUPPORTED;
}

/* ── file-based converter (low-memory path) ───────────────────────── */

esp_err_t image_convert_file_to_epd_raw(const char *input_path, const char *out_path)
{
    if (!input_path || !out_path) return ESP_ERR_INVALID_ARG;

    uint8_t magic[4] = {0};
    FILE *mf = fopen(input_path, "rb");
    if (!mf) { ESP_LOGE(TAG, "open failed: %s", input_path); return ESP_FAIL; }
    size_t mr = fread(magic, 1, 4, mf);
    fclose(mf);
    if (mr < 2) { ESP_LOGE(TAG, "file too small: %s", input_path); return ESP_FAIL; }

    ESP_LOGI(TAG, "convert_file: %s (magic: %02X %02X), free heap: %lu",
             input_path, magic[0], magic[1], (unsigned long)esp_get_free_heap_size());

    if (magic[0] == 0xFF && magic[1] == 0xD8)
        return jpeg_convert_from_file(input_path, out_path);

    if (magic[0] == 0x89 && magic[1] == 0x50) {
        struct stat st;
        if (stat(input_path, &st) != 0 || st.st_size <= 0) return ESP_FAIL;
        if (st.st_size > PNG_MAX_FILE_BYTES) {
            ESP_LOGE(TAG, "PNG too large (%ld bytes, max %u bytes)",
                     (long)st.st_size, (unsigned)PNG_MAX_FILE_BYTES);
            return ESP_ERR_NO_MEM;
        }
        uint8_t *buf = (uint8_t *)img_alloc((size_t)st.st_size, "png file buffer");
        if (!buf) { ESP_LOGE(TAG, "PNG file malloc(%ld) failed", (long)st.st_size); return ESP_ERR_NO_MEM; }
        FILE *pf = fopen(input_path, "rb");
        if (!pf) { free(buf); return ESP_FAIL; }
        size_t got = fread(buf, 1, (size_t)st.st_size, pf);
        fclose(pf);

        if (got >= 24) {
            unsigned pw_hdr = ((unsigned)buf[16] << 24) | ((unsigned)buf[17] << 16) |
                              ((unsigned)buf[18] << 8)  | (unsigned)buf[19];
            unsigned ph_hdr = ((unsigned)buf[20] << 24) | ((unsigned)buf[21] << 16) |
                              ((unsigned)buf[22] << 8)  | (unsigned)buf[23];
            if (pw_hdr == 0 || ph_hdr == 0 ||
                pw_hdr > PNG_MAX_SIDE_PIXELS || ph_hdr > PNG_MAX_SIDE_PIXELS ||
                (uint64_t)pw_hdr * (uint64_t)ph_hdr > PNG_MAX_DECODE_PIXELS) {
                ESP_LOGE(TAG, "PNG dimensions rejected before decode: %ux%u",
                         pw_hdr, ph_hdr);
                free(buf);
                return ESP_ERR_INVALID_SIZE;
            }
        }

        unsigned char *rgba = NULL;
        unsigned pw = 0, ph = 0;
        unsigned perr = lodepng_decode32(&rgba, &pw, &ph, buf, got);
        free(buf);
        if (perr || !rgba) {
            ESP_LOGE(TAG, "PNG decode error %u: %s", perr, lodepng_error_text(perr));
            free(rgba);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "PNG decoded: %ux%u (free heap: %lu)", pw, ph,
                 (unsigned long)esp_get_free_heap_size());
        return png_decode_to_epd(rgba, pw, ph, out_path);
    }

    if (magic[0] == 0x42 && magic[1] == 0x4D) {
        FILE *bf = fopen(input_path, "rb");
        if (!bf) return ESP_FAIL;
        esp_err_t ret = bmp_decode_core(bf, NULL, 0, out_path);
        fclose(bf);
        return ret;
    }

    ESP_LOGE(TAG, "Unknown format in file %s (magic: %02X %02X %02X %02X)",
             input_path, magic[0], magic[1], magic[2], magic[3]);
    return ESP_ERR_NOT_SUPPORTED;
}
