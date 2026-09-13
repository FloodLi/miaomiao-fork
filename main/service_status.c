#include "service_status.h"

#include <stdatomic.h>

static atomic_uint s_available_mask;

static const char *const s_names[APP_SERVICE_COUNT] = {
    "epd", "scheduler", "clock", "message", "canvas", "calendar",
    "timetable", "todo", "countdown", "weather", "codex", "button",
    "battery",
};

void app_service_status_init(void)
{
    atomic_store(&s_available_mask, 0);
}

void app_service_set_available(app_service_t service, bool available)
{
    if (service < 0 || service >= APP_SERVICE_COUNT)
        return;
    unsigned bit = 1u << (unsigned)service;
    if (available)
        atomic_fetch_or(&s_available_mask, bit);
    else
        atomic_fetch_and(&s_available_mask, ~bit);
}

bool app_service_is_available(app_service_t service)
{
    if (service < 0 || service >= APP_SERVICE_COUNT)
        return false;
    return (atomic_load(&s_available_mask) & (1u << (unsigned)service)) != 0;
}

const char *app_service_name(app_service_t service)
{
    return service >= 0 && service < APP_SERVICE_COUNT ? s_names[service] : "unknown";
}
