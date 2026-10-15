Set-StrictMode -Version Latest

function Assert-HeadlessArenaClient {
    param([Parameter(Mandatory)][string]$ClientDirectory)

    foreach ($name in @('client.jar', 'start-owned-starcraft.ps1', 'stop-owned-starcraft.ps1',
                       'ConsoleFreeOwnedStarCraft.exe', 'ConsoleFreeOwnedStarCraft.cs', 'build.json',
                       'HeadlessStarCraft.psm1', 'HeadlessStarCraft.cs',
                       'run-headless-starcraft.ps1', 'MatchProcessOwnership.psm1')) {
        if (!(Test-Path -LiteralPath (Join-Path $ClientDirectory $name) -PathType Leaf)) {
            throw "Campaign requires a rebuilt headless client bundle; missing $name in $ClientDirectory"
        }
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $clientBuild = Get-Content -Raw -LiteralPath (Join-Path $ClientDirectory 'build.json') | ConvertFrom-Json
    $launcherPath = Join-Path $ClientDirectory 'ConsoleFreeOwnedStarCraft.exe'
    if ((Get-FileHash -LiteralPath $launcherPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne
        $clientBuild.outputs.'ConsoleFreeOwnedStarCraft.exe') {
        throw 'Campaign client console-free launcher does not match its build receipt'
    }
    if ((Get-FileHash -LiteralPath (Join-Path $ClientDirectory 'ConsoleFreeOwnedStarCraft.cs') -Algorithm SHA256).Hash.ToLowerInvariant() -ne
        $clientBuild.outputs.'ConsoleFreeOwnedStarCraft.cs') {
        throw 'Campaign client console-free source does not match its build receipt'
    }
    $image = [IO.File]::ReadAllBytes($launcherPath)
    $pe = [BitConverter]::ToInt32($image, 0x3c)
    if ([BitConverter]::ToUInt16($image, $pe + 24 + 68) -ne 2) {
        throw 'Campaign client launcher is not a console-free Windows application'
    }
    $archive=[IO.Compression.ZipFile]::OpenRead((Join-Path $ClientDirectory 'client.jar'))
    try {
        $entry=$archive.GetEntry('client/ClientCommands.class')
        if (!$entry) { throw 'Campaign client has no compiled launch implementation' }
        $stream=$entry.Open()
        $bytes=[IO.MemoryStream]::new()
        try { $stream.CopyTo($bytes); $launch=[Text.Encoding]::ASCII.GetString($bytes.ToArray()) }
        finally { $stream.Dispose(); $bytes.Dispose() }
        if (!$launch.Contains('ConsoleFreeOwnedStarCraft.exe') -or $launch.Contains('powershell.exe')) {
            throw 'Campaign client uses an older launch implementation; rebuild its headless client bundle'
        }
    } finally { $archive.Dispose() }
    # A rebuilt JAR can still be accompanied by old helper files. Require the
    # current owned-process monitor, including its debug handle lifetime fix.
    foreach ($name in @('start-owned-starcraft.ps1', 'stop-owned-starcraft.ps1',
                       'ConsoleFreeOwnedStarCraft.cs',
                       'HeadlessStarCraft.psm1', 'HeadlessStarCraft.cs',
                       'run-headless-starcraft.ps1', 'MatchProcessOwnership.psm1')) {
        $expected = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $name) -Algorithm SHA256).Hash
        $actual = (Get-FileHash -LiteralPath (Join-Path $ClientDirectory $name) -Algorithm SHA256).Hash
        if ($actual -ne $expected) {
            throw "Campaign client contains a stale headless helper ($name); rebuild its headless client bundle"
        }
    }
}

Export-ModuleMember -Function Assert-HeadlessArenaClient
