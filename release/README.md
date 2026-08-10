# 喵哎-MiaooAim v2.3.6 固件包

本目录提供完整烧录固件和网页 OTA 升级固件。两种 BIN 均提供只包含对应 BIN 的 ZIP。

## 文件与校验

| 文件 | 用途 | 大小 | SHA256 |
|------|------|------|--------|
| `epaper_uploader_full_16MB.bin` | ESP32-S3 16MB Flash 首次烧录或完整恢复 | 12713984 bytes | `D896E241C9567F83C0E0F7E0DFF430E4490726E9D1CE88B9D938B6E996AAE80E` |
| `epaper_uploader_full_16MB.zip` | 完整固件 ZIP | 2232201 bytes | `CEF7E59E46578BD35EC414D7561D23750381D81B96F13510A3BD44C84E1F8783` |
| `web_upgrade.bin` | 设备后台网页 OTA 升级 | 2795936 bytes | `EA8EF1FFE9DDF5E28DE96DE94B7FBE74DCA460357B404D5F2962CB26AD528ED7` |
| `web_upgrade.zip` | 网页 OTA 固件 ZIP | 1528977 bytes | `B1C91A23A1B6394CFBD6EAE302811F2BE7B1CE9445855E8EFFC8A88E5BBBF2BD` |

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

新板子、分区变化或配置损坏时可以先 `ERASE`。需要保留已有 NVS 配置和用户 SPIFFS 文件时，不要执行 `ERASE`。

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
