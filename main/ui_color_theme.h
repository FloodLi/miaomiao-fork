#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "fb_render.h"

typedef enum {
    UI_COLOR_PAGE_WELCOME = 0,
    UI_COLOR_PAGE_CLOCK,
    UI_COLOR_PAGE_CALENDAR,
    UI_COLOR_PAGE_WEATHER,
    UI_COLOR_PAGE_TIMETABLE,
    UI_COLOR_PAGE_TODO,
    UI_COLOR_PAGE_COUNTDOWN,
    UI_COLOR_PAGE_CODEX,
    UI_COLOR_PAGE_SYSTEM,
    UI_COLOR_PAGE_COUNT,
} ui_color_page_t;

typedef enum {
    UI_COLOR_PROFILE_BALANCED = 0,
    UI_COLOR_PROFILE_WARM,
    UI_COLOR_PROFILE_VIVID,
    UI_COLOR_PROFILE_MONO,
    UI_COLOR_PROFILE_COUNT,
} ui_color_profile_t;

typedef enum {
    UI_COLOR_ROLE_NEUTRAL = 0,
    UI_COLOR_ROLE_ACCENT,
    UI_COLOR_ROLE_NOTICE,
    UI_COLOR_ROLE_WARNING,
} ui_color_role_t;

typedef struct {
    ui_color_profile_t profiles[UI_COLOR_PAGE_COUNT];
} ui_color_config_t;

esp_err_t ui_color_theme_init(void);
void ui_color_config_get(ui_color_config_t *out);
esp_err_t ui_color_config_set(const ui_color_config_t *cfg);
esp_err_t ui_color_config_reset(void);

bool ui_color_theme_applicable(void);
ui_color_profile_t ui_color_profile_get(ui_color_page_t page);
fb_color_t ui_color_resolve(ui_color_page_t page, ui_color_role_t role,
                            fb_color_t fallback);

const char *ui_color_page_key(ui_color_page_t page);
bool ui_color_page_from_key(const char *key, ui_color_page_t *out);
const char *ui_color_profile_name(ui_color_profile_t profile);
bool ui_color_profile_from_name(const char *name, ui_color_profile_t *out);
