param([int]$Port=8787,[switch]$Restart)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$server=(Resolve-Path (Join-Path $PSScriptRoot 'server.cjs')).Path
$runtime=Join-Path $root 'output/workspace-prototype-deps'
$logs=Join-Path $root 'output/workspace-prototype'
$node=(Get-Command node.exe -ErrorAction Stop).Source
if(!(Test-Path -LiteralPath (Join-Path $runtime 'node_modules/lucide-static/icons/undo-2.svg'))){
    & npm.cmd install --prefix $runtime --no-package-lock --ignore-scripts lucide-static@1.45.0
    if($LASTEXITCODE -ne 0){throw 'Lucide icon installation failed'}
}
New-Item -ItemType Directory -Path $logs -Force | Out-Null
for($candidate=$Port;$candidate -lt $Port+20;$candidate++){
    $connections=@(Get-NetTCPConnection -LocalPort $candidate -State Listen -ErrorAction SilentlyContinue)
    if($connections.Count -gt 0){
        $info=$null
        try{$info=Invoke-RestMethod "http://127.0.0.1:$candidate/api/session" -TimeoutSec 2}catch{}
        if($info.app -eq 'msp-editor-workspace-prototype'){
            if(!$Restart){[pscustomobject]@{url="http://127.0.0.1:$candidate";pid=$info.pid;reused=$true};return}
            $existing=Get-CimInstance Win32_Process -Filter "ProcessId=$($info.pid)"
            if(!$existing -or $existing.CommandLine.IndexOf($server,[StringComparison]::OrdinalIgnoreCase) -lt 0){throw 'Refusing to stop an unrelated process'}
            Stop-Process -Id $info.pid -ErrorAction Stop
        }else{continue}
    }
    $out=Join-Path $logs "server-$candidate.log"
    $err=Join-Path $logs "server-$candidate-error.log"
    $p=Start-Process -FilePath $node -ArgumentList @(('"'+$server+'"'),'--port',"$candidate") -WindowStyle Hidden -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
    for($i=0;$i -lt 30;$i++){
        Start-Sleep -Milliseconds 200
        $p.Refresh();if($p.HasExited){throw (Get-Content -LiteralPath $err -Raw)}
        try{$info=Invoke-RestMethod "http://127.0.0.1:$candidate/api/session" -TimeoutSec 1}catch{continue}
        if($info.app -eq 'msp-editor-workspace-prototype'){[pscustomobject]@{url="http://127.0.0.1:$candidate";pid=$p.Id;reused=$false};return}
    }
    throw 'Prototype server did not become ready'
}
throw 'No free loopback port found'
