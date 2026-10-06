param(
 [string]$Label='candidate',
 [string]$Dll='build/tournament/Release/Protodd.dll',
 [string]$AuditLoadDll='build/tournament/Release/AuditLoad.dll',
 [string]$RuntimePath='build/stock-certification/goal-audit-v23/match-runtime-b',
 [string]$ArchiveParent='build/direct-logs/stock-certification',
 [ValidateSet('patched-diagnostic','stock-certification')][string]$Profile='stock-certification'
)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$runtime=if([IO.Path]::IsPathRooted($RuntimePath)){[IO.Path]::GetFullPath($RuntimePath)}else{[IO.Path]::GetFullPath((Join-Path $repo $RuntimePath))}
$archiveRoot=if([IO.Path]::IsPathRooted($ArchiveParent)){[IO.Path]::GetFullPath($ArchiveParent)}else{[IO.Path]::GetFullPath((Join-Path $repo $ArchiveParent))}
foreach($path in @($runtime,$archiveRoot)){
 if(-not $path.StartsWith([IO.Path]::GetFullPath((Join-Path $repo 'build')).TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw "Audit paths must stay under build/: $path"}
}
& (Join-Path $PSScriptRoot 'verify-runtime-profile.ps1') -Runtime $runtime -Profile $Profile
if($Label -notmatch '^[a-zA-Z0-9-]+$'){throw 'Invalid label'}
if(Get-Process StarCraft -ErrorAction SilentlyContinue){throw 'Existing game; refusing duplicate'}
$archive=Join-Path $archiveRoot "load-$Label"
if(Test-Path $archive){throw 'Existing load result'}
New-Item -ItemType Directory $archive | Out-Null
$dllPath=(Resolve-Path -LiteralPath $Dll).Path
$wrapperPath=(Resolve-Path -LiteralPath $AuditLoadDll).Path
Copy-Item -LiteralPath $dllPath -Destination (Join-Path $runtime 'bwapi-data/AI/Protodd.dll')
Copy-Item -LiteralPath $wrapperPath -Destination (Join-Path $runtime 'bwapi-data/AI/AuditLoad.dll')
$ini=@"
[ai]
ai = bwapi-data/AI/AuditLoad.dll
ai_dbg = bwapi-data/AI/AuditLoad.dll
tournament =
[auto_menu]
auto_menu = LAN
lan_mode = Local PC
character_name = AstraBot
pause_dbg = OFF
auto_restart = OFF
map = maps/audit/load.scx
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
[paths]
log_path = bwapi-data/logs
"@
[IO.File]::WriteAllText((Join-Path $runtime 'bwapi-data/bwapi.ini'),$ini)
'on' | Set-Content (Join-Path $runtime 'bwapi-data/read/CallbackAudit-mode.txt') -Encoding ascii
foreach($name in 'Protodd-learning-mode.txt','Policy-mode.txt'){'frozen' | Set-Content (Join-Path $runtime "bwapi-data/read/$name") -Encoding ascii}
foreach($name in 'LearnedMacro-mode.txt','ProductionDemand-mode.txt'){'off' | Set-Content (Join-Path $runtime "bwapi-data/read/$name") -Encoding ascii}
$staleLog=Join-Path $runtime 'bwapi-data/write/Protodd.log'
if(Test-Path -LiteralPath $staleLog){throw "Preserve or archive the existing Protodd.log before load testing: $staleLog"}
$result=Join-Path $runtime 'bwapi-data/write/load-scenario.csv'
foreach($output in @($result,(Join-Path $runtime 'bwapi-data/write/full-callback-us.bin'))){if(Test-Path -LiteralPath $output){throw "Unarchived load result: $output"}}
$engineManifest=Get-Content -LiteralPath (Join-Path $runtime '.protodd-runtime-profile.json') -Raw | ConvertFrom-Json
[ordered]@{dll=$dllPath;dll_sha256=(Get-FileHash $dllPath).Hash;wrapper_sha256=(Get-FileHash $wrapperPath).Hash;map_sha256=(Get-FileHash (Join-Path $runtime 'maps/audit/load.scx')).Hash;engine_sha256=$engineManifest.engine_dll_sha256;profile=$Profile;started=[DateTime]::UtcNow.ToString('o');purpose='complete actual playing DLL callback under preplaced 200-supply combat; no win labels'} | ConvertTo-Json | Set-Content (Join-Path $archive 'receipt.json')
& (Join-Path $PSScriptRoot 'start-owned-starcraft.ps1') -Runtime $runtime
$deadline=[DateTime]::UtcNow.AddMinutes(4)
do {
 Start-Sleep -Milliseconds 500
 $done=(Test-Path $result) -and [bool](Select-String -Path $result -Pattern '^DONE,')
} while(-not $done -and [DateTime]::UtcNow -lt $deadline)
if(-not $done){throw 'Load fixture did not complete; inspect exact owned process'}
while(Get-Process StarCraft -ErrorAction SilentlyContinue){Start-Sleep -Milliseconds 250}
Copy-Item (Join-Path $runtime 'bwapi-data/bwapi.ini') (Join-Path $archive 'bwapi.ini')
Copy-Item (Join-Path $runtime 'maps/audit/load.scx') (Join-Path $archive 'load.scx')
foreach($name in @('load-scenario.csv','full-callback-us.bin','Protodd.log')) {
 $source=Join-Path $runtime "bwapi-data/write/$name"
 if(-not (Test-Path -LiteralPath $source)){throw "Expected load output is missing: $source"}
 Move-Item -LiteralPath $source -Destination (Join-Path $archive $name)
}
Get-Content (Join-Path $archive 'load-scenario.csv')
