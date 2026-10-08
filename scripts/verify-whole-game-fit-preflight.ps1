param(
    [Parameter(Mandatory = $true)][string]$TrainRelease,
    [Parameter(Mandatory = $true)][string]$ValidationRelease,
    [Parameter(Mandatory = $true)][string]$AuditDirectory,
    [string]$NativeParityReport = 'artifacts/goal-20261005/t108-target-representation-audit-20261007/native-parity/report.json'
)

$ErrorActionPreference = 'Stop'
$workspaceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $workspaceRoot
$python = Join-Path $workspaceRoot 'build/model-venv/Scripts/python.exe'
$auditReport = Join-Path $AuditDirectory 'report.json'
$gateLog = Join-Path $AuditDirectory 'pre-fit-gate.log'

if (-not (Test-Path -LiteralPath $python -PathType Leaf)) {
    throw "Model Python runtime is missing: $python"
}
foreach ($release in @($TrainRelease, $ValidationRelease)) {
    if (-not (Test-Path -LiteralPath (Join-Path $workspaceRoot $release) -PathType Container)) {
        throw "Pre-fit release is missing: $release"
    }
}

if (-not (Test-Path -LiteralPath $auditReport -PathType Leaf)) {
    if (Test-Path -LiteralPath $AuditDirectory) {
        throw "Incomplete pre-fit audit directory exists: $AuditDirectory"
    }
    New-Item -ItemType Directory -Path $AuditDirectory | Out-Null
    & $python -m training.whole_game_representation_audit `
        $TrainRelease $ValidationRelease --games-per-matchup 2 --maximum-frame 3600 `
        --output $auditReport *> $gateLog
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $auditReport -PathType Leaf)) {
        throw "Representation audit failed before fit; inspect $gateLog"
    }
}

& $python -m training.whole_game_fit_gate `
    $TrainRelease $ValidationRelease $auditReport $NativeParityReport *>> $gateLog
if ($LASTEXITCODE -ne 0) {
    throw "Pre-fit gate rejected the requested fit; no optimizer will run. Inspect $gateLog"
}
Write-Output "Pre-fit gate passed for $TrainRelease and $ValidationRelease."
