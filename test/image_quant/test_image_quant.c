#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Include the whole production file, including its static quantizers. */
#include "../../main/image_convert.c"

static epd_panel_t panel;
static int width = 400, height = 300;
static bool accent_red = true, accent_yellow = true;
static size_t fail_size;
static unsigned failed_allocs, fs_allocs;
static char logs[16384];
static size_t log_len;

epd_panel_t epd_get_panel(void) { return panel; }
int epd_width(void) { return width; }
int epd_height(void) { return height; }
bool epd_has_red(void) { return accent_red; }
bool epd_has_yellow(void) { return accent_yellow; }

void host_log(const char *tag, const char *fmt, ...)
{
    (void)tag;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(logs + log_len, sizeof(logs) - log_len, fmt, args);
    va_end(args);
    if (n > 0 && (size_t)n < sizeof(logs) - log_len) log_len += (size_t)n;
}

void *heap_caps_malloc(size_t size, unsigned caps)
{
    (void)caps;
    if (size == ((size_t)width + 2) * sizeof(fs_err_t)) fs_allocs++;
    if (fail_size && size == fail_size) { failed_allocs++; return NULL; }
    return malloc(size);
}

typedef struct { uint8_t *p[5]; size_t bytes; } planes_t;

static planes_t make_planes(int w, int h)
{
    planes_t out = { .bytes = (size_t)(w / 8) * h };
    for (int i = 0; i < 5; i++) {
        out.p[i] = malloc(out.bytes);
        assert(out.p[i]);
        memset(out.p[i], i ? 0 : 255, out.bytes);
    }
    return out;
}

static void free_planes(planes_t *p)
{ for (int i = 0; i < 5; i++) free(p->p[i]); }

/* Host allocation only; PNG decoding and framebuffer packing remain production code. */
fb_t *fb_create(void)
{
    fb_t *fb = calloc(1,sizeof(*fb));
    if (!fb) return NULL;
    fb->width = width; fb->height = height;
    fb->row_bytes = width/8; fb->plane_bytes = fb->row_bytes*height;
    uint8_t **planes[] = {&fb->black,&fb->red,&fb->yellow,&fb->blue,&fb->green};
    bool present[] = {true,accent_red,accent_yellow,image_panel_is_e6_6c(),image_panel_is_e6_6c()};
    for (int i = 0; i < 5; ++i) if (present[i]) {
        *planes[i] = malloc(fb->plane_bytes);
        if (!*planes[i]) { fb_destroy(fb); return NULL; }
        memset(*planes[i],i ? 0 : 255,fb->plane_bytes);
    }
    return fb;
}

void fb_destroy(fb_t *fb)
{
    if (!fb) return;
    free(fb->black); free(fb->red); free(fb->yellow); free(fb->blue); free(fb->green); free(fb);
}

static void valid_planes(const planes_t *p)
{
    for (size_t j = 0; j < p->bytes; j++) {
        unsigned colors = 0;
        for (int i = 1; i < 5; i++) {
            assert(!(colors & p->p[i][j]));
            assert(!(p->p[i][j] & (uint8_t)~p->p[0][j]));
            colors |= p->p[i][j];
        }
    }
}

static planes_t quantize(const uint8_t *rgb, int w, int h, bool bayer)
{
    planes_t out = make_planes(w, h);
    fs_err_t *ec = calloc((size_t)w + 2, sizeof(*ec));
    fs_err_t *en = calloc((size_t)w + 2, sizeof(*en));
    assert(ec && en);
    const image_quant_profile_t *q = image_quant_profile_current();
    bool e6 = image_panel_is_e6_6c();
    for (int y = 0; y < h; y++) {
        const uint8_t *row = rgb + (size_t)y * w * 3;
        memset(en, 0, ((size_t)w + 2) * sizeof(*en));
        if (bayer) {
            for (int x = 0; x < w; x++)
                dither_pixel_bayer(out.p[0], out.p[1], out.p[2], out.p[3], out.p[4],
                    x, y, w, row[x*3], row[x*3+1], row[x*3+2],
                    accent_red, accent_yellow, e6, e6, q);
        } else if (e6) {
            e6_37_fs_dither_row(row, ec, en, out.p[0], out.p[1], out.p[2],
                                out.p[3], out.p[4], y, w);
        } else {
            fs_dither_row(row, ec, en, out.p[0], out.p[1], out.p[2],
                            y, w, accent_red, accent_yellow, q);
        }
        fs_err_t *tmp = ec; ec = en; en = tmp;
    }
    valid_planes(&out);
    free(ec); free(en);
    return out;
}

