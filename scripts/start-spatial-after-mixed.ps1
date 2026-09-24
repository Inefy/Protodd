param(
    [Parameter(Mandatory = $true)]
    [int]$MixedLauncherProcessId
)

$ErrorActionPreference = 'Stop'
$workspaceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $workspaceRoot
$mixed = Join-Path $workspaceRoot 'artifacts/replay-learning/whole-game-stream-fit-mixed-160x3-width512-20260922'
$mixedStatus = Join-Path $workspaceRoot 'build/robust-training-20260922/whole-game-mixed-160.status.json'
$output = Join-Path $workspaceRoot 'artifacts/replay-learning/whole-game-spatial-stream-160x3-20260922'
$statusFile = Join-Path $workspaceRoot 'build/robust-training-20260922/whole-game-spatial-160.status.json'
$python = Join-Path $workspaceRoot 'build/model-venv/Scripts/python.exe'

function Write-Status($stage, $detail, $code) {
    @{ stage = $stage; detail = $detail; exit_code = $code;
       recorded_utc = (Get-Date).ToUniversalTime().ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath $statusFile -Encoding utf8
}

try {
    Write-Status 'waiting_for_mixed' "PID $MixedLauncherProcessId" $null
    if (Get-Process -Id $MixedLauncherProcessId -ErrorAction SilentlyContinue) {
        Wait-Process -Id $MixedLauncherProcessId
    }
    if (-not (Test-Path -LiteralPath $mixedStatus) -or
        (Get-Content -LiteralPath $mixedStatus -Raw | ConvertFrom-Json).stage -ne 'complete' -or
        -not (Test-Path -LiteralPath (Join-Path $mixed 'teacher.pt')) -or
        -not (Test-Path -LiteralPath (Join-Path $mixed 'report.json'))) {
        throw 'Mixed fit did not complete cleanly.'
    }
    if (Test-Path -LiteralPath $output) {
        throw 'Spatial output already exists; refusing to overwrite or resume implicitly.'
    }
    Write-Status 'fitting' 'Streaming spatial head on frozen 160x3 training cohort' $null
    & $python -m training.whole_game_spatial_stream_fit `
        --teacher artifacts/replay-learning/whole-game-stream-fit-160x3-width512-20260922/teacher.pt `
        --release artifacts/replay-learning/whole-game-release-v32c-20260922 `
        --validation-release artifacts/replay-learning/whole-game-release-fullprefix-benchmark-v32-20260922 `
        --quality-index artifacts/replay-learning/whole-game-quality-v2-v32c-20260922.json `
        --validation-quality-index artifacts/replay-learning/whole-game-quality-v2-benchmark-v32-20260922.json `
        --selection artifacts/replay-learning/whole-game-stream-fit-160x3-width512-20260922/run.json `
        --output artifacts/replay-learning/whole-game-spatial-stream-160x3-20260922 `
        --steps-per-group 128 --batch-size 32 --learning-rate 0.0001
    if ($LASTEXITCODE -ne 0) {
        throw "Spatial fit exited with code $LASTEXITCODE."
    }
    Write-Status 'complete' 'Spatial checkpoint and disjoint benchmark report available' 0
} catch {
    Write-Status 'failed' $_.Exception.Message 1
    Write-Error $_
    exit 1
}
