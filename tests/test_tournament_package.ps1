$ErrorActionPreference = "Stop"
$module = Join-Path (Split-Path -Parent $PSScriptRoot) "scripts/TournamentManifest.psm1"
Import-Module $module -Force
$fixture = Join-Path ([IO.Path]::GetTempPath()) ("protodd-package-test-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $fixture -Force | Out-Null
try {
    $repositoryRoot = Split-Path -Parent $PSScriptRoot
    $nestedPrefix = [IO.Path]::GetFullPath((Join-Path $repositoryRoot 'build/tournament-package-archive-fixtures')).TrimEnd('\') + '\'
    $nestedGuid = [Guid]::NewGuid().ToString('N')
    $nestedRoot = [IO.Path]::GetFullPath((Join-Path $nestedPrefix $nestedGuid))
    if (-not $nestedRoot.StartsWith($nestedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Nested archive fixture path escapes the build workspace'
    }
    try {
        New-Item -ItemType Directory -Path (Join-Path $nestedRoot 'src/core') -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Path $nestedRoot 'scripts') -Force | Out-Null
        [IO.File]::WriteAllText((Join-Path $nestedRoot 'CMakeLists.txt'), 'archive source')
        [IO.File]::WriteAllText((Join-Path $nestedRoot 'src/core/sample.cpp'), 'archive source')
        [IO.File]::WriteAllText((Join-Path $nestedRoot 'scripts/direct-match.ps1'), 'archive source')
        if ((Get-TournamentSourceCommit -RepositoryRoot $nestedRoot) -ne 'source-archive') {
            throw 'A package nested inside another Git worktree inherited its parent commit'
        }
        $nestedPaths = @(Get-TournamentSourceInventory -RepositoryRoot $nestedRoot |
            ForEach-Object { $_.path })
        if ($nestedPaths.Count -ne 3 -or $nestedPaths -notcontains 'src/core/sample.cpp' -or
            $nestedPaths -notcontains 'scripts/direct-match.ps1') {
            throw "Nested package inventory did not scan its own source tree: $($nestedPaths -join ', ')"
        }
    } finally {
        $resolvedNestedRoot = [IO.Path]::GetFullPath($nestedRoot)
        if (-not $resolvedNestedRoot.StartsWith($nestedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Refusing to remove a fixture outside its build workspace'
        }
        Remove-Item -LiteralPath $resolvedNestedRoot -Recurse -Force -ErrorAction SilentlyContinue
    }

    foreach ($path in @(
        "CMakeLists.txt", "include/protodd/sample.hpp", "src/core/sample.cpp",
        "scripts/build-tournament.ps1", "scripts/TournamentManifest.psm1",
        "scripts/MatchProvenance.psm1", "scripts/prepare-patched-runtime.ps1",
        "scripts/start-owned-starcraft.ps1", "scripts/stop-owned-starcraft.ps1",
        "training/compare_replay.mjs",
        "bwapi-data/read/WholeGame-mode.txt",
        "bwapi-data/write/Protodd-private.csv", "tools/__pycache__/ignored.pyc",
        "replays/local.rep", "artifacts/local.zip"
    )) {
        $target = Join-Path $fixture $path
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        [IO.File]::WriteAllText($target, "fixture:$path")
    }

    $inventory = @(Get-TournamentSourceInventory -RepositoryRoot $fixture)
    $paths = @($inventory | ForEach-Object { $_.path })
    if ($paths -contains "bwapi-data/read/WholeGame-mode.txt" -or
        $paths -contains "bwapi-data/write/Protodd-private.csv" -or
        $paths -contains "tools/__pycache__/ignored.pyc" -or
        $paths -contains "replays/local.rep" -or $paths -contains "artifacts/local.zip") {
        throw "Package inventory included runtime state or local data: $($paths -join ', ')"
    }
    if ($paths -notcontains "include/protodd/sample.hpp" -or
        $paths -notcontains "scripts/build-tournament.ps1" -or
        $paths -notcontains "scripts/start-owned-starcraft.ps1" -or
        $paths -notcontains "scripts/stop-owned-starcraft.ps1" -or
        $paths -notcontains "scripts/prepare-patched-runtime.ps1" -or
        $paths -notcontains "scripts/MatchProvenance.psm1" -or
        $paths -notcontains "training/compare_replay.mjs") {
        throw "Package inventory omitted required source inputs: $($paths -join ', ')"
    }

    $sourceManifest = [pscustomobject]@{
        source_files = $inventory
        source_snapshot_sha256 = Get-TournamentInventoryHash -Inventory $inventory
    }
    Assert-TournamentSourceMatchesManifest -Manifest $sourceManifest -RepositoryRoot $fixture
    [IO.File]::AppendAllText((Join-Path $fixture "src/core/sample.cpp"), "changed")
    $sourceRejected = $false
    try { Assert-TournamentSourceMatchesManifest -Manifest $sourceManifest -RepositoryRoot $fixture }
    catch { $sourceRejected = $true }
    if (-not $sourceRejected) { throw "Source changes were not rejected after the build snapshot" }

    $dll = Join-Path $fixture "Protodd.dll"
    [IO.File]::WriteAllBytes($dll, [byte[]](1, 2, 3, 4))
    $dllManifest = [pscustomobject]@{ dll_sha256 = (Get-FileHash $dll -Algorithm SHA256).Hash }
    Assert-TournamentDllMatchesManifest -Manifest $dllManifest -DllPath $dll
    [IO.File]::WriteAllBytes($dll, [byte[]](4, 3, 2, 1))
    $dllRejected = $false
    try { Assert-TournamentDllMatchesManifest -Manifest $dllManifest -DllPath $dll }
    catch { $dllRejected = $true }
    if (-not $dllRejected) { throw "A DLL substitution was not rejected" }
    if ((Test-TournamentManifestPath -Path "../outside") -or
        (Test-TournamentManifestPath -Path "C:/outside") -or
        (-not (Test-TournamentManifestPath -Path "src/core/sample.cpp"))) {
        throw "Manifest path validation accepted an unsafe path or rejected a source path"
    }
    Write-Output "Tournament package provenance checks passed"
} finally {
    if (Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force }
}
