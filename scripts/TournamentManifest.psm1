Set-StrictMode -Version Latest

$script:SourceDirectories = @('cmake', 'include', 'src', 'tests', 'tools', 'training', 'docs', 'bwapi-data')
$script:SourceFiles = @(
    'CMakeLists.txt', 'CMakePresets.json', 'LICENSE', 'README.md', 'SUBMISSION.md'
)
$script:SourceScripts = @(
    'scripts/direct-match.ps1',
    'scripts/build-tournament.ps1',
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
    'scripts/MatchProvenance.psm1'
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

Export-ModuleMember -Function @(
    'Test-TournamentSourcePath',
    'Get-TournamentSourceInventory',
    'Get-TournamentInventoryHash',
    'Get-TournamentSourceCommit',
    'Test-TournamentManifestPath',
    'Assert-TournamentSourceMatchesManifest',
    'Assert-TournamentDllMatchesManifest'
)
