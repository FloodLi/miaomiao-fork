# 水墨屏产品开发规划方案

> 项目代号：epaper_uploader  
> 硬件平台：ESP32-S3 + 三色墨水屏（4.2" 400×300 SSD1619 / 5.83" 648×480 UC8179）  
> 技术栈：ESP-IDF v5.5.1 / C / SPI / WiFi SoftAP / HTTP  
> 固件版本：v2.3.0（`CMakeLists.txt` / `GET /version`） | 文档修订：2026-04-15

---

## 更新记录（已完成）

> 说明：下面是从 v1.0 规划落地后的**实际完成项/关键修复**汇总（含多次迭代的经验性修复）。

### 2026-04-15（v2.3.0 — 全面代码审查与质量加固）

- **轮播 + 低功耗联动修复**：
  - 轮播索引 `s_current_index` 持久化到 NVS，顺序模式跨深睡唤醒正确换图。
  - 快速唤醒路径不再放行后台 `slideshow_task`，避免与唤醒刷屏冲突。
- **并发安全加固（6 处）**：
  - `todo.c` HTTP POST 加 `s_cfg_mutex` 保护，消除与渲染线程的数据竞争。
  - `message_board.c` 新增 `portMUX_TYPE s_cfg_mux` 自旋锁，`render_message` 快照配置。
  - `power_mgr.c` idle_monitor 配置读取纳入 `s_activity_mux`。
  - `scheduler.c` slideshow_task 循环中快照配置。
  - `button.c` `s_busy` 从 `volatile bool` 改为 C11 `atomic_bool`。
  - `wifi_manager.c` `wifi_manager_forget` 状态修改纳入 `s_sta_mux`。
- **显示问题修复（10 处）**：
  - 时钟 18 点后"下午"→"晚上"。
  - 天气：底部更新时间自适应屏宽、城市名/描述/预报文字加 `max_w` 防溢出、图标位置 clamp。
  - 日历：今日红圈半径 clamp 防侵入相邻格、竖线高度对齐横线。
  - 待办：删除线宽度改用实际文字绘制宽度。
  - 倒计时：未启用时返回 `ESP_ERR_INVALID_STATE`。
  - 留言板：超 32 行截断显示 "..." 提示。
  - 画布：所有元素坐标/尺寸 clamp 到 ±2000，防恶意 JSON 阻塞。
- **性能优化（3 处）**：
  - `fb_hline` / `fb_fill_rect` 改为字节级 memset 批量填充，大矩形渲染提速 5-10 倍。
  - 中文字库 `font_zh` 按 codepoint 排序 + 二分查找，查找从 O(6763) 降到 O(13)。
  - PNG 文件大小限制 200KB，防止无 PSRAM 设备 OOM。
- **安全加固**：
  - 新增 `json_escape()` 工具函数，轮播图片名/画廊文件名嵌入 JSON 时转义引号和反斜杠。
- **其他修复**：
  - 倒计时 `fb_export` 返回值检查，失败时不刷旧图。
  - BMP 解码 Floyd-Steinberg 路径：行数据越界时填白色，不再复用旧行像素。

### 2026-04（v2.2.0）

- **文档与工程对齐**：
  - `README.md` / `ENGINEERING_DOCS.md` / `文档索引.md` / `IMPLEMENTATION.md`：统一 **HTTP 路由约 57 条**、**双启动（完整 / 定时 quick）**、**无 PSRAM 内存策略**、Gitee 克隆地址；修正 API 示例（如 `POST /show?name=`、`POST /timetable_show` JSON、`countdown_config` 字段）。
  - 工程技术文档补充 **§4.4 低功耗与功能**、**§4.5 HTTP Basic Auth**（含前端 `localStorage` 凭据与天气 Key 脱敏说明）。
- **HTTP 安全加固（固件 + Web）**（与审计一致时）：
  - 敏感推屏/配置类 **POST** 走 `http_check_basic_auth`；`GET /weather_config` 不透传明文 API Key。
  - `web/config.html`、`index.html`、`timetable.html`、`todo.html`、`countdown.html`、`board.html`：`epdFetchOpts` + `epd_auth_*`；配置导出/导入携带鉴权头。
- **课程表功能重写**：
  - 7×12 网格课程模型（grid[day][period]），取代旧的 slots 模型。
  - 学期周次追踪（semester_start 时间戳 + 周次位掩码 weeks bitmask）。
  - 自定义节次时间定义（period_count + periods[]），内置标准 10 节模板。
  - 双屏自适应渲染：5.83" 全周网格视图，4.2" 单日议程视图，当前节次红色高亮。
  - Web UI 课程编辑页面（`/timetable`），支持导出/导入 JSON。
  - 支持按钮切换和 HTTP API 查看指定星期（`POST /timetable_show`，可选 JSON body `{"day":0..6}`）。
- **倒计时模块**：
  - NVS 持久化（最多 3 条，含标题/目标日期/启用开关）。
  - EPD 渲染：单项大字/多项目卡片，分页显示。
  - Web UI 管理页面（`/countdown`），操作后自动保存。
