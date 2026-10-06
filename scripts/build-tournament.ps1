param(
    [string]$BwapiRoot = "build/_deps/bwapi-src",
    [string]$BuildDirectory = "build/tournament",
    [string]$Configuration = "Release",
    [string]$PlatformToolset = "v143"
)

$ErrorActionPreference = "Stop"
$repoPath = Split-Path -Parent $PSScriptRoot
$buildPrefix = [System.IO.Path]::GetFullPath((Join-Path $repoPath 'build')).TrimEnd('\') + '\'
$buildPath = if ([System.IO.Path]::IsPathRooted($BuildDirectory)) {
    [System.IO.Path]::GetFullPath($BuildDirectory)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $repoPath $BuildDirectory))
}
if (-not $buildPath.StartsWith($buildPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Tournament build directory must stay inside $buildPrefix"
}
Import-Module (Join-Path $PSScriptRoot "TournamentManifest.psm1") -Force
$bwapiPath = if ([System.IO.Path]::IsPathRooted($BwapiRoot)) {
    [System.IO.Path]::GetFullPath($BwapiRoot)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $repoPath $BwapiRoot))
}

$bwapiProject = Join-Path $bwapiPath "bwapi/BWAPILIB/BWAPILIB.vcxproj"
$revisionScript = Join-Path $bwapiPath "bwapi/revisionUpdate.vbs"
$bwapiLibrary = Join-Path $bwapiPath "bwapi/BWAPILIB/$Configuration/BWAPILIB.lib"
$bwapiHeader = Join-Path $bwapiPath "bwapi/include/BWAPI.h"
if (-not (Test-Path -LiteralPath $bwapiProject) -or
    -not (Test-Path -LiteralPath $revisionScript) -or
    -not (Test-Path -LiteralPath $bwapiHeader)) {
    throw "BwapiRoot must contain the official BWAPI 4.4 source tree: $bwapiPath"
}

$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "Visual Studio Installer (vswhere.exe) was not found"
}
$vsInstall = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsInstall) {
    throw "Install Visual Studio 2022 Build Tools with the C++ x86/x64 workload"
}
$msbuild = Join-Path $vsInstall "MSBuild/Current/Bin/MSBuild.exe"
if (-not (Test-Path -LiteralPath $msbuild)) {
    throw "MSBuild was not found under $vsInstall"
}

Push-Location (Split-Path -Parent $revisionScript)
try {
    & cscript //nologo (Split-Path -Leaf $revisionScript)
    if ($LASTEXITCODE -ne 0) { throw "BWAPI revision header generation failed" }
} finally {
    Pop-Location
}

