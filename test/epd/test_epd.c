#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>

static void tracked_free(void *ptr);
#define free tracked_free
/* Compile production code directly: callbacks, packing, BUSY and all public paths. */
#include "../../main/epd_stub.c"
#undef free

#define SPI_FAULT 0x7001
#define GPIO_FAULT 0x7002
typedef struct { void *ptr; size_t size; unsigned caps; } allocation_t;
static allocation_t allocations[256];
static int alloc_count, alloc_calls, fail_alloc_at;
static int spi_calls, fail_spi_at, gpio_calls, fail_gpio_at;
static int mutex_depth, complete_count, resets, reads, refreshes;
static int bus_calls, add_calls, fail_bus, fail_add;
static int dc_level, last_cmd, saved_panel = -1, saved_idle = -1, nvs_writes;
static int64_t now_ms, busy_until, busy_from;
static enum { BUSY_AUTO, BUSY_IDLE, BUSY_STUCK } busy_mode;
static int stuck_on_refresh, missing_on_refresh, stuck_on_poweroff;
static int injected, calls_after_error;
static char last_busy_log[768];
static unsigned current_epoch;
static bool change_epoch_on_take;

static void tracked_free(void *ptr)
{
    if (!ptr) return;
    for (int i = 0; i < alloc_count; ++i) {
        if (allocations[i].ptr == ptr) {
            free(ptr);
            allocations[i] = allocations[--alloc_count];
            return;
        }
    }
    assert(!"unowned or double free");
}

void *heap_caps_malloc(size_t size, unsigned caps)
{
    if (++alloc_calls == fail_alloc_at) return NULL;
    void *ptr = malloc(size);
    assert(ptr && alloc_count < 256);
    allocations[alloc_count++] = (allocation_t){ptr, size, caps};
    return ptr;
}
void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    void *ptr = heap_caps_malloc(count * size, caps);
    if (ptr) memset(ptr, 0, count * size);
    return ptr;
}
bool esp_ptr_dma_capable(const void *ptr)
{
    for (int i = 0; i < alloc_count; ++i)
        if (allocations[i].ptr == ptr) return (allocations[i].caps & MALLOC_CAP_DMA) != 0;
    return false;
}
const char *esp_err_to_name(esp_err_t err)
{
    if (err == ESP_OK) return "ESP_OK";
    if (err == ESP_ERR_TIMEOUT) return "ESP_ERR_TIMEOUT";
    return "INJECTED_OR_OTHER_ERROR";
}
void fake_log(const char *tag, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if (strstr(format, "BUSY panel=")) vsnprintf(last_busy_log, sizeof(last_busy_log), format, args);
    va_end(args);
}
void vTaskDelay(TickType_t ticks) { now_ms += ticks; }
TickType_t xTaskGetTickCount(void) { return (TickType_t)now_ms; }
int64_t esp_timer_get_time(void) { return now_ms * 1000; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t handle, TickType_t timeout)
{
    assert(handle && !mutex_depth); mutex_depth++;
    if (change_epoch_on_take) current_epoch++;
    return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t handle)
{
    assert(handle && mutex_depth == 1); mutex_depth--; return pdTRUE;
}
esp_err_t gpio_config(const gpio_config_t *config)
{
    if (++gpio_calls == fail_gpio_at) { injected = 1; return GPIO_FAULT; }
    return ESP_OK;
}
esp_err_t gpio_set_level(int pin, int level)
{
    if (injected) calls_after_error++;
    if (++gpio_calls == fail_gpio_at) { injected = 1; return GPIO_FAULT; }
    if (pin == PIN_EPD_DC) dc_level = level;
    if (pin == PIN_EPD_RST && level == 0) { resets++; busy_until = 0; }
    return ESP_OK;
}
int gpio_get_level(int pin)
{
    reads++;
    int idle = busy_idle_effective();
    if (busy_mode == BUSY_IDLE) return idle;
    if (busy_mode == BUSY_STUCK) return !idle;
    return now_ms >= busy_from && now_ms < busy_until ? !idle : idle;
}
esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *config, int dma)
{
    bus_calls++;
    return fail_bus ? SPI_FAULT : ESP_OK;
}
esp_err_t spi_bus_add_device(int host, const spi_device_interface_config_t *config, spi_device_handle_t *handle)
{
    add_calls++;
    if (fail_add) return SPI_FAULT;
    *handle = (void *)2;
    return ESP_OK;
}
esp_err_t spi_device_transmit(spi_device_handle_t handle, spi_transaction_t *tx)
{
    assert(handle && tx->tx_buffer);
    if (injected) calls_after_error++;
    if (++spi_calls == fail_spi_at) { injected = 1; return SPI_FAULT; }
    /* Read all payload bytes so sanitizers validate production buffer bounds. */
    volatile uint8_t checksum = 0;
    for (size_t i = 0; i < tx->length / 8; ++i) checksum ^= ((const uint8_t *)tx->tx_buffer)[i];
    (void)checksum;
    if (dc_level == 0) {
        last_cmd = *(const uint8_t *)tx->tx_buffer;
        bool update = last_cmd == 0x12 || last_cmd == 0x20 || last_cmd == 0x17 || last_cmd == 0xA0;
        if (update) {
            refreshes++;
            if (stuck_on_refresh == refreshes) busy_mode = BUSY_STUCK;
            if (missing_on_refresh == refreshes) busy_mode = BUSY_IDLE;
        }
        if (last_cmd == 0x02 && stuck_on_poweroff) busy_mode = BUSY_STUCK;
        if (update || last_cmd == 0x04 || last_cmd == 0x02) {
            busy_from = now_ms + 20;
            busy_until = now_ms + 1000;
        }
    }
    return ESP_OK;
}
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle) { *handle = 1; return ESP_OK; }
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    int result = strcmp(key, "panel") == 0 ? saved_panel : saved_idle;
    if (result < 0) return ESP_ERR_NVS_NOT_FOUND;
    *value = (uint8_t)result; return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    nvs_writes++;
    if (!strcmp(key, "panel")) saved_panel = value; else saved_idle = value;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) { return ESP_OK; }
