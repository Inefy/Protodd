param(
    [Parameter(Mandatory = $true)]
    [int]$SpatialLauncherProcessId
)

$ErrorActionPreference = 'Stop'
$workspaceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $workspaceRoot
$output = Join-Path $workspaceRoot 'artifacts/replay-learning/whole-game-stream-fit-conditional-160x3-width512-20260923'
$statusFile = Join-Path $workspaceRoot 'build/robust-training-20260922/whole-game-conditional-160.status.json'
$python = Join-Path $workspaceRoot 'build/model-venv/Scripts/python.exe'
$trainRelease = 'artifacts/replay-learning/whole-game-release-v32c-20260922'
$validationRelease = 'artifacts/replay-learning/whole-game-release-fullprefix-benchmark-v32-20260922'
$preFitAudit = 'artifacts/replay-learning/whole-game-stream-v32c-benchmark-pre-fit-audit-20261007'
$representationAudit = Join-Path $preFitAudit 'report.json'
$nativeParityReport = 'artifacts/goal-20261005/t108-target-representation-audit-20261007/native-parity/report.json'

function Write-Status($stage, $detail, $code) {
    @{ stage = $stage; detail = $detail; exit_code = $code;
       recorded_utc = (Get-Date).ToUniversalTime().ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath $statusFile -Encoding utf8
}

try {
    & (Join-Path $PSScriptRoot 'verify-whole-game-fit-preflight.ps1') `
        -TrainRelease $trainRelease -ValidationRelease $validationRelease `
        -AuditDirectory $preFitAudit -NativeParityReport $nativeParityReport
    Write-Status 'waiting_for_spatial_gpu_slot' "PID $SpatialLauncherProcessId" $null
    if (Get-Process -Id $SpatialLauncherProcessId -ErrorAction SilentlyContinue) {
        Wait-Process -Id $SpatialLauncherProcessId
    }
    if (Test-Path -LiteralPath $output) {
        throw 'Conditional output already exists; refusing to overwrite or resume implicitly.'
    }
    Write-Status 'fitting' 'Actor-conditioned mixed-objective fit on frozen 160x3 cohort' $null
    & $python -m training.whole_game_stream_fit_conditional `
        --release $trainRelease --validation-release $validationRelease `
        --representation-audit $representationAudit --native-parity-report $nativeParityReport `
        --quality-index artifacts/replay-learning/whole-game-quality-v2-v32c-20260922.json `
        --validation-quality-index artifacts/replay-learning/whole-game-quality-v2-benchmark-v32-20260922.json `
        --output artifacts/replay-learning/whole-game-stream-fit-conditional-160x3-width512-20260923 `
        --init-checkpoint artifacts/replay-learning/whole-game-fit-continuation-64x3-width512-action2-20260922/teacher.pt `
        --selection-from artifacts/replay-learning/whole-game-stream-fit-160x3-width512-20260922/run.json `
        --games-per-matchup 160 --chunk-games-per-matchup 8 --collect-workers 2 `
        --validation-games-per-matchup 1 --steps-per-chunk 256 --epochs 1 `
        --batch-size 16 --encoding-cache-mb 256 --width 512 --learning-rate 0.0001
    if ($LASTEXITCODE -ne 0) {
        throw "Conditional fit exited with code $LASTEXITCODE."
    }
    Write-Status 'complete' 'Conditional checkpoint and validation report available' 0
} catch {
    Write-Status 'failed' $_.Exception.Message 1
    Write-Error $_
    exit 1
}