- **WYSIWYG 画布留言板**：
  - Web 端提供基于 HTML Canvas 的可视化编辑器（`/board`），支持拖拽排版并生成 JSON 布局。
  - 固件端解析 JSON 数组，利用 `fb_render` 引擎直接绘制文本、矩形、椭圆、直线以及 1-bit 图标（支持内置与用户上传图标）。
  - 解耦固定布局，使得设备具备完全的用户自定义画面与信息展示灵活性。
- **待办事项模块**：
  - NVS 持久化（最多 16 条，含优先级与完成状态）。
  - EPD 渲染：进度条 + 百分比 + 勾选样式 + 紧急/重要标签。
  - Web UI 管理页面（`/todo`），操作后自动保存。
- **物理按键驱动**：
  - 3 键（SW3/SW4/SW5 → GPIO 9/46/3）轮询 + 去抖。
  - 生产者-消费者架构（轮询 2.5KB + 显示工作者 10KB）。
  - 模式切换：时钟 → 日历 → 课程表 → 天气 → 轮播 → 待办 → 倒计时（`display_mode` 注册顺序；留言板以 Web `msg_show` 为主）。
  - 未配置功能自动显示友好提示屏。
- **图片显示优化**：
  - Floyd-Steinberg 误差扩散抖色（替代 Bayer，所有格式支持，内存不足自动降级）。
  - 拉伸全屏显示（独立 X/Y 缩放，消除黑边/白边）。
  - JPEG 解码缩放优化（偏好解码尺寸 ≥ 屏幕尺寸，减少上采样失真）。
- **并发安全与内存加固（v2.1.0 核心补丁）**：
  - 修复 `clock_display` 互斥锁泄漏问题，消除长期运行后的 LoadProhibited 崩溃。
  - 对 `scheduler`、`weather`、`clock`、`power_mgr` 等模块的全局共享配置加入 `portMUX_TYPE` 自旋锁保护。
  - `power_mgr` 的 64 位时间戳 `s_last_activity_us` 实现自旋锁原子读写，防止双核架构下撕裂。
  - `weather` 模块引入 `weather_get_data_copy()` 提供数据副本，替代原有的裸指针访问，消除数据竞争。
  - `epd_set_panel` 增加 mutex 保护，防止 HTTP 请求与按键切换并发导致面板重新初始化冲突。
  - `epd_display_from_file` 在 SPI DMA 传输后立即释放帧缓冲与文件句柄，不等 EPD 18.8s 刷新结束，将峰值内存占用时间压缩到最低。
  - 图片转换 `fread` 增加返回值检查，防止不完整解码导致的越界。
  - Deep Sleep 前执行优雅关闭：等待当前 EPD 刷新完成并调用 `nvs_flush_all()`，防止配置丢失。
- **低功耗唤醒模式一致性修复**：
  - 问题：原先 Web 端推屏不记录模式，导致深睡定时唤醒后回退到"上次按键模式"发生显示错位。
  - 修复：在所有 Web 手动推屏路由（如 `/timetable_show`, `/weather_show` 等）及快速唤醒恢复路径上，强制补齐 `button_set_current_mode()` 与 `power_mgr_save_mode()`。
  - 新增 `display_mode_index_t` 枚举，解耦硬编码的模式 ID。
- **协议与开源规范对齐**：
  - 全局更换为 PolyForm Noncommercial License 1.0.0。
  - Web UI（`/index`, `/timetable` 等）底部加入统一页脚版权声明（`<div class="epd-credit">Made with ❤️ by 喵喵</div>`）。
  - HTTP 路由注册：增加返回值检查 + 错误日志，`max_uri_handlers` 扩大至 60。
  - `timetable` 与 `todo` 模块新增 `SemaphoreHandle_t s_cfg_mutex` 保护配置读写。
  - `time_sync` / `message_board` / `display_policy` 使用 C11 `atomic_bool` 替代 `volatile bool`。
  - WiFi STA 认证支持 WPA3 SAE（`sae_pwe_h2e = WPA3_SAE_PWE_BOTH`）。
  - OTA 端点强制要求先设置 Basic Auth 凭据，未配置时返回 `403 Forbidden`。
  - 深睡前调用 `usb_serial_jtag_ll_phy_enable_pad(false)` 关闭 USB PHY 降低漏电。
  - `weather` 渲染增加 `time_sync_get_local` 返回值检查，时间未同步时显示占位符。
  - 课程表周表头颜色修复（非当日从误写的 `COLOR_WHITE` 改为 `COLOR_BLACK`）。
  - 画廊 JSON 列表：改为动态分配，消除固定 1KB 缓冲截断。
  - WiFi 断线自动重连：指数退避定时器（10s→300s），长期无人值守不丢连。
  - favicon.ico 处理：204 空响应，消除浏览器 404 日志垃圾。
  - 轮播调度器：`show_next_image()` 大数组改为堆分配，栈增至 10KB，修复栈溢出崩溃。
