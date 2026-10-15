param([Parameter(Mandatory)][string]$Bundle,
      [Parameter(Mandatory)][string]$Java,
      [Parameter(Mandatory)][string]$Javac)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot '../scripts/HeadlessCampaign.psm1') -Force
Import-Module (Join-Path $PSScriptRoot '../scripts/HeadlessStarCraft.psm1') -Force
Assert-HeadlessArenaClient -ClientDirectory $Bundle
$fixture = Join-Path (Split-Path -Parent $Bundle) ('client-smoke-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
foreach ($name in @('client.jar', 'ConsoleFreeOwnedStarCraft.exe')) {
    Copy-Item -LiteralPath (Join-Path $Bundle $name) -Destination $fixture
}
$runtime = Join-Path $fixture 'runtime λ $() & literal'
New-Item -ItemType Directory -Path $runtime | Out-Null
# The real client reads its runtime from a settings file. Read UTF-8 here too;
# this JVM's command-line decoder replaces characters outside the ANSI page.
[IO.File]::WriteAllText((Join-Path $fixture 'runtime-input.txt'), $runtime, [Text.UTF8Encoding]::new($false))
@'
public class ConsoleFreeClientSmoke {
    public static void main(String[] args) {
        String runtime;
        try {
            runtime = new String(java.nio.file.Files.readAllBytes(java.nio.file.Paths.get("runtime-input.txt")), "UTF-8");
            java.nio.file.Files.write(java.nio.file.Paths.get("java-runtime-argument.txt"), runtime.getBytes("UTF-8"));
        }
        catch (Exception e) { throw new RuntimeException(e); }
        client.ClientSettings.Instance().ClientStarcraftDir = runtime;
        if (args[0].equals("start")) client.ClientCommands.Client_StartStarcraft();
        else client.ClientCommands.Client_KillStarcraft();
    }
}
'@ | Set-Content -LiteralPath (Join-Path $fixture 'ConsoleFreeClientSmoke.java')
@'
param([string]$Runtime, [switch]$MonitorOwnedLock)
Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public static class ConsoleFreeClientProbe { [DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow(); }'
$operation = if ($MonitorOwnedLock) { 'start' } else { 'stop' }
[ordered]@{console_window_handle=[ConsoleFreeClientProbe]::GetConsoleWindow().ToInt64(); runtime=$Runtime; monitor_owned_lock=[bool]$MonitorOwnedLock; exit_code=if ($MonitorOwnedLock) {0} else {7}} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Runtime ($operation + '.json'))
# Drain both child pipes through the GUI launcher while retaining diagnostics.
foreach ($index in 1..32) {
    [Console]::Out.WriteLine('stdout-' + ('x' * 4096))
    [Console]::Error.WriteLine('stderr-' + ('y' * 4096))
}
if (!$MonitorOwnedLock) { exit 7 }
'@ | Set-Content -LiteralPath (Join-Path $fixture 'start-owned-starcraft.ps1')
Copy-Item -LiteralPath (Join-Path $fixture 'start-owned-starcraft.ps1') -Destination (Join-Path $fixture 'stop-owned-starcraft.ps1')
$compiler = Start-ConsoleFreeProcess -FilePath $Javac -Arguments @('--release','8','-classpath','client.jar','ConsoleFreeClientSmoke.java') -WorkingDirectory $fixture -RedirectStandardOutput (Join-Path $fixture 'javac.out') -RedirectStandardError (Join-Path $fixture 'javac.err')
if (!$compiler.WaitForExit(15000) -or $compiler.ExitCode -ne 0 -or !$compiler.OutputCompletion.Wait(5000)) { throw 'Client smoke compiler failed' }
$compiler.Dispose()
foreach ($operation in @('start','stop')) {
    $process = Start-ConsoleFreeProcess -FilePath $Java -Arguments @('-classpath','.;client.jar','ConsoleFreeClientSmoke',$operation) -WorkingDirectory $fixture -RedirectStandardOutput (Join-Path $fixture ($operation + '.out')) -RedirectStandardError (Join-Path $fixture ($operation + '.err'))
    if (!$process.WaitForExit(15000) -or !$process.OutputCompletion.Wait(5000)) { throw 'Console-free Java handoff stalled' }
    $expected = if ($operation -eq 'start') {0} else {1}
    if ($process.ExitCode -ne $expected) { throw "Java $operation exit did not propagate helper success/failure" }
    $process.Dispose()
    $receipt = Get-Content -Raw -LiteralPath (Join-Path $runtime ($operation + '.json')) | ConvertFrom-Json
    if ($receipt.console_window_handle -ne 0 -or $receipt.runtime -cne $runtime -or
        $receipt.monitor_owned_lock -ne ($operation -eq 'start')) { throw 'Java helper allocated a console or changed its arguments' }
    $logs = @(Get-ChildItem -LiteralPath $runtime -Filter "headless-client-$operation-*.log")
    if ($logs.Count -ne 1) { throw 'GUI helper diagnostics are missing' }
    $text = [IO.File]::ReadAllText($logs[0].FullName)
    if ([regex]::Matches($text, 'stdout-').Count -ne 32 -or [regex]::Matches($text, 'stderr-').Count -ne 32) { throw 'GUI helper lost child pipe output' }
}
# A stale or replaced helper must be rejected before the campaign starts.
foreach ($file in Get-ChildItem -LiteralPath $Bundle -File) {
    Copy-Item -LiteralPath $file.FullName -Destination $fixture -Force
}
Assert-HeadlessArenaClient -ClientDirectory $fixture
$launcher = Join-Path $fixture 'ConsoleFreeOwnedStarCraft.exe'
$bytes = [IO.File]::ReadAllBytes($launcher)
$bytes[$bytes.Length - 1] = $bytes[$bytes.Length - 1] -bxor 1
[IO.File]::WriteAllBytes($launcher, $bytes)
$rejected = $false
try { Assert-HeadlessArenaClient -ClientDirectory $fixture }
catch { $rejected = $_.Exception.Message -match 'does not match its build receipt' }
if (!$rejected) { throw 'Campaign accepted a replaced console-free executable' }
"Actual rebuilt Java start/stop handoffs have no console, preserve literal runtime paths, drain both pipes and propagate failure: $fixture"
