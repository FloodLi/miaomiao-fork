# Claude 开发备忘录

<!-- release-sync-2026-04-28 -->
## Launch Readiness Note（2026-04-28，本轮修复后）

Before release-oriented work, review `RELEASE_AUDIT_2026-04-28.md`. The first launch-blocking pass is implemented: display epoch arbitration, blocking/cancel-aware semantics for calendar/canvas/message display requests, Basic Auth on API/media GET routes, SPIFFS no-auto-format with degraded AP/Web boot on mount failure and explicit remount/confirmed-format recovery, Basic Auth excluded from config backup/restore, reboot-required panel config, bad-gallery cleanup, manual-screen weather throttling, EPD init graceful fallback, and AP-password log masking. Prioritize validation, OTA/low-power stress, image-algorithm productization, and long-run stability over new features.
<!-- /release-sync-2026-04-28 -->
> 项目路径：C:\Users\43060\Desktop\msp
> 用户自己编译，提问用中文

---

## 项目概况

- **硬件**：ESP32-S3，ESP-IDF（最低 v5.5.1），FreeRTOS
- **屏幕**：墨水屏 SSD1619/UC8179，三色BWR，支持两种尺寸：
  - 4.2寸：400×300
  - 6寸：648×480
- **存储**：SPIFFS 平铺文件系统，路径限制约32字符（不含 `/spiffs` 挂载点）
- **Web**：ESP HTTP Server，HTML内嵌到固件

---

## 关键文件

| 文件 | 作用 |
|------|------|
| `web/board.html` | 画布留言板编辑器（单文件，所有JS/CSS内嵌） |
| `main/canvas_board.c` | 画布渲染逻辑（JSON→fb_t→墨水屏） |
| `main/canvas_board.h` | 画布模块头文件 |
| `main/http_canvas.c` | 画布相关HTTP路由处理 |
| `main/http_app.c` | 所有路由注册，含 `/panel_config` |
| `main/http_internal.h` | HTTP子系统内部共享声明 |
| `main/fb_render.c/h` | 帧缓冲渲染基础函数 |

---

## 画布编辑器（board.html）架构

### 全局状态
```js
let EPD_W, EPD_H  // 从 /panel_config 读取的实际屏幕分辨率
let SCALE         // 显示缩放比：Math.floor(880 / EPD_W)，1~3整数
let elements[]    // 所有画布元素
let selected      // 当前选中元素索引
let CANVAS_IMAGES // {name: {w,h,data:Uint8Array}} 已加载的图片数据
let userImageNames// 已上传图片名列表
```

### 两层Canvas
- `#canvas`（主画布）：渲染所有元素内容
- `#overlay`（叠加层）：渲染选中框、拖拽预览，不影响主画布内容

### 元素类型
| type | 关键字段 |
|------|---------|
| text | x,y,text,scale(1-6),color |
| rect | x,y,w,h,fill,sw,color |
| ellipse | x,y,rx,ry,fill,sw,color |
| line | x1,y1,x2,y2,color |
| path | pts[[x,y],...],sw,color |
| icon | x,y,name,scale(1-4),color |
| image | x,y,name,dw,dh,color |

### color值：0=黑，1=红，2=白

---

## 图片功能（重点）

### 存储格式
- 路径：`/spiffs/cimg/{name}.bin`（注意：不是 canvas_img，太长会报错）
- 格式：4字节头(w u16le + h u16le) + 1-bit行主序位图（MSB优先）
- 最大：50000字节（CANVAS_IMAGE_MAX_BYTES）

### JS侧关键函数
```js
imageFileTo1bit(file, maxW, maxH)  // Floyd-Steinberg抖动转1bit
drawBmpRaw(ctx2d, imgData, x, y, dispW, dispH)  // 支持任意显示尺寸
floydSteinberg(gray, w, h)         // 误差扩散抖动
```

### 图片元素字段
- `dw`/`dh`：在墨水屏上的显示像素尺寸（不是scale倍数）
- 上传时用全屏最大分辨率（EPD_W × EPD_H）
- 拖拽角点等比调整dw/dh，支持任意缩放（放大/缩小都行）

### C侧渲染（canvas_board.c）
- 读取 `dw`/`dh` 字段，最近邻采样，支持任意缩放
- 逐行读取（`malloc(stride)`单行缓冲），避免大内存分配
- 用 `fseek` 跳行提高效率

### HTTP路由
| 路由 | 方法 | 说明 |
|------|------|------|
| /canvas_image_list | GET | 返回已上传图片列表 |
| /canvas_image | GET | 流式读取图片文件（?name=xxx） |
| /canvas_image_upload | POST | 流式写入图片（?name=xxx） |
| /canvas_image_delete | POST | 删除图片（?name=xxx） |

---

## 坐标系说明

- 元素坐标（x,y,dw,dh等）：墨水屏设备像素
- Canvas绘制坐标：元素坐标 × SCALE
- 鼠标→元素坐标：`canvasXY()` 用 getBoundingClientRect 实际比例换算
  ```js
  const scaleX = canvas.width / r.width;
  const px = (e.clientX - r.left) * scaleX;
  x = Math.round(px / SCALE);
  ```
- **不要**给canvas设置 `style.width/height` 固定px值，会导致布局错乱

---

## 选中框 / 拖拽控制点

- 选中框画在 `overlay`（octx），不污染主画布
- `drawSelectionBox(el)` → 用 `octx` 绘制
- `renderOverlaySelection()` → 从 `render()` 调用，绘制前先 clearRect
- `getHandles(el)` → 返回控制点列表 {id, cx, cy}（元素坐标系）
- image类型有4个角点：tl/tr/bl/br，等比缩放

---

## 图标功能

- 内置图标：`BUILTIN_BITMAPS` 对象，16×16 1bit（每行2字节）
- 用户图标：`/spiffs/icons/{name}.bin`，32字节，16×16 1bit
- 预览渲染：`bmpToCanvas(name, 32, 80, 40, 70)` 用块边界数学避免错位

---

## 已知注意事项

1. **SPIFFS路径长度**：内部文件名（不含挂载点）≤32字符，路径过长fopen返回NULL
2. **ESP32大内存**：不能 `malloc(50KB)`，图片上传/下载都用512字节流式处理
3. **JS语法错误定位**：用 `node -e "new vm.Script(code, {filename:'f'})"` 可得精确行号
4. **C侧scale读取**：用 `valueint`（整数截断），JS侧也限制为整数；图片改用dw/dh不再有此问题
5. **两屏尺寸**：4.2寸400×300(SCALE=2)，6寸648×480(SCALE=1)，从/panel_config动态读取

---

## 下次对话如何恢复

新开对话时说：
> "我在继续开发ESP32墨水屏项目，请读取 C:\Users\43060\Desktop\msp\CLAUDE_NOTES.md 了解背景"

然后把需要修改的文件路径告诉我，我用Read工具读取后直接接着改。
