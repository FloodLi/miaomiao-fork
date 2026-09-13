$ErrorActionPreference = 'Stop'
$epdRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$epdLinuxRoot = & wsl.exe -d Ubuntu --exec wslpath -a $epdRoot.Replace('\', '/')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$epdLinuxRoot = $epdLinuxRoot.Trim()
& wsl.exe -d Ubuntu -- cmake -S "$epdLinuxRoot/test/epd" -B /tmp/msp-epd-tests -G Ninja
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& wsl.exe -d Ubuntu -- cmake --build /tmp/msp-epd-tests --parallel 1
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& wsl.exe -d Ubuntu -- ctest --test-dir /tmp/msp-epd-tests --output-on-failure
exit $LASTEXITCODE
