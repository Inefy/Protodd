param([Parameter(Mandatory=$true)][string]$Runtime)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$buildPrefix = [System.IO.Path]::GetFullPath((Join-Path $repo 'build')).TrimEnd('\') + '\'
$runtimePath = [System.IO.Path]::GetFullPath($Runtime)
if (-not $runtimePath.StartsWith($buildPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Match runtime must be inside the build workspace'
}
if (Get-Process StarCraft -ErrorAction SilentlyContinue) {
    throw 'Cannot restore runtime files while StarCraft is running'
}
foreach ($name in @('StarCraft.exe', 'injectory_x86.exe', 'wmode.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $runtimePath $name) -PathType Leaf)) {
        throw "Missing game runtime component: $runtimePath/$name"
    }
}

# Tournament Manager removes these directories between campaigns. Restore
# missing files only, preserving existing configuration, traces and learning.
$template = Join-Path $repo 'build/direct-template'
foreach ($directory in @('bwapi-data', 'characters')) {
    $source = Join-Path $template $directory
    if (-not (Test-Path -LiteralPath $source -PathType Container)) {
        throw "Missing runtime template directory: $source"
    }
    foreach ($file in Get-ChildItem -LiteralPath $source -Recurse -File) {
        $relative = $file.FullName.Substring($source.Length).TrimStart('\', '/')
        $target = Join-Path $runtimePath "$directory/$relative"
        if (-not (Test-Path -LiteralPath $target)) {
            New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
            Copy-Item -LiteralPath $file.FullName -Destination $target
        }
    }
}
foreach ($directory in @('AI', 'read', 'write', 'logs')) {
    New-Item -ItemType Directory -Path (Join-Path $runtimePath "bwapi-data/$directory") -Force | Out-Null
}
if (-not (Test-Path -LiteralPath (Join-Path $runtimePath 'bwapi-data/BWAPI.dll') -PathType Leaf)) {
    throw 'Runtime template does not contain BWAPI.dll'
}

$archivePath = Join-Path $repo 'ladder/maps/maps.zip'
if (-not (Test-Path -LiteralPath $archivePath -PathType Leaf)) {
    throw "Missing local tournament maps archive: $archivePath"
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead($archivePath)
try {
    foreach ($entry in $archive.Entries) {
        if ($entry.FullName.EndsWith('/')) { continue }
        $target = [System.IO.Path]::GetFullPath((Join-Path $runtimePath $entry.FullName))
        if (-not $target.StartsWith($runtimePath.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw "Map archive entry escapes the runtime: $($entry.FullName)"
        }
        if (-not (Test-Path -LiteralPath $target)) {
            New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target)
        }
    }
} finally { $archive.Dispose() }
