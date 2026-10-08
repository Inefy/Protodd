param(
    [Parameter(Mandatory = $true)]
    [int]$FocalLauncherProcessId
)

$ErrorActionPreference = 'Stop'
$workspaceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $workspaceRoot
$python = Join-Path $workspaceRoot 'build/model-venv/Scripts/python.exe'
$statusFile = Join-Path $workspaceRoot 'build/robust-training-20260922/whole-game-multislot-160.status.json'
$focalStatus = Join-Path $workspaceRoot 'build/robust-training-20260922/whole-game-focal-160.status.json'
$trainRelease = 'artifacts/replay-learning/whole-game-release-v32c-20260922'
$validationRelease = 'artifacts/replay-learning/whole-game-release-validation8-v32c-20260923'
$quality = 'artifacts/replay-learning/whole-game-quality-v2-v32c-20260922.json'
$baseline = 'artifacts/replay-learning/whole-game-stream-fit-160x3-width512-20260922/teacher.pt'
$selection = 'artifacts/replay-learning/whole-game-stream-fit-160x3-width512-20260922/run.json'
$representationAuditDir = 'artifacts/replay-learning/whole-game-multislot-pre-fit-audit-v32c-20261007'
$representationAudit = Join-Path $representationAuditDir 'report.json'
$nativeParityReport = 'artifacts/goal-20261005/t108-target-representation-audit-20261007/native-parity/report.json'
$smoke = 'artifacts/replay-learning/whole-game-multislot-gpu-smoke-512x1-20260923'
$output = 'artifacts/replay-learning/whole-game-multislot-fit-160x3-width512-20260923'
$audit = 'artifacts/replay-learning/whole-game-multislot-cadence-validation8-20260923'
$smokeLog = 'build/robust-training-20260922/whole-game-multislot-smoke.log'
$fitLog = 'build/robust-training-20260922/whole-game-multislot-fit.log'
$auditLog = 'build/robust-training-20260922/whole-game-multislot-audit.log'
$fitGateLog = 'build/robust-training-20260922/whole-game-multislot-pre-fit-gate.log'

function Write-Status($stage, $detail, $code) {
    @{ stage = $stage; detail = $detail; exit_code = $code;
       recorded_utc = (Get-Date).ToUniversalTime().ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath $statusFile -Encoding utf8
}

try {
    foreach ($path in @($baseline, $selection, $quality,
                        "$trainRelease/release.json", "$validationRelease/release.json")) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Required pre-fit input is missing: $path"
        }
    }
    if ((Get-Content -LiteralPath "$trainRelease/release.json" -Raw |
            ConvertFrom-Json).complete -ne $true -or
        (Get-Content -LiteralPath "$validationRelease/release.json" -Raw |
            ConvertFrom-Json).complete -ne $true) {
        throw 'Training or validation release is incomplete.'
    }
    if (-not (Test-Path -LiteralPath $representationAudit)) {
        if (Test-Path -LiteralPath $representationAuditDir) {
            throw "Incomplete pre-fit representation audit exists: $representationAuditDir"
        }
        New-Item -ItemType Directory -Path $representationAuditDir | Out-Null
        Write-Status 'pre_fit_representation_audit' 'Bounded label-preserving audit for the exact fit releases' $null
        & $python -m training.whole_game_representation_audit `
            $trainRelease $validationRelease --games-per-matchup 2 --maximum-frame 3600 `
            --output $representationAudit *> $fitGateLog
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $representationAudit)) {
            throw "Pre-fit representation audit failed; inspect $fitGateLog"
        }
    }
    Write-Status 'pre_fit_gate' 'Checking release readiness, split provenance, representation metrics and native parity' $null
    & $python -m training.whole_game_fit_gate `
        $trainRelease $validationRelease $representationAudit $nativeParityReport *>> $fitGateLog
    if ($LASTEXITCODE -ne 0) {
        throw "Pre-fit gate failed; no optimizer will run. Inspect $fitGateLog"
    }

    Write-Status 'waiting_for_focal_gpu_slot' "PID $FocalLauncherProcessId" $null
    if (Get-Process -Id $FocalLauncherProcessId -ErrorAction SilentlyContinue) {
        Wait-Process -Id $FocalLauncherProcessId
    }
    if (-not (Test-Path -LiteralPath $focalStatus) -or
        (Get-Content -LiteralPath $focalStatus -Raw | ConvertFrom-Json).stage -ne 'complete') {
        throw 'Focal run did not complete; multi-slot GPU fit will not take its slot.'
    }
    foreach ($path in @($smoke, $output, $audit)) {
        if (Test-Path -LiteralPath $path) {
            throw "Multi-slot output already exists: $path"
        }
    }
    foreach ($path in @($baseline, $selection, $quality,
                        "$validationRelease/release.json")) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Required input is missing: $path"
        }
    }
    if ((Get-Content -LiteralPath "$validationRelease/release.json" -Raw |
            ConvertFrom-Json).complete -ne $true) {
        throw 'Expanded validation release is incomplete.'
    }

    Write-Status 'gpu_smoke' 'Width-512 causal six-slot fit on one train game per matchup' $null
    & $python -m training.whole_game_multislot_fit `
        --release $trainRelease --validation-release $validationRelease `
        --representation-audit $representationAudit --native-parity-report $nativeParityReport `
        --quality-index $quality --init-checkpoint $baseline `
        --output $smoke --device cuda --games-per-matchup 1 `
        --chunk-games-per-matchup 1 --validation-games-per-matchup 1 `
        --epochs 1 --steps-per-group 6 --batch-size 2 `
        --width 512 --mixture-components 12 --maximum-slots 6 `
        --category-limit 8 --per-game-category-limit 1 `
        --validation-limit-per-category 1 --encoding-cache-mb 128 `
        *> $smokeLog
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath "$smoke/report.json")) {
        throw "Multi-slot GPU smoke failed; inspect $smokeLog"
    }

    Write-Status 'fitting' 'Streaming six-slot GPU teacher on frozen 160x3 replay cohort' $null
    & $python -m training.whole_game_multislot_fit `
        --release $trainRelease --validation-release $validationRelease `
        --representation-audit $representationAudit --native-parity-report $nativeParityReport `
        --quality-index $quality --init-checkpoint $baseline `
        --selection-from $selection --output $output --device cuda `
        --games-per-matchup 160 --chunk-games-per-matchup 8 `
        --validation-games-per-matchup 8 --epochs 1 --steps-per-group 256 `
        --batch-size 8 --width 512 --mixture-components 12 --maximum-slots 6 `
        --category-limit 64 --per-game-category-limit 2 `
        --validation-limit-per-category 8 --encoding-cache-mb 256 `
        *> $fitLog
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath "$output/report.json")) {
        throw "Multi-slot GPU fit failed; inspect $fitLog"
    }
    Write-Status 'auditing' 'Free-running causal six-slot audit on 24 held-out games' $null
    & $python -m training.whole_game_multislot_audit `
        "$output/teacher.pt" $trainRelease $validationRelease $audit `
        --games-per-matchup 8 --device cuda *> $auditLog
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath "$audit/report.json")) {
        throw "Multi-slot cadence audit failed; inspect $auditLog"
    }
    Write-Status 'complete' 'Six-slot teacher and free-running validation audit available' 0
} catch {
    Write-Status 'failed' $_.Exception.Message 1
    Write-Error $_
    exit 1
}
