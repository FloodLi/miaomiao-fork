# epaper_uploader — ESP32-S3 智能墨水屏终端

> **硬件**：ESP32-S3 + 三色墨水屏（4.2" SSD1619 / 5.83" UC8179）  
> **框架**：ESP-IDF v5.5.1  
> **版本**：2.0.0

## 功能概览

| 模块 | 说明 |
|------|------|
| 图片展示 | JPEG / PNG / BMP 上传、Floyd-Steinberg 抖色、拉伸全屏显示 |
| 画廊轮播 | 自动顺序 / 随机轮播，间隔可配置 |
| 时钟表盘 | 自动刷新，数字时钟样式 |
| 万年历 | 月视图 + 农历 + 节气 + 节日 + 今日红圈 |
| 天气 | 和风天气 API，实时 + 3 天预报 |
| 课程表 | 周课程管理，按天显示 |
| 待办事项 | 优先级 + 完成状态 + 进度条 |
| 倒计时 | 目标日期倒数，支持 Web 配置 |
| 留言板 | 自定义文字 / 字号 / 对齐 / 颜色 |
| 物理按键 | 3 键切换模式（上一个 / 刷新 / 下一个） |
| 深度睡眠 | 闲置自动入睡 + 定时器/按键唤醒，电池续航 80+ 天 |

## 网络与配置

- **WiFi**：SoftAP + STA 共存（APSTA），NVS 持久化凭据，断线自动重连
- **mDNS**：`http://epd<mac>.local/` 局域网发现
- **SNTP**：自动时间同步（NTP 阿里云 + pool.ntp.org）
- **OTA**：双分区固件升级，Web 上传 + 进度条 + 回滚保护
- **HTTP Basic Auth**：敏感操作需认证，用户名/密码 NVS 持久化
- **Web UI**：三套主题（Loopy / Hello Kitty / 草莓熊），响应式设计

## 硬件连接

| 功能 | GPIO |
|------|------|
| EPD SPI | SCK=4, MOSI=5, DC=7, CS=15, RST=6, BUSY=16 |
| 按键 SW3 | GPIO 3（上一个模式） |
| 按键 SW4 | GPIO 46（刷新当前） |
| 按键 SW5 | GPIO 9（下一个模式） |

## 编译 & 烧录

```powershell
cd c:\Users\43060\Desktop\msp
idf.py set-target esp32s3
idf.py build
idf.py -p COMXX flash monitor
```

## 首次使用

1. 手机连接 WiFi 热点 `ESP32_EPD_xxxx`（密码 `12345678`）
2. 浏览器打开 `http://192.168.4.1/`
3. 进入「配置」页面连接家庭 WiFi
4. 连接后通过 `http://epd<mac>.local/` 或 STA IP 访问

## 项目结构

```
msp/
├── main/                        # 固件源码（25 个模块）
│   ├── app_main.c               # 启动流程编排（双启动路径）
│   ├── epd_stub.c / epd.h       # EPD 驱动（SSD1619 / UC8179，SPI DMA）
│   ├── image_convert.c/h        # 图片解码 + Floyd-Steinberg 抖色
│   ├── http_app.c/h             # HTTP 服务主框架 + Basic Auth + OTA
│   ├── http_gallery.c           # 画廊相关 HTTP 路由
│   ├── http_features.c          # 功能模块 HTTP 路由
│   ├── wifi_manager.c/h         # WiFi AP/STA + 配网 + 自动重连
│   ├── scheduler.c/h            # 画廊轮播调度
│   ├── weather.c/h              # 和风天气 HTTPS
│   ├── clock_display.c/h        # 时钟表盘
│   ├── calendar_display.c/h     # 万年历 + 农历
│   ├── lunar.c/h                # 农历算法（1900-2100）
│   ├── timetable.c/h            # 课程表（周期制 + 单日视图）
│   ├── todo.c/h                 # 待办事项（NVS + 进度条）
│   ├── countdown.c/h            # 倒计时
│   ├── message_board.c/h        # 留言板（自定义文字/对齐/字号）
│   ├── button.c/h               # 三键驱动（GPIO 轮询 + 去抖）
│   ├── fb_render.c/h            # 帧缓冲渲染引擎（像素/线/矩形/UTF-8）
│   ├── font_data.h              # 点阵字库（ASCII + 中文）
│   ├── display_policy.c/h       # 显示策略协调（互斥 + 手动屏标记）
│   ├── display_mode.c/h         # 显示模式注册与切换
│   ├── power_mgr.c/h            # 深度睡眠与电源管理
│   ├── time_sync.c/h            # SNTP 时间同步
│   ├── device_identity.c/h      # 设备标识（MAC → AP SSID / mDNS）
│   ├── nvs_utils.c/h            # NVS 工具函数
│   ├── spiffs_mount.c/h         # SPIFFS 挂载 + 自动修复
│   └── lodepng.c/h              # LodePNG 第三方 PNG 解码库
├── web/                         # Web UI（构建时嵌入固件）
│   ├── index.html               # 主页：上传 / 画廊 / 状态
│   ├── config.html              # 配置：WiFi / 轮播 / 天气 / OTA / 低功耗
│   ├── timetable.html           # 课程表编辑
│   ├── todo.html                # 待办事项管理
│   └── countdown.html           # 倒计时配置
├── tools/                       # 辅助脚本
│   ├── gen_font.py              # 点阵字库生成工具
│   └── gen_city_excel.py        # 和风天气城市码导出
├── docs/                        # 文档
├── hardware/                    # 硬件 BOM
├── partitions.csv               # 分区表（factory + 双 OTA + 10MB SPIFFS）
├── sdkconfig.defaults           # 项目默认配置
└── ROADMAP.md                   # 开发规划与进度
```

## 分区表

| 分区 | 类型 | 大小 |
|------|------|------|
| nvs | data | 24 KB |
| factory | app | 2 MB |
| ota_0 / ota_1 | app | 各 2 MB |
| spiffs | data | ~10 MB |

## 许可证

MIT
