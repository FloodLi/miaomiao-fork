# 喵哎-MiaooAim v3.7 固件包

本目录提供完整烧录固件和网页 OTA 升级固件。两种 BIN 均提供只包含对应 BIN 的 ZIP。

v3.7 发布于 2026-09-13，包含多屏图片工作区、浅粉主题、黄色留言与显示回执修复。分区表及字库保持不变；软件验证通过不等于全型号实屏验收，实验面板保留原有标记。详细变化见[更新日志](../CHANGELOG.md)。

## 文件与校验

| 文件 | 用途 | 大小 | SHA256 |
|------|------|------|--------|
| `epaper_uploader_full_16MB.bin` | ESP32-S3 16MB Flash 首次烧录或完整恢复 | 12713984 bytes | `390682D1C35FA23CA338BD2D953C152B6D9FD2F07981F870D37CE5835ED30DFA` |
| `epaper_uploader_full_16MB.zip` | 完整固件 ZIP | 2294531 bytes | `F70AEB24F69E5C4DB37432AA609126502462C4CDF40A16B6B411239A18E3DE74` |
| `web_upgrade.bin` | 设备后台网页 OTA 升级 | 2999792 bytes | `ADCF8C124D054C2D92E6B29CBBFAD8B76F1EB6F674145B0C90EB895562E6F6EC` |
| `web_upgrade.zip` | 网页 OTA 固件 ZIP | 1592623 bytes | `F8EF4E82E5AF291EAAB758FE55FE2CAEDCACA36284860F4E4D756DCF7538C1A4` |

应用占用 `0x2dc5f0` 字节，3 MiB 应用分区余 145936 字节（约142.5 KiB）；后续增加嵌入资源需持续检查容量。

## 完整烧录

`epaper_uploader_full_16MB.bin` 面向 ESP32-S3 16MB Flash，已经合并：

```text
0x0000   bootloader/bootloader.bin
0x8000   partition_table/partition-table.bin
0x15000  ota_data_initial.bin
0x20000  epaper_uploader.bin
0x940000 fontfs.bin
```

在 Espressif Flash Download Tool 的 `SPIDownload` 页面选择：

```text
File:      epaper_uploader_full_16MB.bin
Address:   0x0000
Chip:      ESP32-S3
Flash:     16MB
SPI MODE:  DIO
SPI SPEED: 80MHz
```

首次烧录、分区变化或完整恢复可使用整包。整包从 `0x0000` 连续写入到 `0xC20000` 之前，包含 NVS 所在区间；即使不点 `ERASE`，也会覆盖 WiFi、面板等 NVS 配置。操作前备份配置和图片；需要保留配置的日常升级应使用下方网页 OTA。

用户 SPIFFS 从 `0xC20000` 开始，不在此整包写入范围内；工具执行整片 `ERASE` 则会一并删除用户图片区。不要将“不含用户图片区”等同于“保留所有配置”。

## 网页升级

已有可正常打开后台的设备，可以解压 `web_upgrade.zip`，在设备配置页的“固件升级 OTA”中上传：

```text
web_upgrade.bin
```

OTA 前需要先在后台设置管理用户名和密码。上传成功后设备会写入备用 app 分区并重启。

网页 OTA 包只包含 app 固件，不包含 bootloader、分区表和 `fontfs`。首次烧录、分区表变化、字库分区变化或设备无法进入后台时，应使用完整固件。

## 下载说明

Gitee 普通用户可继续使用 `firmware-download` 分支获取完整烧录包：

```text
https://gitee.com/gxp666111/miaomiao/repository/archive/firmware-download.zip
```

网页 OTA 固件从仓库 `release/` 目录下载。若浏览器通过 raw 下载后给文件名附加单引号，只需把文件名改回表格中的固定名称；校验 SHA256 可以确认内容是否完整。