- **深度睡眠与低功耗**：
  - `power_mgr.c/h`：NVS 持久化配置（启用开关、唤醒间隔 1-1440 分钟、闲置超时 30-600 秒）。
  - 双启动路径：按键/上电 → 完整启动（WiFi+HTTP+全部功能）；定时器唤醒 → 快速刷新（刷新内容后立即回睡）。
  - EXT1 GPIO 唤醒（仅 SW3/GPIO9 + SW5/GPIO3，active-HIGH；GPIO46 非 RTC 不可 EXT1），RTC 定时器唤醒。
  - 独立闲置监控任务（4KB 栈），避免 FreeRTOS Tmr Svc 栈溢出。
  - 跨深睡显示模式记忆（NVS 保存 last_mode，唤醒后恢复对应内容）。
  - HTTP 会话 open_fn hook 自动重置闲置计时器；按键按下重置闲置。
  - Web UI「低功耗/深度睡眠」配置卡片（启用开关、唤醒间隔、闲置超时）。
  - 预估功耗：活跃 ~150mA/25s，深睡 ~7μA，60 分钟间隔 + 2000mAh ≈ 80+ 天续航。
- **HTTP Basic Auth 认证**：
  - 上传/OTA/删除/配网等敏感操作需认证。
  - Web UI 配置卡片（用户名/密码），NVS 持久化。

### 2026-03

- **双屏支持（4.2" / 5.83"）**：运行时可切换面板类型，尺寸/红色能力动态适配，配置持久化到 NVS。
- **UC8179 5.83" 三色（BWR）适配完成**：
  - 修复 BUSY 极性差异、初始化序列（Panel Setting BWR 模式、VCOM/Data Interval 2 字节等）。
  - 修复红色极性与平面组合逻辑（红色像素要求 black plane=0, red plane=1）。
  - 修复边框红色异常（VCOM/Data Interval 参数）。
- **开机欢迎/引导屏**：替代测试图案，显示 WiFi AP、密码、访问地址等；并修复内容被底部黑边遮挡问题。
- **P4-2 留言板（自定义文字）**：Web 输入→EPD 渲染（换行/对齐/字号 1~5/黑红/偏移/持久化 NVS）；并发渲染用 pending 机制避免报错。
- **中文字库大幅扩充**：改用 Unifont 点阵生成，覆盖 GB2312 1/2 级及常用扩展字符（修复欢迎页缺字如“浏”）。
- **万年历（日历 + 农历 + 节气 + 节日）**：
  - 新增 `lunar.c/h`（1900-2100 查表 + 天干地支/生肖/节气/节日识别）。
  - 新增 `calendar_display.c/h` 月视图渲染：周末红字、今日红圈、节气/节日小字标注。
  - 新增 HTTP API + Web UI 入口（选择年月、上月/下月）。
- **Web UI 主题重做**：Loopy / Hello Kitty / 草莓熊三主题一键切换（CSS 变量 + localStorage 持久化），并加入左右装饰与角色 SVG。
- **图片上传/显示三色修复与优化**：
  - 修复图片转换输出**未写入红色平面**的问题（JPEG/PNG/BMP 统一写 black+red 两平面）。
  - 修复红色像素未清 black 位导致红色显示不稳定的问题。
  - 5.83" 下修复“红色不显示”的 DMA 内存问题：渲染模块先 `fb_destroy()` 再显示；`epd_display_from_file()` 改为单缓冲复用读写，显著降低峰值内存。
  - 抖动与色彩优化：8×8 Bayer 抖动、HSV 红色识别 + 红色抖动，提升灰度层次与红色识别范围。
  - 图片缩放/版式优化：默认“铺满屏幕（等比居中裁剪）”，避免比例差导致图片显示太小。
- **和风天气城市码 Excel 导出工具**：新增脚本可一键生成桌面 xlsx（含表头/样式/筛选）。

---

## 一、现状评估

### 已完成能力

| 模块 | 状态 | 说明 |
|------|------|------|
| EPD 驱动 | 已完成 | SSD1619(4.2") + UC8179(5.83")，三色(BWR)支持，BUSY 极性自适配，DMA 分块发送 |
| 双屏切换 | 已完成 | Web UI 选择面板，NVS 持久化，尺寸/能力动态适配 |
| WiFi 热点/配网 | 已完成 | SoftAP + STA 配网（APSTA），失败回退 AP，NVS 持久化 |
| HTTP 服务器 | 已完成 | Web UI + API：上传/画廊/显示/删除/状态/配网/OTA/日历等 |
| 图片解码 | 已完成 | JPEG(TJpgDec) + PNG(LodePNG) + BMP，自动缩放与“铺满屏幕”居中裁剪 |
| 三色量化与抖动 | 已完成 | 黑白 8×8 Bayer 抖动 + HSV 红色识别 + 红色抖动（black+red 双平面输出） |
| 文件存储 | 已完成 | SPIFFS 画廊持久化，健康检测与自动修复 |
| 字体渲染 | 已完成 | UTF-8 字符串渲染（ASCII + 16×16 中文点阵），支持缩放与加粗效果 |
| 内容模块 | 已完成 | 天气、时钟表盘、留言板、万年历（月视图+农历/节气/节日） |
| Web 前端 | 已完成 | 主题切换（Loopy/Kitty/草莓熊）+ 屏幕选择/配网/OTA/日历等页面 |