static uint64_t fingerprint(const planes_t *p)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (int i = 0; i < 5; i++)
        for (size_t j = 0; j < p->bytes; j++) {
            hash ^= p->p[i][j];
            hash *= UINT64_C(1099511628211);
        }
    return hash;
}

static uint8_t *mixed_image(int w, int h)
{
    uint8_t *rgb = malloc((size_t)w * h * 3);
    assert(rgb);
    uint32_t state = 0x79665;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *p = rgb + ((size_t)y * w + x) * 3;
            for (int c = 0; c < 3; c++) {
                state = state * 1664525u + 1013904223u;
                p[c] = (uint8_t)(state >> 24);
            }
            if (y < h / 4) memset(p, x * 255 / (w - 1), 3);
            else if (y < h / 2) { p[0] = 255; p[1] = (x & 1) ? 255 : 0; p[2] = 0; }
            if (x == 0 || y == 0 || x == w-1 || y == h-1) memset(p, 0, 3);
        }
    return rgb;
}

static int pixel_color(const planes_t *p, int w, int x, int y)
{
    size_t pos = (size_t)y * (w / 8) + (x >> 3);
    uint8_t bit = 0x80u >> (x & 7);
    if (!(p->p[0][pos] & bit)) return 0;
    for (int i = 1; i < 5; i++) if (p->p[i][pos] & bit) return i + 1;
    return 1;
}

static unsigned white_pixels(const planes_t *p)
{
    unsigned n = 0;
    for (size_t i = 0; i < p->bytes; i++) {
        for (int c = 1; c < 5; c++) assert(p->p[c][i] == 0);
        n += (unsigned)__builtin_popcount(p->p[0][i]);
    }
    return n;
}

static void same_planes(const planes_t *a, const planes_t *b)
{
    assert(a->bytes == b->bytes);
    for (int i = 0; i < 5; i++) assert(!memcmp(a->p[i], b->p[i], a->bytes));
}

static void test_legacy(void)
{
    uint8_t *rgb = mixed_image(400, 300);
    /* Recorded from the working production file before this change, all five
       planes in B/R/Y/blue/green order. No duplicate reference quantizer. */
    const uint64_t golden[2][2] = {
        { UINT64_C(0xd8c274c422e73aa5), UINT64_C(0x3fb2ba03812418a5) },
        { UINT64_C(0x4020abb2cb48480a), UINT64_C(0x342923d0f10c46a8) }
    };
    for (int id = 0; id < EPD_PANEL_COUNT; id++) {
        if (id == 34 || id == 37) continue;
        panel = (epd_panel_t)id;
        assert(image_quant_profile_current() == &s_quant_default);
        assert(image_quant_profile_current()->neutral_chroma_threshold == 0);
        for (int mode = 0; mode < 2; mode++) {
            planes_t p = quantize(rgb, 400, 300, mode);
            assert(fingerprint(&p) == golden[image_panel_is_e6_6c()][mode]);
            free_planes(&p);
        }
    }
    /* Same 400x300 geometry and color capabilities do not select ID37. */
    panel = EPD_PANEL_REF_JD79668_42_BWRY;
    planes_t improved = quantize(rgb, 400, 300, false);
    assert(fingerprint(&improved) != golden[0][0]);
    free_planes(&improved);
    free(rgb);
    puts("PASS: other panel IDs and E6 match pre-change FS/Bayer fingerprints");
}

