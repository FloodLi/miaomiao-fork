#include "display_mode.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "dmode";

static display_mode_entry_t s_modes[DISPLAY_MODE_MAX];
static int s_count;

int display_mode_register(const display_mode_entry_t *entry)
{
    if (!entry || !entry->name || !entry->show) return -1;
    if (s_count >= DISPLAY_MODE_MAX) {
        ESP_LOGE(TAG, "mode registry full (%d)", DISPLAY_MODE_MAX);
        return -1;
    }
    s_modes[s_count] = *entry;
    ESP_LOGI(TAG, "[%d] %s (%s)", s_count, entry->name, entry->label_cn);
    return s_count++;
}

int display_mode_count(void) { return s_count; }

const display_mode_entry_t *display_mode_get(int idx)
{
    if (idx < 0 || idx >= s_count) return NULL;
    return &s_modes[idx];
}

esp_err_t display_mode_show(int idx)
{
    const display_mode_entry_t *m = display_mode_get(idx);
    if (!m) return ESP_ERR_INVALID_ARG;
    return m->show();
}

const char *display_mode_name(int idx)
{
    const display_mode_entry_t *m = display_mode_get(idx);
    return m ? m->name : "?";
}

const char *display_mode_label(int idx)
{
    const display_mode_entry_t *m = display_mode_get(idx);
    return m ? m->label_cn : "?";
}
