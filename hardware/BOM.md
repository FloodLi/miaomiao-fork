# 元器件清单（BOM）

## 硬件提示（2026-06-25）

复刻前请确认 Flash 容量、面板型号、BUSY 极性、电源静态电流、按键 RTC 唤醒能力和烧录流程。固件侧已将面板切换改为“保存后重启生效”，关闭 SPIFFS 挂载失败自动格式化，并在正常启动时尽量保留 AP/Web 用于 EPD 初始化失败诊断。

> **说明**：含料号、供应商、价格等**采购级元器件清单暂不随本仓库公开发布**。配套 4.2" SSD1619 PCB/外壳已开源到嘉立创：[喵哎-MiaooAim 4.2寸 墨水屏 SSD1619](https://oshwhub.com/team_voosogmo/project_fxbcjhaa)。复刻请优先阅读根目录 [`README.md`](../README.md) 的**硬件要求**、**引脚连接**与**分区表**。

## 与固件对齐的硬指标（摘录）

- **Flash**：默认 `partitions.csv` 约需 **16MB** 级 Flash（factory + 双 OTA 各 3MB + 128KB coredump + 2.875MB fontfs + 3.875MB 用户 SPIFFS）。**仅 8MB Flash（如部分 N8 模组）无法直接套用当前默认分区**，须改分区后再烧录。
- **PSRAM**：当前主力样机为 `N16R8`（8MB PSRAM），构建启用Octal PSRAM。N16保留无PSRAM启动降级路径，但不保证大屏、多色帧缓冲、大图解码和Web并发均可用，不能当作等价替代；需按目标功能单独验证。
- **墨水屏**：具体型号、配置ID及已点亮/图片实测范围统一见 [`README.md`](../README.md#面板接入与实测记录)。同尺寸不同控制器不能自动套用验证结论；引脚与BUSY设置按对应面板入口和实际接线核对。

更多硬件说明见 [`ENGINEERING_DOCS.md`](../ENGINEERING_DOCS.md) 第三章「硬件设计」。
