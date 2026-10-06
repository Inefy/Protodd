param(
    [Parameter(Mandatory=$true)][string]$Runtime,
    [ValidateSet('patched-diagnostic', 'stock-certification')]
    [string]$Profile = 'patched-diagnostic'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$runtimeProfiles = Join-Path $PSScriptRoot 'RuntimeProfiles.psm1'
Import-Module $runtimeProfiles -Force
$buildPrefix = [System.IO.Path]::GetFullPath((Join-Path $repo 'build')).TrimEnd('\') + '\'
$runtimePath = [System.IO.Path]::GetFullPath($Runtime)
if (-not $runtimePath.StartsWith($buildPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Match runtime must be inside the build workspace'
}
$stockRoot = [System.IO.Path]::GetFullPath((Join-Path $repo 'build/stock-certification')).TrimEnd('\') + '\'
if ($Profile -eq 'stock-certification' -and
    -not $runtimePath.StartsWith($stockRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Stock certification requires an isolated runtime under $stockRoot"
}
if ($Profile -eq 'patched-diagnostic' -and
    $runtimePath.StartsWith($stockRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'A stock-certification runtime cannot be restored with the patched diagnostic profile'
}
if (Get-Process StarCraft -ErrorAction SilentlyContinue) {
    throw 'Cannot restore runtime files while StarCraft is running'
}
$profileInfo = Get-RuntimeProfileInfo -Profile $Profile -RepositoryRoot $repo
$markerPath = Join-Path $runtimePath '.protodd-runtime-profile.json'
$priorMarker = $null
if (Test-Path -LiteralPath $markerPath -PathType Leaf) {
    $priorMarker = Get-Content -LiteralPath $markerPath -Raw | ConvertFrom-Json
    if ($priorMarker.schema -ne 'protodd-runtime-profile-v1' -or $priorMarker.profile -ne $Profile) {
        throw "Runtime profile marker does not match the requested $Profile profile: $runtimePath"
    }
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
        if ($directory -eq 'bwapi-data' -and $relative -ieq 'BWAPI.dll') { continue }
        $target = Join-Path $runtimePath "$directory/$relative"
        if (-not (Test-Path -LiteralPath $target)) {
            New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
            Copy-Item -LiteralPath $file.FullName -Destination $target
        }
    }
}

$enginePath = Join-Path $runtimePath 'bwapi-data/BWAPI.dll'
$currentEngineHash = if (Test-Path -LiteralPath $enginePath -PathType Leaf) {
    (Get-FileHash -LiteralPath $enginePath -Algorithm SHA256).Hash.ToLowerInvariant()
} else { $null }
if ($Profile -eq 'stock-certification') {
    if ($currentEngineHash -ne $profileInfo.dll_sha256) {
        if ($priorMarker) { throw 'A stock runtime DLL changed after its profile was recorded' }
        $patchedInfo = Get-PatchedRuntimeProfile -RepositoryRoot $repo
        if ($currentEngineHash -and $currentEngineHash -ne $patchedInfo.dll_sha256) {
            throw 'Refusing to replace an unrecognized BWAPI DLL in the stock runtime'
        }
        New-Item -ItemType Directory -Path (Split-Path -Parent $enginePath) -Force | Out-Null
        $temporaryEngine = Join-Path (Split-Path -Parent $enginePath) ('.BWAPI-stock-' + [Guid]::NewGuid().ToString('N') + '.tmp')
        try {
            Export-StockRuntimeDll -RepositoryRoot $repo -Destination $temporaryEngine | Out-Null
            Move-Item -LiteralPath $temporaryEngine -Destination $enginePath -Force
        } finally {
            if (Test-Path -LiteralPath $temporaryEngine) { Remove-Item -LiteralPath $temporaryEngine -Force }
        }
    }
} elseif ($currentEngineHash -ne $profileInfo.dll_sha256) {
    if ($priorMarker) { throw 'A patched diagnostic runtime DLL changed after its profile was recorded' }
    if ($currentEngineHash) {
        throw 'Runtime engine does not match the pinned patched diagnostic profile; no files were changed'
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $enginePath) -Force | Out-Null
    Copy-Item -LiteralPath $profileInfo.path -Destination $enginePath
}
$verifiedEngineHash = (Get-FileHash -LiteralPath $enginePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($verifiedEngineHash -ne $profileInfo.dll_sha256) {
    throw "Runtime engine hash does not match the $Profile profile"
}
foreach ($directory in @('AI', 'read', 'write', 'logs')) {
    New-Item -ItemType Directory -Path (Join-Path $runtimePath "bwapi-data/$directory") -Force | Out-Null
}
if (-not (Test-Path -LiteralPath (Join-Path $runtimePath 'bwapi-data/BWAPI.dll') -PathType Leaf)) {
    throw 'Runtime template does not contain BWAPI.dll'
}

$profileManifest = [ordered]@{
    schema = 'protodd-runtime-profile-v1'
    profile = $Profile
    engine_dll_sha256 = $verifiedEngineHash
    source = $profileInfo.source
    archive_sha256 = if ($profileInfo.PSObject.Properties.Name -contains 'archive_sha256') {
        $profileInfo.archive_sha256
    } else { $null }
    verified_utc = [DateTime]::UtcNow.ToString('o')
}
[IO.File]::WriteAllText($markerPath, ($profileManifest | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
"RUNTIME_PROFILE=$Profile"
"BWAPI_DLL_SHA256=$verifiedEngineHash"

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
