# 喵喵墨水屏固件烧录包

这个下载包给不想安装 ESP-IDF、不会自己编译的小白用户使用。

## 文件

- `epaper_uploader_full_16MB.bin`：整包固件，烧录地址 `0x0000`

## 烧录方式

使用 Espressif Flash Download Tool，在 `SPIDownload` 页面只填一行：

```text
File:    epaper_uploader_full_16MB.bin
Address: 0x0000
```

工具参数建议：

```text
SPI SPEED: 80MHz
SPI MODE:  DIO
FLASH SIZE: 16MB
```

点击 `START` 开始烧录即可。

## 说明

这是整包固件，已经包含 bootloader、partition table、otadata 和 app，不需要再添加其它 bin 文件。

如果是新板子或者配置混乱，可以先点 `ERASE` 再点 `START`；如果想保留 WiFi、天气、面板等配置，不要点 `ERASE`，直接点 `START`。
