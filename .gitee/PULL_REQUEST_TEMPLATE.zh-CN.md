# 改动说明

请简要说明本 PR 解决的问题和主要实现方式。

## 改动类型

- [ ] Bug 修复
- [ ] 新功能
- [ ] 屏幕驱动适配
- [ ] Web 前端
- [ ] 文档
- [ ] 仓库 / 构建 / 发布维护

## 验证结果

- [ ] 已确认目标芯片为 `esp32s3`
- [ ] 已运行 `idf.py build`
- [ ] 已真机烧录 / 验证
- [ ] 已检查 Web 页面
- [ ] 已更新 README / CHANGELOG / 工程文档
- [ ] 若修改 release、分区表或 fontfs，已同步更新 `release/README.md`、`firmware-download` 分支和 SHA256
- [ ] 文档或发布相关改动已运行 `python tools/check_public_docs.py`
- [ ] 已运行 `git diff --check`

## 风险与兼容性

请说明是否影响已有屏幕、分区表、NVS 配置、SPIFFS 数据或 Web API。

## 公开发布检查

- [ ] 未提交 `build/`、`sdkconfig`、`managed_components/`、`spiffs_image/`、本地参考包或个人素材
- [ ] 未提交真实 WiFi 密码、天气 API Key、Codex/中转站 API Key、私钥、签名证书或个人敏感信息
- [ ] 若新增字体/图片/第三方代码，已确认授权允许随本仓库分发

## 日志 / 截图

如有，请附关键串口日志、网页截图或屏幕照片。
