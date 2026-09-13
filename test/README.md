# 单元测试（host 端）

本目录存放**主机端单元测试**，用于在不烧录真机的前提下，对固件中**纯逻辑模块**做回归验证。
测试工程不会修改运行时数据；每个子工程只编译指定生产模块和测试替身。

## 可靠性回归入口

Windows 已安装 Node.js、Ubuntu WSL、GCC、CMake、Ninja 后，从项目根目录执行：

```powershell
./tools/run_host_tests.ps1
```

Linux/WSL 可运行 `sh test/run_host_tests.sh`。设置 `IDF_PATH` 为 Linux 可读的
ESP-IDF 目录，或分别设置 `UNITY_ROOT`、`CJSON_ROOT` 指向源目录。无需完整配置
ESP-IDF Linux 目标；直接编译生产源码。二进制在临时目录生成，结束后自动清理。

- `lunar/host`：运行现有农历 Unity 用例。
- `startup/host`：生产显示事务、模式、留言及画板任务；测试抢占、回滚、后台竞争、任务与内存失败。
- `epd`：直接包含生产驱动，注入 GPIO/BUSY、SPI、DMA、文件及锁等待故障；启用 ASan/UBSan。
- `fb_render`：链接真实绘图源码，比较各色单像素、横线和矩形，在缺色层、无红色指针、跨字节及裁剪场景下的结果；启用ASan/UBSan。
- `image_quant`：直接包含生产图片转换并链接真实 PNG 编解码器，检查 ID34/37 的灰度层次、中性色保护、边框、墨色互斥、预量化直通及其它面板回归；启用 ASan/UBSan。
- `http_receive`：真实接收器与自动提取的 OTA/上传处理函数，测试截止时间、连接关闭、OTA abort、文件保留和资源释放。

这些测试使用硬件和系统 API 替身，不证明真实波形、BUSY 电气极性、掉电恢复或画质。

Windows入口首先执行 `node test/editor/processor_test.cjs`，再运行WSL固件主机套件。Linux直接运行shell入口时，请另行执行上述Node命令。13项处理模块测试覆盖旧四色黄金输出、三种模式、暗部端点、文字无抖动、裁剪约束及Worker协议；无额外Node运行依赖。

多屏接入后，Windows入口还会用`test/editor/probe.sh`读取生产描述及图片配置，生成`output/host-tests/image-profiles.json`，再执行`test/editor/profile_test.cjs`的18项配置/墨色/中性灰/E6旧核心/Worker回归。Linux可先运行探针保存JSON，再将路径作为参数传给该Node测试。`epd`套件包含全部有效描述的能力检查；`image_quant`增加9类尺寸/颜色代表屏的真实PNG直通、打包及已有方向补偿验证。

偏红偏黄候选另用 `node test/editor/color_bias_test.cjs`（17项），Windows统一入口也会运行。覆盖可选强度默认关闭、低色度暖点抑制、参考亮度、纯色/高饱和度保护、浅暖色保留、黑白/文字隔离及确定性；照片浏览器验证为 `prototypes/editor-workspace/color_regression.js`。

Web 回归在本地静态预览和模拟 API 上运行：

```powershell
npx --yes http-server web -a 127.0.0.1 -p 8765 -c-1
# 另一个终端：
New-Item -ItemType Directory -Force output/playwright
npx --yes --package @playwright/cli playwright-cli -s=msp-fixes open http://127.0.0.1:8765/config.html
npx --yes --package @playwright/cli playwright-cli -s=msp-fixes run-code --filename test/web/config_regression.js
npx --yes --package @playwright/cli playwright-cli -s=msp-fixes close
```

覆盖用户名/SSID 文本注入、SSID 选择、BUSY 显式覆盖及桌面/手机布局，截图位于 ignored 的 `output/playwright/`。

当前多屏编辑器回归入口为`test/web/workspace_regression.js`，需使用仅本机、只读的`test/web/workspace_server.cjs`，它服务真实生产HTML/JS及模拟状态，不向真实设备转发。先生成探针数据：

```powershell
./tools/prepare_web_tests.ps1
node test/web/workspace_server.cjs 8765
```

Linux/WSL准备命令为`sh test/web/prepare.sh`。探针数据统一生成到`output/host-tests/image-profiles.json`，服务器第三个参数可指定另一份探针JSON。测试从当前浏览器的loopback地址取得端口，不访问真实设备。

在另一个终端用Playwright CLI打开8765，再依次执行`run-code --filename`指定`workspace_regression.js`、`workspace_theme_regression.js`、`message_color_regression.js`、`gallery_result_regression.js`和`config_regression.js`（均位于`test/web/`）。使用已提交的`test/web/fixtures/`合成图、有效二维码与坏文件，正常回归不依赖私人照片。输出分别位于`output/web-tests/`的workspace/theme/message/gallery/config子目录。

