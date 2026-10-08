param(
    [string]$DllPath = "build/tournament/Release/Protodd.dll",
    [string]$OutputPath = "artifacts/Protodd-AIIDE-2026.zip",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$repoPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
Import-Module (Join-Path $PSScriptRoot "TournamentManifest.psm1") -Force
Assert-TournamentFeatureRegistry -RepositoryRoot $repoPath
$artifactRoot = [IO.Path]::GetFullPath((Join-Path $repoPath "artifacts"))
$resolvedDll = if ([IO.Path]::IsPathRooted($DllPath)) {
    [IO.Path]::GetFullPath($DllPath)
} else {
    [IO.Path]::GetFullPath((Join-Path $repoPath $DllPath))
}
$resolvedOutput = if ([IO.Path]::IsPathRooted($OutputPath)) {
    [IO.Path]::GetFullPath($OutputPath)
} else {
    [IO.Path]::GetFullPath((Join-Path $repoPath $OutputPath))
}
$artifactPrefix = $artifactRoot.TrimEnd('\') + '\'
if (-not $resolvedOutput.StartsWith($artifactPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Package output must stay inside $artifactRoot"
}
if (-not (Test-Path -LiteralPath $resolvedDll -PathType Leaf)) {
    throw "Build the Release|Win32 DLL first: $resolvedDll"
}
if ((Test-Path -LiteralPath $resolvedOutput) -and -not $Force) {
    throw "Package already exists; choose another OutputPath or pass -Force: $resolvedOutput"
}

$manifestPath = Join-Path (Split-Path -Parent $resolvedDll) "Protodd.build-manifest.json"
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "A successful build manifest is required beside the DLL: $manifestPath"
}
$buildManifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($buildManifest.schema -ne "protodd-build-v1") {
    throw "Unsupported build manifest schema: $($buildManifest.schema)"
}
Assert-TournamentDllMatchesManifest -Manifest $buildManifest -DllPath $resolvedDll
Assert-TournamentSourceMatchesManifest -Manifest $buildManifest -RepositoryRoot $repoPath
Assert-TournamentFeatureManifest -Manifest $buildManifest -RepositoryRoot $repoPath

$stagingParent = Join-Path $artifactRoot "staging"
$stagingRoot = Join-Path $stagingParent ("Protodd-" + [Guid]::NewGuid().ToString("N"))
$sourceRoot = Join-Path $stagingRoot "source"
New-Item -ItemType Directory -Path $sourceRoot -Force | Out-Null
$stagingPrefix = $sourceRoot.TrimEnd('\') + '\'
try {
    Copy-Item -LiteralPath $resolvedDll -Destination (Join-Path $stagingRoot "Protodd.dll")
    foreach ($item in $buildManifest.source_files) {
        $relative = [string]$item.path
        if (-not (Test-TournamentManifestPath -Path $relative)) {
            throw "Unsafe path in build manifest: $relative"
        }
        $sourceFile = [IO.Path]::GetFullPath((Join-Path $repoPath $relative))
        $targetFile = [IO.Path]::GetFullPath((Join-Path $sourceRoot $relative))
        if (-not $targetFile.StartsWith($stagingPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Package path escapes its source directory: $relative"
        }
        if (-not (Test-Path -LiteralPath $sourceFile -PathType Leaf)) {
            throw "A build input disappeared before packaging: $relative"
        }
        $currentHash = (Get-FileHash -LiteralPath $sourceFile -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($currentHash -ne ([string]$item.sha256).ToLowerInvariant()) {
            throw "A build input changed after compilation: $relative"
        }
        New-Item -ItemType Directory -Path (Split-Path -Parent $targetFile) -Force | Out-Null
        Copy-Item -LiteralPath $sourceFile -Destination $targetFile
    }
    Copy-Item -LiteralPath (Join-Path $repoPath "SUBMISSION.md") -Destination $stagingRoot
    $packageManifest = [ordered]@{
        schema = "protodd-tournament-package-v1"
        bot = "Protodd"
        race = "Protoss"
        bwapi_version = "4.4.0"
        build_manifest = $buildManifest
        source_snapshot_sha256 = $buildManifest.source_snapshot_sha256
        package_dll_sha256 = $buildManifest.dll_sha256
        packaged_utc = [DateTime]::UtcNow.ToString("o")
    }
    [IO.File]::WriteAllText((Join-Path $stagingRoot "manifest.json"),
        ($packageManifest | ConvertTo-Json -Depth 12), [Text.UTF8Encoding]::new($false))

    $outputDirectory = Split-Path -Parent $resolvedOutput
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    if (Test-Path -LiteralPath $resolvedOutput) {
        if (-not $Force) { throw "Package already exists: $resolvedOutput" }
        Remove-Item -LiteralPath $resolvedOutput -Force
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory(
        $stagingRoot, $resolvedOutput, [IO.Compression.CompressionLevel]::Optimal, $false)
} finally {
    if (Test-Path -LiteralPath $stagingRoot) {
        Remove-Item -LiteralPath $stagingRoot -Recurse -Force
    }
}

$packageHash = (Get-FileHash -LiteralPath $resolvedOutput -Algorithm SHA256).Hash
Write-Output "Tournament package: $resolvedOutput"
Write-Output "SHA256: $packageHash"
