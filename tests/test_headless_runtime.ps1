param([Parameter(Mandatory)][string]$Fixture)
$ErrorActionPreference = 'Stop'
Add-Type -Path (Join-Path $PSScriptRoot '../scripts/HeadlessStarCraft.cs')
$fixtureExe = [IO.Path]::GetFullPath($Fixture)
$testRoot = Join-Path (Split-Path -Parent $fixtureExe) ('headless-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
# Keep diagnostics on failure so the native fault remains inspectable.
foreach($mode in @('clean','crash')) {
    $root = Join-Path $testRoot $mode
    New-Item -ItemType Directory -Path $root | Out-Null
    $desktopPath = Join-Path $root 'desktop.txt'
    $diagnostics = Join-Path $root 'Errors'
    $launch = [Protodd.Runtime.HeadlessStarCraft]::Launch($fixtureExe, ('"' + $desktopPath + '" ' + $mode), $root, $diagnostics)
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    $exitLog = Join-Path $diagnostics 'native-exits.txt'
    while (!(Test-Path -LiteralPath $exitLog) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
    if(!(Test-Path -LiteralPath $exitLog)) { throw "Headless $mode fixture did not exit; see $diagnostics" }
    if((Get-Content -Raw -LiteralPath $desktopPath).Trim() -ne $launch.DesktopName) {
        throw 'Fixture did not run on its assigned private desktop'
    }
    if($mode -eq 'clean') {
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
    if(Test-Path -LiteralPath (Join-Path $diagnostics 'monitor-error.txt')) { throw 'Native crash monitor failed' }
}
"Headless desktop, clean exit, native crash attribution and minidump verified: $testRoot"
