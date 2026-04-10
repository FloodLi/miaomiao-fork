

# epaper_uploader — ESP32-S3 智能墨水屏终端

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5.1-blue)](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/)
[![License](https://img.shields.io/badge/license-PolyForm--Noncommercial-orange)](LICENSE)
[![Hardware](https://img.shields.io/badge/hardware-ESP32--S3-orange)]()

> 一款基于 ESP32-S3 的低功耗三色墨水屏智能终端，支持图片展示、天气、时钟、日历、课程表、待办等功能，通过 Web 界面管理，开箱即用。  
> **发布版本：v2.1.0**

## ✨ 项目亮点

- 🖼️ **三色墨水屏** — 支持黑白红三色显示，Floyd-Steinberg 抖色算法
- 📡 **WiFi 配网** — 手机热点一键配网，mDNS 局域网发现
- 🌐 **Web 管理** — 响应式 UI，三套主题，手机/电脑均可操作
- 🔋 **超低功耗** — 闲置可进深度睡眠（μA 级），定时器/按键唤醒；双启动路径（完整服务 ↔ 快速刷屏）
- 🔄 **OTA 升级** — 双分区设计，Web 端一键升级固件
- 🎓 **课程表** — 学期周次管理，自动高亮当前课程
- 🌤️ **天气日历** — 和风天气 API + 农历 + 节气 + 节日
- 📋 **待办/倒计时** — 优先级管理 + 进度条 + 目标日期倒数

## 📷 功能展示

| 功能 | 说明 |
|------|------|
| 图片展示 | JPEG / PNG / BMP 上传，Floyd-Steinberg 抖色，拉伸全屏显示 |
| 画廊轮播 | 自动顺序 / 随机轮播，间隔可配置 |
| 时钟表盘 | 自动刷新，数字时钟样式，可叠加天气摘要 |
| 万年历 | 月视图 + 农历 + 节气 + 节日 + 今日红圈 |
| 天气 | 和风天气 API，实时 + 3 天预报 |
| 课程表 | 学期周次 + 7×12 网格 + 当前/下节课高亮 |
| 待办事项 | 优先级(普通/重要/紧急) + 完成状态 + 进度条 |
| 倒计时 | 最多 3 个目标日期倒数 |
| 留言板 | 自定义文字 / 字号 / 对齐 / 颜色 |
| 物理按键 | 3 键切换模式（上一个 / 刷新 / 下一个） |
| 深度睡眠 | 闲置自动入睡 + 定时器/按键唤醒 |

## 🔧 硬件要求

主控为 **ESP32-S3**（建议 **16MB Flash**，常见 **N16** 无 PSRAM 或 **N16R8**）；墨水屏可选 **4.2" SSD1619（400×300 BWR）** 或 **5.83" UC8179（648×480 BWR）**；电源 **AMS1117-3.3**、烧录可用 **CH340** 或开发板自带 **USB** 等。

### 引脚连接

| 功能 | GPIO | 连接到 |
|------|------|--------|
| EPD SCK | **4** | 墨水屏 SCL |
| EPD MOSI | **5** | 墨水屏 SDI |
| EPD DC | **7** | 墨水屏 D/C |
| EPD CS | **15** | 墨水屏 CS |
| EPD RST | **6** | 墨水屏 RES |
| EPD BUSY | **16** | 墨水屏 BUSY |
| 按键 SW3 | **3** | 上一个模式 |
| 按键 SW4 | **46** | 刷新当前 |
| 按键 SW5 | **9** | 下一个模式 |

## 🚀 快速开始

### 1. 环境准备

安装 [ESP-IDF v5.5.1](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/get-started/)：

```powershell
# Windows: 下载 ESP-IDF Tools Installer
# https://dl.espressif.com/dl/esp-idf/?id=Windows
```

### 2. 克隆项目

```bash
git clone https://gitee.com/gxp666111/miaomiao.git
cd miaomiao
```

### 3. 编译固件

```powershell
# 设置目标芯片
idf.py set-target esp32s3

# 编译（首次编译约 3-5 分钟）
idf.py build
```

### 4. 烧录固件

```powershell
# 首次烧录需先擦除 Flash
idf.py -p COM3 erase-flash

# 烧录固件
idf.py -p COM3 flash

# 查看串口日志
idf.py -p COM3 monitor
```

### 5. 首次使用

1. 设备上电后自动创建 WiFi 热点 `ESP32_EPD_xxxx`（密码 `12345678`）
2. 手机连接该热点，浏览器打开 `http://192.168.4.1/`
3. 进入「设备配置」→ 连接家庭 WiFi
4. 连接后通过 `http://epdxxxx.local/` 或 STA IP 访问

## 📁 项目结构

```
miaomiao/
├── main/                        # 固件源码（C 模块）
│   ├── app_main.c               # 启动流程（双启动路径）
│   ├── epd_stub.c / epd.h       # EPD 驱动（SSD1619 / UC8179）
│   ├── image_convert.c/h        # 图片解码 + Floyd-Steinberg 抖色
│   ├── http_app.c/h             # HTTP 服务
│   ├── http_gallery.c           # 画廊相关路由
│   ├── http_features.c          # 功能模块路由
│   ├── wifi_manager.c/h         # WiFi AP/STA + 配网
│   ├── scheduler.c/h            # 画廊轮播调度
│   ├── weather.c/h              # 和风天气 HTTPS
│   ├── clock_display.c/h        # 时钟表盘
│   ├── calendar_display.c/h     # 万年历 + 农历
│   ├── lunar.c/h                # 农历算法
│   ├── timetable.c/h            # 课程表
│   ├── todo.c/h                 # 待办事项
│   ├── countdown.c/h            # 倒计时
│   ├── message_board.c/h        # 留言板
│   ├── button.c/h               # 三键驱动
│   ├── fb_render.c/h            # 帧缓冲渲染引擎
│   ├── font_data.h              # 点阵字库
│   ├── display_policy.c/h       # 显示策略协调
│   ├── display_mode.c/h         # 显示模式注册与切换
│   ├── power_mgr.c/h            # 深度睡眠与电源管理
│   └── lodepng.c/h              # PNG 解码库
├── web/                         # Web UI
├── tools/                       # 辅助脚本
├── hardware/                    # 硬件文档
├── partitions.csv               # 分区表
├── sdkconfig.defaults           # 项目默认配置
└── CMakeLists.txt               # 顶层构建脚本
```

## 🔌 分区表

| 分区 | 类型 | 大小 | 说明 |
|------|------|------|------|
| nvs | data | 24 KB | 非易失性配置存储 |
| otadata | data | 8 KB | OTA 状态 |
| factory | app | 2 MB | 出厂固件 |
| ota_0 / ota_1 | app | 各 2 MB | OTA 双分区 |
| spiffs | data | ~10 MB | 图片/文件存储 |

## 🌐 Web API 概览

设备提供约 46 条 HTTP 路由，主要分类：

| 分类 | 路径 | 说明 |
|------|------|------|
| 页面 | `/`, `/config`, `/timetable`, `/todo`, `/countdown` | Web UI 页面 |
| 图片 | `/upload`, `/images`, `/image`, `/show`, `/delete` | 画廊管理 |
| WiFi | `/wifi_status`, `/scan`, `/wifi_connect`, `/wifi_forget` | 网络配置 |
| 天气 | `/weather_config`, `/weather_show` | 天气配置与显示 |
| 课程表 | `/timetable.json`, `/timetable`, `/timetable_show` | 课程表管理 |
| 待办 | `/todo.json`, `/todo`, `/todo_show` | 待办事项管理 |
| 系统 | `/status`, `/version`, `/ota`, `/power_config` | 系统管理 |

## ⚡ 低功耗模式

| 模式 | 电流 | 唤醒源 | 说明 |
|------|------|--------|------|
| 活跃 | 150-300mA | 按键/HTTP | 正常工作 |
| 深度睡眠 |