Set-StrictMode -Version Latest

function ConvertTo-OwnershipUtcTime {
    param([Parameter(Mandatory = $true)][object]$Value)

    if ($Value -is [DateTimeOffset]) { return $Value.UtcDateTime }
    if ($Value -is [DateTime]) { return $Value.ToUniversalTime() }
    return [DateTimeOffset]::Parse(
        [string]$Value, [Globalization.CultureInfo]::InvariantCulture
    ).UtcDateTime
}

function Get-OwnedStarCraftProcesses {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Processes,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][int[]]$RootProcessIds,
        [int[]]$KnownProcessIds = @(),
        [int[]]$BaselineProcessIds = @(),
        [Parameter(Mandatory = $true)][DateTime]$StartedAtUtc
    )

    $sessionStart = ConvertTo-OwnershipUtcTime -Value $StartedAtUtc
    $knownParents = [Collections.Generic.HashSet[int]]::new()
    foreach ($rootId in $RootProcessIds) { [void]$knownParents.Add([int]$rootId) }
    foreach ($knownId in $KnownProcessIds) { [void]$knownParents.Add([int]$knownId) }
    $baseline = [Collections.Generic.HashSet[int]]::new()
    foreach ($baselineId in $BaselineProcessIds) { [void]$baseline.Add([int]$baselineId) }

    $eligible = [Collections.Generic.List[object]]::new()
    foreach ($process in $Processes) {
        if ($null -eq $process.ProcessId -or $null -eq $process.ParentProcessId -or
            $null -eq $process.CreationDate) { continue }
        $processId = [int]$process.ProcessId
        if ($baseline.Contains($processId)) { continue }
        try { $created = ConvertTo-OwnershipUtcTime -Value $process.CreationDate }
        catch { continue }
        # WMI's creation timestamp can differ slightly from the local launch
        # timestamp. Keep a small boundary allowance while requiring a parent
        # chain rooted in a launcher owned by this match.
        if ($created -lt $sessionStart.AddSeconds(-2)) { continue }
        $eligible.Add([pscustomobject]@{
            process_id = $processId
            parent_process_id = [int]$process.ParentProcessId
            created_utc = $created
        })
    }

    $owned = [Collections.Generic.Dictionary[int, object]]::new()
    $changed = $true
    while ($changed) {
        $changed = $false
        foreach ($process in $eligible) {
            if ($owned.ContainsKey($process.process_id)) { continue }
            if (-not $knownParents.Contains($process.parent_process_id)) { continue }
            $owned[$process.process_id] = [pscustomobject]@{
                process_id = $process.process_id
                parent_process_id = $process.parent_process_id
                created_utc = $process.created_utc.ToString('o')
            }
            [void]$knownParents.Add($process.process_id)
            $changed = $true
        }
    }

    return @($owned.Values | Sort-Object { [int]$_.process_id })
}

function Enter-MatchRuntimeLock {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$RuntimeRoot,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $root = [IO.Path]::GetFullPath($RuntimeRoot)
    if (-not (Test-Path -LiteralPath $root -PathType Container)) {
        throw "Runtime root does not exist; prepare it before locking: $root"
    }
    $path = Join-Path $root '.protodd-match.lock'
    try {
        $stream = [IO.File]::Open(
            $path,
            [IO.FileMode]::OpenOrCreate,
            [IO.FileAccess]::ReadWrite,
            [IO.FileShare]::None
        )
    } catch [IO.IOException] {
        throw "Runtime is already owned by another match: $root"
    }

    try {
        $stream.SetLength(0)
        $metadata = [ordered]@{
            pid = $PID
            label = $Label
            runtime_root = $root
            acquired_utc = [DateTime]::UtcNow.ToString('o')
        } | ConvertTo-Json -Compress
        $bytes = [Text.Encoding]::UTF8.GetBytes($metadata)
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
        return [pscustomobject]@{ path = $path; stream = $stream }
    } catch {
        $stream.Dispose()
        throw
    }
}

function Exit-MatchRuntimeLock {
    [CmdletBinding()]
    param([Parameter(Mandatory = $false)][object]$Lock)

    if ($null -ne $Lock -and $null -ne $Lock.stream) {
        $Lock.stream.Dispose()
    }
}

Export-ModuleMember -Function @(
    'Get-OwnedStarCraftProcesses',
    'Enter-MatchRuntimeLock',
    'Exit-MatchRuntimeLock'
)
