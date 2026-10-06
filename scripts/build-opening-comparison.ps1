param(
    [string]$Weights = "artifacts/replay-learning/whole-game-opening-continuation-20260930/package/weights.bin",
    [string]$BuildDirectory = "build/opening-comparison-20261006",
    [string]$BwapiRoot = "build/t112-official-bwapi-7687da8",
    [string]$BwapiLibrary = "build/t112-official-bwapi-7687da8/bwapi/BWAPILIB/Release/BWAPILIB.lib",
    [string]$Python = "build/model-venv/Scripts/python.exe",
    [string]$Configuration = "Release",
    [string]$PlatformToolset = "v143"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$repoPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Import-Module (Join-Path $PSScriptRoot 'TournamentManifest.psm1') -Force

function Resolve-RepoPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $repoPath $Path))
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Invoke-LoggedCommand {
    param(
        [Parameter(Mandatory)][string]$Executable,
        [Parameter(Mandatory)][string[]]$Arguments,
        [Parameter(Mandatory)][string]$LogPath,
        [Parameter(Mandatory)][string]$FailureMessage
    )
    $output = & $Executable @Arguments 2>&1 | Out-String
    [IO.File]::WriteAllText($LogPath, $output, [Text.UTF8Encoding]::new($false))
    if ($LASTEXITCODE -ne 0) { throw "$FailureMessage (exit $LASTEXITCODE). See $LogPath`n$output" }
    return $output
}

function Read-CMakeCache([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "CMake cache is missing: $Path" }
    $cache = @{}
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -match '^([^#/:]+):[^=]*=(.*)$') { $cache[$Matches[1]] = $Matches[2] }
    }
    return $cache
}

