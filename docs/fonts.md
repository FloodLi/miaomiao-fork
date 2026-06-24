# 字库说明

当前固件有三条文字显示路径：

- `main/font_data.h`：紧凑内置字库，ASCII 95 个 8x16 字符 + 中文 6814 个 16x16 字符，用于小字号、兜底和启动期文本。
- `fontfs` 分区：构建时生成的 16/24/32px MEF 位图字库，用于 4.2/5.83 寸主要 UI 文本、标题、正文和较大的普通数字。
- Web 画板/留言：浏览器把用户选择的字体渲染为图片后上传到设备，适合测试宋体、仿宋、书法类等本机或浏览器可用字体；该路径不要求把所有字体内置到固件。

## 字体来源

公开固件字库使用 `tools/fonts/noto/` 下的 OFL 开源字体生成：

- `LXGW975YuanSC-400W.ttf`：中文、英文、数字和常用符号主字形。
- `NotoSansMono-Regular.ttf`：部分符号/等宽字符兜底。

这些字体遵循 SIL Open Font License 1.1。许可文件保留在 `tools/fonts/noto/`，构建脚本为 `tools/fetch_open_fonts.py`、`tools/gen_font.py`、`tools/gen_ext_font.py`。

## 分区与生成

`partitions.csv` 中 `fontfs` 位于 `0x940000`，大小 `0x2E0000`（2.875MB）。构建时会从 `spiffs_image/fonts/` 生成 `fontfs.bin`，并随完整 16MB 固件一起烧录。

当前保留三档 MEF：

- `cjk16.mef`：小标签、状态文字。
- `cjk24.mef`：正文、列表、紧凑页面内容。
- `cjk32.mef`：标题、较大普通数字和宽松页面内容。

48px 或更大的请求默认复用 32px 字库做受控缩放，避免额外 48px 字库占满 flash。

`fontfs` 与用户 `spiffs` 是两个独立分区：fontfs 只放固件随附字库，用户 SPIFFS 只放图片、画布素材和运行时文件。配置页/上传页展示的 SPIFFS 进度条代表用户文件区使用率，不包含 fontfs。

## 发布注意

不要用 Windows 系统字体（微软雅黑、黑体、宋体、等线等）生成公开固件资源。网页留言/画板可以让浏览器临时渲染用户本机字体成图片，但固件内置资源必须来自可再分发字体。若以后替换字体，应同时更新本文件、生成脚本说明和 release 合并说明。
