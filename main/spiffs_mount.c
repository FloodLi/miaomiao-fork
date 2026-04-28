#include "spiffs_mount.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_spiffs.h"

static const char *TAG = "spiffs";
static bool s_spiffs_mounted;

static esp_vfs_spiffs_conf_t spiffs_conf(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 16,
        .format_if_mount_failed = false,
    };
    return conf;
}

bool spiffs_mount_is_mounted(void)
{
    return s_spiffs_mounted;
}

esp_err_t spiffs_mount_init(void)
{
    if (s_spiffs_mounted)
        return ESP_OK;

    s_spiffs_mounted = false;

    esp_vfs_spiffs_conf_t conf = spiffs_conf();

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed; keeping user data intact: %s", esp_err_to_name(ret));
        return ret;
    }
    s_spiffs_mounted = true;

    size_t total = 0;
    size_t used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted: total=%u, used=%u", (unsigned)total, (unsigned)used);
    }

    /* Never format here: a full filesystem can fail this probe too. */
    bool fs_ok = false;
    FILE *tf = fopen("/spiffs/_test.tmp", "wb");
    if (tf) {
        const char probe[] = "SPIFFS_OK";
        bool wrote = (fwrite(probe, 1, sizeof(probe), tf) == sizeof(probe));
        fclose(tf);
        tf = wrote ? fopen("/spiffs/_test.tmp", "rb") : NULL;
        if (tf) {
            char rb[16] = {0};
            fread(rb, 1, sizeof(probe), tf);
            fclose(tf);
            fs_ok = (memcmp(rb, probe, sizeof(probe)) == 0);
        }
        remove("/spiffs/_test.tmp");
    }

    if (fs_ok)
        ESP_LOGI(TAG, "SPIFFS write/read test passed");
    else
        ESP_LOGW(TAG, "SPIFFS write/read test failed; keeping existing filesystem");

    struct stat st;
    if (stat("/spiffs/images", &st) != 0) {
        mkdir("/spiffs/images", 0755);
    }

    return ESP_OK;
}

esp_err_t spiffs_mount_retry(void)
{
    if (s_spiffs_mounted)
        return ESP_OK;

    esp_vfs_spiffs_unregister(NULL);
    return spiffs_mount_init();
}

esp_err_t spiffs_mount_format_and_mount(void)
{
    esp_vfs_spiffs_conf_t conf = spiffs_conf();

    if (s_spiffs_mounted) {
        esp_vfs_spiffs_unregister(conf.partition_label);
        s_spiffs_mounted = false;
    } else {
        esp_vfs_spiffs_unregister(conf.partition_label);
    }

    ESP_LOGW(TAG, "Formatting SPIFFS by explicit user request");
    esp_err_t ret = esp_spiffs_format(conf.partition_label);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS format failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return spiffs_mount_init();
}
