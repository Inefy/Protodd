param([string]$Root = 'build/pvz-gateway-opening-20260926')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repo
$rootPath = (Resolve-Path -LiteralPath $Root).Path
$plan = Get-Content -LiteralPath (Join-Path $rootPath 'pilot-plan.json') -Raw | ConvertFrom-Json
$python = Join-Path $repo 'build/model-venv/Scripts/python.exe'
$statusPath = Join-Path $rootPath 'pilot-status.json'
$lock = [IO.File]::Open($statusPath + '.lock', [IO.FileMode]::CreateNew,
    [IO.FileAccess]::Write, [IO.FileShare]::Read)

function Publish([string]$stage, [string]$detail = '') {
    [ordered]@{ stage = $stage; updated = [DateTime]::UtcNow.ToString('o');
        pid = $PID; detail = $detail; promotion_allowed = $false } |
        ConvertTo-Json | Set-Content -LiteralPath $statusPath
}

function Stop-OwnedLaunchers([string]$run) {
    foreach ($job in (Get-Content -LiteralPath (Join-Path $run 'processes.json') -Raw |
            ConvertFrom-Json)) {
        $process = Get-Process -Id $job.pid -ErrorAction SilentlyContinue
        $expected = ([DateTimeOffset]::Parse($job.started)).UtcDateTime
        if ($process -and $process.ProcessName -eq 'java' -and
            [Math]::Abs(($process.StartTime.ToUniversalTime() - $expected).TotalSeconds) -lt 1) {
            Stop-Process -Id $job.pid -Force
        }
    }
}

function Wait-Campaign([string]$run, [int]$games) {
    $deadline = [DateTime]::UtcNow.AddHours(2)
    $results = Join-Path $run 'server/results.jsonl'
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-Path -LiteralPath $results) {
            try {
                $rows = @(Get-Content -LiteralPath $results |
                    Where-Object { $_ } | ForEach-Object { $_ | ConvertFrom-Json })
            } catch { $rows = @() }
            if ($rows.Count -ge 2 * $games) { break }
        }
        $server = Get-Content -LiteralPath (Join-Path $run 'processes.json') -Raw |
            ConvertFrom-Json | Where-Object { $_.component -eq 'server' }
        if (-not (Get-Process -Id $server.pid -ErrorAction SilentlyContinue)) {
            throw 'Arena server exited before scheduled reports completed'
        }
        Start-Sleep -Seconds 5
    }
    if ([DateTime]::UtcNow -ge $deadline) { throw 'Campaign exceeded two-hour deadline' }
    while (Get-Process StarCraft -ErrorAction SilentlyContinue) {
        if ([DateTime]::UtcNow -ge $deadline) { throw 'StarCraft did not exit by deadline' }
        Start-Sleep -Seconds 5
    }
}

$activeRun = Join-Path $rootPath 'reference'
try {
    if (Test-Path -LiteralPath (Join-Path $rootPath 'pilot-report.json')) {
        throw 'This pilot already has a report'
    }
    foreach ($condition in 'reference', 'candidate') {
        $run = Join-Path $rootPath $condition
        & $python -m training.arena verify $run | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "$condition input verification failed" }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $activeRun 'processes.json'))) {
        throw 'Reference arena has no recorded owner'
    }
    Publish 'reference-running'
    Wait-Campaign $activeRun $plan.games_per_condition
    $health = (& $python -m training.arena inspect $activeRun | Out-String) | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or @($health.structurally_valid).Count -ne $plan.games_per_condition -or
        @($health.excluded).Count) { throw 'Reference games failed normal health gate' }
    Stop-OwnedLaunchers $activeRun
    Publish 'reference-complete'

    $activeRun = Join-Path $rootPath 'candidate'
    if (Test-Path -LiteralPath (Join-Path $activeRun 'processes.json')) {
        throw 'Candidate already has a process owner'
    }
    Publish 'candidate-launching'
    & (Join-Path $repo 'scripts/start-arena.ps1') -Run $activeRun
    if ($LASTEXITCODE -ne 0) { throw 'Candidate arena launcher failed' }
    Publish 'candidate-running'
    Wait-Campaign $activeRun $plan.games_per_condition
    $health = (& $python -m training.arena inspect $activeRun | Out-String) | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or @($health.structurally_valid).Count -ne $plan.games_per_condition -or
        @($health.excluded).Count) { throw 'Candidate games failed normal health gate' }
    Stop-OwnedLaunchers $activeRun
    Publish 'candidate-complete'

    & $python -m training.pvz_gateway_opening_review `
        (Join-Path $rootPath 'pilot-plan.json') `
        (Join-Path $rootPath 'reference') `
        (Join-Path $rootPath 'candidate') `
        (Join-Path $rootPath 'pilot-report.json')
    if ($LASTEXITCODE -ne 0) { throw 'Pilot review failed' }
    $report = Get-Content -LiteralPath (Join-Path $rootPath 'pilot-report.json') -Raw |
        ConvertFrom-Json
    Publish 'complete' ("functional_pass=" + $report.functional_pass +
        '; larger_campaign=' + $report.larger_campaign)
} catch {
    Publish 'failed' $_.Exception.Message
    if ($activeRun -and (Test-Path -LiteralPath (Join-Path $activeRun 'processes.json')) -and
        -not (Get-Process StarCraft -ErrorAction SilentlyContinue)) {
        Stop-OwnedLaunchers $activeRun
    }
    throw
} finally {
    $lock.Dispose()
}
