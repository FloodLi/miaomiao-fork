#pragma once

#include <stdbool.h>

#include "fb_render.h"
#include "ui_color_theme.h"

typedef enum {
    UI_FRAME_RED_ACCENT = 1 << 0,
    UI_FRAME_THIN       = 1 << 1,
} ui_frame_flags_t;

typedef enum {
    UI_LAYOUT_COMPACT = 0,  /* 2.9" class */
    UI_LAYOUT_42,           /* 400x300 4.2" class */
    UI_LAYOUT_583,          /* 600x448 / 648x480 5.83" class */
    UI_LAYOUT_LARGE,        /* 7.5" / E6 3.7" / 800x480+ class */
} ui_layout_class_t;

typedef enum {
    UI_TEXT_SMALL = 0,
    UI_TEXT_BODY,
    UI_TEXT_LABEL,
    UI_TEXT_TITLE,
    UI_TEXT_VALUE,
} ui_text_role_t;

ui_layout_class_t ui_layout_for(const fb_t *fb);
bool ui_layout_is_42(const fb_t *fb);
bool ui_layout_is_583(const fb_t *fb);
bool ui_layout_is_e6_37(const fb_t *fb);
bool ui_layout_is_wide(const fb_t *fb);
bool ui_layout_is_xl(const fb_t *fb);
int  ui_scale_for(const fb_t *fb);
fb_color_t ui_e6_palette_color(const fb_t *fb, int seed, fb_color_t fallback);
int  ui_text_width(const char *s, int scale);
int  ui_fixed_text_width(const fb_t *fb, const char *s, int scale);
int  ui_draw_fixed_text(fb_t *fb, int x, int y, const char *s,
                        fb_color_t color, int scale);
int  ui_draw_fixed_text_maxw(fb_t *fb, int x, int y, const char *s,
                             fb_color_t color, int scale, int max_w);
int  ui_text_width_px(const fb_t *fb, const char *s, int target_px);
int  ui_draw_text_px(fb_t *fb, int x, int y, const char *s,
                     fb_color_t color, int target_px);
int  ui_draw_text_px_maxw(fb_t *fb, int x, int y, const char *s,
                          fb_color_t color, int target_px, int max_w);
int  ui_role_scale(const fb_t *fb, ui_text_role_t role);
int  ui_role_text_width(const fb_t *fb, const char *s, ui_text_role_t role);
int  ui_draw_text_role(fb_t *fb, int x, int y, const char *s,
                       fb_color_t color, ui_text_role_t role);
int  ui_draw_text_role_maxw(fb_t *fb, int x, int y, const char *s,
                            fb_color_t color, ui_text_role_t role, int max_w);
void ui_draw_dotted_hline(fb_t *fb, int x, int y, int w, fb_color_t color, int step);
void ui_draw_dotted_vline(fb_t *fb, int x, int y, int h, fb_color_t color, int step);
void ui_draw_page_frame(fb_t *fb, ui_frame_flags_t flags);
void ui_draw_page_frame_for(fb_t *fb, ui_color_page_t page, ui_frame_flags_t flags);
int  ui_draw_battery_badge(fb_t *fb, int right_x, int y);
int  ui_draw_battery_badge_for(fb_t *fb, ui_color_page_t page, int right_x, int y);
void ui_draw_header(fb_t *fb, const char *title, const char *right, bool red_accent);
void ui_draw_header_for(fb_t *fb, ui_color_page_t page, const char *title,
                        const char *right, bool accent);
void ui_draw_footer(fb_t *fb, const char *left, const char *right);
/** 页脚变体：右侧可选电量徽标（先画徽标，right 文字在其左侧锚定）。 */
void ui_draw_footer_for(fb_t *fb, ui_color_page_t page, const char *left,
                        const char *right, bool with_battery);
void ui_draw_card(fb_t *fb, int x, int y, int w, int h, bool red_accent);
void ui_draw_card_for(fb_t *fb, ui_color_page_t page, int x, int y,
                      int w, int h, bool accent);
void ui_draw_section_label(fb_t *fb, int x, int y, const char *label, fb_color_t color, int scale);
void ui_draw_empty_state_compact(fb_t *fb, const char *title, const char *hint);
void ui_draw_empty_state(fb_t *fb, const char *title, const char *hint);
void ui_draw_empty_state_for(fb_t *fb, ui_color_page_t page,
                             const char *title, const char *hint,
                             ui_color_role_t hint_role);
void ui_draw_progress_bar(fb_t *fb, int x, int y, int w, int h, int percent, fb_color_t fill);