static void test_profile(int id)
{
    panel = (epd_panel_t)id;
    const image_quant_profile_t *q = image_quant_profile_current();
    assert(q == (id == 34 ? &s_quant_jd79665_75 : &s_quant_jd79668_42));
    assert(q->bw_luma_threshold == 104);
    assert(q->fs_error_gain_q8 == (id == 34 ? 192 : 256));
    assert(q->red_gain_q8 == (id == 34 ? 280 : 256));
    assert(q->yellow_gain_q8 == (id == 34 ? 288 : 256));
    assert(q->neutral_chroma_threshold == 20);
    assert(q->neutral_luma_threshold == 128);
    assert(q->neutral_fs_error_gain_q8 == 256);
    assert(image_quant_source_is_neutral(120, 100, 100, q));
    assert(!image_quant_source_is_neutral(121, 100, 100, q));
    log_len = 0; logs[0] = 0;
    image_quant_log_profile("host", q);
    assert(strstr(logs, id == 34 ? "JD79665 ID34 7.5" : "JD79668 ID37 4.2"));
    assert(strstr(logs, "simulated gray (B/W dots, not native grayscale)"));
    assert(strstr(logs, "source chroma<=20 neutral bw=128 serpentine fs=256/256"));

    const uint8_t samples[4][3] = { {127,127,127}, {128,128,128}, {120,100,100}, {121,100,100} };
    const int expected[4] = {0, 1, 0, 1};
    for (int i = 0; i < 4; i++) {
        uint8_t row[24];
        memset(row, 255, sizeof(row));
        memcpy(row, samples[i], 3);
        planes_t p = quantize(row, 8, 1, false);
        assert(pixel_color(&p, 8, 0, 0) == expected[i]);
        free_planes(&p);
    }
}

static void test_gray_patches(void)
{
    enum { W = 256, H = 128 };
    uint8_t *rgb = malloc(W * H * 3);
    assert(rgb);
    for (int mode = 0; mode < 2; mode++) {
        unsigned previous = 0;
        double worst = 0;
        for (int gray = 0; gray < 256; gray++) {
            memset(rgb, gray, W * H * 3);
            planes_t p = quantize(rgb, W, H, mode);
            unsigned count = white_pixels(&p);
            double simulated_luma = 255.0 * count / (W * H);
            double error = simulated_luma - gray;
            if (error < 0) error = -error;
            if (error > worst) worst = error;
            if (count < previous || error > 3.0)
                fprintf(stderr, "ID%d mode%d gray%d white%u previous%u luma%.3f\n",
                        panel, mode, gray, count, previous, simulated_luma);
            assert(count >= previous);
            assert(error <= 3.0);
            if (gray == 0) assert(count == 0);
            if (gray == 255) assert(count == W * H);
            previous = count;
            free_planes(&p);
        }
        printf("PASS: ID%d %s all 256 gray levels monotonic, max luminance error %.3f/255\n",
               panel, mode ? "Bayer" : "FS", worst);
    }
    free(rgb);
}

static void test_diffusion_and_endpoints(void)
{
    const image_quant_profile_t *q = image_quant_profile_current();
    for (int y = 0; y < 2; y++) {
        uint8_t rgb[8 * 3];
        memset(rgb, 255, sizeof(rgb));
        memset(rgb + 3 * 3, 100, 3);
        fs_err_t ec[10] = {0}, en[10] = {0};
        planes_t p = make_planes(8, 2);
        fs_dither_row(rgb, ec, en, p.p[0], p.p[1], p.p[2], y, 8, true, true, q);
        int dir = y ? -1 : 1;
        assert(pixel_color(&p, 8, 3, y) == 0);
        assert(ec[4 + dir].r == 700);
        assert(en[4 - dir].r == 300);
        assert(en[4].r == 500);
        assert(en[4 + dir].r == 100);
        free_planes(&p);
    }
    for (int mode = 0; mode < 2; mode++) {
        uint8_t rgb[8 * 3];
        fs_err_t ec[10] = {0}, en[10] = {0};
        planes_t p = make_planes(8, 2);
        memset(p.p[1], 255, p.bytes);
        memset(p.p[2], 255, p.bytes);
        for (int x = 0; x < 8; x++) {
            memset(rgb + 3*x, (x & 1) ? 255 : 0, 3);
            ec[x+1].r = (x & 1) ? -4080 : 4080;
            ec[x+1].g = 30000; ec[x+1].b = -30000;
        }
        for (int y = 0; y < 2; y++) {
            if (mode) {
                for (int x = 0; x < 8; x++)
                    dither_pixel_bayer(p.p[0], p.p[1], p.p[2], NULL, NULL, x, y, 8,
                        rgb[x*3], rgb[x*3+1], rgb[x*3+2], true, true, false, false, q);
            } else {
                fs_dither_row(rgb, ec, en, p.p[0], p.p[1], p.p[2], y, 8, true, true, q);
            }
            for (int x = 0; x < 8; x++) assert(pixel_color(&p, 8, x, y) == (x & 1));
        }
        valid_planes(&p);
        for (int x = 0; x < 10; x++) assert(en[x].r == 0);
        free_planes(&p);
    }
    /* Strong incoming error cannot change source neutral classification. */
    uint8_t rgb[8 * 3];
    fs_err_t ec[10] = {0}, en[10] = {0};
    planes_t p = make_planes(8, 1);
    for (int x = 0; x < 8; x++) {
        rgb[x*3] = 120; rgb[x*3+1] = 100; rgb[x*3+2] = 100;
        ec[x+1].r = 1000; ec[x+1].g = -1000; ec[x+1].b = -1000;
    }
    fs_dither_row(rgb, ec, en, p.p[0], p.p[1], p.p[2], 0, 8, true, true, q);
    for (int x = 0; x < 8; x++) assert(pixel_color(&p, 8, x, 0) < 2);
    free_planes(&p);
    printf("PASS: ID%d mirrored 7/3/5/1 full gain, source chroma gate, exact endpoints\n", panel);
}

