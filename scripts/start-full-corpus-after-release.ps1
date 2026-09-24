param(
    [Parameter(Mandatory = $true)]
    [int]$ExtractorProcessId,
    [Parameter(Mandatory = $true)]
    [int]$MultislotLauncherProcessId,
    [Parameter(Mandatory = $true)]
    [int]$OfflineGatesProcessId,
    [Parameter(Mandatory = $true)]
    [int]$CadenceAuditProcessId
)

$ErrorActionPreference = 'Stop'
$workspaceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $workspaceRoot
$python = Join-Path $workspaceRoot 'build/model-venv/Scripts/python.exe'
$release = 'artifacts/replay-learning/whole-game-release-v32d-20260923'
$validation = 'artifacts/replay-learning/whole-game-release-validation8-v32c-20260923'
$quality = 'artifacts/replay-learning/whole-game-quality-v2-v32d-20260923.json'
$smoke = 'artifacts/replay-learning/whole-game-multislot-full-smoke-20260923'
$output = 'artifacts/replay-learning/whole-game-multislot-full-2400x3-width512-20260923'
$audit = 'artifacts/replay-learning/whole-game-multislot-full-cadence-validation8-20260923'
$statusFile = 'build/robust-training-20260922/whole-game-multislot-full.status.json'
$lockPath = 'build/robust-training-20260922/whole-game-multislot-full.lock'
$smokeLog = 'build/robust-training-20260922/whole-game-multislot-full-smoke.log'
$fitLog = 'build/robust-training-20260922/whole-game-multislot-full-fit.log'
$auditLog = 'build/robust-training-20260922/whole-game-multislot-full-audit.log'

function Write-Status($stage, $detail, $code) {
    @{ stage = $stage; detail = $detail; exit_code = $code;
       recorded_utc = (Get-Date).ToUniversalTime().ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath $statusFile -Encoding utf8
}

function Test-OwnedProcess($processId, $marker) {
    $process = Get-CimInstance Win32_Process -Filter "ProcessId = $processId"
    return [bool]($process -and $process.CommandLine -like "*$marker*")
}

function Require-CompleteRelease($path) {
    if (-not (Test-Path -LiteralPath "$path/release.json")) { return $false }
    $record = Get-Content -LiteralPath "$path/release.json" -Raw | ConvertFrom-Json
    if ($record.complete -ne $true) { throw "Release is not complete: $path" }
    return $true
}

$lock = $null
try {
    $lock = [System.IO.File]::Open(
        (Join-Path $workspaceRoot $lockPath),
        [System.IO.FileMode]::OpenOrCreate,
        [System.IO.FileAccess]::ReadWrite,
        [System.IO.FileShare]::None)
    if (Test-Path -LiteralPath "$audit/report.json") {
        Write-Status 'complete' 'Full-corpus fit and disjoint causal audit already available' 0
        return
    }
    if (-not (Test-Path -LiteralPath $python)) { throw "GPU Python runtime missing: $python" }
    if (-not (Test-Path -LiteralPath $quality)) { throw "Quality index missing: $quality" }
    if (-not (Require-CompleteRelease $validation)) {
        throw "Expanded validation release is incomplete: $validation"
    }

    Write-Status 'waiting_for_release' 'Waiting for all 12,563 verified whole-game shards' $null
    while (-not (Require-CompleteRelease $release)) {
        if (Test-Path -LiteralPath "$release/progress.json") {
            $progress = Get-Content -LiteralPath "$release/progress.json" -Raw | ConvertFrom-Json
            if (@($progress.failures).Count -gt 0) {
                throw "Replay extraction has $(@($progress.failures).Count) failure(s)"
            }
        }
        if (-not (Test-OwnedProcess $ExtractorProcessId 'whole-game-release-v32d-20260923')) {
            throw 'Replay extraction stopped before writing a complete release receipt'
        }
        Start-Sleep -Seconds 30
    }

    Write-Status 'waiting_for_gpu_slot' 'Waiting for current fit, audit, and offline gates' $null
    while ((Test-OwnedProcess $MultislotLauncherProcessId 'start-multislot-after-focal.ps1') -or
           (Test-OwnedProcess $OfflineGatesProcessId 'start-multislot-offline-gates-after-fit.ps1') -or
           (Test-OwnedProcess $CadenceAuditProcessId 'start-validation8-cadence-gate.ps1')) {
        Start-Sleep -Seconds 30
    }

    $smokeArgs = @(
        '-m', 'training.whole_game_multislot_fit',
        '--release', $release, '--validation-release', $validation,
        '--quality-index', $quality, '--output', $smoke, '--device', 'cuda',
        '--games-per-matchup', '1', '--chunk-games-per-matchup', '1',
        '--validation-games-per-matchup', '1', '--epochs', '1',
        '--steps-per-group', '6', '--batch-size', '2', '--width', '512',
        '--mixture-components', '12', '--maximum-slots', '6',
        '--category-limit', '8', '--per-game-category-limit', '1',
        '--validation-limit-per-category', '1', '--encoding-cache-mb', '128')
    if (-not (Test-Path -LiteralPath "$smoke/report.json")) {
        if (Test-Path -LiteralPath $smoke) { throw "Incomplete smoke output exists: $smoke" }
        Write-Status 'gpu_smoke' 'Fresh six-slot fit on disjoint v32d training and validation shards' $null
        & $python @smokeArgs *>> $smokeLog
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath "$smoke/report.json")) {
            throw "Full-corpus GPU smoke failed; inspect $smokeLog"
        }
    }

    $fitArgs = @(
        '-m', 'training.whole_game_multislot_fit',
        '--release', $release, '--validation-release', $validation,
        '--quality-index', $quality, '--output', $output, '--device', 'cuda',
        '--games-per-matchup', '2400', '--chunk-games-per-matchup', '8',
        '--validation-games-per-matchup', '8', '--epochs', '1',
        '--steps-per-group', '256', '--batch-size', '8', '--width', '512',
        '--mixture-components', '12', '--maximum-slots', '6',
        '--category-limit', '64', '--per-game-category-limit', '2',
        '--validation-limit-per-category', '8', '--encoding-cache-mb', '256')
    if (-not (Test-Path -LiteralPath "$output/report.json")) {
        if (Test-Path -LiteralPath $output) {
            if (-not (Test-Path -LiteralPath "$output/run.json") -or
                -not (Test-Path -LiteralPath "$output/resume.pt")) {
                throw "Fit output cannot be resumed safely: $output"
            }
            $fitArgs += '--resume'
        }
        Write-Status 'fitting' 'Fresh six-slot GPU fit: 2,400 verified games per matchup' $null
        & $python @fitArgs *>> $fitLog
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath "$output/report.json")) {
            throw "Full-corpus fit failed or paused; inspect $fitLog"
        }
    }

    if (-not (Test-Path -LiteralPath "$audit/report.json")) {
        if (Test-Path -LiteralPath $audit) { throw "Incomplete causal audit exists: $audit" }
        Write-Status 'auditing' 'Free-running six-slot audit on 24 held-out games' $null
        & $python -m training.whole_game_multislot_audit `
            "$output/teacher.pt" $release $validation $audit `
            --games-per-matchup 8 --device cuda *>> $auditLog
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath "$audit/report.json")) {
            throw "Full-corpus causal audit failed; inspect $auditLog"
        }
    }
    Write-Status 'complete' 'Full-corpus fit and disjoint causal audit available; no model promoted' 0
} catch {
    Write-Status 'failed' $_.Exception.Message 1
    Write-Error $_
    exit 1
} finally {
    if ($lock) { $lock.Dispose() }
}
