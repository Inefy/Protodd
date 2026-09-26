param([string]$Root='build/pvz-repeat-control-20260925')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repo
$rootPath=(Resolve-Path -LiteralPath $Root).Path
$plan=Get-Content -LiteralPath (Join-Path $rootPath 'repeat-plan.json') -Raw|ConvertFrom-Json
$python=Join-Path $repo 'build/model-venv/Scripts/python.exe'
$statusPath=Join-Path $rootPath 'repeat-status.json'
$lock=[IO.File]::Open($statusPath+'.lock',[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
function Publish([string]$stage,[string]$detail='') {
    $row=[ordered]@{stage=$stage;updated=[DateTime]::UtcNow.ToString('o');pid=$PID;detail=$detail;promotion_allowed=$false}
    $temp=$statusPath+'.tmp'
    $row|ConvertTo-Json|Set-Content -LiteralPath $temp
    Move-Item -LiteralPath $temp -Destination $statusPath -Force
}
function Assert-Hash([string]$path,[string]$expected) {
    $actual=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLower()
    if($actual -ne $expected){throw "Frozen input hash differs: $path"}
}
function Stop-OwnedLaunchers([string]$run) {
    foreach($job in (Get-Content -LiteralPath (Join-Path $run 'processes.json') -Raw|ConvertFrom-Json)) {
        $process=Get-Process -Id $job.pid -ErrorAction SilentlyContinue
        $expected=([DateTimeOffset]::Parse($job.started)).UtcDateTime
        if($process -and $process.ProcessName -eq 'java' -and
           [Math]::Abs(($process.StartTime.ToUniversalTime()-$expected).TotalSeconds) -lt 1) {
            Stop-Process -Id $job.pid -Force
        }
    }
}
try {
    $repeat=Join-Path $rootPath 'repeat'
    $original=Join-Path $repo $plan.original
    if(Test-Path -LiteralPath (Join-Path $rootPath 'repeat-report.json')) {
        throw 'Report already exists; do not rerun a reviewed control'
    }
    if(Test-Path -LiteralPath (Join-Path $repeat 'processes.json')) {
        throw 'Repeat already has a process owner; inspect before recovery'
    }
    Assert-Hash (Join-Path $original 'manifest.json') $plan.original_manifest_sha256
    Assert-Hash (Join-Path $repeat 'manifest.json') $plan.repeat_manifest_sha256
    Assert-Hash (Join-Path $repo 'build/pvz-core-transition-20260925/reference-Protodd.dll') $plan.dll_sha256
    Assert-Hash (Join-Path $repo 'build/pvz-core-transition-20260925/client-bundle/client.jar') $plan.client_jar_sha256
    Assert-Hash (Join-Path $repo 'training/pvz_repeat_control_review.py') $plan.review_source_sha256
    Assert-Hash (Join-Path $repo 'training/pvz_core_transition_review.py') $plan.trace_source_sha256
    & $python -m training.arena verify $original | Out-Null
    if($LASTEXITCODE -ne 0){throw 'Original input verification failed'}
    & $python -m training.arena verify $repeat | Out-Null
    if($LASTEXITCODE -ne 0){throw 'Repeat input verification failed'}
    Publish 'launching'
    & (Join-Path $repo 'scripts/start-arena.ps1') -Run $repeat
    if($LASTEXITCODE -ne 0){throw 'Repeat arena launcher failed'}
    Publish 'running'
    $deadline=[DateTime]::UtcNow.AddHours(2)
    $results=Join-Path $repeat 'server/results.jsonl'
    while([DateTime]::UtcNow -lt $deadline) {
        if(Test-Path -LiteralPath $results) {
            try {$rows=@(Get-Content -LiteralPath $results|Where-Object {$_}|ForEach-Object {$_|ConvertFrom-Json})}catch{$rows=@()}
            if($rows.Count -ge 2*$plan.games){break}
        }
        $server=Get-Content -LiteralPath (Join-Path $repeat 'processes.json') -Raw|ConvertFrom-Json|
            Where-Object {$_.component -eq 'server'}
        if(-not (Get-Process -Id $server.pid -ErrorAction SilentlyContinue)) {
            throw 'Arena server exited before scheduled reports completed'
        }
        Start-Sleep -Seconds 5
    }
    if([DateTime]::UtcNow -ge $deadline){throw 'Control exceeded two-hour deadline'}
    while(Get-Process StarCraft -ErrorAction SilentlyContinue) {
        if([DateTime]::UtcNow -ge $deadline){throw 'StarCraft did not exit by deadline'}
        Start-Sleep -Seconds 5
    }
    $health=(& $python -m training.arena inspect $repeat|Out-String)|ConvertFrom-Json
    if($LASTEXITCODE -ne 0 -or @($health.structurally_valid).Count -ne $plan.games -or
       @($health.excluded).Count){throw 'Repeat failed normal health gate'}
    Stop-OwnedLaunchers $repeat
    Publish 'reviewing'
    & $python -m training.pvz_repeat_control_review (Join-Path $rootPath 'repeat-plan.json') `
        $original $repeat (Join-Path $rootPath 'repeat-report.json')
    if($LASTEXITCODE -ne 0){throw 'Control review failed; preserve artifacts'}
    $outcome=Get-Content -LiteralPath (Join-Path $rootPath 'repeat-report.json') -Raw|ConvertFrom-Json
    if(-not ($outcome.checks.same_inputs -and $outcome.checks.paired -and
              $outcome.checks.normal_runtime)){throw 'Control input/health checks failed'}
    Publish 'complete' ("deterministic="+$outcome.deterministic)
} catch {
    Publish 'failed' $_.Exception.Message
    throw
} finally {$lock.Dispose()}
