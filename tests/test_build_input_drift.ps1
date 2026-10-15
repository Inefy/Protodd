param(
    [Parameter(Mandatory)][string]$DllPath,
    [Parameter(Mandatory)][string]$CMakeCache,
    [Parameter(Mandatory)][string]$BwapiRoot,
    [Parameter(Mandatory)][string]$BwapiLibrary,
    [string]$BuildScript
)

$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $BuildScript) { $BuildScript = Join-Path $repo 'scripts/build-tournament.ps1' }
Import-Module (Join-Path $repo 'scripts/TournamentManifest.psm1') -Force
$inventory = @(Get-TournamentSourceInventory -RepositoryRoot $repo)
$libraryHash = (Get-FileHash -LiteralPath $BwapiLibrary -Algorithm SHA256).Hash.ToLowerInvariant()
$dllHash = (Get-FileHash -LiteralPath $DllPath -Algorithm SHA256).Hash.ToLowerInvariant()
$compilerMetadata = Get-ChildItem -LiteralPath (Join-Path (Split-Path -Parent $CMakeCache) 'CMakeFiles') -Filter 'CMakeCXXCompiler.cmake' -File -Recurse | Select-Object -First 1 -ExpandProperty FullName
if (-not $compilerMetadata) { throw 'Native compiler metadata is required for this test' }
$tempPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
$fixtureRoot = Join-Path $tempPrefix ('protodd-build-input-drift-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureRoot | Out-Null

# Run the actual build entry point, replacing only the expensive compiler/test
# commands. The supplied native DLL, PE/export checks and manifest writer are
# real. Each phase can edit the isolated input after it was read by the build.
function Invoke-FixtureMutation {
    param([string]$Phase)
    $fixture.Calls.Add($Phase)
    if ($fixture.MutateAt -eq $Phase) {
        [IO.File]::AppendAllText($fixture.SourceFile, ([Environment]::NewLine + '// Edited during the build.' + [Environment]::NewLine))
    }
}

function cmake {
    if ($args[0] -eq '--build') {
        Copy-Item -LiteralPath $fixture.DllPath -Destination (Join-Path $fixture.BuildPath 'Release/Protodd.dll')
        Invoke-FixtureMutation -Phase 'compile'
    } else {
        $cachePath = Join-Path $fixture.BuildPath 'CMakeCache.txt'
        $cacheLines = @(Get-Content -LiteralPath $cachePath)
        foreach ($argument in $args) {
            if ($argument -match '^-D([^=]+)=(.*)$') {
                $key = $Matches[1]
                $value = $Matches[2]
                $pattern = '^' + [regex]::Escape($key) + ':[^=]*='
                $cacheLines = @($cacheLines | Where-Object { $_ -notmatch $pattern })
                $cacheLines += "${key}:STRING=$value"
            }
        }
        [IO.File]::WriteAllLines($cachePath, [string[]]$cacheLines)
        Invoke-FixtureMutation -Phase 'configure'
    }
    $global:LASTEXITCODE = 0
}

function ctest {
    Invoke-FixtureMutation -Phase 'test'
    $global:LASTEXITCODE = 0
}

try {
    foreach ($phase in @('unchanged', 'configure', 'compile', 'test')) {
        $sourceRoot = Join-Path $fixtureRoot $phase
        foreach ($entry in $inventory) {
            $destination = Join-Path $sourceRoot $entry.path
            New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
            Copy-Item -LiteralPath (Join-Path $repo $entry.path) -Destination $destination
        }
        Copy-Item -LiteralPath $BuildScript -Destination (Join-Path $sourceRoot 'scripts/build-tournament.ps1') -Force
        $buildPath = Join-Path $sourceRoot 'build/tournament'
        New-Item -ItemType Directory -Path (Join-Path $buildPath 'Release') -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Path $buildPath 'CMakeFiles/fixture') -Force | Out-Null
        Copy-Item -LiteralPath $CMakeCache -Destination (Join-Path $buildPath 'CMakeCache.txt')
        Copy-Item -LiteralPath $compilerMetadata -Destination (Join-Path $buildPath 'CMakeFiles/fixture/CMakeCXXCompiler.cmake')
        $fixture = @{
            MutateAt = $phase
            SourceFile = Join-Path $sourceRoot 'src/core/strategy/StrategyPvP.cpp'
            BuildPath = $buildPath
            DllPath = $DllPath
            Calls = [Collections.Generic.List[string]]::new()
        }
        $failure = $null
        try {
            $parameters = @{
                BwapiRoot = $BwapiRoot
                BuildDirectory = 'build/tournament'
                PinnedBwapiLibrary = $BwapiLibrary
                PinnedBwapiLibrarySha256 = $libraryHash
            }
            & (Join-Path $sourceRoot 'scripts/build-tournament.ps1') @parameters | Out-Null
        } catch {
            $failure = "$($_.Exception.Message) [$($_.ScriptStackTrace)]"
        }
        if (($fixture.Calls -join ',') -ne 'configure,compile,test') {
            throw "Fixture did not exercise all build phases for ${phase}: $failure"
        }
        $manifestPath = Join-Path $buildPath 'Release/Protodd.build-manifest.json'
        if ($phase -eq 'unchanged') {
            if ($failure) { throw "Unchanged source build failed: $failure" }
            $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
            if ($manifest.dll_sha256 -cne $dllHash) { throw 'Unchanged build recorded the wrong DLL' }
            Assert-TournamentSourceMatchesManifest -Manifest $manifest -RepositoryRoot $sourceRoot
        } else {
            if (-not $failure -or $failure -notmatch 'Current source snapshot differs') {
                throw "Source drift during $phase was not rejected: $failure"
            }
            if (Test-Path -LiteralPath $manifestPath) {
                throw "Source drift during $phase published a successful build manifest"
            }
        }
        Write-Output "Build input provenance: $phase passed"
    }
} finally {
    $resolvedFixture = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $fixtureRoot).Path)
    if (-not $resolvedFixture.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolvedFixture) -notmatch '^protodd-build-input-drift-[0-9a-f]{32}$' -or
        ((Get-Item -LiteralPath $resolvedFixture).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'Refusing to remove an unexpected build input fixture path'
    }
    Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
}
