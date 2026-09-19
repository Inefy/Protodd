param([Parameter(Mandatory=$true)][string]$Label)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if ($Label -notmatch '^[A-Za-z0-9_-]+$') { throw 'Invalid label' }
$run = Join-Path $repo "build/strength-20260918/$Label"
if (-not (Test-Path -LiteralPath "$run/manifest.json")) { throw 'Prepare experiment first' }
if (Get-Process StarCraft -ErrorAction SilentlyContinue) { throw 'A StarCraft game is already running' }
if (Test-Path -LiteralPath "$run/server/results.jsonl") { throw 'Existing results: prepare an explicit resume before relaunching' }
if (Get-NetTCPConnection -LocalPort 1347 -State Listen -ErrorAction SilentlyContinue) { throw 'Experiment server port is already in use' }
$javaBin = Join-Path (Split-Path -Parent $repo) 'tools/java/jdk8u504-b01/bin'
$env:PATH = "$javaBin;$env:PATH"
$java = Join-Path $javaBin 'java.exe'
$processes = @()
foreach ($component in @('server','client1','client2')) {
    $dir = Join-Path $run $component
    $kind = if ($component -eq 'server') { 'server' } else { 'client' }
    # The user requested headed games; Java's tournament GUI remains visible.
    $process = Start-Process -FilePath $java -ArgumentList @('-jar',"$kind.jar", "${kind}_settings.json") `
        -WorkingDirectory $dir -RedirectStandardOutput "$dir/stdout.log" `
        -RedirectStandardError "$dir/stderr.log" -PassThru
    $processes += [ordered]@{component=$component; pid=$process.Id; started=$process.StartTime.ToString('o')}
    if ($component -eq 'server') { Start-Sleep -Seconds 3 }
}
$processes | ConvertTo-Json | Set-Content -LiteralPath "$run/processes.json"
$processes | Format-Table
