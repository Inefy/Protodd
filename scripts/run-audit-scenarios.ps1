param(
 [string]$Dll = 'build/test-review-20260926-win32/Release/AuditScenario.dll',
 [string]$Label = 'candidate',
 [string[]]$Cases = @('storm-allies','storm-clear','producer','prerequisite','combat'),
 [switch]$Resume,
 [string]$RuntimePath = 'build/audit-validation-20260926/runtime',
 [string]$ArchiveParent = 'build/audit-validation-20260926'
)
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Import-Module (Join-Path $PSScriptRoot 'MatchProcessOwnership.psm1') -Force
$runtime=if([IO.Path]::IsPathRooted($RuntimePath)){[IO.Path]::GetFullPath($RuntimePath)}else{[IO.Path]::GetFullPath((Join-Path $repo $RuntimePath))}
$archiveRoot=if([IO.Path]::IsPathRooted($ArchiveParent)){[IO.Path]::GetFullPath($ArchiveParent)}else{[IO.Path]::GetFullPath((Join-Path $repo $ArchiveParent))}
foreach($path in @($runtime,$archiveRoot)){if(-not $path.StartsWith([IO.Path]::GetFullPath((Join-Path $repo 'build')).TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw "Audit paths must stay under build/: $path"}}
if ($Label -notmatch '^[a-zA-Z0-9-]+$') { throw 'Invalid label' }
if (Get-Process StarCraft -ErrorAction SilentlyContinue) { throw 'Existing game; refusing duplicate' }
$runtimeRoot=Split-Path -Parent $runtime
$runtimeLock=Enter-MatchRuntimeLock -RuntimeRoot $runtimeRoot -Label "audit-scenarios-$Label"
try {
$dllPath=(Resolve-Path -LiteralPath $Dll).Path
$archive=Join-Path $archiveRoot "scenarios-$Label"
if(Test-Path $archive) {
 if(-not $Resume){throw 'Scenario output already exists'}
 $prior=Get-Content (Join-Path $archive 'receipt.json') -Raw | ConvertFrom-Json
 if($prior.sha256 -ne (Get-FileHash $dllPath).Hash){throw 'Resume DLL differs'}
} else {New-Item -ItemType Directory -Path $archive | Out-Null}
Copy-Item -LiteralPath $dllPath -Destination (Join-Path $runtime 'bwapi-data/AI/AuditScenario.dll')
if(-not $Resume){[ordered]@{dll=$dllPath;sha256=(Get-FileHash $dllPath).Hash;cases=$Cases;started=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content (Join-Path $archive 'receipt.json')}
foreach($case in $Cases) {
 if($case -notin @('storm-allies','storm-clear','producer','producer-pair','cannon-blocker','resource-overlap','construction-budget','build-cancel','learned-cancel-build','builder-evacuation','builder-evacuation-started','worker-issuer','worker-local-defense','worker-mining','scout-issuer','whole-game-issuer','racebot-worker-defense','racebot-combat','command-suppression','observer-safety','fault-callback','fault-lifecycle','lifecycle-reset','lifecycle-early-exit','lifecycle-write-denied','lifecycle-output-blocked','lifecycle-truncated-history','fault-startup','fault-diagnostics','fault-diagnostics-evacuation','fault-end-history','fault-end-reporting','fault-phase','fault-observe','fault-production','fault-production-evacuation','fault-whole-game','slow-observation','slow-observation-model','pylon-loss','prerequisite','combat','combat-corpus','combat-corpus-elevation','unit-memory','footprint-native','dynamic-navigation','supply-anchor','power-recovery')) { throw 'Invalid case' }
 $mapCase=if($case -eq 'supply-anchor'){'prerequisite'}elseif($case -in @('builder-evacuation','builder-evacuation-started')){'build-cancel'}elseif($case -in @('construction-budget','learned-cancel-build','lifecycle-reset','lifecycle-early-exit','lifecycle-write-denied','lifecycle-output-blocked','lifecycle-truncated-history')){'resource-overlap'}elseif($case -in @('fault-production-evacuation','fault-diagnostics-evacuation')){'worker-local-defense'}elseif($case -in @('slow-observation','slow-observation-model')){'load'}elseif($case -in @('scout-issuer','whole-game-issuer')){'worker-mining'}elseif($case -in @('fault-callback','fault-lifecycle','fault-startup','fault-diagnostics','fault-end-history','fault-end-reporting','fault-phase','fault-observe','fault-production','fault-whole-game')){'resource-overlap'}else{$case}
 $mapPath=Join-Path $runtime "maps/audit/$mapCase.scx"
 if($Resume -and (Test-Path (Join-Path $archive "$case.csv"))){
  if(-not (Select-String -Path (Join-Path $archive "$case.csv") -Pattern '^DONE,')){throw 'Archived case incomplete'}
  if((Select-String -Path (Join-Path $archive "$case.csv") -Pattern '^CHECK,.*?,0$') -or
     -not (Select-String -Path (Join-Path $archive "$case.csv") -Pattern '^DONE,\d+,0$') -or
     -not (Select-String -Path (Join-Path $archive "$case.csv") -Pattern '^END,\d+,')){
   throw 'Archived case failed; use a new label for any justified follow-up'
  }
  if((Get-FileHash (Join-Path $archive "$case.scx")).Hash -ne (Get-FileHash $mapPath).Hash){throw 'Resume map differs'}
  continue
 }
 $case | Set-Content (Join-Path $runtime 'bwapi-data/read/scenario.txt') -Encoding ascii
 $playerRace=if($case.StartsWith('racebot-')){'Terran'}else{'Protoss'}
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
map = maps/audit/$mapCase.scx
mapiteration = SEQUENCE
race = $playerRace
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
 $result=if($case -in @('lifecycle-output-blocked','lifecycle-write-denied')){
  Join-Path $runtime 'bwapi-data/lifecycle-result.csv'
 } else { Join-Path $runtime 'bwapi-data/write/scenario.csv' }
 if(Test-Path -LiteralPath $result) { throw 'Unarchived scenario result' }
 $blockedWritePath=$null
 $blockedWriteBackupPath=$null
 if($case -eq 'lifecycle-output-blocked'){
  $blockedWritePath=Join-Path $runtime 'bwapi-data/write'
  $blockedWriteBackupPath=Join-Path $runtime "bwapi-data/write-backup-$Label"
  if(Test-Path -LiteralPath $blockedWriteBackupPath){throw 'Blocked-output backup path already exists'}
  if(Test-Path -LiteralPath $blockedWritePath){Move-Item -LiteralPath $blockedWritePath -Destination $blockedWriteBackupPath}
  try { [IO.File]::WriteAllText($blockedWritePath,'test fixture: output directory blocked') }
  catch {
   if(Test-Path -LiteralPath $blockedWriteBackupPath){Move-Item -LiteralPath $blockedWriteBackupPath -Destination $blockedWritePath}
   throw
  }
 }
 $deadline=[DateTime]::UtcNow.AddMinutes(2)
 try {
  & (Join-Path $PSScriptRoot 'start-owned-starcraft.ps1') -Runtime $runtime -RuntimeLock $runtimeLock
  do {
   Start-Sleep -Milliseconds 500
   $done=(Test-Path $result) -and [bool](Select-String -Path $result -Pattern '^DONE,')
  } while(-not $done -and [DateTime]::UtcNow -lt $deadline)
  if(-not $done) { throw "Scenario $case did not complete" }
 } finally {
  try {
   $ownedPath=Join-Path $runtime 'arena-owned-processes.json'
   if(Test-Path -LiteralPath $ownedPath) {
    $owned=Get-Content $ownedPath -Raw | ConvertFrom-Json
    # Let the fixture deliver onEnd and flush before any window close. Closing
    # between DONE and onEnd can divert StarCraft into its quit dialog.
    for($wait=0;$wait -lt 20;$wait++) {
     $live=@($owned.processes | ForEach-Object {Get-Process -Id $_.pid -ErrorAction SilentlyContinue})
     if($live.Count -eq 0){break}
     Start-Sleep -Milliseconds 250
    }
    if (!($owned.PSObject.Properties.Name -contains 'headless' -and $owned.headless)) {
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
    }
    Start-Sleep -Milliseconds 500
    & (Join-Path $PSScriptRoot 'stop-owned-starcraft.ps1') -Runtime $runtime
   }
  } finally {
   if($case -eq 'lifecycle-output-blocked') {
    if(Test-Path -LiteralPath $blockedWritePath -PathType Leaf){Remove-Item -LiteralPath $blockedWritePath -Force}
    if(Test-Path -LiteralPath $blockedWriteBackupPath -PathType Container){Move-Item -LiteralPath $blockedWriteBackupPath -Destination $blockedWritePath}
   }
  }
 }
 $traceLines=@(Get-Content -LiteralPath $result)
 $failedChecks=@($traceLines | Where-Object {$_ -match '^CHECK,.*?,0$'})
 # Preserve completed failures before rejecting them, just as the load runner
 # preserves its failed timing measurements. Resume never treats a failure as
 # a successful case solely because a DONE record exists.
 Copy-Item (Join-Path $runtime 'bwapi-data/bwapi.ini') (Join-Path $archive "$case.ini")
 Copy-Item $mapPath (Join-Path $archive "$case.scx")
 if($case -in @('racebot-worker-defense','racebot-combat')){
  $raceBotLog=Join-Path $runtime 'bwapi-data/write/RaceBot.log'
  if(Test-Path -LiteralPath $raceBotLog){Copy-Item -LiteralPath $raceBotLog -Destination (Join-Path $archive 'RaceBot.log')}
  $issuer=if($case -eq 'racebot-worker-defense'){'worker-defense'}else{'combat-defense'}
  if(-not (Select-String -Path (Join-Path $archive 'RaceBot.log') -Pattern "^COMMAND,\d+,\d+,$issuer,Attack_Unit,.*accepted=1,error=accepted$")){
   throw "RaceBot did not record an accepted $issuer command"
  }
 }
 if($case -in @('lifecycle-reset','lifecycle-early-exit','lifecycle-output-blocked','lifecycle-truncated-history','fault-end-history','fault-end-reporting')){
  $moduleLog=Join-Path $runtime 'bwapi-data/write/Protodd.log'
  if(Test-Path -LiteralPath $moduleLog -PathType Leaf){Copy-Item -LiteralPath $moduleLog -Destination (Join-Path $archive "$case.Protodd.log")}
 }
 Move-Item -LiteralPath $result -Destination (Join-Path $archive "$case.csv")
 if(-not ($traceLines | Where-Object {$_ -match '^DONE,\d+,\d+$'})) { throw "Scenario $case has no DONE result" }
 if(-not ($traceLines | Where-Object {$_ -match '^END,\d+,'})) { throw "Scenario $case has no engine END callback" }
 if($failedChecks.Count -gt 0) { throw "Scenario $case failed checks: $($failedChecks -join '; ')" }
 if(-not ($traceLines | Where-Object {$_ -match '^DONE,\d+,0$'})) { throw "Scenario $case reports a nonzero failure count" }
 if($case -eq 'lifecycle-truncated-history') {
  $inputHistory=@(Get-ChildItem -LiteralPath (Join-Path $runtime 'bwapi-data/read') -Filter 'Protodd-*.csv' -File)
  $outputHistory=@(Get-ChildItem -LiteralPath (Join-Path $runtime 'bwapi-data/write') -Filter 'Protodd-*.csv' -File)
  if($inputHistory.Count -ne 1 -or $outputHistory.Count -ne 1) { throw 'Expected one input and one output opponent-history snapshot' }
  Copy-Item -LiteralPath $inputHistory[0].FullName -Destination (Join-Path $archive "$case.input-history.csv")
  Copy-Item -LiteralPath $outputHistory[0].FullName -Destination (Join-Path $archive "$case.output-history.csv")
 }
 Get-Content (Join-Path $archive "$case.csv") | Where-Object {$_ -match 'CHECK|DONE'}
}
} finally {
 Exit-MatchRuntimeLock -Lock $runtimeLock
}
