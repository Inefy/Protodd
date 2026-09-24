param([Parameter(Mandatory=$true)][string]$Runtime)
$ErrorActionPreference = 'Stop'
# Exact executable ownership only. A client must never kill its peer's game.
$runtimePath = (Resolve-Path -LiteralPath $Runtime).Path
$executable = Join-Path $runtimePath 'StarCraft.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw 'Missing runtime executable' }
$records = @()
$recordPath = Join-Path $runtimePath 'arena-owned-processes.json'
if (Test-Path -LiteralPath $recordPath) {
    $record = Get-Content -LiteralPath $recordPath -Raw | ConvertFrom-Json
    if (-not [string]::Equals($record.runtime, $runtimePath, [StringComparison]::OrdinalIgnoreCase)) { throw 'Ownership runtime mismatch' }
    $records = @($record.processes)
}
foreach ($process in Get-CimInstance Win32_Process -Filter "Name='StarCraft.exe'") {
    $isOwned = $process.ExecutablePath -and [string]::Equals($process.ExecutablePath, $executable, [StringComparison]::OrdinalIgnoreCase)
    # BWAPI can hide process image information. A launch-time parent + creation
    # record still establishes ownership without guessing from a process name.
    foreach ($item in $records) {
        $recordedTime = if ($item.created -is [DateTime]) { $item.created.ToUniversalTime() } else { ([DateTimeOffset]::Parse($item.created)).UtcDateTime }
        if ($item.pid -eq $process.ProcessId -and $item.parent -eq $process.ParentProcessId -and $process.CreationDate -and
            [Math]::Abs(($process.CreationDate.ToUniversalTime() - $recordedTime).TotalMilliseconds) -lt 10) {
            $isOwned = $true
        }
    }
    if ($isOwned) {
        try {
            Stop-Process -Id $process.ProcessId -Force -ErrorAction Stop
        } catch {
            # Some tournament processes deny external termination. Do not clean
            # their files or dispatch another game while they remain alive.
            # The supervising agent can close the finished game through its UI.
            $pendingPath = Join-Path $runtimePath 'arena-cleanup-needed.json'
            [ordered]@{phase='waiting_for_owned_process_exit'; pid=$process.ProcessId;
                parent=$process.ParentProcessId; created=$process.CreationDate.ToUniversalTime().ToString('o');
                runtime=$runtimePath; reason=$_.Exception.Message} | ConvertTo-Json |
                Set-Content -LiteralPath $pendingPath
            Write-Output "Awaiting normal exit of owned StarCraft PID $($process.ProcessId); runtime files remain untouched."
            do {
                Start-Sleep -Milliseconds 500
                $live = Get-CimInstance Win32_Process -Filter "ProcessId=$($process.ProcessId)"
            } while ($live -and $live.Name -eq 'StarCraft.exe' -and $live.CreationDate -eq $process.CreationDate)
            [ordered]@{phase='owned_process_exited'; pid=$process.ProcessId; runtime=$runtimePath} | ConvertTo-Json |
                Set-Content -LiteralPath $pendingPath
        }
    }
}
