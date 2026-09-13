#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define DISPLAY_MODE_MAX 12

/* Keep in sync with app_main.c registration order. */
typedef enum {
    DISPLAY_MODE_CLOCK = 0,
    DISPLAY_MODE_CALENDAR,
    DISPLAY_MODE_TIMETABLE,
    DISPLAY_MODE_WEATHER,
    DISPLAY_MODE_SLIDESHOW,
    DISPLAY_MODE_TODO,
    DISPLAY_MODE_COUNTDOWN,
    DISPLAY_MODE_CODEX_QUOTA,
} display_mode_index_t;

typedef esp_err_t (*display_mode_show_fn)(void);
typedef bool (*display_mode_manual_policy_fn)(void);
typedef esp_err_t (*display_mode_complete_fn)(void);

typedef struct {
    const char           *name;      /* "clock", "calendar", etc. */
    const char           *label_cn;  /* "时钟", "日历", etc. */
    display_mode_show_fn  show;
    display_mode_manual_policy_fn manual_after_show;
    bool available;
    display_mode_complete_fn wait_complete;
} display_mode_entry_t;

/**
 * Register a display mode. Call in each module's init or from app_main.
 * Returns the index (0-based) or -1 on error.
 */
int display_mode_register(const display_mode_entry_t *entry);

int                        display_mode_count(void);
const display_mode_entry_t *display_mode_get(int index);
esp_err_t                  display_mode_show(int index);
esp_err_t                  display_mode_show_request(int index, unsigned *epoch_out);
esp_err_t                  display_mode_show_background(int index, unsigned expected_epoch);
/* On rejection/unavailability, epoch_out retains expected_epoch so callers
 * cannot accidentally adopt a newer user's epoch for a fallback. */
esp_err_t                  display_mode_show_if_current(int index, unsigned expected_epoch,
                                                        unsigned *epoch_out);
/* Render callbacks capture the invoking transaction, even if a newer user
 * request starts before the callback runs. Outside a callback: current epoch. */
unsigned                   display_mode_render_epoch(void);
const char                *display_mode_name(int index);
const char                *display_mode_label(int index);
bool                       display_mode_is_available(int index);
void                       display_mode_set_available(int index, bool available);

/** Last successfully displayed mode, or -1 before any mode takes ownership. */
int                        display_mode_active(void);
void                       display_mode_set_active(int index);
