#include "display_policy.h"

#include <stdatomic.h>
#include "scheduler.h"
#include "clock_display.h"

static atomic_bool s_manual_screen;
static atomic_bool s_quick_refresh;
static atomic_uint s_display_epoch;

void display_policy_init(void)
{
    atomic_store(&s_manual_screen, false);
    atomic_store(&s_quick_refresh, false);
    atomic_store(&s_display_epoch, 0);
}

void display_policy_set_quick_refresh_active(bool active)
{
    atomic_store(&s_quick_refresh, active);
}

bool display_policy_quick_refresh_active(void)
{
    return atomic_load(&s_quick_refresh);
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
    if (atomic_load(&s_quick_refresh))
        return false;
    if (atomic_load(&s_manual_screen))
        return false;
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
    if (atomic_load(&s_quick_refresh))
        return false;
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

void display_policy_bump_display_epoch(void)
{
    atomic_fetch_add(&s_display_epoch, 1);
}

unsigned display_policy_display_epoch(void)
{
    return atomic_load(&s_display_epoch);
}
