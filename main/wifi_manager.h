#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WIFI_MGR_MODE_NONE,
    WIFI_MGR_MODE_AP,
    WIFI_MGR_MODE_STA,
    WIFI_MGR_MODE_APSTA,
} wifi_mgr_mode_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;   /* wifi_auth_mode_t */
} wifi_mgr_ap_record_t;

/**
 * Initialize WiFi subsystem.
 * Reads stored credentials from NVS; if found, attempts STA connection.
 * Falls back to AP mode on STA failure or if no credentials stored.
 * AP SSID/password used for the fallback/provisioning hotspot.
 */
esp_err_t wifi_manager_init(const char *ap_ssid, const char *ap_pass);

/** 当前 SoftAP 广播的 SSID（与 device_identity / 传入 init 一致） */
const char *wifi_manager_get_ap_ssid(void);

/** 设置 STA 在路由器 DHCP 客户端列表中显示的主机名（建议在 init 后调用一次） */
void wifi_manager_set_sta_hostname(const char *hostname);

/** Current operating mode. */
wifi_mgr_mode_t wifi_manager_get_mode(void);

/** true if STA is connected to an external router. */
bool wifi_manager_sta_connected(void);

/** Get STA IP as string (e.g. "192.168.1.105"). Empty if not connected. */
const char *wifi_manager_get_sta_ip(void);

/** Get the SSID we are connected to (or last attempted). */
const char *wifi_manager_get_sta_ssid(void);

/**
 * Scan nearby APs. Caller provides array + capacity.
 * Returns actual count found (capped to max_records).
 */
int wifi_manager_scan(wifi_mgr_ap_record_t *out, int max_records);

/**
 * Save credentials to NVS and attempt STA connection.
 * On success the device enters APSTA mode.
 * On failure it stays in AP mode.
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password);

/** Erase stored credentials from NVS and revert to AP-only mode. */
esp_err_t wifi_manager_forget(void);