void nvs_close(nvs_handle_t handle) {}
void power_mgr_note_epd_refresh_complete(void) { complete_count++; }
bool display_policy_epoch_is_current(unsigned epoch)
{
    assert(mutex_depth == 1);
    return epoch == current_epoch;
}
void fb_release_reserved_planes(void) {}
fb_t *fb_create(void)
{
    fb_t *fb = heap_caps_calloc(1, sizeof(*fb), MALLOC_CAP_8BIT);
    if (!fb) return NULL;
    fb->width = epd_width(); fb->height = epd_height();
    fb->row_bytes = fb->width / 8; fb->plane_bytes = epd_plane_bytes();
    fb->black = heap_caps_malloc(fb->plane_bytes, MALLOC_CAP_8BIT);
    if (!fb->black) { tracked_free(fb); return NULL; }
    memset(fb->black, 0xFF, fb->plane_bytes);
    return fb;
}
void fb_destroy(fb_t *fb) { if (fb) { tracked_free(fb->black); tracked_free(fb); } }
esp_err_t fb_import(fb_t *fb, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_FAIL;
    size_t size = fread(fb->black, 1, fb->plane_bytes, f);
    fclose(f);
    return size == (size_t)fb->plane_bytes ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
void fb_fill_rect(fb_t *fb, int x, int y, int w, int h, fb_color_t color)
{
    for (int yy = y; yy < y + h; ++yy) for (int xx = x; xx < x + w; ++xx) {
        if (xx < 0 || yy < 0 || xx >= fb->width || yy >= fb->height) continue;
        int off = yy * fb->row_bytes + xx / 8;
        uint8_t bit = 0x80 >> (xx & 7);
        if (color == COLOR_BLACK) fb->black[off] &= ~bit; else fb->black[off] |= bit;
    }
}
void fb_rect(fb_t *fb, int x, int y, int w, int h, fb_color_t c)
{
    fb_fill_rect(fb,x,y,w,1,c); fb_fill_rect(fb,x,y+h-1,w,1,c);
    fb_fill_rect(fb,x,y,1,h,c); fb_fill_rect(fb,x+w-1,y,1,h,c);
}
void fb_hline(fb_t *fb, int x, int y, int w, fb_color_t c) { fb_fill_rect(fb,x,y,w,1,c); }
int ui_fixed_text_width(const fb_t *fb, const char *text, int scale) { return (int)strlen(text)*6*scale; }
int ui_draw_fixed_text(fb_t *fb, int x, int y, const char *text, fb_color_t color, int scale)
{
    fb_fill_rect(fb,x,y,ui_fixed_text_width(fb,text,scale),8*scale,color);
    return ui_fixed_text_width(fb,text,scale);
}

static void reset_fake(epd_panel_t panel)
{
    previous_bw_release();
    assert(!alloc_count && !mutex_depth);
    s_panel = panel; s_busy_idle_level = -1; s_panel_seq_done = false;
    s_spi = (void *)2; s_spi_bus_initialized = true; s_epd_mutex = (void *)1; s_epd_ready = true;
    s_uc8176_register_lut_active = s_uc8179_register_lut_active = false;
    atomic_store(&s_force_full_refresh_next, false);
    alloc_calls = fail_alloc_at = spi_calls = fail_spi_at = gpio_calls = fail_gpio_at = 0;
    resets = reads = refreshes = complete_count = bus_calls = add_calls = fail_bus = fail_add = 0;
    injected = calls_after_error = stuck_on_refresh = missing_on_refresh = stuck_on_poweroff = 0;
    now_ms = busy_until = busy_from = 0; busy_mode = BUSY_AUTO;
    saved_panel = saved_idle = -1; nvs_writes = 0;
    last_busy_log[0] = 0;
    current_epoch = 42; change_epoch_on_take = false;
}
static void assert_failed(esp_err_t actual, esp_err_t expected)
{
    assert(actual == expected);
    assert(!s_panel_seq_done && !s_previous_bw_valid && !s_uc8179_red_signature_valid);
    assert(atomic_load(&s_force_full_refresh_next));
    assert(!mutex_depth && !complete_count && !calls_after_error);
}
static void test_polarity_and_name(void)
{
    reset_fake(EPD_PANEL_VISIONPEAK_EI0583T5PCZ_BW);
    assert(epd_panel_default_busy_idle(s_panel) == 1 && epd_busy_idle_level() == 1);
    for (int level = 0; level <= 1; level++) {
        saved_panel = s_panel; saved_idle = level;
        epd_load_panel_from_nvs();
        assert(epd_busy_idle_level() == level && !nvs_writes);
        epd_panel_t panel; int idle;
        assert(epd_get_saved_panel_config(&panel,&idle) == ESP_OK && panel == s_panel && idle == level);
        assert(epd_save_panel_config(panel,level) == ESP_OK);
        assert(epd_set_busy_idle(level) == ESP_OK && epd_busy_idle_level() == level);
        nvs_writes = 0;
    }
    assert(!strcmp(epd_panel_name(s_panel), panel_desc()->name));
    assert(!strcmp(epd_panel_name((epd_panel_t)-1), "Unknown panel"));
    uint8_t packed[4]; uc8159_interleave_color(packed,0xA5,0);
    assert(packed[0] == 0x30 && packed[1] == 0x30 && packed[2] == 0x03 && packed[3] == 0x03);
}
static void test_busy_recovery(void)
{
    reset_fake(EPD_PANEL_VISIONPEAK_EI0583T5PCZ_BW);
    busy_mode = BUSY_IDLE;
    assert(epd_wait_ready("refresh",true) == ESP_ERR_TIMEOUT);
    assert(now_ms == 800 && strstr(last_busy_log,"phase=0"));
    busy_mode = BUSY_STUCK;
    assert(epd_wait_ready("refresh",true) == ESP_ERR_TIMEOUT);
    int old_reads = reads;
    assert(epd_wait_ready("refresh",true) == ESP_ERR_TIMEOUT && reads > old_reads);
    busy_mode = BUSY_AUTO; busy_from = now_ms + 40; busy_until = now_ms + 1000;
    assert(epd_wait_ready("refresh",true) == ESP_OK && s_last_wait_saw_busy_phase);
    assert(strstr(last_busy_log,"stage=refresh") && strstr(last_busy_log,"idle=1"));
    const epd_panel_t panels[] = { EPD_PANEL_VISIONPEAK_EI0583T5PCZ_BW, EPD_PANEL_REF_UC8179_75_BWR, EPD_PANEL_REF_JD79668_42_BWRY };
    const int durations[] = { 12000, 35000, 120000 };
    for (size_t i = 0; i < 3; ++i) {
        reset_fake(panels[i]); busy_mode = BUSY_STUCK;
        assert(epd_wait_ready("refresh",true) == ESP_ERR_TIMEOUT);
        assert(now_ms == durations[i]);
    }
}
static void test_spi_dma(void)
{
    reset_fake(EPD_PANEL_42_BWR);
    uint8_t data[9000] = {0};
    fail_alloc_at = 1;
    assert_failed(epd_bulk(data,sizeof(data)), ESP_ERR_NO_MEM);
    assert(!spi_calls && !alloc_count);
    fail_alloc_at = 0; fail_spi_at = 2;
    assert_failed(epd_bulk(data,sizeof(data)), SPI_FAULT);
    assert(spi_calls == 2 && !alloc_count);
    reset_fake(EPD_PANEL_REF_UC8176_42_BW);
    epd_dirty_rect_t rect = { .x = 8, .y = 0, .w = 8, .h = 1 };
    fail_alloc_at = 1; fail_spi_at = 13;
    assert_failed(uc81xx_write_region(0x10,data,&rect),ESP_ERR_NO_MEM);
    assert(spi_calls == 12 && !alloc_count);
}
static void test_public_recovery(void)
{
    reset_fake(EPD_PANEL_REF_UC8179_75_BWR);
    fb_t *fb = fb_create();
    stuck_on_refresh = 1;
    assert_failed(epd_display_from_buffer_mode(fb,EPD_REFRESH_FULL), ESP_ERR_TIMEOUT);
    assert(refreshes == 1);
    int old_resets = resets;
    busy_mode = BUSY_AUTO; stuck_on_refresh = 0;
    assert(epd_display_from_buffer_mode(fb,EPD_REFRESH_PARTIAL) == ESP_OK);
    assert(resets > old_resets && complete_count == 1 && s_previous_bw_valid);
    assert(epd_display_from_buffer_mode(fb,EPD_REFRESH_PARTIAL) == ESP_OK && complete_count == 1);
    fb_destroy(fb);
    reset_fake(EPD_PANEL_VISIONPEAK_EI0583T5PCZ_BW);
    fb = fb_create(); stuck_on_poweroff = 1;
    assert_failed(epd_display_from_buffer(fb),ESP_ERR_TIMEOUT);
    assert(strstr(last_busy_log,"stage=poweroff") && strstr(last_busy_log,"ESP_ERR_TIMEOUT"));
    fb_destroy(fb);
    reset_fake(EPD_PANEL_VISIONPEAK_EI0583T5PCZ_BW);
    fb = fb_create(); busy_mode = BUSY_STUCK;
    assert_failed(epd_display_from_buffer(fb),ESP_ERR_TIMEOUT);
    assert(!refreshes && strstr(last_busy_log,"stage=power-on"));
    fb_destroy(fb);
}

static void test_init_retry(void)
{
    reset_fake(EPD_PANEL_VISIONPEAK_EI0583T5PCZ_BW);
    s_spi = NULL; s_spi_bus_initialized = false;
    fail_add = 1;
    assert_failed(epd_init(), SPI_FAULT);
    assert(!epd_is_ready() && bus_calls == 1);
    fail_add = 0; fail_spi_at = spi_calls + 1;
    assert_failed(epd_init(), SPI_FAULT);
    assert(bus_calls == 1 && add_calls == 2 && !epd_is_ready());
    fail_spi_at = injected = 0;
    assert(epd_init() == ESP_OK && epd_is_ready());
    assert(bus_calls == 1 && add_calls == 2);
}

static void test_all_panel_callbacks(void)
{
    for (int panel = 0; panel < EPD_PANEL_COUNT; ++panel) {
        if (!panel_valid(panel)) continue;
        reset_fake(panel);
        fail_spi_at = 1;
        assert_failed(panel_run_full_init(), SPI_FAULT);
        assert(spi_calls == 1);
        reset_fake(panel);
        esp_err_t err = epd_clear_screen();
        if (err != ESP_OK) fprintf(stderr,"panel %d clear: %x log=%s\n",panel,err,last_busy_log);
        assert(err == ESP_OK && !mutex_depth);
        int transfers = spi_calls;
        reset_fake(panel);
        fail_spi_at = transfers;
        assert_failed(epd_clear_screen(), SPI_FAULT);
        assert(spi_calls == transfers && !alloc_count);
        reset_fake(panel);
        fb_t *fb = fb_create();
        err = epd_display_from_buffer(fb);
        if (err != ESP_OK) fprintf(stderr,"panel %d buffer: %x log=%s\n",panel,err,last_busy_log);
        assert(err == ESP_OK && !mutex_depth);
        fb_destroy(fb);
    }
}

static void test_experiments(void)
{
    const epd_panel_t panels[] = { EPD_PANEL_REF_UC8159_75_LOW_BW, EPD_PANEL_SSD16XX_213_BW, EPD_PANEL_GD7965_UC8179_75_BW };
    esp_err_t (*tests[])(epd_refresh_mode_t, epd_refresh_test_result_t *) = {
        epd_uc8159_refresh_test, epd_ssd16xx_213_refresh_test, epd_gd7965_75_refresh_test
    };
    for (size_t i = 0; i < 3; ++i) for (int mode = EPD_REFRESH_FAST; mode <= EPD_REFRESH_PARTIAL; ++mode) {
        epd_refresh_test_result_t result;
        reset_fake(panels[i]);
        assert(tests[i](mode,&result) == ESP_OK);
        assert(result.baseline_busy_ok && result.candidate_busy_ok && complete_count == 1);
        assert(!s_panel_seq_done && !s_previous_bw_valid && !alloc_count);
        int total = spi_calls;
        for (int fault = 1; fault <= 3; ++fault) {
            reset_fake(panels[i]);
            fail_spi_at = fault == 1 ? 1 : fault == 2 ? total/2 : total;
            assert_failed(tests[i](mode,&result), SPI_FAULT);
            assert(!alloc_count);
        }
        reset_fake(panels[i]); missing_on_refresh = 1;
        assert_failed(tests[i](mode,&result),ESP_ERR_TIMEOUT);
        assert(!result.baseline_busy_ok && !result.candidate_busy_ok && !alloc_count);
        reset_fake(panels[i]); stuck_on_refresh = 2;
        assert_failed(tests[i](mode,&result),ESP_ERR_TIMEOUT);
        assert(!result.candidate_busy_ok && !alloc_count);
    }
}

static void test_repair_and_patterns(void)
{
    const epd_panel_t panels[] = { EPD_PANEL_VISIONPEAK_EI0583T5PCZ_BW, EPD_PANEL_REF_JD79668_42_BWRY, EPD_PANEL_E6_37_6C };
    for (size_t i = 0; i < 3; ++i) {
        for (int pattern = 0; pattern < 3; ++pattern) {
            reset_fake(panels[i]); stuck_on_refresh = 1;
            assert_failed(epd_repair_ghosting(8,pattern),ESP_ERR_TIMEOUT);
            assert(refreshes == 1 && !alloc_count);
        }
        reset_fake(panels[i]); missing_on_refresh = 1;
        assert_failed(epd_display_test_pattern(),ESP_ERR_TIMEOUT);
        assert(!alloc_count);
        reset_fake(panels[i]); fail_spi_at = 1;
        assert_failed(epd_display_test_pattern(),SPI_FAULT);
        assert(!alloc_count);
    }
}

static void test_transfer_fault_sweep(void)
{
    reset_fake(EPD_PANEL_VISIONPEAK_EI0583T5PCZ_BW);
    fb_t *fb = fb_create();
    assert(epd_display_from_buffer(fb) == ESP_OK);
    const int total = spi_calls;
    const int dma_allocations = alloc_calls - 2;
    fb_destroy(fb);
    for (int fault = 1; fault <= total; ++fault) {
        reset_fake(EPD_PANEL_VISIONPEAK_EI0583T5PCZ_BW);
        fb = fb_create(); fail_spi_at = fault;
        assert_failed(epd_display_fb_free(fb),SPI_FAULT);
        assert(spi_calls == fault && !alloc_count);
    }
    for (int fault = 1; fault <= dma_allocations; ++fault) {
        reset_fake(EPD_PANEL_VISIONPEAK_EI0583T5PCZ_BW);
        fb = fb_create(); fail_alloc_at = alloc_calls + fault;
        assert_failed(epd_display_fb_free_mode(fb,EPD_REFRESH_FULL),ESP_ERR_NO_MEM);
        assert(!alloc_count);
    }
    printf("ID30: swept %d SPI and %d DMA allocation fault positions\n",total,dma_allocations);
}

static void test_files(void)
{
    const epd_panel_t panels[] = { EPD_PANEL_VISIONPEAK_EI0583T5PCZ_BW, EPD_PANEL_REF_UC8179_75_BWR, EPD_PANEL_E6_37_6C };
    const char *path = "epd-test-frame.bin";
    for (size_t i = 0; i < 3; ++i) {
        reset_fake(panels[i]);
        FILE *f = fopen(path,"wb"); assert(f);
        for (int j = 0; j < epd_plane_bytes(); ++j) fputc(0xFF,f);
        assert(fclose(f) == 0);
        fail_spi_at = 10;
        assert_failed(epd_display_from_file(path),SPI_FAULT);
        assert(!alloc_count);
        fail_spi_at = injected = 0;
        assert(epd_display_from_file_mode(path,EPD_REFRESH_FULL) == ESP_OK);
        previous_bw_release(); assert(!alloc_count);
    }
    reset_fake(EPD_PANEL_42_BWR);
    FILE *f = tmpfile(); assert(f); fputc(0xFF,f); rewind(f);
    assert(panel_write_bw_red_file(f,epd_plane_bytes(),false,false,false,false) == ESP_ERR_INVALID_SIZE);
    assert(!refreshes && !alloc_count); fclose(f);
    reset_fake(EPD_PANEL_VISIONPEAK_EI0583T5PCZ_BW);
    f = tmpfile(); assert(f); fputc(0xFF,f); rewind(f);
    fail_spi_at = 2;
    assert_failed(file_send_uc8159_interleaved(f,epd_plane_bytes(),false),ESP_ERR_INVALID_SIZE);
    assert(spi_calls == 1 && !alloc_count); fclose(f);
    remove(path);
}

static void test_epoch_cancellation(void)
{
    const epd_panel_t panels[] = { EPD_PANEL_VISIONPEAK_EI0583T5PCZ_BW, EPD_PANEL_E6_37_6C };
    for (size_t i = 0; i < 2; ++i) {
        reset_fake(panels[i]);
        change_epoch_on_take = true;
        assert(epd_clear_screen_epoch(current_epoch) == ESP_ERR_INVALID_STATE);
        assert(!spi_calls && !gpio_calls && !mutex_depth);
        change_epoch_on_take = false;
        fb_t *fb = fb_create();
        unsigned epoch = current_epoch;
        change_epoch_on_take = true;
        assert(epd_display_fb_free_mode_epoch(fb,EPD_REFRESH_FULL,epoch) == ESP_ERR_INVALID_STATE);
        assert(!alloc_count && !spi_calls && !gpio_calls && !mutex_depth && !complete_count);
        assert(!atomic_load(&s_force_full_refresh_next));

        const char *path = "epd-epoch-frame.bin";
        FILE *f = fopen(path,"wb"); assert(f);
        for (int j = 0; j < epd_plane_bytes(); ++j) fputc(0xFF,f);
        fclose(f);
        epoch = current_epoch;
        assert(epd_display_from_file_mode_epoch(path,EPD_REFRESH_FULL,epoch) == ESP_ERR_INVALID_STATE);
        assert(!alloc_count && !spi_calls && !gpio_calls && !mutex_depth && !complete_count);
        assert(!atomic_load(&s_force_full_refresh_next));
        change_epoch_on_take = false;
        assert(epd_display_from_file_mode_epoch(path,EPD_REFRESH_FULL,current_epoch) == ESP_OK);
        remove(path);
    }
}

static void test_partial_failure_recovery(void)
{
    const epd_panel_t panels[] = { EPD_PANEL_REF_UC8176_42_BW, EPD_PANEL_REF_UC8179_75_BWR, EPD_PANEL_GDEY037T03_UC8253_BW };
    for (size_t i = 0; i < 3; ++i) {
        reset_fake(panels[i]);
        fb_t *fb = fb_create();
        assert(epd_display_from_buffer(fb) == ESP_OK && s_previous_bw_valid);
        fb->black[0] &= 0x7F;
        assert(epd_display_from_buffer_mode(fb,EPD_REFRESH_PARTIAL) == ESP_OK);
        fb->black[1] &= 0x7F;
        fail_spi_at = spi_calls + 3; complete_count = 0;
        assert_failed(epd_display_from_buffer_mode(fb,EPD_REFRESH_PARTIAL),SPI_FAULT);
        int before = resets;
        fail_spi_at = injected = 0;
        assert(epd_display_from_buffer_mode(fb,EPD_REFRESH_PARTIAL) == ESP_OK);
        assert(resets > before && s_previous_bw_valid && complete_count == 1);
        fb->black[2] &= 0x7F;
        complete_count = 0; missing_on_refresh = refreshes + 1;
        assert_failed(epd_display_from_buffer_mode(fb,EPD_REFRESH_PARTIAL),ESP_ERR_TIMEOUT);
        fb_destroy(fb);
    }
}

static void test_image_profiles(void)
{
    epd_image_profile_t p;
    assert(!epd_panel_image_profile(-1, &p));
    assert(!epd_panel_image_profile(EPD_PANEL_COUNT, &p));
    assert(!epd_panel_image_profile(34, NULL));
    for (int id = 0; id < EPD_PANEL_COUNT; ++id) {
        if (!s_specs[id].name) {
            assert(!epd_panel_image_profile(id, &p));
            continue;
        }
        assert(epd_panel_image_profile(id, &p));
        assert(p.width == s_specs[id].w && p.height == s_specs[id].h);
        assert(p.has_red == s_specs[id].has_red && p.has_yellow == s_specs[id].has_yellow);
        assert((strcmp(p.color_type, "e6") == 0) == (id == EPD_PANEL_E6_37_6C));
    }
    assert(epd_panel_image_profile(47, &p) && p.width == 128 && p.height == 250);
    assert(epd_panel_image_profile(34, &p) && strcmp(p.id, "jd79665-v1") == 0);
    assert(epd_panel_image_profile(37, &p) && strcmp(p.id, "jd79668-v1") == 0);
    assert(epd_panel_image_profile(44, &p) && strcmp(p.id, "ssd2677-v1") == 0);
}

int main(void)
{
    test_image_profiles();
    test_polarity_and_name(); test_busy_recovery(); test_spi_dma(); test_public_recovery();
    test_init_retry(); test_all_panel_callbacks(); test_experiments();
    test_repair_and_patterns(); test_transfer_fault_sweep(); test_files();
    test_epoch_cancellation();
    test_partial_failure_recovery();
    previous_bw_release(); assert(!alloc_count && !mutex_depth);
    puts("EPD production-source fault tests passed");
    return 0;
}
