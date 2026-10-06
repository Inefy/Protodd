Set-StrictMode -Version Latest

$script:StockArchiveSha256 = 'd8c45026283b4ceee21512543d6ed0144b02d706fee717f2e27a26f60a8c33aa'
$script:StockDllSha256 = 'f2e0f937e9592157656118fa7e5ff30c2327694ed56c1d8f55687972ad97d308'
$script:InitialPatchedDllSha256 = '3896192b9214898615d5067b6b376edfe672c813db048b8ef6118c887145a0bd'

function Get-StockRuntimeProfile {
    param([Parameter(Mandatory)][string]$RepositoryRoot)

    $archivePath = Join-Path $RepositoryRoot (
        'build/bwapi-runtime-backup/' + $script:StockArchiveSha256.ToUpperInvariant() + '.zip')
    if (-not (Test-Path -LiteralPath $archivePath -PathType Leaf)) {
        throw "The preserved official BWAPI 4.4.0 archive is missing: $archivePath"
    }
    $archiveHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($archiveHash -ne $script:StockArchiveSha256) {
        throw 'The preserved official BWAPI archive hash does not match the stock-certification pin'
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($archivePath)
    try {
        $entry = $archive.GetEntry('bwapi-data/BWAPI.dll')
        if (-not $entry) { throw 'The official BWAPI archive has no bwapi-data/BWAPI.dll entry' }
        $stream = $entry.Open()
        $algorithm = [Security.Cryptography.SHA256]::Create()
        try { $dllHash = [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
        finally { $algorithm.Dispose(); $stream.Dispose() }
    } finally { $archive.Dispose() }
    if ($dllHash -ne $script:StockDllSha256) {
        throw 'The official BWAPI DLL entry hash does not match the stock-certification pin'
    }
    return [pscustomobject]@{
        profile = 'stock-certification'
        archive_sha256 = $archiveHash
        dll_sha256 = $dllHash
        source = 'preserved official Required_BWAPI_440.zip'
        path = $archivePath
    }
}

function Get-PatchedRuntimeProfile {
    param([Parameter(Mandatory)][string]$RepositoryRoot)

    $root = [IO.Path]::GetFullPath($RepositoryRoot)
    $manifestPath = Join-Path $root 'build/patched-runtime-profile.json'
    $templateDll = Join-Path $root 'build/patched-diagnostic-template/bwapi-data/BWAPI.dll'
    $initialTemplate = $false
    if (-not (Test-Path -LiteralPath $templateDll -PathType Leaf)) {
        $templateDll = Join-Path $root 'build/direct-template/bwapi-data/BWAPI.dll'
        $initialTemplate = $true
    }
    if (-not (Test-Path -LiteralPath $templateDll -PathType Leaf)) {
        throw "Patched diagnostic BWAPI runtime template is missing: $templateDll"
    }
    $dllHash = (Get-FileHash -LiteralPath $templateDll -Algorithm SHA256).Hash.ToLowerInvariant()
    $profile = $null
    if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
        $profile = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
        if ($profile.schema -ne 'protodd-patched-runtime-v1' -or
            $profile.dll_sha256 -ne $dllHash) {
            throw 'Patched BWAPI runtime template differs from its deployment manifest'
        }
    } elseif (-not $initialTemplate -or $dllHash -ne $script:InitialPatchedDllSha256) {
        throw 'Patched BWAPI runtime has no deployment manifest and does not match the pinned initial binary'
    }
    return [pscustomobject]@{
        profile = 'patched-diagnostic'
        dll_sha256 = $dllHash
        source = if ($profile) { $profile.source_commit } else { 'initial local patched runtime binary' }
        patch_sha256 = if ($profile) { $profile.patch_sha256 } else { 'not independently recorded' }
        path = $templateDll
    }
}

function Get-RuntimeProfileInfo {
    param(
        [Parameter(Mandatory)][ValidateSet('patched-diagnostic', 'stock-certification')][string]$Profile,
        [Parameter(Mandatory)][string]$RepositoryRoot
    )

    if ($Profile -eq 'stock-certification') {
        return Get-StockRuntimeProfile -RepositoryRoot $RepositoryRoot
    }
    return Get-PatchedRuntimeProfile -RepositoryRoot $RepositoryRoot
}

function Resolve-MatchRuntimeRoot {
    param(
        [Parameter(Mandatory)][ValidateSet('patched-diagnostic', 'stock-certification')][string]$Profile,
        [Parameter(Mandatory)][string]$RepositoryRoot,
        [string]$RuntimeSet = ''
    )

    $buildRoot = [IO.Path]::GetFullPath((Join-Path $RepositoryRoot 'build'))
    $profileRoot = if ($Profile -eq 'stock-certification') {
        Join-Path $buildRoot 'stock-certification'
    } elseif ([string]::IsNullOrWhiteSpace($RuntimeSet)) {
        # Preserve the established default diagnostic runtime pair for existing
        # local workflows. Named sets use a dedicated profile-owned subtree.
        $buildRoot
    } else {
        Join-Path $buildRoot 'patched-diagnostic'
    }
    if ([string]::IsNullOrWhiteSpace($RuntimeSet)) {
        return [IO.Path]::GetFullPath($profileRoot)
    }
    if ($RuntimeSet -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') {
        throw 'RuntimeSet may contain only letters, digits, dots, underscores, and hyphens'
    }
    $root = [IO.Path]::GetFullPath((Join-Path $profileRoot $RuntimeSet))
    $allowedPrefix = [IO.Path]::GetFullPath($profileRoot).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $root.StartsWith($allowedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Runtime set escapes its isolated $Profile profile directory: $root"
    }
    return $root
}

function Resolve-MatchArchiveRoot {
    param(
        [Parameter(Mandatory)][ValidateSet('patched-diagnostic', 'stock-certification')][string]$Profile,
        [Parameter(Mandatory)][string]$RepositoryRoot,
        [string]$RuntimeSet = ''
    )

    $profileRoot = Join-Path ([IO.Path]::GetFullPath((Join-Path $RepositoryRoot 'build'))) "direct-logs/$Profile"
    if ([string]::IsNullOrWhiteSpace($RuntimeSet)) {
        return [IO.Path]::GetFullPath($profileRoot)
    }
    if ($RuntimeSet -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') {
        throw 'RuntimeSet may contain only letters, digits, dots, underscores, and hyphens'
    }
    $root = [IO.Path]::GetFullPath((Join-Path $profileRoot $RuntimeSet))
    $allowedPrefix = [IO.Path]::GetFullPath($profileRoot).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $root.StartsWith($allowedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Match archive set escapes its isolated $Profile profile directory: $root"
    }
    return $root
}

function Export-StockRuntimeDll {
    param(
        [Parameter(Mandatory)][string]$RepositoryRoot,
        [Parameter(Mandatory)][string]$Destination
    )

    $profile = Get-StockRuntimeProfile -RepositoryRoot $RepositoryRoot
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($profile.path)
    try {
        $entry = $archive.GetEntry('bwapi-data/BWAPI.dll')
        $input = $entry.Open()
        try {
            $output = [IO.File]::Open($Destination, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
            try { $input.CopyTo($output) } finally { $output.Dispose() }
        } finally { $input.Dispose() }
    } finally { $archive.Dispose() }
    $copiedHash = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($copiedHash -ne $profile.dll_sha256) {
        throw 'Extracted stock BWAPI DLL failed its pinned hash check'
    }
    return $profile
}

Export-ModuleMember -Function @(
    'Get-StockRuntimeProfile',
    'Get-PatchedRuntimeProfile',
    'Get-RuntimeProfileInfo',
    'Resolve-MatchRuntimeRoot',
    'Resolve-MatchArchiveRoot',
    'Export-StockRuntimeDll'
)
