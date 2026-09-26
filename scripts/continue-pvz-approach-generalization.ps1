param([string]$Root='build/pvz-approach-generalization-20260925')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repo
$rootPath=(Resolve-Path -LiteralPath $Root).Path
$plan=Get-Content -LiteralPath (Join-Path $rootPath 'comparison-plan.json') -Raw|ConvertFrom-Json
$python=Join-Path $repo 'build/model-venv/Scripts/python.exe'
$statusPath=Join-Path $rootPath 'comparison-status.json'
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
    if(Test-Path -LiteralPath (Join-Path $rootPath 'comparison-report.json')) {
        throw 'Comparison report exists; do not duplicate games'
    }
    if(Get-Process StarCraft -ErrorAction SilentlyContinue) {
        throw 'StarCraft already running; cannot share runtimes'
    }
    foreach($entry in $plan.sha256.PSObject.Properties) {
        Assert-Hash (Join-Path $rootPath $entry.Name) $entry.Value
    }
    Assert-Hash (Join-Path $repo 'training/pvz_approach_generalization_review.py') $plan.reviewer_sha256
    Assert-Hash (Join-Path $repo 'training/pvz_core_transition_review.py') $plan.trace_sha256
    Assert-Hash (Join-Path $repo 'training/pvz_approach_grace_screen.py') $plan.delayed_source_sha256
    Assert-Hash $PSCommandPath $plan.launcher_sha256
    foreach($condition in 'reference-a','reference-b','candidate') {
        $run=Join-Path $rootPath $condition
        if(Test-Path -LiteralPath (Join-Path $run 'processes.json')) {
            throw "$condition already has a process owner; inspect before recovery"
        }
        & $python -m training.arena verify $run | Out-Null
        if($LASTEXITCODE -ne 0){throw "Prepared input verification failed: $condition"}
    }
    $deadline=[DateTime]::UtcNow.AddHours(4)
    foreach($condition in 'reference-a','reference-b','candidate') {
        $run=Join-Path $rootPath $condition
        Publish "$condition-launching"
        & (Join-Path $repo 'scripts/start-arena.ps1') -Run $run
        if($LASTEXITCODE -ne 0){throw "Arena launcher failed: $condition"}
        Publish "$condition-running"
        $results=Join-Path $run 'server/results.jsonl'
        while([DateTime]::UtcNow -lt $deadline) {
            if(Test-Path -LiteralPath $results) {
                try {$rows=@(Get-Content -LiteralPath $results|Where-Object {$_}|ForEach-Object {$_|ConvertFrom-Json})}catch{$rows=@()}
                if($rows.Count -ge 2*$plan.games_per_arm){break}
            }
            $server=Get-Content -LiteralPath (Join-Path $run 'processes.json') -Raw|ConvertFrom-Json|
                Where-Object {$_.component -eq 'server'}
            if(-not (Get-Process -Id $server.pid -ErrorAction SilentlyContinue)) {
                throw "Arena server exited before scheduled reports completed: $condition"
            }
            Start-Sleep -Seconds 5
        }
        if([DateTime]::UtcNow -ge $deadline){throw "Comparison exceeded deadline: $condition"}
        while(Get-Process StarCraft -ErrorAction SilentlyContinue) {
            if([DateTime]::UtcNow -ge $deadline){throw "StarCraft did not exit: $condition"}
            Start-Sleep -Seconds 5
        }
        Stop-OwnedLaunchers $run
        Publish "$condition-complete"
    }
    Publish 'reviewing'
    & $python -m training.pvz_approach_generalization_review `
        (Join-Path $rootPath 'comparison-plan.json') (Join-Path $rootPath 'comparison-report.json')
    if($LASTEXITCODE -ne 0){throw 'Comparison review failed; preserve artifacts'}
    Publish 'complete'
} catch {
    Publish 'failed' $_.Exception.Message
    throw
} finally {$lock.Dispose()}