### 当前短板与改进空间

| 维度 | 现状 | 问题 |
|------|------|------|
| 刷新速度 | 三色全刷较慢 | 三色屏物理全刷较慢（尤其红色），可进一步优化“刷新中”提示与调度 |
| PNG 内存占用 | PNG 需 RGBA 解码 | 无 PSRAM 时大 PNG 仍可能内存吃紧（可后续做更轻量的解码/限制策略） |
| 电源管理 | ✅ 已实现 | 闲置自动深睡 + 定时器/按键唤醒，预估 80+ 天续航 |

---

## 二、产品定位与目标用户

### 产品定位

**个人桌面智能墨水屏终端** — 一块放在桌面/床头/玄关的低功耗显示设备，手机随时传图、定时刷新内容，断电保持画面。

### 目标用户画像

| 用户类型 | 使用场景 | 核心需求 |
|----------|----------|----------|
| 极客/开发者 | 桌面摆件、电子相册 | 可编程、接口开放、自定义内容 |
| 家庭用户 | 床头日历/天气、照片展示 | 手机操控简单、定时轮播 |
| 办公用户 | 会议室门牌、工位标识 | 远程更新、简洁显示 |

### 产品目标（6 周周期）

1. **体验可用** — 手机上传照片 → 5 秒内完成转换和刷屏
2. **功能丰富** — 画廊轮播、定时刷新、STA 联网、天气/日历等内容源
3. **稳定可靠** — OTA 升级、看门狗、异常恢复、7×24 无人值守运行

---

## 三、功能规划（分阶段）

### Phase 1：体验优化（第 1~2 周） ✅ 已完成

> 目标：把"能用"提升为"好用"

#### ✅ P1-1 SPI DMA 批量传输

- **现状**：`epd_send_data()` 逐字节发送，30000 字节需 30000 次 SPI 事务
- **方案**：使用 `spi_device_transmit()` 的批量模式，每次发送 4096 字节
- **预期收益**：数据传输时间从 ~3s 降至 <0.1s
- **涉及文件**：`epd_stub.c`
- **实现**：`epd_send_data_bulk()` 和 `epd_fill_data()` 函数，DMA 4KB 分块传输

#### ✅ P1-2 抖动算法（已落地并迭代）

- **现状**：简单阈值二值化，照片灰度层次丢失严重
- **方案**：在 `image_convert.c` 中实现抖动与三色量化；最终采用更省内存的有序抖动（Bayer）并增强红色识别
- **注意**：三色屏需要先判红，再对非红像素做黑白抖动
- **预期收益**：照片显示效果显著提升，灰度过渡自然
- **涉及文件**：`image_convert.c`
- **实现**：8×8 Bayer 抖动 + HSV 红色识别 + 红色抖动；输出 black+red 两平面

#### ✅ P1-3 图片预览与画廊 UI

- **现状**：前端仅有上传/删除，无法预览已存储图片
- **方案**：
  - 后端新增 `GET /images/<name>` 端点，返回原始 JPEG 缩略图
  - 前端增加画廊网格视图，支持点击预览、选择显示、单张删除
  - 显示 SPIFFS 剩余空间
- **涉及文件**：`http_app.c`（前端 HTML + 后端路由）
- **实现**：8 个 HTTP 端点（`/`, `/upload`, `/status`, `/images`, `/image`, `/show`, `/delete_image`, `/delete`）；画廊网格 + SPIFFS 用量条

#### ✅ P1-4 前端重构：外部化 HTML

- **现状**：HTML 以 C 字符串内嵌在 `http_app.c`，约 120 行，修改需重新编译
- **方案**：
  - 将 HTML/CSS/JS 拆为独立文件，存放在 `web/` 目录
  - 构建时通过 `CMakeLists.txt` 的 `EMBED_FILES` 嵌入二进制
  - 或存放于 SPIFFS 分区从文件系统读取
- **涉及文件**：`main/CMakeLists.txt`、新建 `web/index.html`、`http_app.c`
- **实现**：`web/index.html` 通过 `EMBED_FILES` 嵌入，`Cache-Control: no-cache` 防缓存

#### ✅ P1-额外 浏览器端图片压缩（超出原规划）

- **方案**：浏览器 Canvas 将任意图片缩放到 800×600 以内，转为 JPEG 后上传
- **收益**：大幅减小上传体积（数 MB → ~50KB），适配无 PSRAM 的 ESP32-S3

#### ✅ P1-额外 SPIFFS 自动损坏检测与修复（超出原规划）

- **方案**：启动时快速写读测试，失败则自动格式化重新挂载
- **收益**：避免 SPIFFS 损坏导致设备不可用

#### ✅ P1-额外 JPEG 文件流式解码（超出原规划）

- **方案**：JPEG 直接从 SPIFFS 文件流式读取解码（`jpeg_infunc_file`），不再全量载入内存
- **收益**：峰值内存从 ~355KB 降至 ~155KB，在 240KB 可用内存下稳定运行

---

