param([Parameter(Mandatory)][string]$Runtime,
      [Parameter(Mandatory)][string]$ReceiptPath,
      [switch]$MonitorOwnedLock)
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSScriptRoot 'HeadlessStarCraft.psm1') -Force
$monitorLock=$null
if ($MonitorOwnedLock) {
    Import-Module (Join-Path $PSScriptRoot 'MatchProcessOwnership.psm1') -Force
    # A tournament client cannot pass an in-process FileStream to this driver.
    # The driver holds its own runtime lock until its native process tree exits.
    $monitorLock=Enter-MatchRuntimeLock -RuntimeRoot $Runtime -Label 'headless-tournament-client'
}
try {
$request = Join-Path $Runtime 'headless-close-request.json'
if (Test-Path -LiteralPath $request) { Remove-Item -LiteralPath $request }
$launcher = Start-HeadlessStarCraftLauncher -Runtime $Runtime -InjectorName 'injectory.x86.exe'
[ordered]@{launcher_pid=$launcher.Id; monitor_pid=$PID; desktop=$launcher.Desktop; headless=$true} |
    ConvertTo-Json | Set-Content -LiteralPath $ReceiptPath
while(!$launcher.Monitor.Completed) {
    if(Test-Path -LiteralPath $request) {
        try {
            $close = Get-Content -Raw -LiteralPath $request | ConvertFrom-Json
            foreach($ownedId in $close.process_ids) { $launcher.Monitor.RequestClose([uint32]$ownedId) }
        } catch { Write-Warning "Could not read headless close request: $_" }
    }
    Start-Sleep -Milliseconds 100
}
} finally {
    if ($monitorLock) { Exit-MatchRuntimeLock -Lock $monitorLock }
}
