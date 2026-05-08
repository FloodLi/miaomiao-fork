#pragma once

#include <stdbool.h>

#include "fb_render.h"

typedef enum {
    UI_FRAME_RED_ACCENT = 1 << 0,
    UI_FRAME_THIN       = 1 << 1,
} ui_frame_flags_t;

int  ui_scale_for(const fb_t *fb);
int  ui_text_width(const char *s, int scale);
void ui_draw_dotted_hline(fb_t *fb, int x, int y, int w, fb_color_t color, int step);
void ui_draw_dotted_vline(fb_t *fb, int x, int y, int h, fb_color_t color, int step);
void ui_draw_page_frame(fb_t *fb, ui_frame_flags_t flags);
int  ui_draw_battery_badge(fb_t *fb, int right_x, int y);
void ui_draw_header(fb_t *fb, const char *title, const char *right, bool red_accent);
void ui_draw_footer(fb_t *fb, const char *left, const char *right);
void ui_draw_card(fb_t *fb, int x, int y, int w, int h, bool red_accent);
void ui_draw_section_label(fb_t *fb, int x, int y, const char *label, fb_color_t color, int scale);
void ui_draw_empty_state(fb_t *fb, const char *title, const char *hint);
void ui_draw_progress_bar(fb_t *fb, int x, int y, int w, int h, int percent, fb_color_t fill);
