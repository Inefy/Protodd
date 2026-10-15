param([Parameter(Mandatory)][string]$Fixture)
$ErrorActionPreference = 'Stop'
Add-Type -Path (Join-Path $PSScriptRoot '../scripts/HeadlessStarCraft.cs')
$fixtureExe = [IO.Path]::GetFullPath($Fixture)
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Text;
using System.Runtime.InteropServices;
public static class ResolvedFixturePath {
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern uint GetFinalPathNameByHandle(IntPtr file,StringBuilder path,uint size,uint flags);
 public static string Resolve(string path) {
  using(var file=File.OpenRead(path)) {
   var resolved=new StringBuilder(32768);
   if(GetFinalPathNameByHandle(file.SafeFileHandle.DangerousGetHandle(),resolved,(uint)resolved.Capacity,0)==0)throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
   return resolved.ToString().Replace("\\\\?\\", "");
  }
 }
}
'@
$fixtureExe=[ResolvedFixturePath]::Resolve($fixtureExe)
$testRoot = Join-Path (Split-Path -Parent $fixtureExe) ('headless-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
Import-Module (Join-Path $PSScriptRoot '../scripts/HeadlessStarCraft.psm1') -Force
# Exercise the same nested launch boundary as the owned StarCraft monitor.
# A spaced path and literal shell syntax also verify ArgumentList transport.
$probeScript = Join-Path $testRoot 'console probe.ps1'
$probeReceipt = Join-Path $testRoot 'console probe.json'
@'
param([string]$Receipt, [string]$Sentinel, [switch]$Capture)
$ErrorActionPreference='Stop'
Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public static class NestedConsoleProbe { [DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow(); }'
[ordered]@{console_window_handle=[NestedConsoleProbe]::GetConsoleWindow().ToInt64(); sentinel=$Sentinel} | ConvertTo-Json | Set-Content -LiteralPath $Receipt
if (!$Capture) { return }
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
# Alternate blocks larger than a pipe buffer on both streams. A sequential
# reader would stall before this process could exit.
foreach ($index in 0..127) {
    [Console]::Out.Write(('stdout-λ-' * 1024))
    [Console]::Error.Write(('stderr-λ-' * 1024))
}
'@ | Set-Content -LiteralPath $probeScript
$sentinel = 'spaces "quotes" $() & literal'
$stdoutPath = Join-Path $testRoot 'stdout capture.log'
$stderrPath = Join-Path $testRoot 'stderr capture.log'
$probe = Start-ConsoleFreeProcess -FilePath ([Diagnostics.Process]::GetCurrentProcess().MainModule.FileName) -Arguments @('-NoLogo','-NoProfile','-NonInteractive','-File',$probeScript,'-Receipt',$probeReceipt,'-Sentinel',$sentinel,'-Capture') -WorkingDirectory $testRoot -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
if (!$probe.WaitForExit(15000)) { throw 'Nested console probe did not exit' }
if ($probe.ExitCode -ne 0) { throw 'Nested console probe failed' }
if (!$probe.OutputCompletion.Wait(5000)) { throw 'Console-free helper output did not drain' }
if ([IO.File]::ReadAllText($stdoutPath) -cne ('stdout-λ-' * (1024 * 128))) { throw 'Console-free stdout capture changed or lost bytes' }
if ([IO.File]::ReadAllText($stderrPath) -cne ('stderr-λ-' * (1024 * 128))) { throw 'Console-free stderr capture changed or lost bytes' }
$receipt = Get-Content -Raw -LiteralPath $probeReceipt | ConvertFrom-Json
if ($receipt.console_window_handle -ne 0) { throw 'Headless monitor helper allocated a console' }
if ($receipt.sentinel -cne $sentinel) { throw 'Console-free launch changed a literal argument' }
$probe.Dispose()
# The real owned monitor uses this same launcher without output redirection.
$monitorReceipt = Join-Path $testRoot 'monitor-style probe.json'
$monitorProbe = Start-ConsoleFreeProcess -FilePath ([Diagnostics.Process]::GetCurrentProcess().MainModule.FileName) -Arguments @('-NoLogo','-NoProfile','-NonInteractive','-File',$probeScript,'-Receipt',$monitorReceipt,'-Sentinel',$sentinel) -WorkingDirectory $testRoot
if (!$monitorProbe.WaitForExit(15000) -or $monitorProbe.ExitCode -ne 0) { throw 'Monitor-style console probe failed' }
$monitorResult = Get-Content -Raw -LiteralPath $monitorReceipt | ConvertFrom-Json
if ($monitorResult.console_window_handle -ne 0 -or $monitorResult.sentinel -cne $sentinel) { throw 'Monitor-style launch allocated a console or changed its argument' }
$monitorProbe.Dispose()
# Reproduce the observed StarCraft permission boundary on an owned helper:
# query access is denied while minimal termination remains permitted.
Import-Module (Join-Path $PSScriptRoot '../scripts/MatchProcessOwnership.psm1') -Force
$restrictedScript=Join-Path $testRoot 'query restricted.ps1'
$restrictedReceipt=Join-Path $testRoot 'query restricted.json'
@'
param([string]$Receipt)
$ErrorActionPreference='Stop'
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class RestrictedQueryFixture {
 [DllImport("advapi32.dll",CharSet=CharSet.Unicode,SetLastError=true)] public static extern bool ConvertStringSecurityDescriptorToSecurityDescriptor(string text,uint revision,out IntPtr descriptor,out uint size);
 [DllImport("advapi32.dll",SetLastError=true)] public static extern bool SetKernelObjectSecurity(IntPtr handle,uint information,IntPtr descriptor);
 [DllImport("kernel32.dll")] public static extern IntPtr GetCurrentProcess();
 [DllImport("kernel32.dll",SetLastError=true)] public static extern IntPtr OpenProcess(uint access,bool inherit,uint pid);
 [DllImport("kernel32.dll")] public static extern IntPtr LocalFree(IntPtr pointer);
 [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
}
"@
$sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$descriptor=[IntPtr]::Zero;$size=[uint32]0
if (![RestrictedQueryFixture]::ConvertStringSecurityDescriptorToSecurityDescriptor("D:(D;;0x1400;;;WD)(A;;GA;;;$sid)",1,[ref]$descriptor,[ref]$size)) { throw 'Fixture descriptor creation failed' }
try {
 if (![RestrictedQueryFixture]::SetKernelObjectSecurity([RestrictedQueryFixture]::GetCurrentProcess(),4,$descriptor)) { throw 'Fixture could not deny query access' }
} finally { [void][RestrictedQueryFixture]::LocalFree($descriptor) }
$query=[RestrictedQueryFixture]::OpenProcess(0x1000,$false,[uint32]$PID)
$errorCode=if($query -eq [IntPtr]::Zero){[Runtime.InteropServices.Marshal]::GetLastWin32Error()}else{0}
if($query -ne [IntPtr]::Zero){[void][RestrictedQueryFixture]::CloseHandle($query)}
[ordered]@{pid=$PID;query_denied=($query -eq [IntPtr]::Zero);error=$errorCode}|ConvertTo-Json|Set-Content -LiteralPath $Receipt
Start-Sleep -Seconds 60
'@ | Set-Content -LiteralPath $restrictedScript
$restricted=Start-ConsoleFreeProcess -FilePath ([Diagnostics.Process]::GetCurrentProcess().MainModule.FileName) `
    -Arguments @('-NoLogo','-NoProfile','-NonInteractive','-File',$restrictedScript,'-Receipt',$restrictedReceipt) -WorkingDirectory $testRoot
$ownedIdentity=$null
try {
    $until=[DateTime]::UtcNow.AddSeconds(15)
    while (!(Test-Path -LiteralPath $restrictedReceipt) -and [DateTime]::UtcNow -lt $until) { Start-Sleep -Milliseconds 50 }
    if (!(Test-Path -LiteralPath $restrictedReceipt)) { throw 'Query-restricted fixture did not initialize' }
    $restrictedResult=Get-Content -Raw -LiteralPath $restrictedReceipt|ConvertFrom-Json
    if (!$restrictedResult.query_denied -or $restrictedResult.error -ne 5) { throw 'Fixture did not reproduce denied query access' }
    $native=Get-CimInstance Win32_Process -Filter ('ProcessId = '+$restricted.Id)
    $ownedIdentity=[pscustomobject]@{process_id=$native.ProcessId;parent_process_id=$native.ParentProcessId;created_utc=$native.CreationDate.ToUniversalTime().ToString('o')}
    $wrongIdentity=[pscustomobject]@{process_id=$native.ProcessId;parent_process_id=$native.ParentProcessId;created_utc=$native.CreationDate.ToUniversalTime().AddSeconds(1).ToString('o')}
    $refused=$false
    try { [void](Stop-OwnedMatchProcess -Identity $wrongIdentity) } catch { $refused=$_.Exception.Message -match 'identity changed' }
    if (!$refused -or $restricted.HasExited) { throw 'Cleanup failed to preserve a mismatched process identity' }
    if (!(Stop-OwnedMatchProcess -Identity $ownedIdentity)) { throw 'Minimal owned termination was not issued' }
    if (!$restricted.WaitForExit(5000)) { throw 'Query-restricted owned helper survived termination' }
} finally {
    if (!$restricted.HasExited) {
        if ($ownedIdentity) { [void](Stop-OwnedMatchProcess -Identity $ownedIdentity) }
        else { $restricted.Kill() }
        [void]$restricted.WaitForExit(5000)
    }
    $restricted.Dispose()
}
foreach ($relativePath in @('direct-match.ps1', 'start-arena.ps1', 'start-strength-experiment.ps1', 'start-owned-starcraft.ps1')) {
    $helperPath = Join-Path $PSScriptRoot "../scripts/$relativePath"
    $parseErrors = $null
    $helperAst = [Management.Automation.Language.Parser]::ParseFile($helperPath, [ref]$null, [ref]$parseErrors)
    if ($parseErrors.Count) { throw "Headless helper has syntax errors: $relativePath" }
    $windowLaunches = @($helperAst.FindAll({ param($node)
        $node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -eq 'Start-Process'
    }, $true))
    if ($windowLaunches.Count) { throw "Headless helper still uses a console-allocating launch: $relativePath" }
}
# A unique mapping exercises the real read-only discovery and connection
# wait without touching BWAPI's global table or launching another game.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public sealed class ClientDiscoveryFixture : IDisposable {
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr CreateFileMapping(IntPtr file,IntPtr security,uint protection,uint high,uint low,string name);
 [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr MapViewOfFile(IntPtr map,uint access,uint high,uint low,UIntPtr size);
 [DllImport("kernel32.dll")] static extern bool UnmapViewOfFile(IntPtr view);
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
 IntPtr mapping,view;
 public ClientDiscoveryFixture(string name,uint pid) {
  mapping=CreateFileMapping(new IntPtr(-1),IntPtr.Zero,4,0,96,name);
  if(mapping==IntPtr.Zero)throw new Exception("Cannot create unique fixture mapping");
  view=MapViewOfFile(mapping,6,0,0,(UIntPtr)96);
  if(view==IntPtr.Zero)throw new Exception("Cannot map unique fixture table");
  Marshal.Copy(new byte[96],0,view,96);
  Set(pid,false);
 }
 public void Set(uint pid,bool connected) {Marshal.WriteInt32(view,0,unchecked((int)pid));Marshal.WriteByte(view,4,connected?(byte)1:(byte)0);}
 public void Dispose() {if(view!=IntPtr.Zero){UnmapViewOfFile(view);view=IntPtr.Zero;}if(mapping!=IntPtr.Zero){CloseHandle(mapping);mapping=IntPtr.Zero;}}
}
'@
$mappingName='Local\Protodd-client-test-'+[Guid]::NewGuid().ToString('N')
$discovery=[ClientDiscoveryFixture]::new($mappingName,[uint32]$PID)
try {
    $self=Get-CimInstance Win32_Process -Filter ('ProcessId = '+$PID)
    $identity=[pscustomobject]@{process_id=$PID;parent_process_id=$self.ParentProcessId;created_utc=$self.CreationDate.ToUniversalTime().ToString('o')}
    $rows=@([Protodd.Runtime.HeadlessStarCraft]::ReadServerConnections($mappingName))
    if($rows.Count -ne 1 -or $rows[0].ProcessId -ne $PID -or $rows[0].Connected){throw 'Read-only discovery changed fixture data'}
    $timedOut=$false
    try {[void](Wait-HeadlessProxyConnection -OpponentProcesses @($identity) -MappingName $mappingName -TimeoutSeconds 1)}
    catch {$timedOut=$_.Exception.Message -match 'did not connect'}
    if(!$timedOut){throw 'An unconnected server allowed host creation'}
    $discovery.Set([uint32]$PID,$true)
    $receipt=Wait-HeadlessProxyConnection -OpponentProcesses @($identity) -MappingName $mappingName -TimeoutSeconds 1
    if($receipt.opponent_process_id -ne $PID -or !$receipt.connected -or $receipt.host_created){throw 'Incorrect pre-host connection receipt'}
    $wrongIdentity=[pscustomobject]@{process_id=$PID;parent_process_id=$self.ParentProcessId;created_utc=$self.CreationDate.ToUniversalTime().AddSeconds(1).ToString('o')}
    $refused=$false
    try {[void](Wait-HeadlessProxyConnection -OpponentProcesses @($wrongIdentity) -MappingName $mappingName -TimeoutSeconds 1)}
    catch {$refused=$_.Exception.Message -match 'changed identity'}
    if(!$refused){throw 'Changed process identity authorized client connection'}
    $discovery.Set([uint32]($PID+1),$true)
    $refused=$false
    try {[void](Wait-HeadlessProxyConnection -OpponentProcesses @($identity) -MappingName $mappingName -TimeoutSeconds 1)}
    catch {$refused=$_.Exception.Message -match 'Unexpected BWAPI server'}
    if(!$refused){throw 'Unowned discovery server authorized host creation'}
} finally {$discovery.Dispose()}
# Keep diagnostics on failure so the native fault remains inspectable.
foreach($mode in @('clean','threads','crash')) {
    $root = Join-Path $testRoot $mode
    New-Item -ItemType Directory -Path $root | Out-Null
    $desktopPath = Join-Path $root 'desktop.txt'
    $diagnostics = Join-Path $root 'Errors'
    $launch = [Protodd.Runtime.HeadlessStarCraft]::Launch($fixtureExe, ('"' + $desktopPath + '" ' + $mode), $root, $diagnostics)
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    $exitLog = Join-Path $diagnostics ($launch.DesktopName + '-native-exits.txt')
    while (!(Test-Path -LiteralPath $exitLog) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
    if(!(Test-Path -LiteralPath $exitLog)) { throw "Headless $mode fixture did not exit; see $diagnostics" }
    while (!$launch.Completed -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 20 }
    if (!$launch.Completed) { throw "Headless $mode completion was not published" }
    if((Get-Content -Raw -LiteralPath $desktopPath).Trim() -ne $launch.DesktopName) {
        throw 'Fixture did not run on its assigned private desktop'
    }
    if($mode -ne 'crash') {
        if((Get-Content -Raw -LiteralPath $exitLog) -notmatch ',0x00000000') { throw 'Clean fixture failed' }
    } else {
        $fatal = @(Get-ChildItem -LiteralPath $diagnostics -Filter '*-fatal-*.txt')
        if($fatal.Count -ne 1) { throw "Expected one unhandled native fault; see $diagnostics" }
        $report = Get-Content -Raw -LiteralPath $fatal[0].FullName
        if($report -notmatch 'EXCEPTION: 0xc0000005' -or $report -notmatch 'dump_saved=True' -or
           !$report.Contains("module=$fixtureExe")) { throw "Incomplete native exception attribution: $report" }
        $dumps = @(Get-ChildItem -LiteralPath $diagnostics -Filter '*-fatal-*.dmp')
        if($dumps.Count -ne 1 -or [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($dumps[0].FullName),0,4) -ne 'MDMP') {
            throw 'Native crash dump is missing or invalid'
        }
    }
    if(Test-Path -LiteralPath (Join-Path $diagnostics ($launch.DesktopName + '-monitor-error.txt'))) { throw 'Native crash monitor failed' }
}
# Concurrent monitors and fresh managed events exercise handle reuse while
# Windows retires debug-event thread/process handles on exit continuation.
$concurrent = @()
foreach ($index in 0..7) {
    $root = Join-Path $testRoot "concurrent-$index"
    New-Item -ItemType Directory -Path $root | Out-Null
    $diagnostics = Join-Path $root 'Errors'
    $desktopPath = Join-Path $root 'desktop.txt'
    $launch = [Protodd.Runtime.HeadlessStarCraft]::Launch($fixtureExe, ('"' + $desktopPath + '" threads'), $root, $diagnostics)
    $concurrent += [pscustomobject]@{ Launch = $launch; Diagnostics = $diagnostics; DesktopPath = $desktopPath }
}
$deadline = [DateTime]::UtcNow.AddSeconds(30)
while (@($concurrent | Where-Object { !$_.Launch.Completed }).Count -and [DateTime]::UtcNow -lt $deadline) {
    $event = [Threading.ManualResetEvent]::new($false)
    try { $event.Set() | Out-Null; if (!$event.WaitOne(0)) { throw 'Managed event lost its signal' } }
    finally { $event.Dispose() }
    Start-Sleep -Milliseconds 10
}
foreach ($run in $concurrent) {
    if (!$run.Launch.Completed) { throw "Concurrent monitor did not complete: $($run.Diagnostics)" }
    if ((Get-Content -Raw -LiteralPath $run.DesktopPath).Trim() -ne $run.Launch.DesktopName) { throw 'Concurrent private desktop mismatch' }
    if ((Get-Content -Raw -LiteralPath (Join-Path $run.Diagnostics ($run.Launch.DesktopName + '-native-exits.txt'))) -notmatch ',0x00000000') { throw 'Concurrent fixture failed' }
    if (Test-Path -LiteralPath (Join-Path $run.Diagnostics ($run.Launch.DesktopName + '-monitor-error.txt'))) { throw 'Concurrent monitor failed' }
}
$sharedRoot = Join-Path $testRoot 'shared-diagnostics'
New-Item -ItemType Directory -Path $sharedRoot | Out-Null
$sharedDiagnostics = Join-Path $sharedRoot 'Errors'
New-Item -ItemType Directory -Path $sharedDiagnostics | Out-Null
$historicalExit = Join-Path $sharedDiagnostics 'native-exits.txt'
[IO.File]::WriteAllText($historicalExit, "99999,0xc0000005`n")
$historicalHash = (Get-FileHash -LiteralPath $historicalExit).Hash
$sessions = @()
foreach ($index in 0..1) {
    $desktopPath = Join-Path $sharedRoot "desktop-$index.txt"
    $session = [Protodd.Runtime.HeadlessStarCraft]::Launch($fixtureExe, ('"' + $desktopPath + '" clean'), $sharedRoot, $sharedDiagnostics)
    $sessions += $session
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while (!$session.Completed -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 20 }
    if (!$session.Completed) { throw 'Shared-directory native monitor did not complete' }
}
$sessionLogs = @(Get-ChildItem -LiteralPath $sharedDiagnostics -Filter '*-native-exits.txt' -File)
if ($sessionLogs.Count -ne 2 -or (Get-FileHash -LiteralPath $historicalExit).Hash -ne $historicalHash) {
    throw 'Native launch sessions reused or modified a historical exit journal'
}
foreach ($session in $sessions) {
    $sessionExit = Join-Path $sharedDiagnostics ($session.DesktopName + '-native-exits.txt')
    $expected = '{0},0x00000000' -f $session.ProcessId
    if ((Get-Content -Raw -LiteralPath $sessionExit).Trim() -ne $expected) {
        throw 'A native exit journal contains records from another launch session'
    }
}
foreach ($blockErrorReport in @($false,$true)) {
    $failureRoot = Join-Path $testRoot ('exit-journal-failure-' + $blockErrorReport)
    New-Item -ItemType Directory -Path $failureRoot | Out-Null
    $failureDiagnostics = Join-Path $failureRoot 'Errors'
    $failureDesktop = Join-Path $failureRoot 'desktop.txt'
    $failureLaunch = [Protodd.Runtime.HeadlessStarCraft]::Launch($fixtureExe, ('"' + $failureDesktop + '" delayed-exit'), $failureRoot, $failureDiagnostics)
    $ownedProcess = [Diagnostics.Process]::GetProcessById([int]$failureLaunch.ProcessId)
    try {
        # Capture the live process handle before it exits; cleanup cannot act on a
        # later reuse of the PID. A directory at the journal path forces an I/O error.
        $ownedHandle = $ownedProcess.Handle
        New-Item -ItemType Directory -Path (Join-Path $failureDiagnostics ($failureLaunch.DesktopName + '-native-exits.txt')) | Out-Null
        if ($blockErrorReport) {
            New-Item -ItemType Directory -Path (Join-Path $failureDiagnostics ($failureLaunch.DesktopName + '-monitor-error.txt')) | Out-Null
        }
        $deadline = [DateTime]::UtcNow.AddSeconds(15)
        while (!$failureLaunch.Completed -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 20 }
        if (!$failureLaunch.Failed) { throw 'A failed native exit journal was not published as a monitor failure' }
        if (!$ownedProcess.WaitForExit(5000) -or $ownedProcess.ExitCode -ne 0) {
            throw 'Diagnostic failure prevented continuation of the real clean process exit'
        }
        if (!$failureLaunch.FailureReason.Contains('native-exits.txt')) {
            throw 'A native journal failure lost its original attribution in the monitor API'
        }
        if (!$blockErrorReport -and !(Test-Path -LiteralPath (Join-Path $failureDiagnostics ($failureLaunch.DesktopName + '-monitor-error.txt')) -PathType Leaf)) {
            throw 'A native journal failure lost its diagnostic attribution'
        }
    } finally {
        if (!$ownedProcess.HasExited) { $ownedProcess.Kill(); [void]$ownedProcess.WaitForExit(5000) }
        $ownedProcess.Dispose()
    }
}
"Headless desktop, thread/handle reuse, session isolation, diagnostic failure, native crash attribution and minidump verified: $testRoot"
