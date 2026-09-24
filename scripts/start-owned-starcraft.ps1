param([Parameter(Mandatory=$true)][string]$Runtime)
$ErrorActionPreference = 'Stop'
$runtimePath = (Resolve-Path -LiteralPath $Runtime).Path
$injector = Join-Path $runtimePath 'injectory.x86.exe'
if (-not (Test-Path -LiteralPath $injector -PathType Leaf)) { throw 'Missing runtime injector' }
$launchTime = [DateTime]::UtcNow
$launcher = Start-Process -FilePath $injector -ArgumentList '--launch','StarCraft.exe','--inject','bwapi-data\BWAPI.dll','--set-flags','SEM_NOGPFAULTERRORBOX' `
    -WorkingDirectory $runtimePath -WindowStyle Hidden -PassThru
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
[ordered]@{runtime=$runtimePath; processes=$owned} | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath (Join-Path $runtimePath 'arena-owned-processes.json')
