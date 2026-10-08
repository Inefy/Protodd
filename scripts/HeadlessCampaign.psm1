Set-StrictMode -Version Latest

function Assert-HeadlessArenaClient {
    param([Parameter(Mandatory)][string]$ClientDirectory)

    foreach ($name in @('client.jar', 'start-owned-starcraft.ps1', 'stop-owned-starcraft.ps1',
                       'HeadlessStarCraft.psm1', 'HeadlessStarCraft.cs',
                       'run-headless-starcraft.ps1', 'MatchProcessOwnership.psm1')) {
        if (!(Test-Path -LiteralPath (Join-Path $ClientDirectory $name) -PathType Leaf)) {
            throw "Campaign requires a rebuilt headless client bundle; missing $name in $ClientDirectory"
        }
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive=[IO.Compression.ZipFile]::OpenRead((Join-Path $ClientDirectory 'client.jar'))
    try {
        $entry=$archive.GetEntry('client/ClientCommands.class')
        if (!$entry) { throw 'Campaign client has no compiled launch implementation' }
        $stream=$entry.Open()
        $bytes=[IO.MemoryStream]::new()
        try { $stream.CopyTo($bytes); $launch=[Text.Encoding]::ASCII.GetString($bytes.ToArray()) }
        finally { $stream.Dispose(); $bytes.Dispose() }
        if (!$launch.Contains('start-owned-starcraft.ps1') -or !$launch.Contains('-MonitorOwnedLock')) {
            throw 'Campaign client uses an older launch implementation; rebuild its headless client bundle'
        }
    } finally { $archive.Dispose() }
}

Export-ModuleMember -Function Assert-HeadlessArenaClient
