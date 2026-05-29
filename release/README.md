# ESP32-S3 E-Paper Firmware Flash Guide

本文档给不熟悉 ESP-IDF 的用户使用。只需要一个整包固件文件和 Espressif 下载工具即可完成烧录。

## 固件文件

推荐下载烧录包：

```text
epaper_uploader_full_16MB.zip
```

解压后使用整包固件：

```text
epaper_uploader_full_16MB.bin
```

校验信息：

```text
Size:   2017520 bytes
SHA256: D9E3F6D23158A442CF8597A2998E9F878DD1F4C5FAF960AE3F82503E0039B5B1
```

ZIP 校验信息：

```text
Size:   1056314 bytes
SHA256: E8DB81E17B2DDCE875A061BEB65733DFD759B3340B602C9106202D3E38CA1F52
```

如果直接下载 `.bin` 后文件名带了单引号，例如：

```text
'epaper_uploader_full_16MB.bin'
epaper_uploader_full_16MB.bin'
```

请手动重命名为：

```text
epaper_uploader_full_16MB.bin
```

或者直接下载 `.zip`，解压出来的文件名会是正常的。

适用范围：

```text
Chip:       ESP32-S3
Flash:      16MB
SPI Mode:   DIO
SPI Speed:  80MHz
App offset: 0x20000
```

## 下载工具一行烧录

在 Espressif Flash Download Tool 的 `SPIDownload` 页面只填一行：

```text
File:    release\epaper_uploader_full_16MB.bin
Address: 0x0000
```

推荐参数：

```text
SPI SPEED: 80MHz
SPI MODE:  DIO
DoNotChgBin: 勾选
BAUD: 115200 或 460800
COM: 选择设备对应串口
```

如果工具里有 Flash Size 选项，请选择：

```text
16MB
```

然后点击：

```text
START
```

等待下载完成后，设备会自动重启。

## 是否需要点 ERASE

新板子、配置混乱、SPIFFS 挂载异常时：

```text
可以先点 ERASE，再点 START
```

已经配置过 WiFi、天气、日历，不想丢配置时：

```text
不要点 ERASE，直接点 START
```

说明：

```text
整包固件会写入 bootloader、partition table、otadata 和 app。
不点 ERASE 时，NVS 配置区和 SPIFFS 数据区不会被主动清空。
```

## 整包内包含的分区

这个整包固件已经合并以下文件：

```text
0x0000   bootloader/bootloader.bin
0x8000   partition_table/partition-table.bin
0x15000  ota_data_initial.bin
0x20000  epaper_uploader.bin
```

因此普通用户不需要再分别选择 4 个文件。

## 常见问题

### 下载失败或一直等待

请检查：

```text
1. COM 口是否选对
2. USB 数据线是否支持数据传输
3. 串口是否被 IDF monitor、Arduino、其他下载工具占用
4. 必要时按住 BOOT 键，再点 START，开始后松开 BOOT
```

### 烧录后还是旧配置

如果希望清空旧 WiFi、天气、面板等配置：

```text
先点 ERASE，再点 START
```

### 烧录后无法访问设备

如果没有保存 WiFi 配置，设备会开启 AP：

```text
SSID: ESP32_EPD_xxxxxx
Web:  http://192.168.4.1/
```

连接 AP 后进入网页配置 WiFi、天气和屏幕参数。

## 开发者重新生成整包

在 ESP-IDF 环境中先编译：

```powershell
idf.py build
```

然后合并固件：

```powershell
python -m esptool --chip esp32s3 merge_bin `
  -o release\epaper_uploader_full_16MB.bin `
  --flash_mode dio --flash_freq 80m --flash_size 16MB `
  0x0 build\bootloader\bootloader.bin `
  0x8000 build\partition_table\partition-table.bin `
  0x15000 build\ota_data_initial.bin `
  0x20000 build\epaper_uploader.bin
```