& $msbuild $bwapiProject /m "/p:Configuration=$Configuration" /p:Platform=Win32 `
    "/p:PlatformToolset=$PlatformToolset" /verbosity:minimal
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $bwapiLibrary)) {
    throw "Official BWAPILIB Release|Win32 build failed"
}

& cmake -S $repoPath -B $buildPath -G "Visual Studio 17 2022" -A Win32 `
    -DPROTODD_BUILD_TESTS=ON -DPROTODD_BUILD_BWAPI_MODULE=ON `
    -DPROTODD_TOURNAMENT_PROFILE=ON -DPROTODD_DEVELOPER_PROFILE=OFF `
    -DPROTODD_WHOLE_GAME_RESEARCH_AUTHORITY=OFF `
    "-DBWAPI_ROOT=$bwapiPath" "-DBWAPI_LIBRARY=$bwapiLibrary"
if ($LASTEXITCODE -ne 0) { throw "Tournament CMake configuration failed" }
& cmake --build $buildPath --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "Tournament DLL build failed" }
& ctest --test-dir $buildPath -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Tournament Release tests failed" }

$dllPath = Join-Path $buildPath "$Configuration/Protodd.dll"
if (-not (Test-Path -LiteralPath $dllPath)) {
    throw "Expected tournament DLL was not produced: $dllPath"
}
$vsInstall = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
$toolsetRoot = Join-Path $vsInstall "VC/Tools/MSVC"
$toolset = Get-ChildItem -LiteralPath $toolsetRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
if (-not $toolset) { throw "MSVC toolset directory was not found: $toolsetRoot" }
$dumpbin = Join-Path $toolset.FullName "bin/Hostx64/x86/dumpbin.exe"
if (-not (Test-Path -LiteralPath $dumpbin)) { throw "Win32 dumpbin was not found: $dumpbin" }
$headers = (& $dumpbin /nologo /headers $dllPath 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0 -or $headers -notmatch "14C machine \(x86\)") {
    throw "Tournament DLL is not a valid 32-bit x86 PE image: $dllPath"
}
$exports = (& $dumpbin /nologo /exports $dllPath 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0 -or $exports -notmatch "(?m)\bgameInit\b" -or
    $exports -notmatch "(?m)\bnewAIModule\b") {
    throw "Tournament DLL is missing a required BWAPI entry point: $dllPath"
}

$sourceFiles = @(Get-TournamentSourceInventory -RepositoryRoot $repoPath)
$sourceSnapshotHash = Get-TournamentInventoryHash -Inventory $sourceFiles
$sourceCommit = Get-TournamentSourceCommit -RepositoryRoot $repoPath
$sourceDirty = $false
$sourceDiffHash = 'source-archive'
if ($sourceCommit -ne 'source-archive') {
    $sourceDiff = & git -C $repoPath diff HEAD --binary 2>$null
    if ($LASTEXITCODE -eq 0) {
        $sourceDiffBytes = [Text.Encoding]::UTF8.GetBytes(($sourceDiff -join "`n"))
        $sourceDiffAlgorithm = [Security.Cryptography.SHA256]::Create()
        try { $sourceDiffHash = [BitConverter]::ToString($sourceDiffAlgorithm.ComputeHash($sourceDiffBytes)).Replace('-', '').ToLowerInvariant() }
        finally { $sourceDiffAlgorithm.Dispose() }
    } else { $sourceDiffHash = 'unavailable' }
    $sourceDirty = @(& git -C $repoPath status --porcelain -- cmake include src tests tools training docs bwapi-data CMakeLists.txt CMakePresets.json README.md LICENSE SUBMISSION.md scripts 2>$null).Count -gt 0
}
$cachePath = Join-Path $buildPath "CMakeCache.txt"
if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
    throw "CMake cache was not produced: $cachePath"
}
$cache = @{}
foreach ($line in Get-Content -LiteralPath $cachePath) {
    if ($line -match '^([^#/:]+):[^=]*=(.*)$') { $cache[$Matches[1]] = $Matches[2] }
}
$featureManifest = [ordered]@{}
foreach ($name in @($cache.Keys | Where-Object { $_.StartsWith('PROTODD_', [StringComparison]::Ordinal) } | Sort-Object)) {
    $value = $cache[$name]
    if ($name -eq 'PROTODD_WHOLE_GAME_WEIGHTS' -and $value -and
        (Test-Path -LiteralPath $value -PathType Leaf)) {
        $value = (Get-FileHash -LiteralPath $value -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $featureManifest[$name] = $value
}
$compilerMetadataPath = Get-ChildItem -LiteralPath (Join-Path $buildPath 'CMakeFiles') `
    -Filter 'CMakeCXXCompiler.cmake' -File -Recurse | Select-Object -First 1 -ExpandProperty FullName
$compilerMetadata = if ($compilerMetadataPath) {
    [IO.File]::ReadAllText($compilerMetadataPath)
} else { '' }
$compilerId = if ($compilerMetadata -match 'set\(CMAKE_CXX_COMPILER_ID "([^"]+)"\)') {
    $Matches[1]
} else { $cache["CMAKE_CXX_COMPILER_ID"] }
$compilerVersion = if ($compilerMetadata -match 'set\(CMAKE_CXX_COMPILER_VERSION "([^"]+)"\)') {
    $Matches[1]
} else { $cache["CMAKE_CXX_COMPILER_VERSION"] }
$bwapiSourceCommit = Get-TournamentSourceCommit -RepositoryRoot $bwapiPath
$bwapiPatch = & git -C $bwapiPath diff HEAD --binary 2>$null
$bwapiPatchHash = if ($LASTEXITCODE -eq 0) {
    $patchBytes = [Text.Encoding]::UTF8.GetBytes(($bwapiPatch -join "`n"))
    $sha = [Security.Cryptography.SHA256]::Create()
    try { [BitConverter]::ToString($sha.ComputeHash($patchBytes)).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
} else { "unavailable" }
$bwapiStatus = & git -C $bwapiPath status --porcelain 2>$null
$bwapiLibraryHash = (Get-FileHash -LiteralPath $bwapiLibrary -Algorithm SHA256).Hash.ToLowerInvariant()
$dllHash = (Get-FileHash -LiteralPath $dllPath -Algorithm SHA256).Hash.ToLowerInvariant()
$manifestPath = Join-Path (Split-Path -Parent $dllPath) "Protodd.build-manifest.json"
$manifest = [ordered]@{
    schema = "protodd-build-v1"
    source_commit = $sourceCommit
    source_snapshot_sha256 = $sourceSnapshotHash
    source_diff_sha256 = $sourceDiffHash
    source_files = $sourceFiles
    source_dirty = $sourceDirty
    configuration = $Configuration
    cmake_generator = $cache["CMAKE_GENERATOR"]
    cmake_platform = $cache["CMAKE_GENERATOR_PLATFORM"]
    compiler_id = $compilerId
    compiler_version = $compilerVersion
    cmake_cache_sha256 = (Get-FileHash -LiteralPath $cachePath -Algorithm SHA256).Hash.ToLowerInvariant()
    cmake_options = [ordered]@{
        build_bwapi_module = $cache["PROTODD_BUILD_BWAPI_MODULE"]
        tournament_profile = $cache["PROTODD_TOURNAMENT_PROFILE"]
        developer_profile = $cache["PROTODD_DEVELOPER_PROFILE"]
        learned_research_authority = $cache["PROTODD_WHOLE_GAME_RESEARCH_AUTHORITY"]
        whole_game_control = $cache["PROTODD_WHOLE_GAME_CONTROL"]
        whole_game_hybrid = $cache["PROTODD_WHOLE_GAME_HYBRID"]
        native_allin_opening = $cache["PROTODD_NATIVE_ALLIN_OPENING"]
        weights = if ($cache["PROTODD_WHOLE_GAME_WEIGHTS"]) {
            (Get-FileHash -LiteralPath $cache["PROTODD_WHOLE_GAME_WEIGHTS"] -Algorithm SHA256).Hash.ToLowerInvariant()
        } else { $null }
    }
    feature_manifest = $featureManifest
    bwapi_version = "4.4.0"
    bwapi_source_commit = $bwapiSourceCommit
    bwapi_source_dirty = @($bwapiStatus).Count -gt 0
    bwapi_source_patch_sha256 = $bwapiPatchHash
    bwapi_library_sha256 = $bwapiLibraryHash
    dll_sha256 = $dllHash
    pe_machine = "x86"
    bwapi_exports = @("gameInit", "newAIModule")
    built_utc = [DateTime]::UtcNow.ToString("o")
}
[IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
Write-Output "Protodd tournament DLL: $dllPath"
Write-Output "Build manifest: $manifestPath"
Write-Output "SHA256: $dllHash"