static uint8_t *border_and_color_image(int w, int h)
{
    uint8_t *rgb = mixed_image(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *p = rgb + ((size_t)y * w + x) * 3;
            int gray = 20 + x * 215 / (w - 1);
            switch (x % 4) {
            case 0: p[0] = 255; p[1] = p[2] = 0; break;
            case 2: p[0] = p[1] = 255; p[2] = 0; break;
            default: p[0] = gray + 10; p[1] = gray; p[2] = gray - 10; break;
            }
            if (x == 0 || y == 0 || x == w-1 || y == h-1) memset(p, 0, 3);
            else if (x == 1 || y == 1 || x == w-2 || y == h-2) memset(p, 255, 3);
        }
    return rgb;
}

static void test_borders_and_colors(void)
{
    enum { W = 256, H = 64 };
    uint8_t *rgb = border_and_color_image(W, H);
    for (int mode = 0; mode < 2; mode++) {
        planes_t p = quantize(rgb, W, H, mode);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                const uint8_t *src = rgb + (y * W + x) * 3;
                int color = pixel_color(&p, W, x, y);
                if (image_quant_source_is_neutral(src[0], src[1], src[2], image_quant_profile_current())) {
                    assert(color < 2);
                    if (src[0] == 0) assert(color == 0);
                    if (src[0] == 255) assert(color == 1);
                } else if (src[1] == 0) assert(color != 3);
                else assert(color != 2);
            }
        free_planes(&p);
        for (int yellow = 0; yellow < 2; yellow++) {
            for (int i = 0; i < W * H; i++) {
                rgb[i*3] = 255; rgb[i*3+1] = yellow ? 255 : 0; rgb[i*3+2] = 0;
            }
            p = quantize(rgb, W, H, mode);
            unsigned dots = 0;
            for (size_t i = 0; i < p.bytes; i++) {
                dots += (unsigned)__builtin_popcount(p.p[yellow ? 2 : 1][i]);
                assert(p.p[yellow ? 1 : 2][i] == 0);
            }
            assert(dots >= W * H * 95 / 100);
            if (panel == 34) assert(dots == W * H);
            free_planes(&p);
        }
        free(rgb);
        rgb = border_and_color_image(W, H);
    }
    free(rgb);
    printf("PASS: ID%d neutral ramp next to red/yellow, sharp borders, saturated coverage\n", panel);
}

static planes_t read_raw(const char *path, int w, int h)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    planes_t p = make_planes(w, h);
    bool present[] = {true, accent_red, accent_yellow, image_panel_is_e6_6c(), image_panel_is_e6_6c()};
    for (int i = 0; i < 5; i++)
        if (present[i]) assert(fread(p.p[i], 1, p.bytes, f) == p.bytes);
    assert(fgetc(f) == EOF);
    assert(fclose(f) == 0);
    assert(remove(path) == 0);
    valid_planes(&p);
    return p;
}

