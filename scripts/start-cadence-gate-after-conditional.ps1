param(
    [Parameter(Mandatory = $true)]
    [int]$ConditionalLauncherProcessId
)

$ErrorActionPreference = 'Stop'
$workspaceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $workspaceRoot
$statusFile = Join-Path $workspaceRoot 'build/robust-training-20260922/whole-game-cadence-gate.status.json'
$python = Join-Path $workspaceRoot 'build/model-venv/Scripts/python.exe'
$trainRelease = 'artifacts/replay-learning/whole-game-release-v32c-20260922'
$validationRelease = 'artifacts/replay-learning/whole-game-release-fullprefix-benchmark-v32-20260922'
$mixed = 'artifacts/replay-learning/whole-game-stream-fit-mixed-160x3-width512-20260922/teacher.pt'
$conditional = 'artifacts/replay-learning/whole-game-stream-fit-conditional-160x3-width512-20260923/teacher.pt'
$mixedAudit = 'artifacts/replay-learning/whole-game-cadence-audit-mixed-20260923'
$conditionalAudit = 'artifacts/replay-learning/whole-game-cadence-audit-conditional-20260923'
$comparison = 'artifacts/replay-learning/whole-game-cadence-comparison-four-fits-20260923.json'

function Write-Status($stage, $detail, $code) {
    @{ stage = $stage; detail = $detail; exit_code = $code;
       recorded_utc = (Get-Date).ToUniversalTime().ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath $statusFile -Encoding utf8
}

try {
    Write-Status 'waiting_for_conditional' "PID $ConditionalLauncherProcessId" $null
    if (Get-Process -Id $ConditionalLauncherProcessId -ErrorAction SilentlyContinue) {
        Wait-Process -Id $ConditionalLauncherProcessId
    }
    if (-not (Test-Path -LiteralPath $mixed) -or -not (Test-Path -LiteralPath $conditional)) {
        throw 'A queued teacher checkpoint is missing; cadence gate cannot compare both fits.'
    }
    foreach ($item in @(
        @{ Name = 'mixed'; Checkpoint = $mixed; Output = $mixedAudit },
        @{ Name = 'conditional'; Checkpoint = $conditional; Output = $conditionalAudit }
    )) {
        if (Test-Path -LiteralPath $item.Output) {
            throw "Cadence audit output already exists: $($item.Output)"
        }
        Write-Status 'auditing' $item.Name $null
        & $python -m training.whole_game_cadence_action_audit `
            $item.Checkpoint $trainRelease $validationRelease $item.Output
        if ($LASTEXITCODE -ne 0) {
            throw "Cadence audit for $($item.Name) exited with code $LASTEXITCODE."
        }
    }
    if (Test-Path -LiteralPath $comparison) {
        throw "Cadence comparison already exists: $comparison"
    }
    Write-Status 'comparing' 'Identical deployable-cadence windows' $null
    & $python -m training.whole_game_cadence_compare $comparison `
        'baseline=artifacts/replay-learning/whole-game-cadence-audit-baseline-20260923/report.json' `
        'structured=artifacts/replay-learning/whole-game-cadence-audit-structured-20260923/report.json' `
        "mixed=$mixedAudit/report.json" "conditional=$conditionalAudit/report.json"
    if ($LASTEXITCODE -ne 0) {
        throw "Cadence comparison exited with code $LASTEXITCODE."
    }
    Write-Status 'complete' $comparison 0
} catch {
    Write-Status 'failed' $_.Exception.Message 1
    Write-Error $_
    exit 1
}
