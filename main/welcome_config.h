#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define WELCOME_TITLE_MAX_BYTES   64
#define WELCOME_MESSAGE_MAX_BYTES 128

typedef struct {
    bool enabled;
    char title[WELCOME_TITLE_MAX_BYTES];
    char message[WELCOME_MESSAGE_MAX_BYTES];
} welcome_config_t;

esp_err_t welcome_config_init(void);
void welcome_config_get(welcome_config_t *out);
esp_err_t welcome_config_set(const welcome_config_t *cfg);
void welcome_config_defaults(welcome_config_t *out);
