Set-StrictMode -Version Latest

function Start-HeadlessStarCraftLauncher {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Runtime,
          [string]$InjectorName = 'injectory_x86.exe',
          [switch]$LegacyWindowPlugin)
    if (-not ('Protodd.Runtime.HeadlessStarCraft' -as [type])) {
        Add-Type -Path (Join-Path $PSScriptRoot 'HeadlessStarCraft.cs')
    }
    $root = [IO.Path]::GetFullPath($Runtime)
    $injector = Join-Path $root $InjectorName
    if (-not (Test-Path -LiteralPath $injector -PathType Leaf)) { throw "Missing injector: $injector" }
    $arguments = '--launch StarCraft.exe --inject bwapi-data\BWAPI.dll'
    if ($LegacyWindowPlugin) { $arguments += ' wmode.dll' }
    $arguments += ' --set-flags SEM_NOGPFAULTERRORBOX SEM_FAILCRITICALERRORS'
    $monitor = [Protodd.Runtime.HeadlessStarCraft]::Launch($injector, $arguments, $root, (Join-Path $root 'Errors'))
    [pscustomobject]@{Id=[int]$monitor.ProcessId; Headless=$true; Desktop=$monitor.DesktopName; Monitor=$monitor}
}

Export-ModuleMember -Function Start-HeadlessStarCraftLauncher
