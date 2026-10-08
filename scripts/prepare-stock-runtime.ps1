param(
    [string]$HostRuntime = 'build/match-runtime-a',
    [string]$OpponentRuntime = 'build/match-runtime-b',
    [string]$RuntimeSet = '',
    [object]$RuntimeLock = $null
)

$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildPrefix = [IO.Path]::GetFullPath((Join-Path $repo 'build')).TrimEnd('\') + '\'
Import-Module (Join-Path $PSScriptRoot 'RuntimeProfiles.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'MatchProcessOwnership.psm1') -Force
$runtimeRoot = Resolve-MatchRuntimeRoot -Profile 'stock-certification' -RepositoryRoot $repo -RuntimeSet $RuntimeSet
$profileRoot = [IO.Path]::GetFullPath((Join-Path $repo 'build/stock-certification'))
if (Get-Process StarCraft -ErrorAction SilentlyContinue) {
    throw 'Stop StarCraft before preparing the isolated stock runtime profile'
}
$stockProfile = Get-StockRuntimeProfile -RepositoryRoot $repo
$patchedProfile = Get-PatchedRuntimeProfile -RepositoryRoot $repo
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
}
foreach ($target in $targetPaths) {
    $resolved = [IO.Path]::GetFullPath($target)
    if (-not $resolved.StartsWith($profileRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Stock runtime destination escapes its isolated profile directory: $target"
    }
    if (Test-Path -LiteralPath $resolved) {
        throw "Stock runtime destination already exists; refusing to merge runtime state: $resolved"
    }
}

New-Item -ItemType Directory -Path $runtimeRoot -Force | Out-Null
$ownedRuntimeLock = $null
if ($null -ne $RuntimeLock) {
    $expectedLockPath = [IO.Path]::GetFullPath((Join-Path $runtimeRoot '.protodd-match.lock'))
    if ($null -eq $RuntimeLock.stream -or -not $RuntimeLock.stream.CanWrite -or
        [IO.Path]::GetFullPath([string]$RuntimeLock.path) -ine $expectedLockPath) {
        throw 'The supplied runtime lock does not own the stock runtime set'
    }
    $runtimeLock = $RuntimeLock
} else {
    $runtimeLock = Enter-MatchRuntimeLock -RuntimeRoot $runtimeRoot -Label 'prepare-stock-runtime'
    $ownedRuntimeLock = $runtimeLock
}
try {
foreach ($target in $targetPaths) {
    $resolved = [IO.Path]::GetFullPath($target)
    if (Test-Path -LiteralPath $resolved) {
        throw "Stock runtime destination already exists; refusing to merge runtime state: $resolved"
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
        -Runtime $target -Profile 'stock-certification' -RuntimeLock $runtimeLock
}
if ($stockProfile.dll_sha256 -eq $patchedProfile.dll_sha256) {
    throw 'The stock and patched profiles unexpectedly resolve to the same BWAPI engine binary'
}
Write-Output "Prepared isolated stock-certification runtimes under $runtimeRoot"
Write-Output "Stock BWAPI DLL SHA256: $($stockProfile.dll_sha256)"
} finally {
    Exit-MatchRuntimeLock -Lock $ownedRuntimeLock
}