### Phase 2：连接能力（第 3~4 周） ✅ 已完成

> 目标：从"局域孤岛"升级为"联网终端"

#### ✅ P2-1 WiFi STA 模式 + 配网

- **现状**：仅 SoftAP，设备无法联网
- **方案**：
  - 支持 AP+STA 共存模式（APSTA）
  - 首次启动进入 AP 模式，通过 Web 页面配置家庭 WiFi SSID/密码
  - WiFi 凭据存入 NVS，下次开机自动连接
  - 连接失败回退 AP 模式（3 次重试）
- **涉及文件**：`wifi_ap.c` → 重构为 `wifi_manager.c`、`wifi_manager.h`、`app_main.c`、`http_app.c`、新建 `web/config.html`
- **实现**：
  - `wifi_manager.c`：NVS 凭据存取、事件驱动连接管理、PHY 就绪等待（`BIT_STA_STARTED`）、WiFi 扫描（不断开 AP）
  - HTTP 路由：`/config`、`/wifi_status`、`/scan`、`/wifi_connect`、`/wifi_forget`
  - 配网 Web 页面：WiFi 扫描列表 + 信号强度 + 密码输入 + 连接/忘记

#### ✅ P2-2 mDNS 局域网发现

- **现状**：STA 模式下 IP 地址不可预知
- **方案**：启用 ESP-IDF 的 mDNS 组件（managed component `espressif/mdns`），注册 `epd.local`
- **预期收益**：用户浏览器输入 `http://epd.local/` 即可访问
- **涉及文件**：`app_main.c`、`main/idf_component.yml`
- **实现**：`start_mdns()` 注册 hostname `epd`、service `_http._tcp` port 80

#### ✅ P2-3 SNTP 时间同步

- **现状**：设备无时间概念
- **方案**：STA 连接后通过 SNTP 同步时间，支持时区配置（CST-8）
- **用途**：为定时轮播、日历显示、日志提供时间基础
- **涉及文件**：新建 `time_sync.c`、`time_sync.h`、`http_app.c`（`/status` 返回时间）、`web/index.html`（显示设备时间）
- **实现**：NTP 服务器 aliyun + pool.ntp.org + time.windows.com，1 小时同步间隔，Web 端实时显示

#### ✅ P2-4 OTA 固件升级

- **现状**：固件更新需 USB 物理连接
- **方案**：
  - 分区表增加 `otadata` + `ota_0` / `ota_1` 双分区（各 2MB）
  - Web 配置页面新增"固件升级"卡片，上传 `.bin` 文件，带进度条
  - 使用 ESP-IDF `esp_ota_ops` 组件，4KB 流式写入
  - 启动时 `esp_ota_mark_app_valid_cancel_rollback()` 防止回滚（仅 OTA 分区）
  - 保留 factory 分区作为兜底
- **涉及文件**：`partitions.csv`、`http_app.c`（`/ota` + `/version`）、`web/config.html`、`app_main.c`、`main/CMakeLists.txt`
- **实现**：版本信息展示 + 文件上传进度条 + 自动重启 + 回滚保护

---

### Phase 3：智能内容（第 5~6 周） ✅ 已完成

> 目标：从"手动传图"进化为"智能内容终端"

#### ✅ P3-1 定时轮播

- **现状**：手动上传一张显示一张
- **方案**：
  - 画廊中的图片按顺序/随机自动轮播
  - 支持设置轮播间隔（5 分钟 ~ 24 小时）
  - 配置存入 NVS，断电恢复
- **涉及文件**：新建 `scheduler.c`、`scheduler.h`、`http_app.c`、`web/config.html`
- **实现**：FreeRTOS 任务 + EventGroup 事件驱动，顺序/随机模式，Web UI 配置卡片，启用后立即显示首张图片

#### ✅ P3-2 天气 + 日历信息源

- **现状**：仅支持图片展示
- **方案**：
  - STA 联网后，定时拉取和风天气 QWeather API（HTTPS）
  - 设备端渲染：内置 8×16 ASCII + 16×16 中文点阵字库（Unifont 生成并扩充字库）
  - 帧缓冲区渲染器：像素/线条/矩形/位图/UTF-8 混排文字绘制
  - 天气画面：日期+星期+城市、实时温度+天气图标+体感+湿度+风力+气压、3 天预报
  - gzip 自动解压（ROM miniz）、EPD 水平镜像修正
- **涉及文件**：新建 `weather.c`、`weather.h`、`fb_render.c`、`fb_render.h`、`font_data.h`、`tools/gen_font.py`；修改 `http_app.c`、`web/config.html`、`epd_stub.c`、`app_main.c`、`CMakeLists.txt`、`sdkconfig.defaults`
- **实现**：
  - 和风天气 QWeather API（独立 API Host + HTTPS + 证书包）
  - 95 个 ASCII + 81 个中文字符点阵字体（SimHei 渲染）
  - 24×24 天气图标（晴/多云/雨/雪）
  - Web 配置卡片（API Key、API Host、城市 ID、刷新间隔）
  - NVS 持久化配置，自动定时刷新

#### ✅ P3-3 深度睡眠与低功耗

