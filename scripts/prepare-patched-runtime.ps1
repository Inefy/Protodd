param(
    [Parameter(Mandatory=$true)][string]$RuntimeSet,
    [string]$HostRuntime = 'build/match-runtime-a',
    [string]$OpponentRuntime = 'build/match-runtime-b',
    [object]$RuntimeLock = $null
)

$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildPrefix = [IO.Path]::GetFullPath((Join-Path $repo 'build')).TrimEnd('\') + '\'
Import-Module (Join-Path $PSScriptRoot 'RuntimeProfiles.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'MatchProcessOwnership.psm1') -Force
$runtimeRoot = Resolve-MatchRuntimeRoot -Profile 'patched-diagnostic' -RepositoryRoot $repo -RuntimeSet $RuntimeSet
$profileRoot = [IO.Path]::GetFullPath((Join-Path $repo 'build/patched-diagnostic'))
if (Get-Process StarCraft -ErrorAction SilentlyContinue) {
    throw 'Stop StarCraft before preparing an isolated patched diagnostic runtime'
}
$patchedProfile = Get-PatchedRuntimeProfile -RepositoryRoot $repo
$stockProfile = Get-StockRuntimeProfile -RepositoryRoot $repo
if ($patchedProfile.dll_sha256 -eq $stockProfile.dll_sha256) {
    throw 'The patched diagnostic and stock certification profiles must use different BWAPI engine binaries'
}
$sourcePaths = @(
    [IO.Path]::GetFullPath((Join-Path $repo $HostRuntime)),
    [IO.Path]::GetFullPath((Join-Path $repo $OpponentRuntime))
)
$targetPaths = @(
    (Join-Path $runtimeRoot 'match-runtime-a'),
    (Join-Path $runtimeRoot 'match-runtime-b')
)
foreach ($source in $sourcePaths) {
    if (-not $source.StartsWith($buildPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath (Join-Path $source 'StarCraft.exe') -PathType Leaf)) {
        throw "Source game runtime must be a valid runtime inside build/: $source"
    }
    if ($source.StartsWith($runtimeRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $runtimeRoot.StartsWith($source.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Source and destination runtimes overlap; choose an existing independent source runtime: $source"
    }
}
foreach ($target in $targetPaths) {
    $resolved = [IO.Path]::GetFullPath($target)
    if (-not $resolved.StartsWith($profileRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Patched runtime destination escapes its isolated profile directory: $resolved"
    }
    if (Test-Path -LiteralPath $resolved) {
        throw "Patched runtime destination already exists; refusing to merge runtime state: $resolved"
    }
}

New-Item -ItemType Directory -Path $runtimeRoot -Force | Out-Null
$ownedRuntimeLock = $null
if ($null -ne $RuntimeLock) {
    $expectedLockPath = [IO.Path]::GetFullPath((Join-Path $runtimeRoot '.protodd-match.lock'))
    if ($null -eq $RuntimeLock.stream -or -not $RuntimeLock.stream.CanWrite -or
        [IO.Path]::GetFullPath([string]$RuntimeLock.path) -ine $expectedLockPath) {
        throw 'The supplied runtime lock does not own the patched runtime set'
    }
    $runtimeLock = $RuntimeLock
} else {
    $runtimeLock = Enter-MatchRuntimeLock -RuntimeRoot $runtimeRoot -Label 'prepare-patched-runtime'
    $ownedRuntimeLock = $runtimeLock
}
try {
foreach ($target in $targetPaths) {
    $resolved = [IO.Path]::GetFullPath($target)
    if (Test-Path -LiteralPath $resolved) {
        throw "Patched runtime destination already exists; refusing to merge runtime state: $resolved"
    }
}
$excludedDirectories = @('bwapi-data', 'characters', 'maps', 'Errors', 'Replays')
for ($index = 0; $index -lt $sourcePaths.Count; $index++) {
    $source = $sourcePaths[$index]
    $target = $targetPaths[$index]
    $excludedPaths = @($excludedDirectories | ForEach-Object { Join-Path $source $_ })
    $robocopyArgs = @($source, $target, '/E', '/COPY:DAT', '/DCOPY:DAT', '/R:1', '/W:1',
        '/NFL', '/NDL', '/NP', '/XF', '.protodd-runtime-profile.json',
        'arena-owned-processes.json', 'arena-cleanup-needed.json', 'pluto*.log',
        'SCScrnShot_*.*', '/XD') + $excludedPaths
    & robocopy @robocopyArgs | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Runtime copy failed with robocopy exit code $LASTEXITCODE" }
    & (Join-Path $PSScriptRoot 'restore-match-runtime.ps1') `
        -Runtime $target -Profile 'patched-diagnostic' -RuntimeLock $runtimeLock
}
Write-Output "Prepared isolated patched-diagnostic runtimes under $runtimeRoot"
Write-Output "Patched BWAPI DLL SHA256: $($patchedProfile.dll_sha256)"
} finally {
    Exit-MatchRuntimeLock -Lock $ownedRuntimeLock
}
