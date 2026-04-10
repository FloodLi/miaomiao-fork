# ESP32 墨水屏项目工程技术文档

> **项目版本**: v2.1.0（`CMakeLists.txt` → `PROJECT_VER` → `GET /version` 中 `version`）  
> **文档版本**: 2.1（随工程文档修订）  
> **最后更新**: 2026-04-10  
> **硬件平台**: ESP32-S3 + SSD1619(4.2") / UC8179(5.83")  
> **软件框架**: ESP-IDF v5.5.1  
> **文档状态**: ✅ 完整

---

## 目录

1. [项目概述](#一项目概述)
2. [系统架构](#二系统架构)
3. [硬件设计](#三硬件设计)
4. [软件架构](#四软件架构)
5. [模块详细说明](#五模块详细说明)
6. [API 接口文档](#六 api 接口文档)
7. [Web 前端文档](#七 web 前端文档)
8. [构建与部署](#八构建与部署)
9. [故障排查](#九故障排查)
10. [附录](#十附录)

---

## 一、项目概述

### 1.1 项目简介

ESP32 墨水屏项目是一款基于 ESP32-S3 的低功耗智能显示终端，支持黑白红三色墨水屏显示。项目集成了 WiFi 连接、图片上传、天气显示、时钟日历、课程表、待办事项等多种功能，可通过 Web 界面进行配置和管理。

### 1.2 核心特性

- ✅ **双屏支持**: 4.2" (400×300) 和 5.83" (648×480) 三色墨水屏
- ✅ **WiFi 连接**: STA/AP 模式，mDNS 发现，自动重连
- ✅ **图片显示**: JPEG/PNG/BMP 格式，Floyd-Steinberg 抖动算法
- ✅ **Web 管理**: 响应式 UI，三套主题，OTA 升级
- ✅ **内容丰富**: 天气、时钟、日历、课程表、待办、倒计时、留言板
- ✅ **低功耗**: 深度睡眠，定时器/按键唤醒，80+ 天续航
- ✅ **物理交互**: 三按键设计，模式切换，便捷操作

### 1.3 技术栈

| 层级 | 技术 | 版本 |
|------|------|------|
| 硬件 | ESP32-S3（常见 N16 无 PSRAM；可选 N16R8） | 16MB Flash；**固件内存策略按无 PSRAM** |
| 框架 | ESP-IDF | v5.5.1 |
| 驱动 | SSD1619 / UC8179 | 原厂驱动 |
| 文件系统 | SPIFFS | 10MB |
| 网络 | WiFi 802.11 b/g/n | SoftAP + STA |
| 协议 | HTTP/1.1, SPI, I2C | - |
| 前端 | HTML5/CSS3/JavaScript | 原生实现 |

### 1.4 性能指标

| 指标 | 数值 | 备注 |
|------|------|------|
| 图片上传→显示 | <5 秒 | 含解码 + 传输 + 刷新 |
| WiFi 连接时间 | <3 秒 | 冷启动 |
| 屏幕刷新时间 | 15-20 秒 | 三色全刷 |
| 待机功耗 | <10μA | 深度睡眠 |
| 工作电流 | 150-300mA | WiFi+ 刷新 |
| 续航时间 | 80+ 天 | 60 分钟刷新间隔 |
| 堆内存空闲 | 因场景波动 | 无 PSRAM 下需控制 JPEG/PNG 峰值；见 `IMPLEMENTATION.md` |

---

## 二、系统架构

### 2.1 硬件架构

```
┌─────────────────────────────────────────────────────┐
│                  ESP32-S3-WROOM-1U                   │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  │
│  │   CPU Dual  │  │  WiFi/BT    │  │ Flash 16MB  │  │
│  │   Core 240M │  │  802.11n    │  │ PSRAM 可选  │  │
│  └─────────────┘  └─────────────┘  └─────────────┘  │
│         │                │                 │         │
│  ┌──────▼────────────────▼─────────────────▼──────┐  │
│  │              System Bus                         │  │
│  └──┬──────────┬──────────┬──────────┬──────────┬─┘  │
│     │          │          │          │          │    │
│  ┌──▼──┐  ┌────▼───┐  ┌──▼────┐  ┌─▼────┐  ┌─▼───┐ │
│  │ SPI │  │  USB   │  │  I2C  │  │ GPIO │  │ ADC │ │
│  │ EPD │  │  UART  │  │ NVS   │  │ BTN  │  │ PWR │ │
│  └─────┘  └────────┘  └───────┘  └──────┘  └─────┘ │
└─────────────────────────────────────────────────────┘
     │            │          │        │       │
┌────▼────┐  ┌───▼───┐  ┌──▼──┐  ┌─▼──┐  ┌─▼────┐
│  EPD    │  │USB-C  │  │NVS  │  │按键│  │电源  │
│ 4.2/5.83│  │CH340  │  │Flash│  │×3  │  │管理  │
└─────────┘  └───────┘  └─────┘  └────┘  └──────┘
```

### 2.2 软件架构

```
┌─────────────────────────────────────────────────────┐
│              Web Frontend (HTML/JS)                  │
│  ┌────────┐ ┌────────┐ ┌────────┐ ┌──────────────┐  │
│  │  主页  │ │  配置  │ │课程表  │ │  待办事项    │  │
│  └───┬────┘ └───┬────┘ └───┬────┘ └─────┬────────┘  │
└──────┼──────────┼──────────┼────────────┼───────────┘
       │          │          │            │
       │  HTTP REST API (JSON)           │
       │          │          │            │
┌──────▼──────────▼──────────▼────────────▼───────────┐
│              HTTP Server (http_app.c)                │
│         ~46 Routes + Basic Auth + OTA                │
└────┬──────────┬──────────┬──────────┬──────────────┘
     │          │          │          │
┌────▼───┐ ┌───▼────┐ ┌───▼─────┐ ┌─▼────────────┐
│ 图片   │ │ WiFi   │ │ 业务    │ │ 系统管理     │
│ 处理   │ │ 管理   │ │ 逻辑    │ │              │
└────┬───┘ └───┬────┘ └───┬─────┘ └─┬────────────┘
     │          │          │         │
┌────▼──────────▼──────────▼─────────▼──────────────┐
│              Hardware Abstraction Layer            │
│  ┌────────┐ ┌────────┐ ┌────────┐ ┌────────────┐  │
│  │  EPD   │ │ SPIFFS │ │  NVS   │ │  Power     │  │
│  │ Driver │ │  FS    │ │ Storage│ │  Manager   │  │
│  └────────┘ └────────┘ └────────┘ └────────────┘  │
└─────────────────────────────────────────────────────┘
```

### 2.3 数据流

```
用户操作
   │
   ▼
Web UI / 按键输入
   │
   ▼
HTTP Server / Button Handler
   │
   ▼
业务逻辑处理 (Weather/Clock/Todo/Timetable)
   │
   ▼
数据持久化 (NVS/SPIFFS)
   │
   ▼
显示渲染 (Framebuffer)
   │
   ▼
EPD 驱动刷新
   │
   ▼
墨水屏显示
```

---

## 三、硬件设计

### 3.1 核心元器件

#### ESP32-S3 模组（与 `partitions.csv` 匹配）

**Flash 容量：** 本仓库分区表约需 **16MB** 级 Flash（factory + 双 OTA 各 2MB + 约 10MB SPIFFS）。**仅 8MB Flash（如部分 N8 模组）无法直接用于当前默认分区布局**，需改分区表后再谈兼容。

**PSRAM：** 常见硬件为 **N16（无 PSRAM）**；亦可选 **N16R8**。**固件逻辑按无 PSRAM 做内存峰值控制**，带 PSRAM 机型可直接烧录同一固件。

**示例模组名：** `ESP32-S3-WROOM-1U-N16`、`ESP32-S3-WROOM-1U-N16R8`（具体以采购料号为准）。

**规格参数（典型）:**
- CPU: 双核 Xtensa LX7, 240MHz
- WiFi: 802.11 b/g/n (2.4GHz)
- Bluetooth: 5.0 LE（视模组是否裁切）
- 封装：SMD 模组

**应用引脚分配（4.2" 默认接法，见 `README.md`；接线矢量图见 `hardware/wiring-epd-42-default.svg`）:**
```
SPI (EPD):
  SCK  = GPIO4
  MOSI = GPIO5
  DC   = GPIO7
  CS   = GPIO15
  RST  = GPIO6
  BUSY = GPIO16

USB (CH340):
  TX   = GPIO43
  RX   = GPIO44

按键:
  BOOT = GPIO0
  RST  = GPIO18
  SW3  = GPIO3
  SW4  = GPIO46
  SW5  = GPIO9

电源管理:
  EN   = GPIO1
  ADC  = GPIO10
```

#### 墨水屏

**4.2" SSD1619:**
- 分辨率：400 × 300
- 颜色：黑白红三色
- 接口：SPI
- 刷新：~15 秒
- 功耗：~150mA (刷新)

**5.83" UC8179:**
- 分辨率：648 × 480
- 颜色：黑白红三色
- 接口：SPI
- 刷新：~20 秒
- 功耗：~200mA (刷新)

### 3.2 电源设计

```
USB 5V
   │
   ▼
┌─────────────┐
│ AMS1117-3.3 │─── 3.3V (系统电源)
└─────────────┘
   │
   ├─ ESP32-S3 (最大 500mA)
   ├─ EPD (最大 300mA)
   └─ 外设 (50mA)
```

**电源监控:**
- 输入电压：5V ±5%
- 输出电压：3.3V ±3%
- 最大电流：1A
- 效率：>85%

### 3.3 PCB 设计

**板层:** 2 层
**尺寸:** 80×60mm
**工艺:**
- 线宽/线距：≥6mil
- 过孔：≥0.3mm
- 板厚：1.6mm
- 表面处理：有铅喷锡

**布局要点:**
1. ESP32 模组居中放置
2. 电源部分靠近输入端
3. EPD 接口靠近一侧
4. USB-C 靠近边缘
5. 按键均匀分布

---

## 四、软件架构

### 4.1 启动流程（双路径）

`app_main()` 在 `power_mgr_init()` 之后分支：

```
上电 / 复位 / 唤醒
        │
        ▼
   power_mgr_init()  （读深睡配置 + 唤醒原因）
        │
        ├── 深睡已启用 且 唤醒原因为 RTC 定时器 ──► quick_refresh_and_sleep()
        │        · STA 联网、SNTP、EPD、业务模块最小集
        │        · 刷新 NVS 记忆的显示模式后再次 deep sleep
        │        · 无 SoftAP、无 mDNS、无 http_app、无按键任务
        │
        └── 其他（上电、按键 GPIO3/9 唤醒等）──────► full_boot()
                 · AP+STA、mDNS、HTTP、按键、欢迎屏逻辑、power_mgr_arm()
```

完整逻辑见 `main/app_main.c`。

### 4.2 主要 FreeRTOS 任务（非 exhaustive）

| 组件 | 典型任务名 | 说明 |
|------|------------|------|
| HTTP | `httpd` | `esp_http_server` 处理请求；`open_fn` 可重置闲置计时 |
| 轮播 | `slideshow` | EventGroup 驱动，`scheduler.c` |
| 天气 | `weather` | 定时/事件拉取 HTTPS，`weather.c` |
| 时钟 | `clock_disp` | 表盘刷新，`clock_display.c` |
| 闲置 | `pwr_idle` | 深空前闲置检测，`power_mgr.c` |
| 按键 | `btn_poll` / `btn_disp` | 轮询 + 显示队列，`button.c` |

### 4.3 内存管理

**内部 SRAM**（无 PSRAM 机型）：动态堆约数百 KB 量级，JPEG 解码 + 帧缓冲不能长时间叠加大分配；详见 `IMPLEMENTATION.md` 峰值表与优化手段。

**要点：** 双平面帧缓冲合并一次 `malloc`、`epd_display_fb_free` 早释放、PNG 路径内存高于 JPEG、画廊/转换时注意 SPIFFS 与堆竞争。

### 4.4 低功耗与功能的交集

| 场景 | HTTP | 轮播定时切图 | 天气后台刷新 | 备注 |
|------|------|--------------|--------------|------|
| `full_boot` 运行中 | ✅ | ✅ | ✅ | `power_mgr_arm()` 后闲置超时可深睡 |
| 定时器唤醒 quick 路径 | ❌ | ❌ | ❌ | 仅刷当前模式一帧；依赖上次完整运行缓存的数据 |
| 按键唤醒 | ✅（完整启动） | ✅ | ✅ | EXT1，非 timer wake |

**OTA / 大文件上传**：闲置计时在 **新 TCP 会话建立** 时重置；极慢的传输若超过 `idle_timeout_s`，理论上可能触发深睡，刷机时可暂时关闭深睡或拉大闲置时间。

### 4.5 HTTP 安全（Basic Auth）

- **策略**：在 NVS 中写入用户名/密码后即视为启用鉴权；`http_check_basic_auth()` 对配置的敏感 **POST**（及个别「推屏」类接口）校验 `Authorization: Basic …`。**未配置凭据时通常不拦截**（便于首次配网）。
- **天气**：`GET /weather_config` 不返回明文 API Key；`POST` 可通过空字符串跳过改 key、`api_key_clear` 等策略与固件版本一致。
- **前端**：`config.html` / `index.html` / `timetable.html` / `todo.html` / `countdown.html` 使用 `localStorage`（`epd_auth_u`、`epd_auth_p`）构造 Basic 头；**需先在配置页保存认证**，子页面才能匿名以外的方式通过鉴权。

---

## 五、模块详细说明

### 5.1 EPD 驱动模块 (`epd_stub.c`)

**功能:**
- SSD1619/UC8179 驱动
- SPI DMA 批量传输
- BUSY 信号处理
- 刷新序列控制

**关键函数:**
```c
esp_err_t epd_init(void);
esp_err_t epd_display_from_buffer(const fb_t *fb);
esp_err_t epd_display_from_file(const char *path);
esp_err_t epd_set_panel(epd_panel_t panel);
epd_panel_t epd_get_panel(void);
```

**技术要点:**
1. DMA 4KB 分块传输
2. BUSY 极性自动检测
3. 双屏参数自动适配
4. 刷新序列优化

### 5.2 图片转换模块 (`image_convert.c`)

**功能:**
- JPEG/PNG/BMP解码
- Floyd-Steinberg 抖动
- HSV 红色识别
- 自适应缩放

**支持格式:**
```
JPEG: Baseline, Progressive
PNG:  8/16/24/32-bit
BMP:  24/32-bit RGB
```

**算法流程:**
```
输入图片
   │
   ▼
格式检测 (Magic Bytes)
   │
   ▼
解码 (TJpgDec/LodePNG/BMP)
   │
   ▼
RGBA → RGB 转换
   │
   ▼
HSV 红色识别
   │
   ▼
Floyd-Steinberg 抖动
   │
   ▼
Black/Red Plane 输出
```

### 5.3 WiFi 管理模块 (`wifi_manager.c`)

**功能:**
- AP/STA 共存
- 配网管理
- 自动重连
- mDNS 服务

**状态机:**
```
INIT → AP_MODE → SCANNING → CONNECTING → CONNECTED
                      ↑                      │
                      └────── DISCONNECT ────┘
```

**重连策略:**
- 指数退避：10s → 300s
- 最大重试：10 次
- 失败回退：AP 模式

### 5.4 HTTP 服务器模块 (`http_app.c` + `http_gallery.c` + `http_features.c`)

**路由数量:** 46 条（`http_app_start` 中 `uris[]` 静态表，`sizeof(uris)/sizeof(uris[0])`）
**认证方式:** HTTP Basic Auth
**最大连接:** 5 个并发

**路由分组（共 46 条；以下仅作检索，**勿将旧版文档中的分组条数相加验算**）：**

| 分组 | 路径示例 |
|------|-----------|
| 页面与状态 | `GET /` `/config` `/favicon.ico` `/status` |
| 画廊 | `POST /upload` · `GET /images` `/image` · `POST /show` `/delete_image` `/delete` |
| 网络与面板 | `GET /wifi_status` `/scan` · `POST /wifi_connect` `/wifi_forget` · `GET|POST /panel_config` |
| 轮播 / 认证 / 电源 | `GET|POST /slideshow` · `GET|POST /auth_config` · `GET|POST /power_config` |
| 天气 / 时钟 / 留言 | `GET|POST /weather_config` · `POST /weather_show` · `GET|POST /clock_config` · `POST /clock_show` · `GET|POST /msg_config` · `POST /msg_show` |
| 日历 / 课程表 / 待办 / 倒计时 | `POST /calendar_show` · `GET /timetable` `/timetable.json` · `POST /timetable` `/timetable_show` · `GET /todo` `/todo.json` · `POST /todo` `/todo_show` · `GET /countdown` · `GET|POST /countdown_config` · `POST /countdown_show` |
| 系统 | `GET /version` · `POST /ota` |

### 5.5 帧缓冲渲染模块 (`fb_render.c`)

**功能:**
- 像素绘制
- 几何图形
- UTF-8 文字渲染
- 位图绘制

**字库:**
- ASCII: 95 字符 (8×16)
- 中文：81 字符 (16×16, Unifont)
- 扩展：常用汉字 3000+

**渲染原语:**
```c
fb_pixel(fb, x, y, color);
fb_line(fb, x1, y1, x2, y2, color);
fb_rect(fb, x, y, w, h, color);
fb_fill_rect(fb, x, y, w, h, color);
fb_utf8_scaled(fb, x, y, text, color, scale);
fb_bitmap(fb, x, y, black, red, w, h);
```

### 5.6 电源管理模块 (`power_mgr.c`)

**功能:**
- 深度睡眠
- 定时器唤醒
- EXT1 GPIO 唤醒
- 闲置监控

**功耗模式:**
| 模式 | 电流 | 唤醒源 |
|------|------|--------|
| 活跃 | 150-300mA | 按键/HTTP |
| 闲置 | 50mA | 按键/定时器 |
| 深睡 | <10μA | EXT1/RTC |

**唤醒后的软件路径**（与 `power_mgr_is_timer_wake()` 一致：仅 **定时器 + 深睡启用** 才走 quick）：

- **RTC 定时器唤醒** → `quick_refresh_and_sleep()` → 无 HTTP。
- **EXT1 按键（GPIO3/9）/ 上电复位** → `full_boot()` → 有 HTTP。

---

## 六、API 接口文档

### 6.1 图片管理 API

#### POST /upload

**功能:** 上传图片

**请求:**
```
Content-Type: multipart/form-data

file: <image.jpeg>
```

**响应:**
```json
{
  "ok": true,
  "path": "/spiffs/image_001.jpeg"
}
```

#### GET /images

**功能:** 获取图片列表

**响应:**
```json
{
  "files": [
    {"name": "image_001.jpeg", "size": 52340},
    {"name": "image_002.png", "size": 48120}
  ],
  "total": 2,
  "free_space": 9487654
}
```

#### POST /show

**功能:** 将画廊中已有文件解码并刷新到墨水屏（需 Basic Auth，若已启用）。

**请求:** Query 参数 `name` — 与 `GET /images` 返回的文件名一致，例如：

```http
POST /show?name=img_20260101.jpg
```

成功时正文多为纯文本 `OK`（与前端约定一致）。

### 6.2 配置 API

#### GET /wifi_status

**功能:** 获取 WiFi 状态

**响应:**
```json
{
  "sta_connected": true,
  "ssid": "Xiaomi",
  "rssi": -45,
  "ip": "192.168.1.100",
  "ap_ssid": "ESP32_EPD_2E5C78"
}
```

#### POST /wifi_connect

**功能:** 连接 WiFi

**请求:**
```json
{
  "ssid": "Xiaomi",
  "password": "12345678"
}
```

### 6.3 业务 API

#### GET /timetable.json

**功能:** 获取课程表配置

**响应:**
```json
{
  "enabled": true,
  "period_count": 10,
  "show_days": 5,
  "semester_start": 1743465600,
  "periods": [
    {"start_hour": 8, "start_minute": 0, "end_hour": 8, "end_minute": 45},
    {"start_hour": 8, "start_minute": 55, "end_hour": 9, "end_minute": 40}
  ],
  "grid": [
    [
      {"name": "高等数学", "room": "A101", "weeks": "1-16"},
      null,
      {"name": "大学英语", "room": "B203", "weeks": "1,3,5,7-9"}
    ],
    [null, null, null]
  ]
}
```

**数据结构说明:**
- `period_count`: 节次数量（1-12）
- `show_days`: 显示天数（5=工作日，7=含周末）
- `semester_start`: 学期起始 Unix 时间戳
- `periods[]`: 每节课的时间定义
- `grid[day][period]`: 7天×12节课程网格
  - `name`: 课程名称（最多 39 字符）
  - `room`: 教室（最多 19 字符）
  - `weeks`: 上课周次（如 "1-5,7,9-12"）

#### POST /timetable

**功能:** 设置课程表

**请求:** 同上 (JSON)

#### POST /timetable_show

**功能:** 显示课程表（需 Basic Auth，若已启用）。

**请求体（可选）** JSON：`{"day":0..6}`，`0`=周一 … `6`=周日；空体或无字段则按「今天」策略由固件解析。

**响应:**
```json
{"ok": true}
```

#### GET /countdown_config

**功能:** 获取倒计时配置

**响应:**
```json
{
  "enabled": true,
  "count": 2,
  "items": [
    {"title": "期末考试", "year": 2026, "month": 7, "day": 1, "active": true},
    {"title": "暑假", "year": 2026, "month": 7, "day": 15, "active": true}
  ]
}
```

#### POST /countdown_config

**功能:** 设置倒计时配置（需 Basic Auth，若已启用）

**请求:** 与 GET 结构相同：`enabled`、`count`（可选，由 `items` 推导）、`items[]` 每项含 `title`、`year`、`month`、`day`、`active`

#### POST /countdown_show

**功能:** 显示倒计时

**响应:**
```json
{"ok": true}
```

---

## 七、Web 前端文档

### 7.1 页面结构

**认证联动：** 各页面使用 `epdAuthHeaders()` / `epdFetchOpts()`（或等价逻辑）在 `fetch` 时附加 Basic 凭据；凭据来自配置页写入的 `localStorage`。

#### index.html - 主页

**功能模块:**
1. 设备信息卡片
2. 图片上传区域
3. 画廊网格
4. 网络状态
5. 快捷操作

**技术栈:**
- HTML5 + CSS3
- 原生 JavaScript (无框架)
- Fetch API
- localStorage

#### config.html - 配置页

**配置项:**
1. WiFi 配置
2. 屏幕选择（4.2" / 5.83"）
3. 轮播配置
4. 天气配置
5. 时钟配置
6. 留言板配置
7. 低功耗配置
8. HTTP 认证
9. 配置导出/导入
10. OTA 固件升级

#### timetable.html - 课程表页

**功能:**
1. 学期开始日期设置
2. 节次时间定义（自定义每节课时间）
3. 7天×N节课程网格编辑
4. 课程编辑（名称/教室/上课周次）
5. 导出/导入 JSON
6. 推送到屏幕

#### todo.html - 待办事项页

**功能:**
1. 待办列表管理（最多 16 条）
2. 优先级切换（普通/重要/紧急）
3. 完成状态勾选
4. 进度条显示
5. 推送到屏幕

#### countdown.html - 倒计时页

**功能:**
1. 倒计时条目管理（最多 3 条）
2. 目标日期设置
3. 推送到屏幕

### 7.2 主题系统

**三套主题:**
1. **Loopy** (粉色系)
2. **Kitty** (可爱系)
3. **Lotso** (草莓熊)

**实现方式:**
```css
[data-theme="loopy"] {
  --bg: #FFF0F5;
  --ac: #FF9BB3;
  --tx: #5A3050;
  /* ... */
}
```

**切换逻辑:**
```javascript
function setTheme(name) {
  document.documentElement.setAttribute('data-theme', name);
  localStorage.setItem('epd_theme', name);
}
```

### 7.3 响应式设计

**断点:**
```css
@media (max-width: 900px) { /* 平板 */ }
@media (max-width: 480px) { /* 手机 */ }
```

**适配策略:**
- 弹性布局 (Flexbox)
- 网格布局 (Grid)
- 相对单位 (rem, %)
- 媒体查询

---

## 八、构建与部署

### 8.1 开发环境

**系统要求:**
- Windows 10/11 (64-bit)
- 空闲磁盘：10GB
- 内存：≥8GB

**软件安装:**
1. ESP-IDF Tools v5.5.1
2. Python 3.8+
3. CMake 3.16+
4. Git

### 8.2 编译步骤

```powershell
# 1.下载源码解压
cd msp

# 2. 设置目标芯片
idf.py set-target esp32s3

# 3. 配置项目 (可选)
idf.py menuconfig

# 4. 编译
idf.py build

# 5. 烧录
idf.py -p COM3 flash

# 6. 监控
idf.py -p COM3 monitor
```

### 8.3 分区表

```csv
# Name,      Type, SubType,  Offset,    Size
nvs,         data, nvs,      0x9000,    0x6000      # 24KB
otadata,     data, ota,      0xF000,    0x2000      # 8KB
phy_init,    data, phy,      0x11000,   0x1000      # 4KB
factory,     app,  factory,  0x20000,   0x200000    # 2MB
ota_0,       app,  ota_0,    0x220000,  0x200000    # 2MB
ota_1,       app,  ota_1,    0x420000,  0x200000    # 2MB
spiffs,      data, spiffs,   0x620000,  0x9E0000    # ~10MB
```

### 8.4 OTA 升级

**Web 界面升级:**
1. 打开 `/config` 页面
2. 选择固件文件 (.bin)
3. 点击"上传并升级"
4. 等待进度条完成
5. 设备自动重启

**命令行升级:**
```powershell
idf.py -p COM3 ota_flash
```

---

## 九、故障排查

### 9.1 常见问题

#### 问题 1: 上电无反应

**排查:**
1. 检查 USB 线
2. 测 USB 5V 输入
3. 测 3.3V 输出
4. 检查 AMS1117

#### 问题 2: 无法识别串口

**排查:**
1. 安装 CH340 驱动
2. 更换 USB 线
3. 检查 CH340 焊接
4. 测 USB D+/D-

#### 问题 3: WiFi 连接失败

**排查:**
1. 检查天线
2. 测 3.3V 电源
3. 查看串口日志
4. 检查 WiFi 配置

#### 问题 4: 墨水屏不显示

**排查:**
1. 检查排线方向
2. 重新插拔排线
3. 检查 FPC 焊接
4. 查看屏幕配置

### 9.2 日志级别

**ESP-IDF 日志:**
```
E (错误) - 严重错误
W (警告) - 潜在问题
I (信息) - 正常运行
D (调试) - 调试信息
V (详细) - 详细信息
```

**查看日志:**
```powershell
idf.py -p COM3 monitor
```

---

## 十、附录

### A. 文件清单

**源代码:**
- `main/app_main.c` - 主入口（双启动路径）
- `main/epd_stub.c` - EPD 驱动（SSD1619 / UC8179）
- `main/http_app.c` - HTTP 服务器核心 + Basic Auth + OTA
- `main/http_gallery.c` - 画廊 HTTP 路由
- `main/http_features.c` - 功能模块 HTTP 路由
- `main/wifi_manager.c` - WiFi 管理
- `main/image_convert.c` - 图片转换
- `main/fb_render.c` - 帧缓冲渲染
- `main/weather.c` - 天气模块
- `main/clock_display.c` - 时钟显示
- `main/calendar_display.c` - 日历显示
- `main/lunar.c` - 农历算法
- `main/timetable.c` - 课程表（7×12 网格 + 学期周次）
- `main/todo.c` - 待办事项
- `main/countdown.c` - 倒计时
- `main/message_board.c` - 留言板
- `main/button.c` - 按键驱动
- `main/power_mgr.c` - 电源管理
- `main/scheduler.c` - 任务调度
- `main/display_mode.c` - 显示模式注册与切换
- `main/display_policy.c` - 显示策略协调
- `main/time_sync.c` - SNTP 时间同步
- `main/device_identity.c` - 设备标识
- `main/nvs_utils.c` - NVS 工具函数
- `main/spiffs_mount.c` - SPIFFS 挂载
- `main/lodepng.c` - LodePNG 第三方库

**Web 前端:**
- `web/index.html` - 主页
- `web/config.html` - 配置页
- `web/timetable.html` - 课程表
- `web/todo.html` - 待办事项
- `web/countdown.html` - 倒计时

**文档:**
- `README.md` - 项目介绍
- `文档索引.md` - 文档导航
- `ENGINEERING_DOCS.md` - 本文档
- `IMPLEMENTATION.md` - 实现说明
- `ROADMAP.md` - 开发规划
- `COURSE_TABLE_README.md` - 课程表专题
- `hardware/BOM_ACTUAL.md` / `hardware/BOM.md` - 硬件与固件对齐提要（采购级 BOM 不公开）

### B. 术语表

| 术语 | 英文 | 说明 |
|------|------|------|
| EPD | Electronic Paper Display | 电子纸显示器 |
| BWR | Black White Red | 黑白红三色 |
| PSRAM | Pseudo Static RAM | 伪静态 RAM |
| NVS | Non-Volatile Storage | 非易失性存储 |
| SPIFFS | SPI Flash File System | SPI 闪存文件系统 |
| OTA | Over-The-Air | 空中升级 |
| mDNS | Multicast DNS | 多播 DNS |
| STA | Station | 工作站模式 |
| AP | Access Point | 接入点模式 |
| DMA | Direct Memory Access | 直接内存访问 |

### C. 参考资料

1. [ESP32-S3 技术手册](https://www.espressif.com.cn/products/socs/esp32s3)
2. [ESP-IDF 编程指南](https://docs.espressif.com/projects/esp-idf/zh_CN/v5.5.1/)

---

**文档版本:** 2.1  
**最后更新:** 2026-04-10  
**维护者:** 项目维护者  

---

*本文档随项目迭代持续更新；仓库以 Gitee 为准时请同步拉取最新提交。*
