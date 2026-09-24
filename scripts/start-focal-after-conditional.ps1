param(
    [Parameter(Mandatory = $true)]
    [int]$ConditionalLauncherProcessId
)

$ErrorActionPreference = 'Stop'
$workspaceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $workspaceRoot
$python = Join-Path $workspaceRoot 'build/model-venv/Scripts/python.exe'
$statusFile = Join-Path $workspaceRoot 'build/robust-training-20260922/whole-game-focal-160.status.json'
$conditionalStatus = Join-Path $workspaceRoot 'build/robust-training-20260922/whole-game-conditional-160.status.json'
$output = 'artifacts/replay-learning/whole-game-stream-fit-focal-160x3-width512-20260923'

function Write-Status($stage, $detail, $code) {
    @{ stage = $stage; detail = $detail; exit_code = $code;
       recorded_utc = (Get-Date).ToUniversalTime().ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath $statusFile -Encoding utf8
}

try {
    Write-Status 'waiting_for_conditional_gpu_slot' "PID $ConditionalLauncherProcessId" $null
    if (Get-Process -Id $ConditionalLauncherProcessId -ErrorAction SilentlyContinue) {
        Wait-Process -Id $ConditionalLauncherProcessId
    }
    if (-not (Test-Path -LiteralPath $conditionalStatus) -or
        (Get-Content -LiteralPath $conditionalStatus -Raw | ConvertFrom-Json).stage -ne 'complete') {
        throw 'Conditional fit did not complete; focal run needs its validated GPU slot.'
    }
    if (Test-Path -LiteralPath $output) {
        throw "Focal output already exists: $output"
    }
    Write-Status 'fitting' 'Cadence focal loss on frozen 160x3 cohort and baseline initialization' $null
    & $python -m training.whole_game_stream_fit_focal `
        --release artifacts/replay-learning/whole-game-release-v32c-20260922 `
        --validation-release artifacts/replay-learning/whole-game-release-fullprefix-benchmark-v32-20260922 `
        --quality-index artifacts/replay-learning/whole-game-quality-v2-v32c-20260922.json `
        --validation-quality-index artifacts/replay-learning/whole-game-quality-v2-benchmark-v32-20260922.json `
        --output $output `
        --init-checkpoint artifacts/replay-learning/whole-game-fit-continuation-64x3-width512-action2-20260922/teacher.pt `
        --selection-from artifacts/replay-learning/whole-game-stream-fit-160x3-width512-20260922/run.json `
        --games-per-matchup 160 --chunk-games-per-matchup 8 --collect-workers 2 `
        --validation-games-per-matchup 1 --steps-per-chunk 256 --epochs 1 `
        --batch-size 16 --encoding-cache-mb 256 --width 512 --learning-rate 0.0001
    if ($LASTEXITCODE -ne 0) {
        throw "Focal fit exited with code $LASTEXITCODE."
    }
    Write-Status 'complete' 'Focal checkpoint and validation report available' 0
} catch {
    Write-Status 'failed' $_.Exception.Message 1
    Write-Error $_
    exit 1
}