static planes_t convert_png(const uint8_t *rgb, bool force_bayer, bool via_file, bool prequantized)
{
    size_t pixels = (size_t)width * height;
    uint8_t *rgba = malloc(pixels * 4);
    assert(rgba);
    for (size_t i = 0; i < pixels; i++) {
        memcpy(rgba + i*4, rgb + i*3, 3);
        rgba[i*4+3] = 255;
    }
    bool threshold_candidate = png_looks_bw_preprocessed(rgba, width, height);
    unsigned char *png = NULL;
    size_t len = 0;
    assert(lodepng_encode32(&png, &len, rgba, width, height) == 0);
    free(rgba);
    assert(len > 8 && !memcmp(png, "\x89PNG\r\n\x1a\n", 8));
    char out[128], in[128];
    snprintf(out, sizeof(out), "/tmp/msp-quant-%ld.raw", (long)getpid());
    snprintf(in, sizeof(in), "/tmp/msp-quant-%ld.png", (long)getpid());
    fail_size = force_bayer ? ((size_t)width + 2) * sizeof(fs_err_t) : 0;
    failed_allocs = fs_allocs = 0;
    log_len = 0; logs[0] = 0;
    esp_err_t result;
    if (via_file) {
        FILE *f = fopen(in, "wb");
        assert(f && fwrite(png, 1, len, f) == len);
        assert(fclose(f) == 0);
        result = image_convert_file_to_epd_raw(in, out);
        assert(remove(in) == 0);
    } else result = image_convert_png_to_epd_raw(png, len, out);
    assert(result == ESP_OK);
    if (prequantized) {
        assert(fs_allocs == 0 && failed_allocs == 0);
        assert(strstr(logs, "PNG prequantized passthrough:"));
        assert(!strstr(logs, "simulated gray"));
    } else {
        assert(fs_allocs == 2);
        assert(failed_allocs == (force_bayer ? 2u : 0u));
        assert(strstr(logs, "simulated gray"));
        assert(!strstr(logs, "PNG BW threshold passthrough:"));
        if (threshold_candidate) assert(strstr(logs, "Converted PNG ->"));
        if (force_bayer) assert(strstr(logs, "F-S alloc failed, using Bayer"));
    }
    fail_size = 0;
    free(png);
    return read_raw(out, width, height);
}

static void test_encoded_png(void)
{
    width = panel == 34 ? 800 : 400;
    height = panel == 34 ? 480 : 300;
    uint8_t *rgb = border_and_color_image(width, height);
    for (int mode = 0; mode < 2; mode++) {
        planes_t expected = quantize(rgb, width, height, mode);
        planes_t actual = convert_png(rgb, mode, mode, false);
        same_planes(&actual, &expected);
        free_planes(&actual); free_planes(&expected);
    }
    /* A small tonal ramp on white satisfies the old threshold-only heuristic. */
    memset(rgb, 255, (size_t)width * height * 3);
    uint8_t *rgba = malloc((size_t)width * height * 4);
    assert(rgba);
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            size_t i = (size_t)y * width + x;
            if (y >= 2 && y < 18) memset(rgb + i*3, x*255/(width-1), 3);
            memcpy(rgba + i*4, rgb + i*3, 3);
            rgba[i*4+3] = 255;
        }
    assert(png_looks_bw_preprocessed(rgba, width, height));
    free(rgba);
    for (int mode = 0; mode < 2; mode++) {
        planes_t expected = quantize(rgb, width, height, mode);
        planes_t actual = convert_png(rgb, mode, mode, false);
        same_planes(&actual, &expected);
        free_planes(&actual); free_planes(&expected);
    }
    /* Extremes previously accepted by the loose prequantized/threshold paths. */
    const uint8_t grays[] = { 1, 16, 36, 48, 64, 72, 104, 128, 183, 207, 235, 254 };
    for (size_t i = 0; i < sizeof(grays); i++) {
        memset(rgb, grays[i], (size_t)width * height * 3);
        for (int mode = 0; mode < 2; mode++) {
            planes_t expected = quantize(rgb, width, height, mode);
            planes_t actual = convert_png(rgb, mode, false, false);
            same_planes(&actual, &expected);
            free_planes(&actual); free_planes(&expected);
        }
    }
    const uint8_t palette[4][3] = { {0,0,0}, {255,255,255}, {255,0,0}, {255,255,0} };
    planes_t expected = make_planes(width, height);
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            int color = ((x * 17 + y * 31) ^ (x >> 2)) & 3;
            memcpy(rgb + ((size_t)y * width + x)*3, palette[color], 3);
            size_t byte = (size_t)y * (width / 8) + (x >> 3);
            uint8_t bit = 0x80u >> (x & 7);
            if (!color) expected.p[0][byte] &= (uint8_t)~bit;
            if (color >= 2) expected.p[color-1][byte] |= bit;
        }
    for (int mode = 0; mode < 2; mode++) {
        planes_t actual = convert_png(rgb, mode, mode, true);
        same_planes(&actual, &expected);
        free_planes(&actual);
    }
    /* Browser ink tokens must remain exact even for dense color halftones. */
    const uint8_t web_palette[4][3] = { {0,0,0}, {255,255,255}, {192,57,43}, {210,165,24} };
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            int color = ((x * 17 + y * 31) ^ (x >> 2)) & 3;
            memcpy(rgb + ((size_t)y * width + x)*3, web_palette[color], 3);
        }
    for (int mode = 0; mode < 2; mode++) {
        planes_t actual = convert_png(rgb, mode, mode, true);
        same_planes(&actual, &expected);
        free_planes(&actual);
    }
    free_planes(&expected);
    free(rgb);
    printf("PASS: ID%d real encoded PNG memory/file paths, exact passthrough, threshold-eligible ramp, FS allocation fallback\n", panel);
}

