param(
    [Parameter(Mandatory = $false)]
    [string]$Token = $env:GITEE_TOKEN,

    [Parameter(Mandatory = $false)]
    [string]$Owner = "gxp666111",

    [Parameter(Mandatory = $false)]
    [string]$Repo = "miaomiao"
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Token)) {
    throw "Missing Gitee token. Set `$env:GITEE_TOKEN first, then rerun this script."
}

$Description = 'ESP32-S3 智能三色墨水屏终端（epaper_uploader），面向 4.2"/5.83" BWR 电子纸的源码公开、非商业许可固件。支持图片上传与画廊、画板/二维码名片、天气/时钟/万年历、课程表、待办、倒计时、留言板、轮播、低功耗、OTA 与 Web 配网管理；已补齐多屏适配、字库补强、SPIFFS 诊断/格式化和实机 UI 优化。基于 ESP-IDF v5.5.x，当前 v2.3.1。许可：PolyForm Noncommercial 1.0.0；商用/量产需提前取得书面授权。'

$Body = @{
    access_token = $Token
    description  = $Description
    homepage     = "https://gitee.com/$Owner/$Repo"
} | ConvertTo-Json -Compress

$Uri = "https://gitee.com/api/v5/repos/$Owner/$Repo"
$Resp = Invoke-RestMethod -Uri $Uri -Method Patch -ContentType "application/json; charset=utf-8" -Body $Body

[pscustomobject]@{
    ok          = $true
    full_name   = $Resp.full_name
    description = $Resp.description
    homepage    = $Resp.homepage
}
