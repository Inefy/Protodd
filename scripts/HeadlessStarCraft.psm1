Set-StrictMode -Version Latest

function Start-HeadlessStarCraftLauncher {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Runtime,
          [string]$InjectorName = 'injectory_x86.exe',
          [switch]$LegacyWindowPlugin)
    if (-not ('Protodd.Runtime.HeadlessStarCraft' -as [type])) {
        Add-Type -Path (Join-Path $PSScriptRoot 'HeadlessStarCraft.cs')
    }
    $root = [IO.Path]::GetFullPath($Runtime)
    $injector = Join-Path $root $InjectorName
    if (-not (Test-Path -LiteralPath $injector -PathType Leaf)) { throw "Missing injector: $injector" }
    $arguments = '--launch StarCraft.exe --inject bwapi-data\BWAPI.dll'
    if ($LegacyWindowPlugin) { $arguments += ' wmode.dll' }
    $arguments += ' --set-flags SEM_NOGPFAULTERRORBOX SEM_FAILCRITICALERRORS'
    $monitor = [Protodd.Runtime.HeadlessStarCraft]::Launch($injector, $arguments, $root, (Join-Path $root 'Errors'))
    [pscustomobject]@{Id=[int]$monitor.ProcessId; Headless=$true; Desktop=$monitor.DesktopName; Monitor=$monitor}
}

# Avoid creating a console at all; hiding a newly allocated console still gives
# it desktop affinity and can briefly expose it during startup.
function Start-ConsoleFreeProcess {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$FilePath,
          [Parameter(Mandatory)][string[]]$Arguments,
          [Parameter(Mandatory)][string]$WorkingDirectory,
          [string]$RedirectStandardOutput,
          [string]$RedirectStandardError)
    if (-not ('Protodd.Runtime.ConsoleOutputCopy' -as [type])) {
        # Both pipes drain concurrently in .NET. No PowerShell event callbacks
        # or extra shell processes are needed to retain the output logs.
        Add-Type -TypeDefinition @'
using System.IO;
using System.Threading.Tasks;
namespace Protodd.Runtime {
    public static class ConsoleOutputCopy {
        public static async Task CopyAsync(Stream input, Stream output) {
            using (output) {
                await input.CopyToAsync(output).ConfigureAwait(false);
                await output.FlushAsync().ConfigureAwait(false);
            }
        }
    }
}
'@
    }
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = (Resolve-Path -LiteralPath $FilePath).Path
    $startInfo.WorkingDirectory = (Resolve-Path -LiteralPath $WorkingDirectory).Path
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    foreach ($argument in $Arguments) { $startInfo.ArgumentList.Add($argument) }
    $outputFile = $null
    $errorFile = $null
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        if ($RedirectStandardOutput) { $outputPath = [IO.Path]::GetFullPath($RedirectStandardOutput, $startInfo.WorkingDirectory) }
        if ($RedirectStandardError) { $errorPath = [IO.Path]::GetFullPath($RedirectStandardError, $startInfo.WorkingDirectory) }
        if ($RedirectStandardOutput -and $RedirectStandardError -and $outputPath -ieq $errorPath) {
            throw 'Console-free stdout and stderr require separate log files'
        }
        if ($RedirectStandardOutput) {
            $outputFile = [IO.FileStream]::new($outputPath, [IO.FileMode]::Create,
                [IO.FileAccess]::Write, [IO.FileShare]::Read, 4096, $true)
            $startInfo.RedirectStandardOutput = $true
        }
        if ($RedirectStandardError) {
            $errorFile = [IO.FileStream]::new($errorPath, [IO.FileMode]::Create,
                [IO.FileAccess]::Write, [IO.FileShare]::Read, 4096, $true)
            $startInfo.RedirectStandardError = $true
        }
        if (!$process.Start()) { throw 'Console-free process failed to start' }
        $copies = [Collections.Generic.List[Threading.Tasks.Task]]::new()
        if ($outputFile) {
            $copies.Add([Protodd.Runtime.ConsoleOutputCopy]::CopyAsync($process.StandardOutput.BaseStream, $outputFile))
            $outputFile = $null # The async copy now owns and closes this file.
        }
        if ($errorFile) {
            $copies.Add([Protodd.Runtime.ConsoleOutputCopy]::CopyAsync($process.StandardError.BaseStream, $errorFile))
            $errorFile = $null
        }
        $process | Add-Member -NotePropertyName OutputCompletion -NotePropertyValue ([Threading.Tasks.Task]::WhenAll($copies.ToArray()))
        return $process
    } catch {
        $process.Dispose()
        throw
    } finally {
        if ($outputFile) { $outputFile.Dispose() }
        if ($errorFile) { $errorFile.Dispose() }
    }
}

function Wait-HeadlessProxyConnection {
    [CmdletBinding()]
    param([Parameter(Mandatory)][object[]]$OpponentProcesses,
          [ValidateRange(1,300)][int]$TimeoutSeconds=90,
          [string]$MappingName='Local\bwapi_shared_memory_game_list')
    if (-not ('Protodd.Runtime.HeadlessStarCraft' -as [type])) {
        Add-Type -Path (Join-Path $PSScriptRoot 'HeadlessStarCraft.cs')
    }
    $until=[DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $pids=@($OpponentProcesses|ForEach-Object {[int]$_.process_id})
    while ([DateTime]::UtcNow -lt $until) {
        foreach($identity in $OpponentProcesses) {
            $current=Get-CimInstance Win32_Process -Filter ('ProcessId = '+$identity.process_id)
            if (!$current -or !(Test-OwnedMatchProcessIdentity -Identity $identity -Process $current)) {
                throw 'Proxy opponent exited or changed identity before connection'
            }
        }
        $servers=@([Protodd.Runtime.HeadlessStarCraft]::ReadServerConnections($MappingName))
        if (@($servers|Where-Object {[int]$_.ProcessId -notin $pids}).Count) {
            throw 'Unexpected BWAPI server before DLL host launch'
        }
        $connected=@($servers|Where-Object Connected)
        if ($connected.Count -eq 1) {
            return [pscustomobject]@{opponent_process_id=[int]$connected[0].ProcessId;
                connected=$true;observed_utc=[DateTime]::UtcNow.ToString('o');host_created=$false}
        }
        Start-Sleep -Milliseconds 100
    }
    throw 'Proxy did not connect to its owned opponent before DLL host launch'
}

Export-ModuleMember -Function Start-HeadlessStarCraftLauncher, Start-ConsoleFreeProcess, Wait-HeadlessProxyConnection