static void put_le32(uint8_t *p, uint32_t value)
{ for (int i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (i * 8)); }

static void test_bmp_and_jpeg_bands(void)
{
    width = 64; height = 67;
    uint8_t *rgb = border_and_color_image(width, height);
    /* A real bottom-up 24-bit BMP exercises the production decoder and packing. */
    size_t bmp_len = 54 + (size_t)width * height * 3;
    uint8_t *bmp = calloc(1, bmp_len);
    assert(bmp);
    bmp[0] = 'B'; bmp[1] = 'M';
    put_le32(bmp + 2, bmp_len); put_le32(bmp + 10, 54);
    put_le32(bmp + 14, 40); put_le32(bmp + 18, width); put_le32(bmp + 22, height);
    bmp[26] = 1; bmp[28] = 24;
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++)
            for (int c = 0; c < 3; c++)
                bmp[54 + ((size_t)(height - 1 - y) * width + x)*3+c] =
                    rgb[((size_t)y * width + x)*3 + 2-c];
    for (int mode = 0; mode < 2; mode++) {
        planes_t expected = quantize(rgb, width, height, mode);
        char path[128];
        snprintf(path, sizeof(path), "/tmp/msp-quant-bmp-%ld.raw", (long)getpid());
        fail_size = mode ? ((size_t)width + 2) * sizeof(fs_err_t) : 0;
        failed_allocs = 0;
        assert(image_convert_bmp_to_epd_raw(bmp, bmp_len, path) == ESP_OK);
        assert(failed_allocs == (mode ? 2u : 0u));
        fail_size = 0;
        planes_t actual = read_raw(path, width, height);
        same_planes(&actual, &expected);
        free_planes(&actual);

        actual = make_planes(width, height);
        jpeg_ctx_t ctx = {0};
        ctx.out = (epd_out_t){
            .black = actual.p[0], .red = actual.p[1], .yellow = actual.p[2],
            .w = width, .h = height, .sw = width, .sh = height,
            .fit = {0, 0, width, height}, .band_w = width, .band_h = 16, .band_top = -1,
            .has_red = true, .has_yellow = true, .quant_profile = image_quant_profile_current(),
            .band = malloc((size_t)width * 16 * 3), .rgb_row = malloc((size_t)width * 3),
            .ec = calloc((size_t)width + 2, sizeof(fs_err_t)),
            .en = calloc((size_t)width + 2, sizeof(fs_err_t))
        };
        assert(ctx.out.band && ctx.out.rgb_row && ctx.out.ec && ctx.out.en);
        memset(ctx.out.band, 255, (size_t)width * 16 * 3);
        JDEC jd = { .device = &ctx };
        /* Odd band height catches resetting serpentine direction at a band edge. */
        for (int top = 0; top < height; top += 15) {
            int rows = height - top < 15 ? height - top : 15;
            for (int left = 0; left < width; left += 16) {
                uint8_t block[16 * 16 * 3];
                for (int y = 0; y < rows; y++)
                    memcpy(block + y * 16 * 3, rgb + ((size_t)(top+y)*width+left)*3, 16*3);
                JRECT rect = { .left = left, .right = left+15, .top = top, .bottom = top+rows-1 };
                assert((mode ? jpeg_outfunc_bayer(&jd, block, &rect) :
                               jpeg_outfunc_fs(&jd, block, &rect)) == 1);
            }
        }
        if (!mode) fs_flush_band(&ctx.out);
        valid_planes(&actual);
        same_planes(&actual, &expected);
        free(ctx.out.band); free(ctx.out.rgb_row); free(ctx.out.ec); free(ctx.out.en);
        free_planes(&actual); free_planes(&expected);
    }
    free(bmp); free(rgb);
    printf("PASS: ID%d BMP decode/fallback and JPEG MCU callbacks across odd band boundaries\n", panel);
}

