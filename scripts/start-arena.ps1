param([Parameter(Mandatory=$true)][string]$Run)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$arenaRun = (Resolve-Path -LiteralPath $Run).Path
if (Get-Process StarCraft -ErrorAction SilentlyContinue) { throw 'A StarCraft game is already running; leave its runtimes untouched' }
if (Test-Path -LiteralPath (Join-Path $arenaRun 'processes.json')) { throw 'Existing launch record; inspect its process handles before recovery' }
if (Test-Path -LiteralPath (Join-Path $arenaRun 'server/results.jsonl')) { throw 'Existing results; an explicit continuation must preserve completed games' }
Push-Location $repo
try {
    & "$repo/build/model-venv/Scripts/python.exe" -m training.arena verify $arenaRun
    if ($LASTEXITCODE -ne 0) { throw 'Campaign verification failed' }
} finally { Pop-Location }
$settings = Get-Content -LiteralPath (Join-Path $arenaRun 'server/server_settings.json') -Raw | ConvertFrom-Json
if (Get-NetTCPConnection -LocalPort $settings.serverPort -State Listen -ErrorAction SilentlyContinue) { throw 'Arena server port is already occupied' }
$javaLauncher = (Get-Command java -ErrorAction Stop).Source
# Oracle javapath is a launcher that creates another java.exe. Start the actual
# JVM so processes.json contains live worker handles, not short-lived wrappers.
$javaProperty = & $javaLauncher -XshowSettings:properties -version 2>&1 |
    ForEach-Object { [string]$_ } | Where-Object { $_ -match '^\s*java.home\s*=' } | Select-Object -First 1
if (-not $javaProperty) { throw 'Cannot resolve the installed Java runtime' }
$javaRuntimeRoot = ($javaProperty -replace '^\s*java.home\s*=\s*','').Trim()
$arenaJava = Join-Path $javaRuntimeRoot 'bin/java.exe'
if (-not (Test-Path -LiteralPath $arenaJava -PathType Leaf)) { throw 'Resolved Java executable is missing' }
$processes = @()
foreach ($component in @('server','client1','client2')) {
    $directory = Join-Path $arenaRun $component
    $kind = if ($component -eq 'server') { 'server' } else { 'client' }
    $process = Start-Process -FilePath $arenaJava -ArgumentList '-jar',"$kind.jar","${kind}_settings.json" `
        -WorkingDirectory $directory -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $directory 'stdout.log') `
        -RedirectStandardError (Join-Path $directory 'stderr.log')
    $processes += [ordered]@{component=$component; pid=$process.Id; started=$process.StartTime.ToString('o')}
    $processes | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $arenaRun 'processes.json')
    if ($component -eq 'server') {
        Start-Sleep -Seconds 3
        if ($process.HasExited) { throw 'Arena server exited; inspect server/stderr.log' }
    }
}
$processes | Format-Table
