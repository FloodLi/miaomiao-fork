# 元器件清单（BOM）- 实际采购版

## 硬件提示（2026-06-25）

本文件仅保留与固件兼容相关的硬件摘要。复刻前请确认器件替代、屏幕批次、BUSY 极性、低功耗电流和排障口径。固件侧已避免运行时热切屏，保护 SPIFFS 用户数据不被挂载失败自动格式化，并在 EPD 初始化失败时保留基础网络诊断能力。

> **说明**：班级/立创等**实采明细（含 LCSC 编号、批次具体屏型料号等）暂不公开**。配套 4.2" SSD1619 PCB/外壳已开源到嘉立创：[喵哎-MiaooAim 4.2寸 墨水屏 SSD1619](https://oshwhub.com/team_voosogmo/project_fxbcjhaa)。下列仅为与固件**兼容性**相关的提要，避免买错 Flash 或屏驱。

## 与 epaper_uploader（本仓库固件）的对应关系

| 项目 | 说明 |
|------|------|
| **模组** | 常见 **ESP32-S3-WROOM** 系列须满足 **16MB Flash**（如 N16、N16R8）；**勿用仅 8MB Flash（N8）** 直接烧录当前 `partitions.csv`。 |
| **PSRAM** | 当前主力样机N16R8带8MB PSRAM，默认构建启用Octal模式；N16仅保留降级路径，大屏、多色帧缓冲、图片解码和Web并发须单独验证，不保证与N16R8等价。 |
| **Flash 分区** | 当前默认布局包含 factory、双 OTA、128KB coredump、2.875MB fontfs 和 3.875MB 用户 SPIFFS；分区表或 fontfs 变化后不能只 OTA app。 |
| **墨水屏** | 实测记录统一见 [`README.md`](../README.md#面板接入与实测记录)。需按具体型号/ID核对FPC、控制器、尺寸、墨色与驱动；“已点亮”不能外推同尺寸其它型号或全部屏幕批次。 |

引脚与功能默认值以 [`README.md`](../README.md) 为准；工程总览见 [`ENGINEERING_DOCS.md`](../ENGINEERING_DOCS.md)。