static void test_workspace_ink_passthrough(void)
{
    const struct { int id, w, h; bool red, yellow; } cases[] = {
        {47,128,250,false,false}, {19,152,522,true,false}, {20,792,272,true,false},
        {10,648,480,true,false}, {34,800,480,true,true}, {37,400,300,true,true},
        {44,960,672,true,true}, {43,792,528,true,true}, {17,128,296,true,false},
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        panel = cases[i].id; width = cases[i].w; height = cases[i].h;
        accent_red = cases[i].red; accent_yellow = cases[i].yellow;
        bool e6 = image_panel_is_e6_6c();
        const uint8_t colors[][3] = {{0,0,0},{255,255,255},
            {e6?196:192,e6?36:57,e6?28:43},{e6?255:210,e6?232:165,e6?0:24},
            {20,80,185},{40,150,90}};
        int count = e6 ? 6 : accent_yellow ? 4 : accent_red ? 3 : 2;
        uint8_t *rgb = malloc((size_t)width*height*3); assert(rgb);
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
            memcpy(rgb + ((size_t)y*width+x)*3, colors[(x+y)%count], 3);
        planes_t p = convert_png(rgb, false, true, true);
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            int source_y = panel == EPD_PANEL_HINK_SSD1619_29_BWR ? height-1-y : y;
            assert(pixel_color(&p,width,x,y) == (x+source_y)%count);
        }
        free_planes(&p); free(rgb);
    }
    puts("PASS: workspace ink PNG passthrough, geometry, packing and existing orientation (9 panels)");
}

static void test_message_yellow_passthrough(void)
{
    const struct { int id, w, h; } cases[] = {{34,800,480},{37,400,300},{43,792,528},{44,960,672}};
    const uint8_t yellow[] = {255,232,0};
    for (size_t n = 0; n < sizeof(cases)/sizeof(cases[0]); ++n) {
        panel = cases[n].id; width = cases[n].w; height = cases[n].h;
        accent_red = accent_yellow = true;
        uint8_t *rgb = malloc((size_t)width*height*3); assert(rgb);
        memset(rgb,255,(size_t)width*height*3);
        for (int y = 8; y < height-8; ++y) for (int x = 8; x < width-8; ++x)
            if (x%11 < 2 || y%17 < 2) memcpy(rgb + ((size_t)y*width+x)*3,yellow,3);
        planes_t p = convert_png(rgb,false,true,true);
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            bool stroke = x>=8 && x<width-8 && y>=8 && y<height-8 && (x%11<2 || y%17<2);
            assert(pixel_color(&p,width,x,y) == (stroke ? 3 : 1));
        }
        size_t pixels = (size_t)width*height;
        uint8_t *rgba = malloc(pixels*4); assert(rgba);
        for (size_t i = 0; i < pixels; ++i) { memcpy(rgba+i*4,rgb+i*3,3); rgba[i*4+3]=255; }
        unsigned char *png = NULL; size_t png_len = 0;
        assert(lodepng_encode32(&png,&png_len,rgba,width,height) == 0);
        fb_t *fb = NULL; assert(image_convert_to_epd_fb(png,png_len,&fb) == ESP_OK);
        assert(fb && fb->yellow && fb->width == width && fb->height == height);
        const uint8_t *actual[] = {fb->black,fb->red,fb->yellow,fb->blue,fb->green};
        for (int i = 0; i < 5; ++i) if (actual[i]) assert(memcmp(actual[i],p.p[i],p.bytes) == 0);
        fb_destroy(fb); free(png); free(rgba);
        free_planes(&p); free(rgb);
    }
    puts("PASS: browser yellow message ink passes PNG decode to raw and framebuffer unchanged");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_legacy();
    for (int id = 34; id <= 37; id += 3) {
        test_profile(id);
        test_gray_patches();
        test_diffusion_and_endpoints();
        test_borders_and_colors();
        test_encoded_png();
        test_bmp_and_jpeg_bands();
    }
    test_workspace_ink_passthrough();
    test_message_yellow_passthrough();
    puts("All image quantization host tests passed.");
    return 0;
}
