param(
 [string]$Dll = 'build/test-review-20260926-win32/Release/AuditScenario.dll',
 [string]$Label = 'candidate',
 [string[]]$Cases = @('storm-allies','storm-clear','producer','prerequisite','combat'),
 [switch]$Resume
)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$root=Join-Path $repo 'build/audit-validation-20260926'
$runtime=Join-Path $root 'runtime'
if ($Label -notmatch '^[a-zA-Z0-9-]+$') { throw 'Invalid label' }
if (Get-Process StarCraft -ErrorAction SilentlyContinue) { throw 'Existing game; refusing duplicate' }
$dllPath=(Resolve-Path -LiteralPath $Dll).Path
$archive=Join-Path $root "scenarios-$Label"
if(Test-Path $archive) {
 if(-not $Resume){throw 'Scenario output already exists'}
 $prior=Get-Content (Join-Path $archive 'receipt.json') -Raw | ConvertFrom-Json
 if($prior.sha256 -ne (Get-FileHash $dllPath).Hash){throw 'Resume DLL differs'}
} else {New-Item -ItemType Directory -Path $archive | Out-Null}
Copy-Item -LiteralPath $dllPath -Destination (Join-Path $runtime 'bwapi-data/AI/AuditScenario.dll')
if(-not $Resume){[ordered]@{dll=$dllPath;sha256=(Get-FileHash $dllPath).Hash;cases=$Cases;started=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content (Join-Path $archive 'receipt.json')}
foreach($case in $Cases) {
 if($case -notin @('storm-allies','storm-clear','producer','prerequisite','combat')) { throw 'Invalid case' }
 if($Resume -and (Test-Path (Join-Path $archive "$case.csv"))){
  if(-not (Select-String -Path (Join-Path $archive "$case.csv") -Pattern '^DONE,')){throw 'Archived case incomplete'}
  if((Get-FileHash (Join-Path $archive "$case.scx")).Hash -ne (Get-FileHash (Join-Path $runtime "maps/audit/$case.scx")).Hash){throw 'Resume map differs'}
  continue
 }
 $case | Set-Content (Join-Path $runtime 'bwapi-data/read/scenario.txt') -Encoding ascii
 $ini=@"
[ai]
ai = bwapi-data/AI/AuditScenario.dll
ai_dbg = bwapi-data/AI/AuditScenario.dll
tournament =
[auto_menu]
auto_menu = LAN
lan_mode = Local PC
character_name = AstraBot
pause_dbg = OFF
auto_restart = OFF
map = maps/audit/$case.scx
mapiteration = SEQUENCE
race = Protoss
enemy_count = 1
enemy_race = Zerg
game_type = USE_MAP_SETTINGS
wait_for_min_players = 1
wait_for_max_players = 1
wait_for_time = 1000
[config]
holiday = OFF
shared_memory = ON
console_attach_on_startup = FALSE
console_alloc_on_startup = FALSE
console_attach_auto = FALSE
console_alloc_auto = FALSE
[window]
windowed = ON
left = 0
top = 0
width = 640
height = 480
[starcraft]
sound = OFF
speed_override = 0
seed_override = 202609700
drop_players = ON
[paths]
log_path = bwapi-data/logs
"@
 [IO.File]::WriteAllText((Join-Path $runtime 'bwapi-data/bwapi.ini'),$ini)
 $result=Join-Path $runtime 'bwapi-data/write/scenario.csv'
 if(Test-Path -LiteralPath $result) { throw 'Unarchived scenario result' }
 & (Join-Path $PSScriptRoot 'start-owned-starcraft.ps1') -Runtime $runtime
 $deadline=[DateTime]::UtcNow.AddMinutes(2)
 try {
  do {
   Start-Sleep -Milliseconds 500
   $done=(Test-Path $result) -and [bool](Select-String -Path $result -Pattern '^DONE,')
  } while(-not $done -and [DateTime]::UtcNow -lt $deadline)
  if(-not $done) { throw "Scenario $case did not complete" }
 } finally {
  $owned=Get-Content (Join-Path $runtime 'arena-owned-processes.json') -Raw | ConvertFrom-Json
  # Let the fixture deliver onEnd and flush before any window close. Closing
  # between DONE and onEnd can divert StarCraft into its quit dialog.
  for($wait=0;$wait -lt 20;$wait++) {
   $live=@($owned.processes | ForEach-Object {Get-Process -Id $_.pid -ErrorAction SilentlyContinue})
   if($live.Count -eq 0){break}
   Start-Sleep -Milliseconds 250
  }
  foreach($item in $owned.processes) {
   $native=Get-CimInstance Win32_Process -Filter "ProcessId=$($item.pid)"
   $expected=([DateTimeOffset]::Parse($item.created)).UtcDateTime
   if($native -and $native.ParentProcessId -eq $item.parent -and [Math]::Abs(($native.CreationDate.ToUniversalTime()-$expected).TotalMilliseconds) -lt 10) {
    [void](Get-Process -Id $item.pid).CloseMainWindow()
    Start-Sleep -Milliseconds 1000
    $still=Get-Process -Id $item.pid -ErrorAction SilentlyContinue
    if($still){[void]$still.CloseMainWindow()}
   }
  }
  Start-Sleep -Milliseconds 500
  & (Join-Path $PSScriptRoot 'stop-owned-starcraft.ps1') -Runtime $runtime
 }
 Copy-Item (Join-Path $runtime 'bwapi-data/bwapi.ini') (Join-Path $archive "$case.ini")
 Copy-Item (Join-Path $runtime "maps/audit/$case.scx") (Join-Path $archive "$case.scx")
 Move-Item -LiteralPath $result -Destination (Join-Path $archive "$case.csv")
 Get-Content (Join-Path $archive "$case.csv") | Where-Object {$_ -match 'CHECK|DONE'}
}
