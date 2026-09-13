param([string]$Distribution='Ubuntu')
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$linuxRoot=& wsl.exe -d $Distribution --exec wslpath -a $root.Replace('\','/')
if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
New-Item -ItemType Directory -Force (Join-Path $root 'output/host-tests') | Out-Null
foreach($name in @('workspace','theme','message','gallery','config')){
    New-Item -ItemType Directory -Force (Join-Path $root "output/web-tests/$name") | Out-Null
}
& wsl.exe -d $Distribution --exec sh "$($linuxRoot.Trim())/test/editor/probe.sh" |
    Set-Content -Encoding utf8 -LiteralPath (Join-Path $root 'output/host-tests/image-profiles.json')
exit $LASTEXITCODE
