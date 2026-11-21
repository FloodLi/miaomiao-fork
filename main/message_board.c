#include "message_board.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include "fb_render.h"
#include "epd.h"
#include "scheduler.h"
#include "display_policy.h"
#include "ui_theme.h"
#include "time_sync.h"
#include "wecom_bot.h"
#include "wechat_bot.h"

static const char *TAG    = "msgboard";
static const char *NVS_NS = "msgboard";

static msg_config_t s_cfg = {
    .text      = "",
    .font_size = 0,
    .align     = MSG_ALIGN_CENTER,
    .color     = 0,
    .x_offset  = 0,
    .y_offset  = 0,
    .px        = 0,
};

static portMUX_TYPE s_cfg_mux = portMUX_INITIALIZER_UNLOCKED;

/* ── NVS ──────────────────────────────────────────────────────────── */

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    size_t len = sizeof(s_cfg.text);
    nvs_get_str(h, "text", s_cfg.text, &len);

    uint8_t v8;
    if (nvs_get_u8(h, "font", &v8) == ESP_OK) s_cfg.font_size = v8;
    if (nvs_get_u8(h, "align", &v8) == ESP_OK) s_cfg.align = (msg_align_t)v8;
    if (nvs_get_u8(h, "color", &v8) == ESP_OK) s_cfg.color = v8;

    int16_t v16;
    if (nvs_get_i16(h, "xoff", &v16) == ESP_OK) s_cfg.x_offset = v16;
    if (nvs_get_i16(h, "yoff", &v16) == ESP_OK) s_cfg.y_offset = v16;
    if (nvs_get_u8(h, "px", &v8) == ESP_OK) s_cfg.px = v8; /* 旧版本无此键 → 0 */

    nvs_close(h);
}

static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "text",  s_cfg.text);
    nvs_set_u8(h,  "font",  s_cfg.font_size);
    nvs_set_u8(h,  "align", (uint8_t)s_cfg.align);
    nvs_set_u8(h,  "color", s_cfg.color);
    nvs_set_i16(h, "xoff",  s_cfg.x_offset);
    nvs_set_i16(h, "yoff",  s_cfg.y_offset);
    nvs_set_u8(h,  "px",    s_cfg.px);
    nvs_commit(h);
    nvs_close(h);
}

/* ── UTF-8 helpers ────────────────────────────────────────────────── */

static int utf8_decode(const char **pp)
{
    const uint8_t *p = (const uint8_t *)*pp;
    if (p[0] == 0)
        return -1;
    if (p[0] < 0x80) {
        *pp += 1;
        return p[0];
    }
    if ((p[0] & 0xE0) == 0xC0) {
        if ((p[1] & 0xC0) != 0x80) { *pp += 1; return -1; }
        *pp += 2;
        return ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    }
    if ((p[0] & 0xF0) == 0xE0) {
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) { *pp += 1; return -1; }
        *pp += 3;
        return ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    }
    if ((p[0] & 0xF8) == 0xF0) {
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) { *pp += 1; return -1; }
        *pp += 4;
        return ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    }
    *pp += 1;
    return -1;
}

