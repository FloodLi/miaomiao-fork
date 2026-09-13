#pragma once

#include <stdbool.h>

typedef enum {
    APP_SERVICE_EPD = 0,
    APP_SERVICE_SCHEDULER,
    APP_SERVICE_CLOCK,
    APP_SERVICE_MESSAGE,
    APP_SERVICE_CANVAS,
    APP_SERVICE_CALENDAR,
    APP_SERVICE_TIMETABLE,
    APP_SERVICE_TODO,
    APP_SERVICE_COUNTDOWN,
    APP_SERVICE_WEATHER,
    APP_SERVICE_CODEX,
    APP_SERVICE_BUTTON,
    APP_SERVICE_BATTERY,
    APP_SERVICE_COUNT,
} app_service_t;

void app_service_status_init(void);
void app_service_set_available(app_service_t service, bool available);
bool app_service_is_available(app_service_t service);
const char *app_service_name(app_service_t service);
