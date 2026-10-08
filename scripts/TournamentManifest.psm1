Set-StrictMode -Version Latest

$script:SourceDirectories = @('cmake', 'include', 'src', 'tests', 'tools', 'training', 'docs', 'bwapi-data')
$script:SourceFiles = @(
    'CMakeLists.txt', 'CMakePresets.json', 'LICENSE', 'README.md', 'SUBMISSION.md'
)
$script:SourceScripts = @(
    'scripts/direct-match.ps1',
    'scripts/build-tournament.ps1',
    'scripts/build-trained-controller.ps1',
    'scripts/build-opening-comparison.ps1',
    'scripts/build-bwapi-runtime.ps1',
    'scripts/package-tournament.ps1',
    'scripts/prepare-patched-runtime.ps1',
    'scripts/prepare-stock-runtime.ps1',
    'scripts/restore-match-runtime.ps1',
    'scripts/verify-runtime-profile.ps1',
    'scripts/start-owned-starcraft.ps1',
    'scripts/stop-owned-starcraft.ps1',
    'scripts/verify.ps1',
    'scripts/TournamentManifest.psm1',
    'scripts/RuntimeProfiles.psm1',
    'scripts/MatchProvenance.psm1',
    'scripts/MatchProcessOwnership.psm1',
    'scripts/MatchResourceBudget.psm1',
    'scripts/HeadlessStarCraft.psm1',
    'scripts/HeadlessStarCraft.cs',
    'scripts/run-headless-starcraft.ps1',
    'scripts/run-audit-load.ps1',
    'scripts/run-audit-scenarios.ps1',
    'scripts/start-arena.ps1',
    'scripts/start-strength-experiment.ps1',
    'scripts/HeadlessCampaign.psm1'
)