主题测试验证三套浅色底、文字对比度、悬停/焦点/选中态、滑轨、连接色、9种主题/视口组合、跨标签页同步和异常主题回退。图库回执测试覆盖排队与最终成功分离、失败/取消/不可用、网络及总时限、迟到结果、页面退出和删除/清屏的实际结果。

`editor_regression.js`、`editor_assets_regression.js`和`jd_image_regression.js`已属于v3/旧界面历史测试，不应再针对当前上传页运行；旧量化算法保留在`test/editor/fixtures/legacy_processor.js`作回归基线。

文字留言测试通过Playwright路由加载真实页面并模拟状态/上传，覆盖各颜色在80/170/220阈值下的字形一致性、PNG实际POST、错误重试、空文字和三个视口。HTTP主机套件直接提取生产查询/删除处理函数，注入任务状态、无效epoch、文件不存在、删除I/O失败、清屏失败及抢占。

私人原照片只用于可选的人工对照，留在ignored输出目录，不加入Git。历史v3/样稿测试中的日期化素材路径不再是正式验收入口。公开夹具的生成说明位于`test/web/fixtures/README.md`，图像不包含私人素材。

上述浏览器测试只使用本地模拟API。上传测试截获POST并核对最新完整帧，不向设备写入；源码不分发用户照片。早期的 `jd75_color_regression.js`、`preview_photo_regression.js` 也仅用于历史追溯，不作为当前验收入口。实屏画质和颜色仍须记录实际刷新照片。

原有 `lunar`、`startup` 工程也可运行在 **ESP-IDF Linux 主机目标** 上，使用 ESP-IDF 自带的 **Unity** 框架；下面保留原工程运行说明。

## 目录结构

```
test/
├── lunar/                  # 针对 main/lunar.c（农历算法）的测试工程
    ├── CMakeLists.txt       # 顶层 IDF 工程
    └── main/
        ├── CMakeLists.txt   # 编译 main/lunar.c + 测试用例，依赖 unity 组件
        └── test_lunar.c     # Unity 测试用例 + 运行入口
└── startup/                # 显示事务、抢占、回滚和模式可用性测试
```

`startup/` 使用同样的 Linux-host Unity 结构，编译真实的
`display_policy.c` 与 `display_mode.c`，覆盖显示事务抢占、失败的新请求回滚到
最后成功页面、不可用模式和最终手动画面策略。运行方式与 `lunar/` 相同，只需把目录改为
`test/startup`。

## 运行方法

在**已激活 ESP-IDF 环境**的终端里执行（需要 ESP-IDF v5.5.1+）：

```powershell
cd test/lunar

# 1. 切换到 Linux 主机目标（首次执行，需要 --preview）
idf.py --preview set-target linux

# 2. 编译
idf.py build

# 3. 运行生成的可执行文件
#    Windows（需安装 MinGW；ESP-IDF Tools 通常已附带）：
.\build\lunar_unit_test.elf
#    Linux / macOS：
./build/lunar_unit_test.elf
```

测试通过时进程返回码为 `0`，有失败用例时返回 `1`，便于接入 CI。

> 说明：Linux 主机目标依赖宿主机的 C 编译器。Windows 上若未安装 MinGW，可在 WSL / Linux / macOS 中运行；
> 或改用 `idf.py --preview set-target linux` 配套的工具链。`lunar.c` 为纯 C（仅依赖 `<stdio.h>`/`<string.h>`），
> 不引入任何 ESP-IDF 运行时依赖，因此可干净地在主机端编译。

## 已覆盖范围（`test/lunar`）

针对 `main/lunar.c` 公开 API：

| 测试 | 覆盖函数 | 关键锚点 |
|------|----------|----------|
| 公历→农历转换 | `lunar_from_solar` | 表基准 1900-01-31、2000/2023/2024 春节、2023 闰二月、中秋/端午 |
| 越界处理 | `lunar_from_solar` | 1899 / 2101 / 基准日之前返回 false |
| 日期字符串 | `lunar_day_str` `lunar_month_str` | 初一/十五/廿一/三十、正月/冬月/腊月、越界返回 "" |
| 干支与生肖 | `lunar_year_gz` `lunar_year_sx` | 1984=甲子鼠、2020=庚子鼠、2024=甲辰龙 |
| 24 节气 | `lunar_solar_term` | 2024 清明/冬至、非节气日、2000-2099 范围外 |
| 节日 | `lunar_festival` | 公历元旦/劳动/国庆/妇女/儿童，农历春节/中秋/端午 |

## 如何扩展

后续可为其它**纯逻辑函数**新增同样结构的子工程（例如课程表周次解析、`json_escape` 等）。
每个子工程只需：

1. 在 `main/CMakeLists.txt` 的 `SRCS` 里加入被测 `.c`（用相对路径指向 `../../../main/xxx.c`）。
2. 新增 `test_xxx.c`，编写 `RUN_TEST(...)` 用例。

注意：被测模块若依赖 ESP-IDF 运行时（NVS、FreeRTOS、esp_log 等），需要先评估能否在主机端 mock，
或改用真机 `test_apps` 方案。优先从零依赖的纯算法模块入手。
