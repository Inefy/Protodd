param(
    [Parameter(Mandatory = $true)]
    [int]$FitLauncherProcessId
)

$ErrorActionPreference = 'Stop'
$workspaceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $workspaceRoot
$python = Join-Path $workspaceRoot 'build/model-venv/Scripts/python.exe'
$probe = Join-Path $workspaceRoot 'build/whole-game-cpu-probe-win32/Release/whole_game_cpu_probe.exe'
$fitStatus = 'build/robust-training-20260922/whole-game-multislot-160.status.json'
$statusFile = 'build/robust-training-20260922/whole-game-multislot-offline-gates.status.json'
$teacher = 'artifacts/replay-learning/whole-game-multislot-fit-160x3-width512-20260923/teacher.pt'
$audit = 'artifacts/replay-learning/whole-game-multislot-cadence-validation8-20260923/report.json'
$validation = 'artifacts/replay-learning/whole-game-release-validation8-v32c-20260923'
$root = 'artifacts/replay-learning/whole-game-multislot-offline-gates-20260923'
$package = Join-Path $root 'package'
$early = Join-Path $root 'parity-early'
$mid = Join-Path $root 'parity-mid'
$late = Join-Path $root 'parity-late'
$benchmark = Join-Path $root 'win32-lategame-benchmark.json'
$log = 'build/robust-training-20260922/whole-game-multislot-offline-gates.log'

function Write-Status($stage, $detail, $code) {
    @{ stage = $stage; detail = $detail; exit_code = $code;
       recorded_utc = (Get-Date).ToUniversalTime().ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath $statusFile -Encoding utf8
}

try {
    Write-Status 'waiting_for_fit' "PID $FitLauncherProcessId" $null
    if (Get-Process -Id $FitLauncherProcessId -ErrorAction SilentlyContinue) {
        Wait-Process -Id $FitLauncherProcessId
    }
    if (-not (Test-Path -LiteralPath $fitStatus) -or
        (Get-Content -LiteralPath $fitStatus -Raw | ConvertFrom-Json).stage -ne 'complete' -or
        -not (Test-Path -LiteralPath $teacher)) {
        throw 'Six-slot fit did not complete.'
    }
    # The disjoint causal audit may be running in a separate launcher after
    # the fit exits. Give it time to commit its report before evaluating gates.
    $auditDeadline = (Get-Date).AddMinutes(15)
    while (-not (Test-Path -LiteralPath $audit)) {
        if ((Get-Date) -ge $auditDeadline) {
            throw 'Six-slot causal audit did not commit within 15 minutes.'
        }
        Write-Status 'waiting_for_audit' 'Waiting for committed causal audit report' $null
        Start-Sleep -Seconds 15
    }
    $auditReport = Get-Content -LiteralPath $audit -Raw | ConvertFrom-Json
    if ($auditReport.schema -ne 'protodd-whole-game-multislot-audit-v1' -or
        $auditReport.overall.early_mass_probe_move_patterns -ne 0) {
        throw 'Causal audit is invalid or predicts repeated early mass Probe moves.'
    }
    if (Test-Path -LiteralPath $root) {
        throw "Offline gate output already exists: $root"
    }
    if (-not (Test-Path -LiteralPath $probe) -or
        -not (Test-Path -LiteralPath "$validation/release.json")) {
        throw 'Win32 probe or expanded validation release is missing.'
    }
    New-Item -ItemType Directory -Path $root | Out-Null
    Write-Status 'exporting' 'Frozen six-slot checkpoint to deterministic float32 package' $null
    & $python -m training.whole_game_multislot_export $teacher $package *> $log
    if ($LASTEXITCODE -ne 0) { throw "Six-slot export failed; inspect $log" }

    foreach ($stage in @(
        @{ Name = 'early'; Frame = 0; Rows = 3; Output = $early },
        @{ Name = 'mid'; Frame = 3000; Rows = 3; Output = $mid },
        @{ Name = 'late'; Frame = 9000; Rows = 1; Output = $late }
    )) {
        Write-Status "parity_$($stage.Name)" "Win32 causal replay parity from frame $($stage.Frame)" $null
        & $python -m training.whole_game_multislot_cpu_parity `
            $package $teacher $probe $validation $stage.Output `
            --max-rows $stage.Rows --min-frame $stage.Frame *>> $log
        if ($LASTEXITCODE -ne 0 -or
            -not (Test-Path -LiteralPath "$($stage.Output)/report.json")) {
            throw "$($stage.Name) Win32 parity failed; inspect $log"
        }
    }

    $lateReport = Get-Content -LiteralPath "$late/report.json" -Raw | ConvertFrom-Json
    $fixture = Join-Path $late 'input-0.bin'
    if ($lateReport.cases[0].entities -lt 100) {
        # This is a frozen 124-entity legal replay observation. The timing
        # report pins its hash; game-state parity uses the candidate fixtures.
        $fixture = 'build/robust-training-20260922/multislot-cpu-parity-width512-smoke/parity-win32-lategame/input-0.bin'
    }
    Write-Status 'benchmarking' 'Twenty Win32 late-game forward passes' $null
    & $python -m training.whole_game_win32_benchmark `
        $package $probe $fixture $benchmark --runs 20 *>> $log
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $benchmark)) {
        throw "Win32 benchmark failed; inspect $log"
    }
    Write-Status 'complete' $root 0
} catch {
    Write-Status 'failed' $_.Exception.Message 1
    Write-Error $_
    exit 1
}
