param([string]$Root='build/pvz-approach-grace-20260925')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repo
$rootPath=(Resolve-Path -LiteralPath $Root).Path
$plan=Get-Content -LiteralPath (Join-Path $rootPath 'candidate-plan.json') -Raw|ConvertFrom-Json
$python=Join-Path $repo 'build/model-venv/Scripts/python.exe'
$statusPath=Join-Path $rootPath 'candidate-status.json'
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
    $arena=Join-Path $rootPath 'candidate-owned'
    if(Test-Path -LiteralPath (Join-Path $rootPath 'candidate-report.json')) {
        throw 'Candidate report exists; do not duplicate games'
    }
    if(Test-Path -LiteralPath (Join-Path $arena 'processes.json')) {
        throw 'Candidate already has a process owner; inspect before recovery'
    }
    if(Get-Process StarCraft -ErrorAction SilentlyContinue) {
        throw 'StarCraft is already running; cannot safely share the runtimes'
    }
    foreach($entry in $plan.sha256.PSObject.Properties) {
        Assert-Hash (Join-Path $rootPath $entry.Name) $entry.Value
    }
    Assert-Hash (Join-Path $repo 'training/pvz_build_lease_mixed_review.py') $plan.reviewer_sha256
    Assert-Hash (Join-Path $repo 'training/pvz_approach_grace_screen.py') $plan.screen_sha256
    Assert-Hash $PSCommandPath $plan.launcher_sha256
    Assert-Hash (Join-Path $repo 'build/pvz-build-lease-diagnostic-20260925/diagnostic-report-capped.json') $plan.reference_report_sha256
    & $python -m training.arena verify $arena | Out-Null
    if($LASTEXITCODE -ne 0){throw 'Prepared input verification failed'}
    Publish 'launching'
    & (Join-Path $repo 'scripts/start-arena.ps1') -Run $arena
    if($LASTEXITCODE -ne 0){throw 'Arena launcher failed'}
    Publish 'running'
    $deadline=[DateTime]::UtcNow.AddHours(2)
    $results=Join-Path $arena 'server/results.jsonl'
    while([DateTime]::UtcNow -lt $deadline) {
        if(Test-Path -LiteralPath $results) {
            try {$rows=@(Get-Content -LiteralPath $results|Where-Object {$_}|ForEach-Object {$_|ConvertFrom-Json})}catch{$rows=@()}
            if($rows.Count -ge 2*$plan.games){break}
        }
        $server=Get-Content -LiteralPath (Join-Path $arena 'processes.json') -Raw|ConvertFrom-Json|
            Where-Object {$_.component -eq 'server'}
        if(-not (Get-Process -Id $server.pid -ErrorAction SilentlyContinue)) {
            throw 'Arena server exited before scheduled reports completed'
        }
        Start-Sleep -Seconds 5
    }
    if([DateTime]::UtcNow -ge $deadline){throw 'Candidate exceeded two-hour deadline'}
    while(Get-Process StarCraft -ErrorAction SilentlyContinue) {
        if([DateTime]::UtcNow -ge $deadline){throw 'StarCraft did not exit by deadline'}
        Start-Sleep -Seconds 5
    }
    Stop-OwnedLaunchers $arena
    Publish 'reviewing'
    & $python -m training.pvz_build_lease_mixed_review (Join-Path $rootPath 'candidate-plan.json') `
        (Join-Path $rootPath 'candidate-report.json')
    if($LASTEXITCODE -ne 0){throw 'Candidate review failed; preserve artifacts'}
    & $python -m training.pvz_approach_grace_screen (Join-Path $rootPath 'candidate-plan.json') `
        (Join-Path $rootPath 'candidate-report.json') (Join-Path $rootPath 'screen.json')
    if($LASTEXITCODE -ne 0){throw 'Candidate screen failed to run; preserve artifacts'}
    Publish 'complete'
} catch {
    Publish 'failed' $_.Exception.Message
    throw
} finally {$lock.Dispose()}
