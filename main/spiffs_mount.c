#include "spiffs_mount.h"

#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_spiffs.h"

static const char *TAG = "spiffs";

esp_err_t spiffs_mount_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 16,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed: %s, will format and retry", esp_err_to_name(ret));
        esp_vfs_spiffs_unregister(conf.partition_label);
        esp_err_t fret = esp_spiffs_format(conf.partition_label);
        if (fret != ESP_OK) {
            ESP_LOGE(TAG, "SPIFFS format failed: %s", esp_err_to_name(fret));
            return ret;
        }
        ret = esp_vfs_spiffs_register(&conf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPIFFS mount retry failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    size_t total = 0;
    size_t used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted: total=%u, used=%u", (unsigned)total, (unsigned)used);
    }

    /* quick write/read test — much faster than esp_spiffs_check() */
    bool fs_ok = false;
    FILE *tf = fopen("/spiffs/_test.tmp", "wb");
    if (tf) {
        const char probe[] = "SPIFFS_OK";
        fwrite(probe, 1, sizeof(probe), tf);
        fclose(tf);
        tf = fopen("/spiffs/_test.tmp", "rb");
        if (tf) {
            char rb[16] = {0};
            fread(rb, 1, sizeof(probe), tf);
            fclose(tf);
            fs_ok = (memcmp(rb, probe, sizeof(probe)) == 0);
        }
        remove("/spiffs/_test.tmp");
    }

    if (fs_ok) {
        ESP_LOGI(TAG, "SPIFFS write/read test passed");
    } else {
        ESP_LOGW(TAG, "SPIFFS write/read test FAILED — formatting");
        esp_vfs_spiffs_unregister(conf.partition_label);
        esp_spiffs_format(conf.partition_label);
        ret = esp_vfs_spiffs_register(&conf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPIFFS re-mount after format failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "SPIFFS formatted and re-mounted");
    }

    struct stat st;
    if (stat("/spiffs/images", &st) != 0) {
        mkdir("/spiffs/images", 0755);
    }

    return ESP_OK;
}