static int utf8_encode_codepoint(int cp, char out[5])
{
    if (cp < 0)
        cp = '?';
    if (cp < 0x80) {
        out[0] = (char)cp;
        out[1] = '\0';
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        out[2] = '\0';
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        out[3] = '\0';
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    out[4] = '\0';
    return 4;
}

static int msg_font_px_for_scale(int scale)
{
    if (scale < 1) scale = 1;
    if (scale > 6) scale = 6;
    return 16 * scale;
}

static fb_color_t msg_color_from_config(uint8_t color)
{
    switch (color) {
    case 1:
        return COLOR_RED;
    case 2:
        return epd_has_yellow() ? COLOR_YELLOW : COLOR_RED;
    case 3:
        return epd_get_panel() == EPD_PANEL_E6_37_6C ? COLOR_BLUE : COLOR_BLACK;
    case 4:
        return epd_get_panel() == EPD_PANEL_E6_37_6C ? COLOR_GREEN : COLOR_BLACK;
    case 0:
    default:
        return COLOR_BLACK;
    }
}

static int utf8_char_width_px(int cp, int target_px)
{
    char one[5];
    utf8_encode_codepoint(cp, one);
    return ui_text_width_px(NULL, one, target_px);
}

/* ── 缺字过滤：无法绘制的字符不画“方框带x”占位 ─────────────────────
 * 常见中文标点映射成 ASCII 等价物，其余（emoji、生僻符号等）直接丢弃。 */
typedef struct { uint32_t cp; char repl; } msg_punct_map_t;
static const msg_punct_map_t s_msg_punct_map[] = {
    {0x3002, '.'}, {0x3001, ','}, {0x2026, '.'}, {0x2014, '-'}, {0x2013, '-'},
    {0x00B7, '.'}, {0x300A, '<'}, {0x300B, '>'}, {0x3008, '<'}, {0x3009, '>'},
    {0x300C, '['}, {0x300D, ']'}, {0x3010, '['}, {0x3011, ']'},
    {0x2018, '\''}, {0x2019, '\''}, {0x201C, '"'}, {0x201D, '"'},
};

static void msg_sanitize_unrenderable(char *s, int target_px)
{
    if (!s)
        return;
    char *w = s;
    const char *r = s;
    while (*r) {
        const char *next = r;
        int cp = utf8_decode(&next);
        if (cp < 0) {
            r = next;
            continue;
        }
        if (cp == '\n' || fb_glyph_available((uint32_t)cp, target_px)) {
            while (r < next)
                *w++ = *r++;
            continue;
        }
        char repl = 0;
        for (size_t i = 0; i < sizeof(s_msg_punct_map) / sizeof(s_msg_punct_map[0]); i++) {
            if (s_msg_punct_map[i].cp == (uint32_t)cp) {
                repl = s_msg_punct_map[i].repl;
                break;
            }
        }
        if (repl)
            *w++ = repl;
        /* else: emoji/生僻符号等缺字字符整体丢弃，避免画出方框占位 */
        r = next;
    }
    *w = '\0';
}

/* ── word-wrap rendering ──────────────────────────────────────────── */

#define MARGIN_X  20
#define MARGIN_Y  16

static void render_message(unsigned epoch)
{
    fb_t *fb = fb_create();
    if (!fb) {
        ESP_LOGE(TAG, "fb_create failed");
        (void)display_policy_finish_epoch(epoch, ESP_ERR_NO_MEM, true);
        return;
    }

    msg_config_t cfg;
    portENTER_CRITICAL(&s_cfg_mux);
    cfg = s_cfg;
    portEXIT_CRITICAL(&s_cfg_mux);
    /* 本渲染覆盖的来信序号快照：渲染期间新到的消息留给下一轮渲染回执 */
    const uint32_t disp_seq = wecom_msg_seq();
    const uint32_t disp_seq_wx = wechat_msg_seq();

    if (cfg.text[0] == '\0') {
        fb_destroy(fb);
        ESP_LOGW(TAG, "No message to display");
        (void)display_policy_finish_epoch(epoch, ESP_ERR_INVALID_STATE, true);
        return;
    }

    /* 正文缺字过滤：无法绘制的字符（emoji、生僻符号）不画“方框带x”占位 */
    char disp_text[MSG_MAX_LEN + 1 + 48];
    snprintf(disp_text, sizeof(disp_text), "%s", cfg.text);

    int scale = cfg.font_size + 1;
    if (scale < 1) scale = 1;
    if (scale > 6) scale = 6;
    /* px>0（微信来信固定25）直接用指定像素，否则按 font_size 档位（16*scale） */
    int target_px = (cfg.px >= 8 && cfg.px <= 200) ? cfg.px : msg_font_px_for_scale(scale);
    msg_sanitize_unrenderable(disp_text, target_px);
    int char_h = target_px;
    int line_h = char_h + 2 * scale;
    int max_w  = fb->width - 2 * MARGIN_X - 16;
    fb_color_t color = msg_color_from_config(cfg.color);
    int page_s = ui_scale_for(fb);
    int content_top = 34 * page_s;
    int content_bottom = fb->height - 30 * page_s;
    if (content_bottom <= content_top + line_h)
        content_bottom = fb->height - 20 * page_s;

    ESP_LOGI(TAG, "Rendering: scale=%d px=%d, color=%d, align=%d, offset=(%d,%d), text_len=%d",
             scale, target_px, cfg.color, (int)cfg.align, cfg.x_offset, cfg.y_offset,
             (int)strlen(cfg.text));

    ui_draw_page_frame(fb, UI_FRAME_RED_ACCENT | UI_FRAME_THIN);
    ui_draw_header(fb, "留言板", "留言", true);

    /* 更新时间紧跟标题“留言板”字样后（MM月DD日 hh:mm，MM为月份）；
     * 未校时则不画。仅渲染，不写入配置/内容框。 */
    struct tm now_tm;
    if (time_sync_get_local_relaxed(&now_tm)) {
        char upd[32];
        snprintf(upd, sizeof(upd), "更新时间：%02d月%02d日 %02d:%02d",
                 now_tm.tm_mon + 1, now_tm.tm_mday, now_tm.tm_hour, now_tm.tm_min);
        int ux = 14 * page_s + ui_fixed_text_width(fb, "留言板", 1) + 8 * page_s;
        int umax = fb->width - ux - 130 * page_s; /* 给右侧“留言”+电量徽标留位 */
        if (umax > 0)
            ui_draw_fixed_text_maxw(fb, ux, 8 * page_s, upd, COLOR_BLACK, 1, umax);
    }

    /* first pass: calculate lines for vertical centering */
    typedef struct { const char *start; int len; int px_w; } line_t;
    line_t lines[32];
    int line_count = 0;

    const char *p = disp_text;
    const char *line_start = p;
    int cur_w = 0;

    while (*p && line_count < 32) {
        if (*p == '\n') {
            lines[line_count].start = line_start;
            lines[line_count].len   = (int)(p - line_start);
            lines[line_count].px_w  = cur_w;
            line_count++;
            p++;
            line_start = p;
            cur_w = 0;
            continue;
        }

        const char *next = p;
        int cp = utf8_decode(&next);
        if (cp < 0) { p = next; continue; }
        int cw = utf8_char_width_px(cp, target_px);

        if (cur_w + cw > max_w && cur_w > 0) {
            lines[line_count].start = line_start;
            lines[line_count].len   = (int)(p - line_start);
            lines[line_count].px_w  = cur_w;
            line_count++;
            line_start = p;
            cur_w = 0;
            if (line_count >= 32) break;
        }

        cur_w += cw;
        p = next;
    }

    if (line_count < 32 && (cur_w > 0 || p > line_start)) {
        lines[line_count].start = line_start;
        lines[line_count].len   = (int)(p - line_start);
        lines[line_count].px_w  = cur_w;
        line_count++;
    }

    bool truncated = (*p != '\0' && line_count >= 32);

    int total_h = line_count * line_h;
    int content_h = content_bottom - content_top;
    int start_y = content_top + (content_h - total_h) / 2 + cfg.y_offset;
    if (start_y < content_top) start_y = content_top;

    for (int i = 0; i < line_count; i++) {
        int y = start_y + i * line_h;
        if (y + char_h > content_bottom) break;

        int x;
        if (cfg.align == MSG_ALIGN_CENTER) {
            x = (fb->width - lines[i].px_w) / 2 + cfg.x_offset;
        } else {
            x = MARGIN_X + 8 * page_s + cfg.x_offset;
        }
        if (x < MARGIN_X)
            x = MARGIN_X;

        char buf[MSG_MAX_LEN + 1];
        int copy_len = lines[i].len;
        if (copy_len > MSG_MAX_LEN) copy_len = MSG_MAX_LEN;
        memcpy(buf, lines[i].start, copy_len);
        buf[copy_len] = '\0';

        ui_draw_text_px(fb, x, y, buf, color, target_px);
    }

    if (truncated) {
        int hint_y = start_y + line_count * line_h;
        if (hint_y + 16 <= fb->height)
            ui_draw_text_px(fb,
                            (fb->width - ui_text_width_px(fb, "...", 16)) / 2,
                            hint_y, "...", COLOR_BLACK, 16);
    }

    ui_draw_footer(fb, "",               /* 底部“留言板”字样按需求去掉，仅留对齐提示 */
                   cfg.align == MSG_ALIGN_CENTER ? "居中" : "左对齐");

    if (!display_policy_epoch_is_current(epoch)) {
        fb_destroy(fb);
        ESP_LOGI(TAG, "skip stale message render");
        return;
    }

    /* 缓冲区直显：与网页「发送到墨水屏」按钮（/msg_image_show）同一显示原语
     * epd_display_fb_free_mode_epoch——之前走的流式文件路径（fb_export →
     * epd_display_from_file，JD79686A 用 panel_write_bw_red_file）在本面板上
     * 输出全红；而按钮/日历所走的缓冲区路径实测正常（日志 “display from buffer”）。 */
    if (epd_is_ready()) {
        ESP_LOGI(TAG, "Displaying message from buffer (%d lines, scale=%d px=%d)...",
                 line_count, scale, target_px);
        esp_err_t disp_err = epd_display_fb_free_mode_epoch(fb, EPD_REFRESH_FAST, epoch);
        if (disp_err == ESP_OK && !display_policy_epoch_is_current(epoch)) {
            ESP_LOGI(TAG, "Message refresh completed but was superseded");
        } else if (disp_err == ESP_OK) {
            (void)display_policy_finish_epoch(epoch, ESP_OK, true);
            scheduler_notify_manual_show();
            ESP_LOGI(TAG, "Message displayed on EPD");
        } else {
            (void)display_policy_finish_epoch(epoch, disp_err, true);
            ESP_LOGE(TAG, "display failed: %s", esp_err_to_name(disp_err));
        }
        wecom_notify_display_result(disp_err == ESP_OK, disp_seq);
        wechat_notify_display_result(disp_err == ESP_OK, disp_seq_wx);
    } else {
        fb_destroy(fb);
        (void)display_policy_finish_epoch(epoch, ESP_ERR_INVALID_STATE, true);
        ESP_LOGE(TAG, "message render failed: epd not ready");
        wecom_notify_display_result(false, disp_seq);
        wechat_notify_display_result(false, disp_seq_wx);
    }
}

/* ── public API ───────────────────────────────────────────────────── */

/** 修整被截断的 UTF-8 尾部：去掉结尾不完整的码点。 */
static void msg_utf8_fix_tail(char *s)
{
    if (!s)
        return;
    size_t i = strlen(s);
    size_t n = i;
    while (n > 0 && ((unsigned char)s[n - 1] & 0xC0) == 0x80)
        n--;
    if (n == 0)
        return;
    unsigned char lead = (unsigned char)s[n - 1];
    int need = lead < 0x80 ? 0
             : lead >= 0xF0 ? 3
             : lead >= 0xE0 ? 2
             : lead >= 0xC0 ? 1 : -1;
    if (need >= 0 && i - (n - 1) < (size_t)(need + 1))
        s[n - 1] = '\0';
}

esp_err_t message_board_show_inbound_text(const char *text)
{
    if (!text || !text[0])
        return ESP_ERR_INVALID_ARG;

    msg_config_t cfg;
    if (message_board_get_config(&cfg) != ESP_OK)
        return ESP_ERR_INVALID_STATE;

    /* UTF-8 安全拷贝（最多 MSG_MAX_LEN 字节） */
    size_t i = 0;
    for (; text[i] && i + 1 < sizeof(cfg.text); i++)
        cfg.text[i] = text[i];
    cfg.text[i] = '\0';
    if (text[i])
        msg_utf8_fix_tail(cfg.text);

    /* \r\n -> \n，孤立 \r -> \n（渲染层只认 \n） */
    int w = 0;
    for (int r = 0; cfg.text[r] && w + 1 < (int)sizeof(cfg.text); r++) {
        char c = cfg.text[r];
        if (c == '\r') {
            if (cfg.text[r + 1] == '\n')
                continue;
            c = '\n';
        }
        cfg.text[w++] = c;
    }
    cfg.text[w] = '\0';
    if (!cfg.text[0])
        return ESP_ERR_INVALID_ARG;

    /* IM 来信固定样式：默认档位、字号25（px 覆盖渲染）、黑色、左对齐、偏移归零 */
    cfg.font_size = 0;
    cfg.align = MSG_ALIGN_LEFT;
    cfg.color = 0;
    cfg.x_offset = 0;
    cfg.y_offset = 0;
    cfg.px = 25;

    esp_err_t err = message_board_set_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "inbound config rejected: %s", esp_err_to_name(err));
        return err;
    }
    err = message_board_show_queued(NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "inbound show queue failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "inbound text queued (%u bytes)", (unsigned)strlen(cfg.text));
    return ESP_OK;
}

