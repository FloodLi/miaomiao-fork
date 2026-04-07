#pragma once

#include <stdbool.h>

/**
 * 用 WiFi STA MAC 后 3 字节生成唯一标识，避免多台设备同局域网时
 * SoftAP / mDNS 名称冲突。须在 nvs_flash_init 之后、wifi_manager_init 之前调用。
 */
void device_identity_init(void);

const char *device_identity_get_ap_ssid(void);
/** mDNS 主机名（小写、无 .local 后缀），如 epd2e5c78 */
const char *device_identity_get_mdns_hostname(void);
/** mDNS 服务实例展示名 */
const char *device_identity_get_mdns_instance(void);
/** 6 位十六进制大写，如 2E5C78，便于在 UI 标注 */
const char *device_identity_get_id6_upper(void);
