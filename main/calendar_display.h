#pragma once

#include "esp_err.h"

esp_err_t calendar_display_init(void);
esp_err_t calendar_display_show(int year, int month);
esp_err_t calendar_display_show_current(void);