- **现状**：已实现，设备闲置后自动深睡，定时器或按键唤醒
- **方案**：
  - 闲置超时后自动进入 deep sleep（可配置 30-600 秒）
  - 唤醒方式：RTC 定时器（可配置 1-1440 分钟）+ EXT1 GPIO（SW3/SW5；SW4 不支持深睡唤醒）
  - 双启动路径：按键唤醒 → 完整启动；定时器唤醒 → 快速刷新后立即回睡
  - NVS 跨深睡保存当前显示模式，唤醒后恢复对应内容
  - 预期电池续航：2000mAh 锂电池，60 分钟刷新间隔，约 80+ 天
- **涉及文件**：新建 `power_mgr.c`、`power_mgr.h`；修改 `app_main.c`、`http_app.c`、`button.c`、`web/config.html`、`CMakeLists.txt`

#### ✅ P3-4 PNG + BMP 支持

- **现状**：仅支持 JPEG
- **方案**：
  - PNG：集成 `lodepng`（纯 C，无依赖，~60KB 代码）
  - BMP：手动解析 BMP 头 + 逐行读取像素，支持 24-bit / 32-bit 无压缩 BMP
  - 前端 `accept` 属性扩展
- **涉及文件**：`image_convert.c`、`image_convert.h`、`http_app.c`、`web/index.html`
- **实现**：
  - PNG：lodepng 解码 + Bayer 抖动 + 自动缩放/铺满裁剪 + 红色抖动
  - BMP：`bmp_decode_core()` 支持文件流和内存两种输入，逐行读取（内存高效），自动缩放/铺满裁剪 + Bayer 抖动 + HSV 红色检测
  - 自动格式检测：magic bytes `0x42 0x4D` (BMP)、`0xFF 0xD8` (JPEG)、`0x89 0x50` (PNG)
  - 画廊文件扩展名和 MIME type 支持 `.bmp` / `image/bmp`
- **注意**：由于无 PSRAM，大图片文件由浏览器端自动压缩为 JPEG 后上传

---

## 四、技术架构（目标状态）

```
┌─────────────────────────────────────────────────────┐
│                    Web 前端 (HTML/JS)                │
│  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌───────────┐ │
│  │ 上传 │ │ 画廊 │ │ 配网 │ │ 设置 │ │ OTA 升级  │ │
│  └──┬───┘ └──┬───┘ └──┬───┘ └──┬───┘ └─────┬─────┘ │
└─────┼────────┼────────┼────────┼───────────┼───────┘
      │ HTTP   │        │        │           │
┌─────▼────────▼────────▼────────▼───────────▼───────┐
│               HTTP Server (http_app.c)              │
│  /upload  /images  /config  /settings  /ota         │
└────┬──────────┬──────────┬──────────┬──────────┬───┘
     │          │          │          │          │
┌────▼───┐ ┌───▼────┐ ┌───▼─────┐ ┌─▼──────┐ ┌▼────┐
│ Image  │ │ SPIFFS │ │  WiFi   │ │Schedu- │ │ OTA │
│Convert │ │ Store  │ │ Manager │ │  ler   │ │     │
└────┬───┘ └────────┘ │ AP+STA  │ └───┬────┘ └─────┘
     │                │  mDNS   │     │
┌────▼───┐            │  SNTP   │ ┌───▼─────────────┐
│  EPD   │            └─────────┘ │ Content Sources  │
│ Driver │                        │ Weather/Calendar │
│SSD1619 │                        │ Font Render      │
└────┬───┘                        └─────────────────┘
     │ SPI
┌────▼───────────────────┐
│ GDEH042Z96 (400×300)   │
│ 三色墨水屏面板          │
└────────────────────────┘
```

---

## 五、文件结构规划（目标状态）

