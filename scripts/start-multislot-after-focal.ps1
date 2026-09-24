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
$smoke = 'artifacts/replay-learning/whole-game-multislot-gpu-smoke-512x1-20260923'
$output = 'artifacts/replay-learning/whole-game-multislot-fit-160x3-width512-20260923'
$audit = 'artifacts/replay-learning/whole-game-multislot-cadence-validation8-20260923'
$smokeLog = 'build/robust-training-20260922/whole-game-multislot-smoke.log'
$fitLog = 'build/robust-training-20260922/whole-game-multislot-fit.log'
$auditLog = 'build/robust-training-20260922/whole-game-multislot-audit.log'

function Write-Status($stage, $detail, $code) {
    @{ stage = $stage; detail = $detail; exit_code = $code;
       recorded_utc = (Get-Date).ToUniversalTime().ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath $statusFile -Encoding utf8
}

try {
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
