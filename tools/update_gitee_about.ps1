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

$AboutPath = Join-Path (Split-Path -Parent $PSScriptRoot) "docs\gitee-about.md"
$About = Get-Content -LiteralPath $AboutPath -Encoding UTF8 -Raw
$Match = [regex]::Match($About, '```text\s*(.*?)\s*```',
                        [System.Text.RegularExpressions.RegexOptions]::Singleline)
if (-not $Match.Success) {
    throw "Could not find description text block in docs/gitee-about.md"
}
$Description = $Match.Groups[1].Value.Trim()

$Body = @{
    access_token = $Token
    description  = $Description
    homepage     = "https://oshwhub.com/team_voosogmo/project_fxbcjhaa"
} | ConvertTo-Json -Compress

$Uri = "https://gitee.com/api/v5/repos/$Owner/$Repo"
$Resp = Invoke-RestMethod -Uri $Uri -Method Patch -ContentType "application/json; charset=utf-8" -Body $Body

[pscustomobject]@{
    ok          = $true
    full_name   = $Resp.full_name
    description = $Resp.description
    homepage    = $Resp.homepage
}