esp_err_t message_board_get_config(msg_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_cfg_mux);
    *out = s_cfg;
    portEXIT_CRITICAL(&s_cfg_mux);
    return ESP_OK;
}

esp_err_t message_board_set_config(const msg_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_cfg_mux);
    s_cfg = *cfg;
    portEXIT_CRITICAL(&s_cfg_mux);
    nvs_save();
    ESP_LOGI(TAG, "Config saved (text_len=%d)", (int)strlen(cfg->text));
    return ESP_OK;
}

static SemaphoreHandle_t s_render_mutex;
static SemaphoreHandle_t s_queue_mutex;
static unsigned s_pending_epoch;

esp_err_t message_board_init(void)
{
    s_render_mutex = xSemaphoreCreateBinary();
    if (!s_render_mutex)
        return ESP_ERR_NO_MEM;
    s_queue_mutex = xSemaphoreCreateMutex();
    if (!s_queue_mutex) {
        vSemaphoreDelete(s_render_mutex);
        s_render_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_render_mutex);
    nvs_load();
    ESP_LOGI(TAG, "Message board init (text_len=%d, font=%d, align=%d, color=%d, xy=%d,%d)",
             (int)strlen(s_cfg.text), s_cfg.font_size, (int)s_cfg.align, s_cfg.color,
             s_cfg.x_offset, s_cfg.y_offset);
    return ESP_OK;
}

