param(
    [Parameter(Mandatory = $true)]
    [int]$FocalLauncherProcessId,
    [Parameter(Mandatory = $true)]
    [int]$ValidationLauncherProcessId
)

$ErrorActionPreference = 'Stop'
$workspaceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $workspaceRoot
$python = Join-Path $workspaceRoot 'build/model-venv/Scripts/python.exe'
$statusFile = Join-Path $workspaceRoot 'build/robust-training-20260922/whole-game-validation8-cadence-gate.status.json'
$focalStatus = Join-Path $workspaceRoot 'build/robust-training-20260922/whole-game-focal-160.status.json'
$validationStatus = Join-Path $workspaceRoot 'build/robust-training-20260922/whole-game-validation8.status.json'
$trainRelease = 'artifacts/replay-learning/whole-game-release-v32c-20260922'
$validationRelease = 'artifacts/replay-learning/whole-game-release-validation8-v32c-20260923'
$comparison = 'artifacts/replay-learning/whole-game-validation8-cadence-comparison-20260923.json'
$candidates = @(
    @{ Name = 'baseline'; Checkpoint = 'artifacts/replay-learning/whole-game-stream-fit-160x3-width512-20260922/teacher.pt' },
    @{ Name = 'structured'; Checkpoint = 'artifacts/replay-learning/whole-game-stream-fit-structured-160x3-width512-20260922/teacher.pt' },
    @{ Name = 'mixed'; Checkpoint = 'artifacts/replay-learning/whole-game-stream-fit-mixed-160x3-width512-20260922/teacher.pt' },
    @{ Name = 'conditional'; Checkpoint = 'artifacts/replay-learning/whole-game-stream-fit-conditional-160x3-width512-20260923/teacher.pt' },
    @{ Name = 'focal'; Checkpoint = 'artifacts/replay-learning/whole-game-stream-fit-focal-160x3-width512-20260923/teacher.pt' }
)

function Write-Status($stage, $detail, $code) {
    @{ stage = $stage; detail = $detail; exit_code = $code;
       recorded_utc = (Get-Date).ToUniversalTime().ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath $statusFile -Encoding utf8
}

try {
    Write-Status 'waiting_for_fits_and_validation' "Focal PID $FocalLauncherProcessId; validation PID $ValidationLauncherProcessId" $null
    foreach ($id in @($FocalLauncherProcessId, $ValidationLauncherProcessId)) {
        if (Get-Process -Id $id -ErrorAction SilentlyContinue) {
            Wait-Process -Id $id
        }
    }
    if ((Get-Content -LiteralPath $focalStatus -Raw | ConvertFrom-Json).stage -ne 'complete' -or
        (Get-Content -LiteralPath $validationStatus -Raw | ConvertFrom-Json).stage -ne 'complete') {
        throw 'Focal fit or expanded validation release did not complete.'
    }
    $namedReports = @()
    foreach ($candidate in $candidates) {
        if (-not (Test-Path -LiteralPath $candidate.Checkpoint)) {
            throw "Missing teacher checkpoint: $($candidate.Checkpoint)"
        }
        $output = "artifacts/replay-learning/whole-game-validation8-cadence-$($candidate.Name)-20260923"
        if (Test-Path -LiteralPath $output) {
            throw "Cadence audit already exists: $output"
        }
        Write-Status 'auditing' $candidate.Name $null
        & $python -m training.whole_game_cadence_action_audit `
            $candidate.Checkpoint $trainRelease $validationRelease $output `
            --games-per-matchup 8
        if ($LASTEXITCODE -ne 0) {
            throw "Cadence audit for $($candidate.Name) exited with code $LASTEXITCODE."
        }
        $namedReports += "$($candidate.Name)=$output/report.json"
    }
    if (Test-Path -LiteralPath $comparison) {
        throw "Cadence comparison already exists: $comparison"
    }
    Write-Status 'comparing' 'Identical 24-game validation cadence windows' $null
    & $python -m training.whole_game_cadence_compare $comparison @namedReports
    if ($LASTEXITCODE -ne 0) {
        throw "Cadence comparison exited with code $LASTEXITCODE."
    }
    Write-Status 'complete' $comparison 0
} catch {
    Write-Status 'failed' $_.Exception.Message 1
    Write-Error $_
    exit 1
}
