param([string]$Root='build/bridge-zealot-20260925')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repo
$rootPath=(Resolve-Path -LiteralPath $Root).Path
$plan=Get-Content -LiteralPath (Join-Path $rootPath 'pilot-plan.json') -Raw|ConvertFrom-Json
$python=Join-Path $repo 'build/model-venv/Scripts/python.exe'
$statusPath=Join-Path $rootPath 'pilot-status.json'
$lock=[IO.File]::Open($statusPath+'.lock',[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
function Publish([string]$stage,[string]$detail='') {
    $row=[ordered]@{stage=$stage;updated=[DateTime]::UtcNow.ToString('o');pid=$PID;detail=$detail;promotion_allowed=$false}
    $temp=$statusPath+'.tmp'
    $row|ConvertTo-Json|Set-Content -LiteralPath $temp
    Move-Item -LiteralPath $temp -Destination $statusPath -Force
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
function Wait-Campaign([string]$run,[int]$games) {
    $deadline=[DateTime]::UtcNow.AddHours(2)
    $results=Join-Path $run 'server/results.jsonl'
    while([DateTime]::UtcNow -lt $deadline) {
        if(Test-Path -LiteralPath $results) {
            try {$rows=@(Get-Content -LiteralPath $results|Where-Object {$_}|ForEach-Object {$_|ConvertFrom-Json})}catch{$rows=@()}
            if($rows.Count -ge 2*$games){break}
        }
        $server=(Get-Content -LiteralPath (Join-Path $run 'processes.json') -Raw|ConvertFrom-Json|
            Where-Object {$_.component -eq 'server'})
        if(-not (Get-Process -Id $server.pid -ErrorAction SilentlyContinue)) {
            throw 'Arena server exited before scheduled reports completed'
        }
        Start-Sleep -Seconds 5
    }
    if([DateTime]::UtcNow -ge $deadline){throw 'Campaign exceeded two-hour deadline'}
    while(Get-Process StarCraft -ErrorAction SilentlyContinue) {
        if([DateTime]::UtcNow -ge $deadline){throw 'StarCraft did not exit by deadline'}
        Start-Sleep -Seconds 5
    }
}
$activeRun=$null
try {
    $reference=Join-Path $rootPath 'reference'
    $candidate=Join-Path $rootPath 'candidate'
    if(-not (Test-Path -LiteralPath (Join-Path $reference 'processes.json'))) {
        throw 'Reference was not launched by its verified arena owner'
    }
    if(Test-Path -LiteralPath (Join-Path $candidate 'processes.json')) {
        throw 'Candidate already has a process owner; no duplicate launch'
    }
    $activeRun=$reference
    Publish 'reference-running'
    Wait-Campaign $reference $plan.games_per_condition
    $health=(& $python -m training.arena inspect $reference|Out-String)|ConvertFrom-Json
    if($LASTEXITCODE -ne 0 -or @($health.structurally_valid).Count -ne $plan.games_per_condition -or
       @($health.excluded).Count){throw 'Reference failed normal paired health gate'}
    Stop-OwnedLaunchers $reference
    Publish 'reference-complete'
    $activeRun=$candidate
    & (Join-Path $repo 'scripts/start-arena.ps1') -Run $candidate
    if($LASTEXITCODE -ne 0){throw 'Candidate arena launcher failed'}
    Publish 'candidate-running'
    Wait-Campaign $candidate $plan.games_per_condition
    $health=(& $python -m training.arena inspect $candidate|Out-String)|ConvertFrom-Json
    if($LASTEXITCODE -ne 0 -or @($health.structurally_valid).Count -ne $plan.games_per_condition -or
       @($health.excluded).Count){throw 'Candidate failed normal paired health gate'}
    Stop-OwnedLaunchers $candidate
    $report=Join-Path $rootPath 'pilot-report.json'
    & $python -m training.bridge_zealot_review (Join-Path $rootPath 'pilot-plan.json') `
        $reference $candidate $report
    if($LASTEXITCODE -ne 0){throw 'Paired review failed; preserve artifacts'}
    $outcome=Get-Content -LiteralPath $report -Raw|ConvertFrom-Json
    Publish 'complete' ("screen_more_games="+$outcome.screen_more_games)
} catch {
    Publish 'failed' $_.Exception.Message
    if($activeRun -and (Test-Path -LiteralPath (Join-Path $activeRun 'processes.json')) -and
       -not (Get-Process StarCraft -ErrorAction SilentlyContinue)) {
        Stop-OwnedLaunchers $activeRun
    }
    throw
} finally {$lock.Dispose()}