static void render_task(void *arg)
{
    unsigned epoch = (unsigned)(uintptr_t)arg;
    for (;;) {
        ESP_LOGI(TAG, "Render task started");
        render_message(epoch);
        ESP_LOGI(TAG, "Render task done");

        /* Submission and worker exit share a lock so no queued epoch is lost. */
        xSemaphoreTake(s_queue_mutex, portMAX_DELAY);
        epoch = s_pending_epoch;
        s_pending_epoch = 0;
        if (!epoch)
            xSemaphoreGive(s_render_mutex);
        xSemaphoreGive(s_queue_mutex);
        if (!epoch)
            break;
    }

    vTaskDelete(NULL);
}

esp_err_t message_board_show_queued(unsigned *out_epoch)
{
    if (out_epoch)
        *out_epoch = 0;
    portENTER_CRITICAL(&s_cfg_mux);
    bool has_text = s_cfg.text[0] != '\0';
    portEXIT_CRITICAL(&s_cfg_mux);
    if (!has_text) {
        ESP_LOGW(TAG, "No message text set");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_render_mutex || !s_queue_mutex)
        return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_queue_mutex, portMAX_DELAY);
    unsigned epoch = display_policy_begin_manual_display_mode(EPD_REFRESH_FAST);
    if (out_epoch)
        *out_epoch = epoch;

    if (xSemaphoreTake(s_render_mutex, 0) != pdTRUE) {
        s_pending_epoch = epoch;
        xSemaphoreGive(s_queue_mutex);
        ESP_LOGI(TAG, "Render queued (will re-render after current finishes)");
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreate(render_task, "msg_render", 8192,
                                 (void *)(uintptr_t)epoch, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create render task");
        xSemaphoreGive(s_render_mutex);
        (void)display_policy_finish_epoch(epoch, ESP_ERR_NO_MEM, true);
        xSemaphoreGive(s_queue_mutex);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreGive(s_queue_mutex);
    return ESP_OK;
}

display_result_t message_board_wait_result(unsigned epoch)
{
    return display_policy_wait_result(epoch);
}

esp_err_t message_board_show(void)
{
    return message_board_show_queued(NULL);
}

void message_board_wait_idle(void)
{
    if (!s_render_mutex || !s_queue_mutex)
        return;
    for (;;) {
        /* An idle waiter must not make submission think a worker is running. */
        xSemaphoreTake(s_queue_mutex, portMAX_DELAY);
        bool idle = xSemaphoreTake(s_render_mutex, 0) == pdTRUE;
        if (idle)
            xSemaphoreGive(s_render_mutex);
        xSemaphoreGive(s_queue_mutex);
        if (idle)
            return;
        vTaskDelay(1);
    }
}
