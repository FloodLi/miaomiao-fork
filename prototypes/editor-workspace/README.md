# 多屏图片工作区：第一阶段样稿

本目录保留第一阶段的独立本地样稿，不嵌入固件，不接真实设备。2026-09-13用户确认先收尾、暂缓实屏验收后，新工作区已接入生产`web/index.html`及`/status.image_profile`；本目录继续作为旧样稿对照，正式源码及新回归入口见`IMPLEMENTATION.md`、`test/web/workspace_regression.js`。

样稿的旧彩色路径现在读取固定回归夹具`test/editor/fixtures/legacy_processor.js`，不再从已清理的生产页面抽取。下方“尚未接入”等描述属于各日期当时状态；实屏校色仍未完成。

## 9月13日 4.2寸独立测试

用户认为7.5寸已基本满意，转测JD79668 4.2寸四色（ID37，400x300）。本地入口 `http://127.0.0.1:8787/?panel=37`；此ID按型号接入共享四色混色/柔化点阵，不按分辨率选择算法。ID34输出和偏色抑制35不变，ID37采用中性调色、偏色抑制0，不套用7.5寸修正强度。

ID37候选使用独立`preview-37-color-v1`配置标识，旧`preview-37`参数保留但不自动混入；支持暗部、饱和度、柔化点阵及可选偏色抑制，隐藏旧红黄提取阈值。原有其他型号彩色路径不变。切换示例素材时立即使旧图失效，读取期间不能上传或导出旧结果。

新增`jd42_regression.js`的32项检查，原尺寸色阶、原照片新旧对照、可选偏色20、黑白照片、文字二维码及说明位于`output/jd42-color-20260913/`。69项多屏回归、10项ID34偏色回归、Node13+17项和固件图片转换ASan/UBSan套件通过。预处理PNG经当前旧ID37照片默认路径复核不变，可先在现有固件上人工上传对比；不表示已完成物理校色或扫码验收。

## 9月12日偏色候选

用户已反馈布局“还可以”，进一步指出偏红、偏黄。样稿现在只对ID34色彩模式默认启用35%的“偏色抑制”（高级调整内，0关闭），其他型号和黑白/文字模式不启用。

共享处理模块新增可选`neutralProtection`，默认0以保持既有调用输出；样稿明确传35。使用连续的低色度/低饱和度权重，将部分红黄覆盖率替换为参考色板下等亮的黑白组合，不改编码墨色、纯红黄或高饱和度暗色，也不修改电压/LUT。
这会适度减弱真实的浅暖色，属于可调的偏色修正候选，不是实屏测色校准。当前照片软件统计中红点约减少14%、黄点约减少24%，低色度区域暖色点约减少32%；这些比例不是所有照片或实屏的保证。

新增 `node test/editor/color_bias_test.cjs` 的17项检查，以及浏览器 `color_regression.js` 的10项原图/参数/隔离检查。对照图片和报告在 `output/workspace-prototype/color-bias-*`。本次未打包新固件，多屏统一接入仍未完成。

## 启动

在项目根目录运行：

```powershell
./prototypes/editor-workspace/start.ps1
```

需要Node.js/npm。脚本仅将Lucide静态图标1.45.0安装到ignored的`output/workspace-prototype-deps/`，随后隐藏启动本地Node服务。默认端口8787；已有本样稿则复用，不相关服务占用时换端口。显式加`-Restart`只允许重启本目录对应的服务进程。

服务仅绑定127.0.0.1，样稿不能从其他电脑或手机直接连接；手机布局可通过缩窄电脑窗口验证。浏览器打开脚本输出的URL，不需要刷固件。

## 样稿包含

- 无外层卡片的独立工作区，默认300px、可拖动至260至380px或收起的参数栏。
- 39个有效面板描述快照，实际宽高来自当前C驱动表；保留128x250 RAM等特殊尺寸，不应用驱动地址偏移。
- 真实图片处理、各模式调节、独立观看与构图缩放、拖动取景、原图按住对比、撤销/重做、原尺寸PNG导出。
- 模拟上传只在本机内存验证PNG尺寸，随后丢弃图像数据；真实`/upload`、`/panel_config`等路径返回405，没有代理设备请求的代码。
- 浏览器仅保存本样稿命名空间的参数，不覆盖生产编辑器的键，不保存原图。

这一步复用现有代码，不代表已优化所有屏幕画质：ID34和ID37候选色彩及黑白/文字调用`web/image_processor.js`；其他彩色路径由服务器从`web/index.html`提取原有处理函数，E6按明确ID43识别。未接入的暗部/饱和度控件隐藏，不展示无效滑块。进一步的各屏参数整理、生产能力接口和固件接入仍未完成。

## 素材与安全边界

默认素材已改为`test/web/fixtures/`中可公开的合成图与二维码，不再读取私人照片或日期化输出。历史截图/报告对应开发当时状态，不作为当前正式验收；当前入口见`test/README.md`。所有静态资源采用精确路径白名单，不能浏览工作区任意文件；CSP限制连接到同源，服务器拒绝外部Origin及非loopback Host。

图标来自`lucide-static`1.45.0，许可证保留在安装包的`LICENSE`中（含ISC及衍生图标相关许可）；不纳入固件依赖。

## 验证

```powershell
npx --yes --package @playwright/cli playwright-cli -s=workspace-prototype open http://127.0.0.1:8787
npx --yes --package @playwright/cli playwright-cli -s=workspace-prototype run-code --filename prototypes/editor-workspace/regression.js
npx --yes --package @playwright/cli playwright-cli -s=workspace-prototype run-code --filename prototypes/editor-workspace/color_regression.js
npx --yes --package @playwright/cli playwright-cli -s=workspace-prototype run-code --filename prototypes/editor-workspace/jd42_regression.js
```

当前69项检查通过：39个面板的尺寸和合法墨色、实时调节/历史、构图与对比、参数栏调整、快速切屏和解码期间换屏、Worker回退、原始PNG模拟上传、真实设备路径阻断、六种窗口布局和本地请求约束。报告和截图在`output/workspace-prototype/`。这些不是固件或实屏画质验收。

`panels.json`是编译`panels_probe.c`读取生产驱动描述所得的快照，不是手工按名称/分辨率猜测。探针仅执行自己的打印入口，不执行导入的测试入口或驱动初始化。更新面板表后可用现有WSL/GCC和`test/epd/include`重新编译探针，核对JSON后更新快照；第二阶段再接入生产能力接口。

9月13日本轮未改生产`web/index.html`、`web/image_processor.js`、驱动、HTTP接口或固件构建文件。共享处理模块仅含前一轮已记录的可选偏色修正；本轮不生成或替换测试固件。
