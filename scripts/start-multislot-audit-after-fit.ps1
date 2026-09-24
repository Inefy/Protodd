param(
    [Parameter(Mandatory = $true)]
    [int]$FitLauncherProcessId
)

$ErrorActionPreference = 'Stop'
$workspaceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $workspaceRoot
$python = Join-Path $workspaceRoot 'build/model-venv/Scripts/python.exe'
$fitStatus = 'build/robust-training-20260922/whole-game-multislot-160.status.json'
$statusFile = 'build/robust-training-20260922/whole-game-multislot-cadence-audit.status.json'
$teacher = 'artifacts/replay-learning/whole-game-multislot-fit-160x3-width512-20260923/teacher.pt'
$trainRelease = 'artifacts/replay-learning/whole-game-release-v32c-20260922'
$validationRelease = 'artifacts/replay-learning/whole-game-release-validation8-v32c-20260923'
$output = 'artifacts/replay-learning/whole-game-multislot-cadence-validation8-20260923'
$log = 'build/robust-training-20260922/whole-game-multislot-audit.log'

function Write-Status($stage, $detail, $code) {
    @{ stage = $stage; detail = $detail; exit_code = $code;
       recorded_utc = (Get-Date).ToUniversalTime().ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath $statusFile -Encoding utf8
}

try {
    Write-Status 'waiting_for_multislot_fit' "PID $FitLauncherProcessId" $null
    if (Get-Process -Id $FitLauncherProcessId -ErrorAction SilentlyContinue) {
        Wait-Process -Id $FitLauncherProcessId
    }
    if (-not (Test-Path -LiteralPath $fitStatus) -or
        (Get-Content -LiteralPath $fitStatus -Raw | ConvertFrom-Json).stage -ne 'complete' -or
        -not (Test-Path -LiteralPath $teacher)) {
        throw 'Multi-slot fit did not complete with a teacher checkpoint.'
    }
    if (Test-Path -LiteralPath "$output/report.json") {
        Write-Status 'complete' 'Free-running validation audit already available' 0
        return
    }
    if (Test-Path -LiteralPath $output) {
        throw "Incomplete audit output already exists: $output"
    }
    Write-Status 'auditing' 'Free-running causal six-slot audit on 24 held-out games' $null
    & $python -m training.whole_game_multislot_audit `
        $teacher $trainRelease $validationRelease $output `
        --games-per-matchup 8 --device cuda *> $log
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath "$output/report.json")) {
        throw "Multi-slot cadence audit failed; inspect $log"
    }
    Write-Status 'complete' 'Free-running six-slot validation audit available' 0
} catch {
    Write-Status 'failed' $_.Exception.Message 1
    Write-Error $_
    exit 1
}