```
msp/
├── main/                          # 固件源码（21 个 .c 模块）
│   ├── app_main.c                 # 入口：双启动路径（完整启动 / 深睡快速刷新）
│   ├── epd_stub.c / epd.h        # EPD 驱动（SSD1619 4.2" + UC8179 5.83"，SPI DMA）
│   ├── image_convert.c/h         # JPEG/PNG/BMP → EPD 帧缓冲（Floyd-Steinberg 抖色）
│   ├── http_app.c/h              # HTTP 服务 + 约 57 条 API 路由 + Basic Auth + OTA
│   ├── wifi_manager.c/h          # WiFi AP/STA + 配网 + 自动重连
│   ├── scheduler.c/h             # 画廊轮播调度器
│   ├── weather.c/h               # 和风天气 HTTPS 拉取与渲染
│   ├── clock_display.c/h         # 时钟表盘
│   ├── calendar_display.c/h      # 万年历（月视图 + 农历/节气/节日）
│   ├── lunar.c/h                 # 农历算法（1900-2100）
│   ├── timetable.c/h             # 课程表（周期制 + 单日视图）
│   ├── todo.c/h                  # 待办事项（NVS 持久化 + 进度条）
│   ├── countdown.c/h             # 倒计时
│   ├── message_board.c/h         # 留言板（自定义文字/对齐/字号）
│   ├── canvas_board.c/h          # WYSIWYG 画布留言板解析与渲染
│   ├── http_canvas.c             # 画布 Web 编辑器路由与素材管理
│   ├── canvas_icons.h            # 内置 1-bit 画布图标字库
│   ├── button.c/h                # 三键驱动（GPIO 轮询 + 去抖 + 模式切换）
│   ├── power_mgr.c/h             # 深度睡眠与电源管理
│   ├── fb_render.c/h             # 帧缓冲渲染引擎（像素/线/矩形/UTF-8）
│   ├── font_data.h               # 点阵字库数据（ASCII + 中文）
│   ├── display_policy.c/h        # 显示策略协调（互斥 + 手动屏标记）
│   ├── device_identity.c/h       # 设备标识（MAC → AP SSID / mDNS）
│   ├── time_sync.c/h             # SNTP 时间同步
│   ├── spiffs_mount.c/h          # SPIFFS 挂载 + 自动修复
│   ├── lodepng.c/h               # LodePNG 第三方 PNG 解码库
│   └── CMakeLists.txt
├── web/                           # Web UI（构建时嵌入固件）
│   ├── index.html                 # 主页：上传 / 画廊 / 状态
│   ├── config.html                # 配置：WiFi / 轮播 / 天气 / OTA / 低功耗
│   ├── timetable.html             # 课程表编辑
│   ├── todo.html                  # 待办事项管理
│   ├── countdown.html             # 倒计时配置
│   └── board.html                 # 画布留言板可视化编辑器
├── .gitignore
├── partitions.csv                 # 分区表（factory + 双 OTA + 10MB SPIFFS）
├── sdkconfig.defaults
├── CMakeLists.txt
├── README.md
├── IMPLEMENTATION.md              # 技术实现文档
└── ROADMAP.md                     # 本文档
```

---

## 六、里程碑与时间线

```
Week 1          Week 2          Week 3          Week 4          Week 5          Week 6
──┬───────────────┬───────────────┬───────────────┬───────────────┬───────────────┬──
  │  Phase 1      │               │  Phase 2      │               │  Phase 3      │
  │               │               │               │               │               │
  ├─ P1-1 DMA     │               ├─ P2-1 STA配网  │               ├─ P3-1 定时轮播│
  ├─ P1-2 抖动    │               ├─ P2-2 mDNS    │               ├─ P3-2 天气日历│
  │               ├─ P1-3 画廊UI  │               ├─ P2-3 SNTP    │               │
  │               ├─ P1-4 HTML外部│               ├─ P2-4 OTA     ├─ P3-3 深度睡眠│
  │               │               │               │               ├─ P3-4 PNG支持 │
  │               │               │               │               │               │
  ▼ M1:体验优化   ▼               ▼ M2:联网能力    ▼               ▼ M3:智能终端   ▼
```

| 里程碑 | 交付物 | 验收标准 |
|--------|--------|----------|
| ✅ **M1** 体验优化 | DMA 驱动 + 抖动算法 + 画廊 UI + HTML 外部化 | 上传照片 → 刷屏 <5s；灰度照片有明显层次感；画廊可浏览/选择/删除 |
| ✅ **M2** 联网能力 | STA 配网 + mDNS + SNTP + OTA | 首次 AP 配网 → 自动连 WiFi → `epd.local` 可访问 → OTA 升级成功 |
| ✅ **M3** 智能终端 | 轮播 + 天气日历 + 深度睡眠 + PNG | ✅ 自动轮播画廊图片 ✅ 天气信息正确显示 ✅ deep sleep |

---

## 七、优先级矩阵

按 **用户价值** × **实现难度** 排列：

| 优先级 | 功能 | 用户价值 | 实现难度 | 建议 |
|--------|------|----------|----------|------|
| P0 | SPI DMA 批量传输 | 高 | 低 | 立即做，性能基础 |
| P0 | 抖动算法（Bayer/误差扩散择优） | 高 | 中 | 已完成（当前为 8×8 Bayer + 红色抖动） |
| P1 | 画廊 UI + 图片预览 | 高 | 中 | 第一周完成 |
| P1 | HTML 外部化 | 中 | 低 | 为后续迭代打基础 |
| P1 | WiFi STA + 配网 | 高 | 中 | 联网是后续功能的前置条件 |
| P2 | mDNS | 中 | 低 | 体验优化，实现简单 |
| P2 | OTA 升级 | 高 | 中 | 远程维护必备 |
| P2 | SNTP 时间同步 | 中 | 低 | 定时功能前置条件 |
| P2 | 定时轮播 | 高 | 中 | 核心卖点功能 |
| P3 | 天气/日历 | 中 | 高 | 需要字库和渲染引擎 |
| P3 | 深度睡眠 | 中 | 中 | 电池供电场景需要 |
| P3 | PNG/BMP 支持 | 低 | 中 | 锦上添花 |

---

## 八、风险评估与应对

