param(
    [string]$Root='build/worker-outcome-20260925'
)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repo
$rootPath=(Resolve-Path -LiteralPath $Root).Path
$planPath=Join-Path $rootPath 'pilot-plan.json'
$plan=Get-Content -LiteralPath $planPath -Raw|ConvertFrom-Json
$python=Join-Path $repo 'build/model-venv/Scripts/python.exe'
$statusPath=Join-Path $rootPath 'pilot-status.json'
$lock=[IO.File]::Open($statusPath+'.lock',[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
function Publish([string]$stage,[string]$detail='') {
    $row=[ordered]@{stage=$stage;updated=[DateTime]::UtcNow.ToString('o');pid=$PID;detail=$detail;promotion_allowed=$false}
    $temporary=$statusPath+'.tmp'
    $row|ConvertTo-Json|Set-Content -LiteralPath $temporary
    Move-Item -LiteralPath $temporary -Destination $statusPath -Force
}
function Stop-OwnedLaunchers([string]$run) {
    foreach($job in (Get-Content -LiteralPath (Join-Path $run 'processes.json') -Raw|ConvertFrom-Json)) {
        $process=Get-Process -Id $job.pid -ErrorAction SilentlyContinue
        $expected=if($job.started -is [DateTime]){$job.started.ToUniversalTime()}else{([DateTimeOffset]::Parse($job.started)).UtcDateTime}
        # DateTimeOffset.Parse can truncate stored fractional seconds on this host.
        # PID, executable name, and a subsecond launch match identify our process.
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
        # A failed launcher must not leave the queue waiting forever.
        $server=(Get-Content -LiteralPath (Join-Path $run 'processes.json') -Raw|ConvertFrom-Json|
            Where-Object {$_.component -eq 'server'})
        if(-not (Get-Process -Id $server.pid -ErrorAction SilentlyContinue)) {
            throw 'Arena server exited before the scheduled reports completed'
        }
        Start-Sleep -Seconds 5
    }
    if([DateTime]::UtcNow -ge $deadline){throw 'Arena campaign exceeded its two-hour queue deadline'}
    while(Get-Process StarCraft -ErrorAction SilentlyContinue) {
        if([DateTime]::UtcNow -ge $deadline){throw 'StarCraft did not exit by the campaign deadline'}
        Start-Sleep -Seconds 5
    }
}
$activeRun=$null
try {
    if(Get-Process StarCraft -ErrorAction SilentlyContinue){throw 'Another StarCraft game is active'}
    foreach($profile in @('baseline','plus-one','plus-two')) {
        $run=Join-Path $rootPath $profile
        $activeRun=$run
        if(Test-Path -LiteralPath (Join-Path $run 'processes.json')) {
            throw "$profile already has a process owner; inspect its artifacts before recovery"
        }
        Publish "$profile-starting"
        & (Join-Path $repo 'scripts/start-arena.ps1') -Run $run
        if($LASTEXITCODE -ne 0){throw "$profile arena launcher failed"}
        Publish "$profile-running"
        Wait-Campaign $run $plan.games_per_condition
        $health=(& $python -m training.arena inspect $run|Out-String)|ConvertFrom-Json
        if($LASTEXITCODE -ne 0 -or @($health.structurally_valid).Count -ne $plan.games_per_condition -or
           @($health.excluded).Count){throw "$profile outcomes failed arena health review"}
        Stop-OwnedLaunchers $run
        Publish "$profile-complete" ("normal games="+@($health.structurally_valid).Count)
    }
    $report=Join-Path $rootPath 'pilot-report.json'
    & $python -m training.worker_outcome_review $planPath (Join-Path $rootPath 'baseline') `
        (Join-Path $rootPath 'plus-one') (Join-Path $rootPath 'plus-two') $report
    if($LASTEXITCODE -ne 0){throw 'Outcome review failed; preserve all training artifacts'}
    $result=Get-Content -LiteralPath $report -Raw|ConvertFrom-Json
    Publish 'complete' ("usable_training_outcomes="+$result.usable_training_outcomes+"; no promotion or extra games")
} catch {
    Publish 'failed' $_.Exception.Message
    if($activeRun -and (Test-Path -LiteralPath (Join-Path $activeRun 'processes.json')) -and
       -not (Get-Process StarCraft -ErrorAction SilentlyContinue)) {
        Stop-OwnedLaunchers $activeRun
    }
    throw
} finally {$lock.Dispose()}
