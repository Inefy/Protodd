param(
    [Parameter(Mandatory = $true)]
    [int]$StructuredLauncherProcessId
)

$ErrorActionPreference = 'Stop'
$workspaceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $workspaceRoot
$structured = Join-Path $workspaceRoot 'artifacts/replay-learning/whole-game-stream-fit-structured-160x3-width512-20260922'
$mixed = Join-Path $workspaceRoot 'artifacts/replay-learning/whole-game-stream-fit-mixed-160x3-width512-20260922'
$statusFile = Join-Path $workspaceRoot 'build/robust-training-20260922/whole-game-mixed-160.status.json'
$structuredStatus = Join-Path $workspaceRoot 'build/robust-training-20260922/whole-game-structured-160.status.json'
$python = Join-Path $workspaceRoot 'build/model-venv/Scripts/python.exe'

function Write-Status($stage, $detail, $code) {
    @{ stage = $stage; detail = $detail; exit_code = $code;
       recorded_utc = (Get-Date).ToUniversalTime().ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath $statusFile -Encoding utf8
}

try {
    Write-Status 'waiting_for_structured' "PID $StructuredLauncherProcessId" $null
    if (Get-Process -Id $StructuredLauncherProcessId -ErrorAction SilentlyContinue) {
        Wait-Process -Id $StructuredLauncherProcessId
    }
    if (-not (Test-Path -LiteralPath $structuredStatus) -or
        (Get-Content -LiteralPath $structuredStatus -Raw | ConvertFrom-Json).stage -ne 'complete' -or
        -not (Test-Path -LiteralPath (Join-Path $structured 'teacher.pt')) -or
        -not (Test-Path -LiteralPath (Join-Path $structured 'report.json'))) {
        throw 'Structured fit did not complete cleanly.'
    }
    if (Test-Path -LiteralPath $mixed) {
        throw 'Mixed output already exists; refusing to overwrite or resume implicitly.'
    }
    Write-Status 'fitting' 'Mixed action schedule on frozen 160x3 baseline cohort' $null
    & $python -m training.whole_game_stream_fit_mixed `
        --release artifacts/replay-learning/whole-game-release-v32c-20260922 `
        --validation-release artifacts/replay-learning/whole-game-release-fullprefix-benchmark-v32-20260922 `
        --quality-index artifacts/replay-learning/whole-game-quality-v2-v32c-20260922.json `
        --validation-quality-index artifacts/replay-learning/whole-game-quality-v2-benchmark-v32-20260922.json `
        --output artifacts/replay-learning/whole-game-stream-fit-mixed-160x3-width512-20260922 `
        --init-checkpoint artifacts/replay-learning/whole-game-fit-continuation-64x3-width512-action2-20260922/teacher.pt `
        --selection-from artifacts/replay-learning/whole-game-stream-fit-160x3-width512-20260922/run.json `
        --games-per-matchup 160 --chunk-games-per-matchup 8 --collect-workers 2 `
        --validation-games-per-matchup 1 --steps-per-chunk 256 --epochs 1 `
        --batch-size 16 --encoding-cache-mb 256 --width 512 --learning-rate 0.0001
    $fitExit = $LASTEXITCODE
    if ($fitExit -ne 0) {
        throw "Mixed fit exited with code $fitExit."
    }
    Write-Status 'complete' 'Teacher checkpoint and validation report available' 0
} catch {
    Write-Status 'failed' $_.Exception.Message 1
    Write-Error $_
    exit 1
}