function Test-TournamentSourcePath {
    param([Parameter(Mandatory)][string]$Path)

    $normalized = $Path.Replace('\', '/').TrimStart('./')
    if ($normalized -match '(^|/)__pycache__(/|$)|\.(pyc|pyo)$') { return $false }
    if ($normalized -match '^bwapi-data/(read|write|logs)(/|$)') { return $false }
    if ($script:SourceFiles -contains $normalized -or $script:SourceScripts -contains $normalized) {
        return $true
    }
    foreach ($directory in $script:SourceDirectories) {
        if ($normalized.StartsWith("$directory/", [StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
    }
    return $false
}

function Test-TournamentRepositoryRoot {
    param([Parameter(Mandatory)][string]$RepositoryRoot)

    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) { return $false }
    $root = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\', '/')
    $top = & $git.Source -C $root rev-parse --show-toplevel 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $top) { return $false }
    $gitRoot = [IO.Path]::GetFullPath(([string]$top).Trim()).TrimEnd('\', '/')
    return [StringComparer]::OrdinalIgnoreCase.Equals($root, $gitRoot)
}

function Get-TournamentSourceInventory {
    param([Parameter(Mandatory)][string]$RepositoryRoot)

    $root = [IO.Path]::GetFullPath($RepositoryRoot)
    $paths = @()
    $git = Get-Command git -ErrorAction SilentlyContinue
    if ($git -and (Test-TournamentRepositoryRoot -RepositoryRoot $root)) {
        $specs = @($script:SourceFiles) + $script:SourceDirectories + @('scripts')
        $gitPaths = & $git.Source -C $root ls-files --cached --others --exclude-standard -- @specs
        if ($LASTEXITCODE -eq 0) {
            $paths = @($gitPaths | ForEach-Object { $_.Replace('\', '/') } |
                Where-Object { Test-TournamentSourcePath -Path $_ })
        }
    }

    if ($paths.Count -eq 0) {
        $found = [Collections.Generic.List[string]]::new()
        foreach ($file in $script:SourceFiles) {
            if (Test-Path -LiteralPath (Join-Path $root $file) -PathType Leaf) {
                $found.Add($file)
            }
        }
        foreach ($directory in $script:SourceDirectories) {
            $directoryPath = Join-Path $root $directory
            if (Test-Path -LiteralPath $directoryPath -PathType Container) {
                foreach ($item in Get-ChildItem -LiteralPath $directoryPath -File -Recurse) {
                    $found.Add($item.FullName.Substring($root.Length).TrimStart('\', '/').Replace('\', '/'))
                }
            }
        }
        foreach ($file in $script:SourceScripts) {
            if (Test-Path -LiteralPath (Join-Path $root $file) -PathType Leaf) {
                $found.Add($file)
            }
        }
        $paths = @($found | Where-Object { Test-TournamentSourcePath -Path $_ })
    }

    $uniquePaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $inventory = [Collections.Generic.List[object]]::new()
    foreach ($relative in $paths) {
        $path = $relative.Replace('\', '/').TrimStart('./')
        if (-not (Test-TournamentSourcePath -Path $path) -or -not $uniquePaths.Add($path)) { continue }
        $absolute = [IO.Path]::GetFullPath((Join-Path $root $path))
        $prefix = $root.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
        if (-not $absolute.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Source path escapes the repository: $path"
        }
        if (-not (Test-Path -LiteralPath $absolute -PathType Leaf)) { continue }
        $inventory.Add([ordered]@{
            path = $path
            sha256 = (Get-FileHash -LiteralPath $absolute -Algorithm SHA256).Hash.ToLowerInvariant()
            size_bytes = (Get-Item -LiteralPath $absolute).Length
        })
    }

    $items = @($inventory.ToArray())
    [Array]::Sort($items, [Comparison[object]]{
        param($left, $right)
        return [StringComparer]::Ordinal.Compare([string]$left.path, [string]$right.path)
    })
    return $items
}

function Get-TournamentInventoryHash {
    param([Parameter(Mandatory)][object[]]$Inventory)

    $items = @($Inventory)
    [Array]::Sort($items, [Comparison[object]]{
        param($left, $right)
        return [StringComparer]::Ordinal.Compare([string]$left.path, [string]$right.path)
    })
    $content = [Text.StringBuilder]::new()
    foreach ($item in $items) {
        [void]$content.Append([string]$item.path).Append("`t").Append(
            ([string]$item.sha256).ToLowerInvariant()).Append("`n")
    }
    $bytes = [Text.Encoding]::UTF8.GetBytes($content.ToString())
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return [BitConverter]::ToString($algorithm.ComputeHash($bytes)).Replace('-', '').ToLowerInvariant()
    }
    finally { $algorithm.Dispose() }
}

function Get-TournamentSourceCommit {
    param([Parameter(Mandatory)][string]$RepositoryRoot)

    $git = Get-Command git -ErrorAction SilentlyContinue
    if ($git -and (Test-TournamentRepositoryRoot -RepositoryRoot $RepositoryRoot)) {
        $commit = & $git.Source -C $RepositoryRoot rev-parse HEAD 2>$null
        if ($LASTEXITCODE -eq 0 -and $commit) { return ([string]$commit).Trim() }
    }
    return 'source-archive'
}

function Test-TournamentManifestPath {
    param([Parameter(Mandatory)][string]$Path)

    $normalized = $Path.Replace('\', '/')
    if ([string]::IsNullOrWhiteSpace($normalized) -or $normalized.StartsWith('/') -or
        $normalized -match '^[A-Za-z]:' -or $normalized.Split('/') -contains '..') {
        return $false
    }
    return $true
}

function Assert-TournamentSourceMatchesManifest {
    param(
        [Parameter(Mandatory)][object]$Manifest,
        [Parameter(Mandatory)][string]$RepositoryRoot
    )

    if (-not $Manifest.source_files -or -not $Manifest.source_snapshot_sha256) {
        throw 'Build manifest does not contain a complete source inventory'
    }
    $current = @(Get-TournamentSourceInventory -RepositoryRoot $RepositoryRoot)
    $currentHash = Get-TournamentInventoryHash -Inventory $current
    if ($currentHash -ne [string]$Manifest.source_snapshot_sha256) {
        throw 'Current source snapshot differs from the DLL build manifest'
    }
    if ($current.Count -ne @($Manifest.source_files).Count) {
        throw 'Current source inventory count differs from the DLL build manifest'
    }
    for ($index = 0; $index -lt $current.Count; $index++) {
        if ($current[$index].path -cne $Manifest.source_files[$index].path -or
            $current[$index].sha256 -cne $Manifest.source_files[$index].sha256) {
            throw "Current source file differs from the DLL build manifest: $($current[$index].path)"
        }
    }
}

function Assert-TournamentDllMatchesManifest {
    param(
        [Parameter(Mandatory)][object]$Manifest,
        [Parameter(Mandatory)][string]$DllPath
    )

    if (-not (Test-Path -LiteralPath $DllPath -PathType Leaf)) { throw "DLL is missing: $DllPath" }
    $hash = (Get-FileHash -LiteralPath $DllPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne ([string]$Manifest.dll_sha256).ToLowerInvariant()) {
        throw 'DLL hash differs from the successful build manifest'
    }
}

function Assert-TournamentFeatureManifest {
    param(
        [Parameter(Mandatory)][object]$Manifest,
        [Parameter(Mandatory)][string]$RepositoryRoot
    )

    $root = [IO.Path]::GetFullPath($RepositoryRoot)
    $registryPath = Join-Path $root 'docs/feature-registry.json'
    if (-not (Test-Path -LiteralPath $registryPath -PathType Leaf)) {
        throw 'The registered feature inventory is missing from the source tree'
    }
    $registryHash = (Get-FileHash -LiteralPath $registryPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $registry = Get-Content -LiteralPath $registryPath -Raw | ConvertFrom-Json
    if ($registry.schema -ne 'protodd-feature-registry-v1' -or
        $Manifest.feature_registry_sha256 -ne $registryHash) {
        throw 'Build manifest is not bound to the current feature registry'
    }
    $active = $Manifest.registered_feature_manifest
    if (-not $active -or $active.schema -ne 'protodd-active-features-v1' -or
        $active.registry_sha256 -ne $registryHash) {
        throw 'Build manifest has no complete registered feature snapshot'
    }

    foreach ($entry in $registry.options) {
        $matches = @($active.build_options | Where-Object { $_.id -ceq $entry.id })
        if ($matches.Count -ne 1 -or
            [string]$matches[0].value -cne [string]$entry.tournament_value -or
            [string]$matches[0].tournament_value -cne [string]$entry.tournament_value) {
            throw "Tournament package contains an unapproved or unregistered build feature: $($entry.id)"
        }
    }
    if (@($active.build_options).Count -ne @($registry.options).Count) {
        throw 'Build manifest has missing or extra registered CMake options'
    }

    foreach ($entry in $registry.build_inputs) {
        $matches = @($active.build_inputs | Where-Object { $_.id -ceq $entry.id })
        if ($matches.Count -ne 1 -or
            [string]$matches[0].value -cne [string]$entry.tournament_value -or
            [string]$matches[0].tournament_value -cne [string]$entry.tournament_value) {
            throw "Tournament package contains an unapproved or unregistered build input: $($entry.id)"
        }
    }
    if (@($active.build_inputs).Count -ne @($registry.build_inputs).Count) {
        throw 'Build manifest has missing or extra registered CMake inputs'
    }

    foreach ($entry in $registry.runtime_controls) {
        $matches = @($active.runtime_controls | Where-Object { $_.id -ceq $entry.id })
        if ($matches.Count -ne 1 -or [string]$matches[0].file -cne [string]$entry.file -or
            @($matches[0].allowed_values).Count -ne @($entry.allowed_values).Count) {
            throw "Build manifest has an incomplete runtime feature contract: $($entry.id)"
        }
        foreach ($value in $entry.allowed_values) {
            if (@($matches[0].allowed_values) -cnotcontains $value) {
                throw "Build manifest has a stale runtime feature contract: $($entry.id)"
            }
        }
    }
    if (@($active.runtime_controls).Count -ne @($registry.runtime_controls).Count) {
        throw 'Build manifest has missing or extra runtime feature controls'
    }
}

function Assert-TournamentFeatureRegistry {
    param([Parameter(Mandatory)][string]$RepositoryRoot)

    $validator = Join-Path ([IO.Path]::GetFullPath($RepositoryRoot)) 'tools/feature_registry.py'
    if (-not (Test-Path -LiteralPath $validator -PathType Leaf)) {
        throw 'The feature registry validator is missing from the source tree'
    }
    $python = Get-Command python -ErrorAction SilentlyContinue
    if (-not $python) { throw 'Python is required to validate the tournament feature registry' }
    $output = & $python.Source $validator 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "The tournament feature registry is stale or invalid:`n$output"
    }
}

Export-ModuleMember -Function @(
    'Test-TournamentSourcePath',
    'Get-TournamentSourceInventory',
    'Get-TournamentInventoryHash',
    'Get-TournamentSourceCommit',
    'Test-TournamentManifestPath',
    'Assert-TournamentSourceMatchesManifest',
    'Assert-TournamentDllMatchesManifest',
    'Assert-TournamentFeatureManifest',
    'Assert-TournamentFeatureRegistry'
)
