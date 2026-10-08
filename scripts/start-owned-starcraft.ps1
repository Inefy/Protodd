[CmdletBinding(DefaultParameterSetName='ExternalLock')]
param(
    [Parameter(Mandatory=$true)][string]$Runtime,
    [Parameter(Mandatory=$true,ParameterSetName='ExternalLock')][object]$RuntimeLock,
    [Parameter(Mandatory=$true,ParameterSetName='MonitorLock')][switch]$MonitorOwnedLock
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'HeadlessStarCraft.psm1') -Force
$runtimePath = (Resolve-Path -LiteralPath $Runtime).Path
$expectedLockPath = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $runtimePath) '.protodd-match.lock'))
if (!$MonitorOwnedLock -and ($null -eq $RuntimeLock.stream -or -not $RuntimeLock.stream.CanWrite -or
    [IO.Path]::GetFullPath([string]$RuntimeLock.path) -ine $expectedLockPath)) {
    throw 'The supplied runtime lock does not own the requested game runtime'
}
$injector = Join-Path $runtimePath 'injectory.x86.exe'
if (-not (Test-Path -LiteralPath $injector -PathType Leaf)) { throw 'Missing runtime injector' }
$launchTime = [DateTime]::UtcNow
$receiptPath = Join-Path $runtimePath ('headless-launch-' + [Guid]::NewGuid().ToString('N') + '.json')
$driver = Join-Path $PSScriptRoot 'run-headless-starcraft.ps1'
$shellPath = (Get-Command pwsh -ErrorAction Stop).Source
$driverArguments = @('-NoProfile','-File',
    ('"' + $driver + '"'), '-Runtime', ('"' + $runtimePath + '"'),
    '-ReceiptPath', ('"' + $receiptPath + '"'))
if ($MonitorOwnedLock) { $driverArguments += '-MonitorOwnedLock' }
$driverProcess = Start-Process -FilePath $shellPath -ArgumentList $driverArguments -WindowStyle Hidden -PassThru
$readyDeadline = [DateTime]::UtcNow.AddSeconds(15)
while(!(Test-Path -LiteralPath $receiptPath) -and [DateTime]::UtcNow -lt $readyDeadline) {
    if($driverProcess.HasExited) { throw 'Headless process monitor exited before launch readiness' }
    Start-Sleep -Milliseconds 100
}
if(!(Test-Path -LiteralPath $receiptPath)) { throw 'Headless launch readiness timed out' }
$headlessReceipt=Get-Content -Raw -LiteralPath $receiptPath | ConvertFrom-Json
$launcher = [pscustomobject]@{Id=[int]$headlessReceipt.launcher_pid}
$owned = @()
for ($attempt=0; $attempt -lt 50 -and $owned.Count -eq 0; ++$attempt) {
    $owned = @(Get-CimInstance Win32_Process -Filter "Name='StarCraft.exe'" | Where-Object {
        $_.ParentProcessId -eq $launcher.Id -and $_.CreationDate -and
        $_.CreationDate.ToUniversalTime() -ge $launchTime.AddSeconds(-1)
    } | ForEach-Object {
        [ordered]@{pid=$_.ProcessId; parent=$_.ParentProcessId; created=$_.CreationDate.ToUniversalTime().ToString('o')}
    })
    if ($owned.Count -eq 0) { Start-Sleep -Milliseconds 100 }
}
if ($owned.Count -eq 0) { throw 'Could not establish ownership of the launched StarCraft process' }
[ordered]@{runtime=$runtimePath; processes=$owned; headless=$true; desktop=$headlessReceipt.desktop;
           monitor_pid=$driverProcess.Id} | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath (Join-Path $runtimePath 'arena-owned-processes.json')
