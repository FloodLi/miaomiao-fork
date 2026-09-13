#include "ui_color_theme.h"

#include <string.h>

#include "epd.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "ui_color";
static const char *NVS_NS = "ui_color";

static const char *const s_page_keys[UI_COLOR_PAGE_COUNT] = {
    "welcome", "clock", "calendar", "weather", "timetable",
    "todo", "countdown", "codex", "system",
};

static const char *const s_profile_names[UI_COLOR_PROFILE_COUNT] = {
    "balanced", "warm", "vivid", "mono",
};

static ui_color_config_t s_config;
static portMUX_TYPE s_config_mux = portMUX_INITIALIZER_UNLOCKED;

static void config_defaults(ui_color_config_t *cfg)
{
    if (!cfg)
        return;
    for (int i = 0; i < UI_COLOR_PAGE_COUNT; i++)
        cfg->profiles[i] = UI_COLOR_PROFILE_BALANCED;
}

static bool config_valid(const ui_color_config_t *cfg)
{
    if (!cfg)
        return false;
    for (int i = 0; i < UI_COLOR_PAGE_COUNT; i++) {
        if (cfg->profiles[i] < UI_COLOR_PROFILE_BALANCED ||
            cfg->profiles[i] >= UI_COLOR_PROFILE_COUNT)
            return false;
    }
    return true;
}

static void config_publish(const ui_color_config_t *cfg)
{
    portENTER_CRITICAL(&s_config_mux);
    s_config = *cfg;
    portEXIT_CRITICAL(&s_config_mux);
}

esp_err_t ui_color_theme_init(void)
{
    ui_color_config_t cfg;
    config_defaults(&cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        for (int i = 0; i < UI_COLOR_PAGE_COUNT; i++) {
            uint8_t value = UI_COLOR_PROFILE_BALANCED;
            if (nvs_get_u8(h, s_page_keys[i], &value) == ESP_OK &&
                value < UI_COLOR_PROFILE_COUNT)
                cfg.profiles[i] = (ui_color_profile_t)value;
        }
        nvs_close(h);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "NVS load failed: %s", esp_err_to_name(err));
    }

    config_publish(&cfg);
    ESP_LOGI(TAG, "JD79668 semantic color profiles ready");
    return ESP_OK;
}

void ui_color_config_get(ui_color_config_t *out)
{
    if (!out)
        return;
    portENTER_CRITICAL(&s_config_mux);
    *out = s_config;
    portEXIT_CRITICAL(&s_config_mux);
}

esp_err_t ui_color_config_set(const ui_color_config_t *cfg)
{
    if (!config_valid(cfg))
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;

    for (int i = 0; i < UI_COLOR_PAGE_COUNT && err == ESP_OK; i++)
        err = nvs_set_u8(h, s_page_keys[i], (uint8_t)cfg->profiles[i]);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK)
        config_publish(cfg);
    return err;
}

esp_err_t ui_color_config_reset(void)
{
    ui_color_config_t cfg;
    config_defaults(&cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK)
        config_publish(&cfg);
    return err;
}

bool ui_color_theme_applicable(void)
{
    return epd_get_panel() == EPD_PANEL_REF_JD79668_42_BWRY;
}

ui_color_profile_t ui_color_profile_get(ui_color_page_t page)
{
    if (page < 0 || page >= UI_COLOR_PAGE_COUNT)
        return UI_COLOR_PROFILE_BALANCED;
    portENTER_CRITICAL(&s_config_mux);
    ui_color_profile_t profile = s_config.profiles[page];
    portEXIT_CRITICAL(&s_config_mux);
    return profile;
}

fb_color_t ui_color_resolve(ui_color_page_t page, ui_color_role_t role,
                            fb_color_t fallback)
{
    if (!ui_color_theme_applicable() || page < 0 || page >= UI_COLOR_PAGE_COUNT)
        return fallback;

    ui_color_profile_t profile = ui_color_profile_get(page);
    if (role == UI_COLOR_ROLE_NEUTRAL)
        return COLOR_BLACK;
    if (profile == UI_COLOR_PROFILE_MONO)
        return COLOR_BLACK;

    if (role == UI_COLOR_ROLE_WARNING)
        return COLOR_RED;
    if (role == UI_COLOR_ROLE_NOTICE)
        return COLOR_YELLOW;

    if (profile == UI_COLOR_PROFILE_WARM)
        return COLOR_YELLOW;
    if (profile == UI_COLOR_PROFILE_VIVID)
        return COLOR_RED;
    return COLOR_BLACK;
}

const char *ui_color_page_key(ui_color_page_t page)
{
    return (page >= 0 && page < UI_COLOR_PAGE_COUNT) ? s_page_keys[page] : NULL;
}

bool ui_color_page_from_key(const char *key, ui_color_page_t *out)
{
    if (!key)
        return false;
    for (int i = 0; i < UI_COLOR_PAGE_COUNT; i++) {
        if (strcmp(key, s_page_keys[i]) == 0) {
            if (out)
                *out = (ui_color_page_t)i;
            return true;
        }
    }
    return false;
}

const char *ui_color_profile_name(ui_color_profile_t profile)
{
    return (profile >= 0 && profile < UI_COLOR_PROFILE_COUNT)
           ? s_profile_names[profile] : NULL;
}

bool ui_color_profile_from_name(const char *name, ui_color_profile_t *out)
{
    if (!name)
        return false;
    for (int i = 0; i < UI_COLOR_PROFILE_COUNT; i++) {
        if (strcmp(name, s_profile_names[i]) == 0) {
            if (out)
                *out = (ui_color_profile_t)i;
            return true;
        }
    }
    return false;
}
