#pragma once

#include "esp_err.h"

/**
 * Three physical buttons (TS35CA, active-HIGH with 4.7K pull-down):
 *   SW3 = GPIO 3   (left)
 *   SW4 = GPIO 46  (middle)
 *   SW5 = GPIO 9   (right)
 *
 * Display mode cycle:  Clock -> Calendar -> Timetable -> Weather -> Clock ...
 *   Left   (SW3) : previous mode
 *   Middle (SW4) : refresh current mode
 *   Right  (SW5) : next mode
 */

esp_err_t button_init(void);

int button_get_current_mode(void);
