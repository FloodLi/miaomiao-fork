#include "display_policy.h"

#include <stdatomic.h>
#include "scheduler.h"
#include "clock_display.h"

static atomic_bool s_manual_screen;

void display_policy_init(void)
{
    atomic_store(&s_manual_screen, false);
}

bool display_policy_slideshow_owns_display(void)
{
    slideshow_config_t sc;
    if (scheduler_get_config(&sc) != ESP_OK)
        return false;
    return sc.enabled;
}

bool display_policy_clock_may_auto_refresh(void)
{
    if (atomic_load(&s_manual_screen))
        return false;
    if (display_policy_slideshow_owns_display())
        return false;
    clock_config_t cc;
    if (clock_display_get_config(&cc) != ESP_OK)
        return false;
    return cc.enabled;
}

bool display_policy_weather_use_full_page(bool user_explicit)
{
    if (user_explicit)
        return true;
    if (display_policy_slideshow_owns_display())
        return false;
    clock_config_t cc;
    if (clock_display_get_config(&cc) != ESP_OK)
        return true;
    return !cc.enabled;
}

bool display_policy_weather_may_network_fetch(bool user_explicit)
{
    if (user_explicit)
        return true;
    return !display_policy_slideshow_owns_display();
}

void display_policy_set_manual_screen_active(bool active)
{
    atomic_store(&s_manual_screen, active);
}

bool display_policy_manual_screen_active(void)
{
    return atomic_load(&s_manual_screen);
}