| 风险 | 影响 | 概率 | 应对策略 |
|------|------|------|----------|
| SPIFFS 写入寿命 | Flash 磨损导致文件系统损坏 | 中 | 限制写入频率；考虑迁移到 LittleFS（磨损均衡更好） |
| 中文字库体积 | GB2312 全字库 ~256KB 占用 Flash | 低 | 仅嵌入常用 3000 字；或使用外部 SPI Flash |
| STA 断线重连 | 设备离线无法远程控制 | 中 | 实现自动重连机制（指数退避）；断线回退 AP 模式 |
| JPEG 解码内存峰值 | 大尺寸 JPEG 解码时 OOM | 中 | 限制上传文件大小 2MB；缩放选择优先小比例 |
| 三色屏刷新慢 | 全刷新需 15~20 秒，用户体验差 | 高 | UI 明确提示刷新中状态；后期考虑局部刷新（限黑白） |
| OTA 升级失败变砖 | 固件损坏无法启动 | 低 | 使用 ESP-IDF rollback 机制；保留 factory 分区 |

---

## 九、分区表（已部署）

```csv
# Name,      Type, SubType,  Offset,    Size
nvs,         data, nvs,      0x9000,    0x6000      # 24KB
otadata,     data, ota,      0xF000,    0x2000      # 8KB — OTA 启动选择
phy_init,    data, phy,      0x11000,   0x1000      # 4KB
factory,     app,  factory,  0x20000,   0x200000    # 2MB — 出厂固件 (回滚保护)
ota_0,       app,  ota_0,    0x220000,  0x200000    # 2MB — OTA 分区 A
ota_1,       app,  ota_1,    0x420000,  0x200000    # 2MB — OTA 分区 B
spiffs,      data, spiffs,   0x620000,  0x9E0000    # ~10MB — 图片存储
```

---

## 十、质量保障

### 测试策略

| 层级 | 方法 | 覆盖内容 |
|------|------|----------|
| 单元 | 主机端 C 测试 | 图片转换、抖动算法、颜色判定 |
| 集成 | 真机自动化 | WiFi 连接、HTTP 上传、EPD 刷新完整流程 |
| 压力 | 循环上传脚本 | 连续 100 次上传/刷新，监控内存泄漏和 SPIFFS 稳定性 |
| 兼容 | 多浏览器 | iOS Safari、Android Chrome、Windows Edge |

### 关键指标监控

| 指标 | 目标值 | 监控方式 |
|------|--------|----------|
| 堆内存水位 | 空闲 >80KB | `esp_get_free_heap_size()` 定期打印 |
| 上传→刷屏延迟 | <5 秒（含解码+传输+刷新） | HTTP 响应时间 |
| WiFi 重连成功率 | >99% | 日志统计 |
| SPIFFS 可用空间 | >20% 时告警 | `/status` API 返回 |
| Deep sleep 电流 | <10μA | 万用表实测 |

---

## 十一、下一步行动

### Phase 3 全部完成

1. ~~**P3-1 定时轮播** — 已完成~~
2. ~~**P3-2 天气 + 日历** — 已完成~~
3. ~~**P3-3 深度睡眠** — 已完成（闲置自动入睡 + 定时器/按键唤醒 + 双启动路径 + 模式记忆）~~

### 完成检查清单

- [x] 确认当前固件可正常编译烧录（`idf.py build && idf.py flash`）
- [x] 确认墨水屏可正常显示上传的 JPEG/PNG/BMP 图片（黑白红三色）
- [x] 确认图片“铺满屏幕（等比居中裁剪）”显示效果正常
- [x] 确认 SPIFFS 读写正常（上传、删除、列表）
- [x] 备份当前可用版本（`git init && git add . && git commit`）
- [x] Phase 1 已全部完成（DMA + 抖动 + 画廊 + HTML 外部化 + 浏览器压缩 + SPIFFS 自修复 + 流式解码）
- [x] Phase 2 已全部完成（WiFi STA 配网 + mDNS + SNTP + OTA）
- [x] P3-1 定时轮播已完成
- [x] P3-2 天气 + 日历信息源已完成
- [x] 万年历（月视图 + 农历/节气/节日 + 今日红圈）已完成
- [x] 留言板（自定义文字/字号/对齐/黑红/偏移/NVS）已完成
- [x] WYSIWYG 画布留言板（JSON 解析/文字/线条/图形/自定义图标编辑器）已完成
- [x] 双屏支持（4.2"/5.83" 可切换，BWR）已完成
- [x] Web UI 三主题（Loopy/Kitty/草莓熊）已完成
- [x] 课程表功能（周期制 + 单日视图 + Web 编辑）已完成
- [x] 待办事项功能（NVS + EPD 渲染 + Web UI）已完成
- [x] 物理按键驱动（3 键模式切换 + 去抖 + 未配置提示）已完成
- [x] Floyd-Steinberg 抖色 + 拉伸全屏显示已完成
- [x] WiFi 断线自动重连（指数退避）已完成
- [x] P3-3 深度睡眠与低功耗（独立闲置监控任务 + EXT1 唤醒 + 双启动路径 + 模式记忆）已完成
- [x] HTTP Basic Auth 认证已完成

---

*本文档将随开发进展持续更新。每个 Phase 完成后进行回顾，调整后续计划。*
