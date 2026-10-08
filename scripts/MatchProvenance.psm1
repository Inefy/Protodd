Set-StrictMode -Version Latest

function Get-MatchProvenanceSha256 {
    param([Parameter(Mandatory)][byte[]]$Bytes)

    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return [BitConverter]::ToString($algorithm.ComputeHash($Bytes)).Replace('-', '').ToLowerInvariant()
    } finally {
        $algorithm.Dispose()
    }
}

function Get-MatchConfigurationJson {
    param([Parameter(Mandatory)][object]$Configuration)

    return ConvertTo-Json -InputObject $Configuration -Depth 100 -Compress
}

function New-MatchInputManifest {
    param(
        [Parameter(Mandatory)][System.Collections.IDictionary]$Files,
        [Parameter(Mandatory)][System.Collections.IDictionary]$Configuration
    )

    if ($Files.Count -eq 0) { throw 'A match preflight must bind at least one input file' }
    $items = [Collections.Generic.List[object]]::new()
    foreach ($role in @($Files.Keys | ForEach-Object { [string]$_ } | Sort-Object -CaseSensitive)) {
        $path = [IO.Path]::GetFullPath([string]$Files[$role])
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Match preflight input is missing ($role): $path"
        }
        $file = Get-Item -LiteralPath $path
        $items.Add([ordered]@{
            role = $role
            path = $path
            sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
            size_bytes = [long]$file.Length
        })
    }
    $configurationJson = Get-MatchConfigurationJson -Configuration $Configuration
    return [ordered]@{
        schema = 'protodd-match-preflight-v1'
        created_utc = [DateTime]::UtcNow.ToString('o')
        artifacts = @($items.ToArray())
        configuration = $Configuration
        configuration_sha256 = Get-MatchProvenanceSha256 -Bytes ([Text.Encoding]::UTF8.GetBytes($configurationJson))
    }
}

function Assert-MatchInputManifest {
    param([Parameter(Mandatory)][System.Collections.IDictionary]$Manifest)

    if ($Manifest.schema -ne 'protodd-match-preflight-v1') {
        throw 'Unsupported match preflight manifest schema'
    }
    if (-not $Manifest.artifacts -or @($Manifest.artifacts).Count -eq 0) {
        throw 'Match preflight manifest has no input artifacts'
    }
    $configurationJson = Get-MatchConfigurationJson -Configuration $Manifest.configuration
    $configurationHash = Get-MatchProvenanceSha256 -Bytes ([Text.Encoding]::UTF8.GetBytes($configurationJson))
    if ($configurationHash -ne [string]$Manifest.configuration_sha256) {
        throw 'Match gameplay configuration changed after preflight was recorded'
    }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($artifact in $Manifest.artifacts) {
        $role = [string]$artifact.role
        if ([string]::IsNullOrWhiteSpace($role) -or -not $seen.Add($role)) {
            throw 'Match preflight manifest has an empty or duplicate artifact role'
        }
        $path = [string]$artifact.path
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Match preflight input disappeared before launch ($role): $path"
        }
        $file = Get-Item -LiteralPath $path
        $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -ne ([string]$artifact.sha256).ToLowerInvariant() -or
            [long]$file.Length -ne [long]$artifact.size_bytes) {
            throw "Match preflight input changed before launch ($role): $path"
        }
    }
    return $true
}

function Write-MatchAtomicCreateNew {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][byte[]]$Bytes
    )

    $destination = [IO.Path]::GetFullPath($Path)
    $temporary = Join-Path (Split-Path -Parent $destination) (
        '.' + (Split-Path -Leaf $destination) + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
    )
    $stream = $null
    try {
        $stream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write, [IO.FileShare]::None)
        $stream.Write($Bytes, 0, $Bytes.Length)
        $stream.Flush($true)
        $stream.Dispose()
        $stream = $null
        # Same-directory rename publishes a complete file and refuses to
        # replace an artifact already ingested under this identity.
        [IO.File]::Move($temporary, $destination)
    } finally {
        if ($null -ne $stream) { $stream.Dispose() }
        if (Test-Path -LiteralPath $temporary -PathType Leaf) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
}

function Write-MatchInputManifest {
    param(
        [Parameter(Mandatory)][System.Collections.IDictionary]$Manifest,
        [Parameter(Mandatory)][string]$Path
    )

    [void](Assert-MatchInputManifest -Manifest $Manifest)
    $destination = [IO.Path]::GetFullPath($Path)
    $json = (ConvertTo-Json -InputObject $Manifest -Depth 100) + [Environment]::NewLine
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($json)
    Write-MatchAtomicCreateNew -Path $destination -Bytes $bytes
    return [pscustomobject]@{
        path = $destination
        sha256 = Get-MatchProvenanceSha256 -Bytes $bytes
    }
}

function Write-MatchRecordImmutable {
    param(
        [Parameter(Mandatory)][object]$Record,
        [Parameter(Mandatory)][string]$Path
    )

    $destination = [IO.Path]::GetFullPath($Path)
    $json = (ConvertTo-Json -InputObject $Record -Depth 100) + [Environment]::NewLine
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($json)
    Write-MatchAtomicCreateNew -Path $destination -Bytes $bytes
    return [pscustomobject]@{
        path = $destination
        sha256 = Get-MatchProvenanceSha256 -Bytes $bytes
    }
}

Export-ModuleMember -Function @(
    'New-MatchInputManifest',
    'Assert-MatchInputManifest',
    'Write-MatchInputManifest',
    'Write-MatchRecordImmutable'
)
