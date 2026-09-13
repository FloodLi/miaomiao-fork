param([string]$Distribution = 'Ubuntu')
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path.Replace('\', '/')
& node "$repoRoot/test/editor/processor_test.cjs"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& node "$repoRoot/test/editor/color_bias_test.cjs"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$linuxRoot = & wsl.exe -d $Distribution --exec wslpath -a $repoRoot
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$profileDir = Join-Path $repoRoot 'output/host-tests'
New-Item -ItemType Directory -Force -Path $profileDir | Out-Null
$profileFile = Join-Path $profileDir 'image-profiles.json'
& wsl.exe -d $Distribution --exec sh "$($linuxRoot.Trim())/test/editor/probe.sh" | Set-Content -LiteralPath $profileFile -Encoding utf8
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& node "$repoRoot/test/editor/profile_test.cjs" $profileFile
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& wsl.exe -d $Distribution --exec sh "$($linuxRoot.Trim())/test/run_host_tests.sh"
exit $LASTEXITCODE
