#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef struct { uint64_t pin_bit_mask; int mode; int pull_up_en; } gpio_config_t;
#define GPIO_MODE_OUTPUT 1
#define GPIO_MODE_INPUT 2
esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_set_level(int pin, int level);
int gpio_get_level(int pin);
