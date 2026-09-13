#include "welcome_config.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

static const char *TAG = "welcome_cfg";
static const char *NVS_NS = "welcome";

static portMUX_TYPE s_cfg_mux = portMUX_INITIALIZER_UNLOCKED;
static welcome_config_t s_cfg;

void welcome_config_defaults(welcome_config_t *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->enabled = true;
    snprintf(out->title, sizeof(out->title), "%s", "喵哎-MiaooAim");
}

static void sanitize_line(char *text)
{
    if (!text)
        return;
    for (char *p = text; *p; p++) {
        if ((unsigned char)*p < 0x20)
            *p = ' ';
    }
}

static bool config_valid(const welcome_config_t *cfg)
{
    if (!cfg)
        return false;
    if (strnlen(cfg->title, sizeof(cfg->title)) >= sizeof(cfg->title))
        return false;
    if (strnlen(cfg->message, sizeof(cfg->message)) >= sizeof(cfg->message))
        return false;
    return true;
}

static void publish(const welcome_config_t *cfg)
{
    portENTER_CRITICAL(&s_cfg_mux);
    s_cfg = *cfg;
    portEXIT_CRITICAL(&s_cfg_mux);
}

esp_err_t welcome_config_init(void)
{
    welcome_config_t cfg;
    welcome_config_defaults(&cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        uint8_t value = 0;
        if (nvs_get_u8(h, "enabled", &value) == ESP_OK)
            cfg.enabled = value != 0;
        size_t len = sizeof(cfg.title);
        if (nvs_get_str(h, "title", cfg.title, &len) != ESP_OK)
            snprintf(cfg.title, sizeof(cfg.title), "%s", "喵哎-MiaooAim");
        len = sizeof(cfg.message);
        if (nvs_get_str(h, "message", cfg.message, &len) != ESP_OK)
            cfg.message[0] = '\0';
        nvs_close(h);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "NVS load failed: %s", esp_err_to_name(err));
    }

    sanitize_line(cfg.title);
    sanitize_line(cfg.message);
    if (!cfg.title[0])
        snprintf(cfg.title, sizeof(cfg.title), "%s", "喵哎-MiaooAim");
    publish(&cfg);
    ESP_LOGI(TAG, "ready enabled=%d custom_message=%d",
             cfg.enabled ? 1 : 0,
             cfg.message[0] ? 1 : 0);
    return ESP_OK;
}

void welcome_config_get(welcome_config_t *out)
{
    if (!out)
        return;
    portENTER_CRITICAL(&s_cfg_mux);
    *out = s_cfg;
    portEXIT_CRITICAL(&s_cfg_mux);
}

esp_err_t welcome_config_set(const welcome_config_t *cfg)
{
    if (!config_valid(cfg))
        return ESP_ERR_INVALID_ARG;

    welcome_config_t next = *cfg;
    sanitize_line(next.title);
    sanitize_line(next.message);
    if (!next.title[0])
        snprintf(next.title, sizeof(next.title), "%s", "喵哎-MiaooAim");

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = nvs_set_u8(h, "enabled", next.enabled ? 1 : 0);
    if (err == ESP_OK)
        err = nvs_set_str(h, "title", next.title);
    if (err == ESP_OK)
        err = nvs_set_str(h, "message", next.message);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK)
        publish(&next);
    return err;
}