$buildRoot = [IO.Path]::GetFullPath((Join-Path $repoPath 'build')).TrimEnd('\') + '\'
$outputRoot = Resolve-RepoPath $BuildDirectory
if (-not $outputRoot.StartsWith($buildRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Comparison output directory must stay inside $buildRoot"
}
if (Test-Path -LiteralPath $outputRoot) {
    throw "Comparison output already exists; choose a fresh BuildDirectory: $outputRoot"
}

$weightsPath = Resolve-RepoPath $Weights
$weightsManifestPath = Join-Path (Split-Path -Parent $weightsPath) 'manifest.json'
$bwapiPath = Resolve-RepoPath $BwapiRoot
$bwapiLibraryPath = Resolve-RepoPath $BwapiLibrary
$pythonPath = Resolve-RepoPath $Python
foreach ($required in @(
    (Join-Path $bwapiPath 'bwapi/include/BWAPI.h'),
    (Join-Path $bwapiPath 'bwapi/revisionUpdate.vbs'),
    $bwapiLibraryPath,
    $weightsPath,
    $weightsManifestPath,
    $pythonPath
)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Required comparison input is missing: $required" }
}

$weightHash = Get-Sha256 $weightsPath
$weightManifest = Get-Content -LiteralPath $weightsManifestPath -Raw | ConvertFrom-Json
if ($weightManifest.schema -ne 'protodd-whole-game-multislot-weights-v2' -or
    $weightManifest.maximum_slots -ne 6 -or $weightManifest.weights_sha256 -ne $weightHash) {
    throw 'Hybrid profiles require an intact six-slot weights package with a matching manifest'
}

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer (vswhere.exe) was not found' }
$vsInstall = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { throw 'Install Visual Studio 2022 Build Tools with the C++ x86/x64 workload' }
$toolsetRoot = Join-Path $vsInstall 'VC/Tools/MSVC'
$toolset = Get-ChildItem -LiteralPath $toolsetRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
if (-not $toolset) { throw "MSVC toolset directory was not found: $toolsetRoot" }
$dumpbin = Join-Path $toolset.FullName 'bin/Hostx64/x86/dumpbin.exe'
if (-not (Test-Path -LiteralPath $dumpbin)) { throw "Win32 dumpbin was not found: $dumpbin" }

New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
$variants = @(
    [pscustomobject]@{ Name = 'native-standard'; Control = 'OFF'; Hybrid = 'OFF'; Evaluation = 'OFF'; AllIn = 'OFF'; Dll = 'Protodd.dll'; Weights = $false },
    [pscustomobject]@{ Name = 'native-all-in'; Control = 'OFF'; Hybrid = 'OFF'; Evaluation = 'OFF'; AllIn = 'ON'; Dll = 'Protodd.dll'; Weights = $false },
    [pscustomobject]@{ Name = 'hybrid-standard'; Control = 'ON'; Hybrid = 'ON'; Evaluation = 'ON'; AllIn = 'OFF'; Dll = 'ProtoddEvaluation.dll'; Weights = $true },
    [pscustomobject]@{ Name = 'hybrid-all-in'; Control = 'ON'; Hybrid = 'ON'; Evaluation = 'ON'; AllIn = 'ON'; Dll = 'ProtoddEvaluation.dll'; Weights = $true }
)
$profileRecords = [Collections.Generic.List[object]]::new()
$generator = 'Visual Studio 17 2022'

foreach ($variant in $variants) {
    Write-Output "Building profile: $($variant.Name)"
    $profileRoot = Join-Path $outputRoot $variant.Name
    $cmakeRoot = Join-Path $profileRoot 'build'
    New-Item -ItemType Directory -Path $profileRoot -Force | Out-Null
    $configureArgs = @(
        '-S', $repoPath, '-B', $cmakeRoot, '-G', $generator, '-A', 'Win32',
        '-DPROTODD_BUILD_TESTS=ON', '-DPROTODD_BUILD_BWAPI_MODULE=ON',
        '-DPROTODD_TOURNAMENT_PROFILE=ON', '-DPROTODD_DEVELOPER_PROFILE=OFF',
        '-DPROTODD_WHOLE_GAME_RESEARCH_AUTHORITY=OFF',
        "-DPROTODD_WHOLE_GAME_CONTROL=$($variant.Control)",
        "-DPROTODD_WHOLE_GAME_HYBRID=$($variant.Hybrid)",
        "-DPROTODD_WHOLE_GAME_EVALUATION_BUILD=$($variant.Evaluation)",
        "-DPROTODD_NATIVE_ALLIN_OPENING=$($variant.AllIn)",
        "-DBWAPI_ROOT=$bwapiPath", "-DBWAPI_LIBRARY=$bwapiLibraryPath",
        "-DCMAKE_VS_PLATFORM_TOOLSET=$PlatformToolset"
    )
    if ($variant.Weights) {
        $configureArgs += @("-DPROTODD_WHOLE_GAME_WEIGHTS=$weightsPath", "-DPython3_EXECUTABLE=$pythonPath")
    }
    [void](Invoke-LoggedCommand -Executable 'cmake' -Arguments $configureArgs `
        -LogPath (Join-Path $profileRoot 'configure.log') -FailureMessage "CMake configuration failed for $($variant.Name)")
    $cachePath = Join-Path $cmakeRoot 'CMakeCache.txt'
    $cache = Read-CMakeCache $cachePath
    foreach ($expectation in @(
        @('PROTODD_WHOLE_GAME_CONTROL', $variant.Control),
        @('PROTODD_WHOLE_GAME_HYBRID', $variant.Hybrid),
        @('PROTODD_WHOLE_GAME_EVALUATION_BUILD', $variant.Evaluation),
        @('PROTODD_NATIVE_ALLIN_OPENING', $variant.AllIn)
    )) {
        if ($cache[$expectation[0]] -ne $expectation[1]) {
            throw "$($variant.Name) cache recorded $($expectation[0])=$($cache[$expectation[0]]); expected $($expectation[1])"
        }
    }
    if ($variant.Weights) {
        if ((Get-Sha256 $cache['PROTODD_WHOLE_GAME_WEIGHTS']) -ne $weightHash) {
            throw "$($variant.Name) does not reference the verified model weights"
        }
    } elseif ($cache['PROTODD_WHOLE_GAME_WEIGHTS']) {
        throw "$($variant.Name) unexpectedly depends on learned weights"
    }

    [void](Invoke-LoggedCommand -Executable 'cmake' -Arguments @('--build', $cmakeRoot, '--config', $Configuration, '--parallel', '8') `
        -LogPath (Join-Path $profileRoot 'build.log') -FailureMessage "Build failed for $($variant.Name)")
    $testOutput = Invoke-LoggedCommand -Executable 'ctest' -Arguments @('--test-dir', $cmakeRoot, '-C', $Configuration, '--output-on-failure') `
        -LogPath (Join-Path $profileRoot 'ctest.log') -FailureMessage "CTest failed for $($variant.Name)"
    if ($testOutput -notmatch '(?m)100% tests passed,\s+\d+ tests failed out of \d+') {
        throw "CTest summary was missing or incomplete for $($variant.Name); see $(Join-Path $profileRoot 'ctest.log')"
    }
    $summary = [regex]::Match($testOutput, '(?m)(\d+)% tests passed,\s+(\d+) tests failed out of (\d+)').Value
    $testCounts = [regex]::Match($testOutput, '(?m)(\d+)% tests passed,\s+(\d+) tests failed out of (\d+)')
    $dllPath = Join-Path $cmakeRoot "$Configuration/$($variant.Dll)"
    if (-not (Test-Path -LiteralPath $dllPath -PathType Leaf)) { throw "Expected profile DLL is missing: $dllPath" }
    $headers = (& $dumpbin /nologo /headers $dllPath 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0 -or $headers -notmatch '14C machine \(x86\)') {
        throw "$($variant.Name) did not produce a valid 32-bit x86 DLL"
    }
    $exports = (& $dumpbin /nologo /exports $dllPath 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0 -or $exports -notmatch '(?m)\bgameInit\b' -or $exports -notmatch '(?m)\bnewAIModule\b') {
        throw "$($variant.Name) DLL is missing BWAPI entry points"
    }

    $featureManifest = [ordered]@{}
    foreach ($name in @($cache.Keys | Where-Object { $_.StartsWith('PROTODD_', [StringComparison]::Ordinal) } | Sort-Object)) {
        $value = $cache[$name]
        if ($name -eq 'PROTODD_WHOLE_GAME_WEIGHTS' -and $value) { $value = Get-Sha256 $value }
        $featureManifest[$name] = $value
    }
    $buildManifest = [ordered]@{
        schema = 'protodd-opening-profile-build-v1'
        profile = $variant.Name
        controller = if ($variant.Control -eq 'ON') { 'weighted-hybrid' } else { 'native' }
        opening = if ($variant.AllIn -eq 'ON') { 'native-all-in' } else { 'standard' }
        source_files = $null
        source_snapshot_sha256 = $null
        source_commit = Get-TournamentSourceCommit -RepositoryRoot $repoPath
        source_diff_sha256 = $null
        cmake_generator = $cache['CMAKE_GENERATOR']
        cmake_platform = $cache['CMAKE_GENERATOR_PLATFORM']
        cmake_cache_sha256 = Get-Sha256 $cachePath
        cmake_options = $featureManifest
        weights_sha256 = if ($variant.Weights) { $weightHash } else { $null }
        bwapi_version = '4.4.0'
        bwapi_source_commit = Get-TournamentSourceCommit -RepositoryRoot $bwapiPath
        bwapi_source_patch_sha256 = $null
        bwapi_library_sha256 = Get-Sha256 $bwapiLibraryPath
        dll = [IO.Path]::GetRelativePath($repoPath, $dllPath).Replace('\', '/')
        dll_sha256 = Get-Sha256 $dllPath
        pe_machine = 'x86'
        bwapi_exports = @('gameInit', 'newAIModule')
        tests = [ordered]@{ summary = $summary; passed = [int]$testCounts.Groups[1].Value; failed = [int]$testCounts.Groups[2].Value; total = [int]$testCounts.Groups[3].Value; ctest_log = [IO.Path]::GetRelativePath($repoPath, (Join-Path $profileRoot 'ctest.log')).Replace('\', '/') }
        built_utc = [DateTime]::UtcNow.ToString('o')
    }
    $profileRecords.Add([pscustomobject]@{ Variant = $variant; Root = $profileRoot; CMakeRoot = $cmakeRoot; CachePath = $cachePath; BuildManifest = $buildManifest; WeightHash = $weightHash })
}

$inventory = @(Get-TournamentSourceInventory -RepositoryRoot $repoPath)
$sourceSnapshotHash = Get-TournamentInventoryHash -Inventory $inventory
$sourceCommit = Get-TournamentSourceCommit -RepositoryRoot $repoPath
$sourceDiffHash = 'source-archive'
if ($sourceCommit -ne 'source-archive') {
    $sourceDiff = & git -C $repoPath diff HEAD --binary 2>$null
    if ($LASTEXITCODE -eq 0) {
        $bytes = [Text.Encoding]::UTF8.GetBytes(($sourceDiff -join "`n"))
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $sourceDiffHash = [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-', '').ToLowerInvariant() }
        finally { $sha.Dispose() }
    } else { $sourceDiffHash = 'unavailable' }
}
$bwapiSourceCommit = Get-TournamentSourceCommit -RepositoryRoot $bwapiPath
$bwapiPatch = & git -C $bwapiPath diff HEAD --binary 2>$null
$bwapiPatchHash = if ($LASTEXITCODE -eq 0) {
    $bytes = [Text.Encoding]::UTF8.GetBytes(($bwapiPatch -join "`n"))
    $sha = [Security.Cryptography.SHA256]::Create()
    try { [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
} else { 'unavailable' }

$profileManifests = [Collections.Generic.List[object]]::new()
foreach ($record in $profileRecords) {
    $manifest = $record.BuildManifest
    $manifest.source_files = $inventory
    $manifest.source_snapshot_sha256 = $sourceSnapshotHash
    $manifest.source_diff_sha256 = $sourceDiffHash
    $manifest.bwapi_source_commit = $bwapiSourceCommit
    $manifest.bwapi_source_patch_sha256 = $bwapiPatchHash
    $jsonPath = Join-Path $record.Root 'build-manifest.json'
    [IO.File]::WriteAllText($jsonPath, ($manifest | ConvertTo-Json -Depth 12), [Text.UTF8Encoding]::new($false))
    $profileManifests.Add([pscustomobject]@{ profile = $manifest.profile; manifest = [IO.Path]::GetRelativePath($repoPath, $jsonPath).Replace('\', '/'); dll_sha256 = $manifest.dll_sha256; weights_sha256 = $manifest.weights_sha256; control = $manifest.controller; opening = $manifest.opening; tests = $manifest.tests })
}

$comparison = [ordered]@{
    schema = 'protodd-opening-comparison-v1'
    source_commit = $sourceCommit
    source_snapshot_sha256 = $sourceSnapshotHash
    source_diff_sha256 = $sourceDiffHash
    model_weights_sha256 = $weightHash
    bwapi_version = '4.4.0'
    bwapi_source_commit = $bwapiSourceCommit
    bwapi_source_patch_sha256 = $bwapiPatchHash
    profiles = $profileManifests.ToArray()
    attribution = [ordered]@{
        native_opening_effect = 'Compare native-all-in against native-standard; controller mode is fixed to native.'
        hybrid_opening_effect = 'Compare hybrid-all-in against hybrid-standard; controller mode and weights are fixed.'
        controller_effect_standard_opening = 'Compare hybrid-standard against native-standard; opening is fixed to standard.'
        controller_effect_all_in_opening = 'Compare hybrid-all-in against native-all-in; opening is fixed to native all-in.'
        outcome_claim = 'Build manifests establish profile identity; claim improvement only from matched game results linked to these exact manifests.'
    }
    built_utc = [DateTime]::UtcNow.ToString('o')
}
$comparisonPath = Join-Path $outputRoot 'opening-comparison.json'
[IO.File]::WriteAllText($comparisonPath, ($comparison | ConvertTo-Json -Depth 12), [Text.UTF8Encoding]::new($false))
Write-Output "Opening comparison manifest: $comparisonPath"
Write-Output "Source snapshot SHA256: $sourceSnapshotHash"
Write-Output "Model weights SHA256: $weightHash"
