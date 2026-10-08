param(
 [string]$Label='candidate',
 [string]$Dll='build/tournament/Release/Protodd.dll',
 [string]$AuditLoadDll='build/tournament/Release/AuditLoad.dll',
 [ValidateSet('off','local-target')][string]$TacticalTargetMode='off',
 [string]$TacticalTargetWeightsPath='',
 [string]$RuntimePath='build/stock-certification/goal-audit-v23/match-runtime-b',
 [string]$Map='maps/audit/load.scx',
 [string]$ArchiveParent='build/direct-logs/stock-certification',
 [ValidateSet('patched-diagnostic','stock-certification')][string]$Profile='stock-certification'
)
$ErrorActionPreference='Stop'
$ownershipModule=Join-Path $PSScriptRoot 'MatchProcessOwnership.psm1'
Import-Module $ownershipModule -Force
Import-Module (Join-Path $PSScriptRoot 'MatchResourceBudget.psm1') -Force
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$runtime=if([IO.Path]::IsPathRooted($RuntimePath)){[IO.Path]::GetFullPath($RuntimePath)}else{[IO.Path]::GetFullPath((Join-Path $repo $RuntimePath))}
$archiveRoot=if([IO.Path]::IsPathRooted($ArchiveParent)){[IO.Path]::GetFullPath($ArchiveParent)}else{[IO.Path]::GetFullPath((Join-Path $repo $ArchiveParent))}
foreach($path in @($runtime,$archiveRoot)){
 if(-not $path.StartsWith([IO.Path]::GetFullPath((Join-Path $repo 'build')).TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw "Audit paths must stay under build/: $path"}
}
$mapPath=[IO.Path]::GetFullPath((Join-Path $runtime $Map))
if(-not $mapPath.StartsWith($runtime.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase) -or
   -not (Test-Path -LiteralPath $mapPath -PathType Leaf)){throw "Load fixture map must exist under the runtime: $mapPath"}
$targetWeightsPath=$null
$targetWeightsSha256=$null
if($TacticalTargetMode -eq 'local-target'){
 if([string]::IsNullOrWhiteSpace($TacticalTargetWeightsPath)){throw 'local-target mode requires TacticalTargetWeightsPath'}
 $targetWeightsPath=(Resolve-Path -LiteralPath $TacticalTargetWeightsPath -ErrorAction Stop).Path
 if(-not (Test-Path -LiteralPath $targetWeightsPath -PathType Leaf)){throw "Tactical target weights must be a file: $targetWeightsPath"}
 $targetWeightsSha256=(Get-FileHash -LiteralPath $targetWeightsPath -Algorithm SHA256).Hash.ToLowerInvariant()
}elseif(-not [string]::IsNullOrWhiteSpace($TacticalTargetWeightsPath)){
 throw 'TacticalTargetWeightsPath is only valid in local-target mode'
}
& (Join-Path $PSScriptRoot 'verify-runtime-profile.ps1') -Runtime $runtime -Profile $Profile
if($Label -notmatch '^[a-zA-Z0-9-]+$'){throw 'Invalid label'}
if(Get-Process StarCraft -ErrorAction SilentlyContinue){throw 'Existing game; refusing duplicate'}
$archive=Join-Path $archiveRoot "load-$Label"
if(Test-Path $archive){throw 'Existing load result'}
$runtimeRoot=Split-Path -Parent $runtime
$runtimeLock=Enter-MatchRuntimeLock -RuntimeRoot $runtimeRoot -Label "audit-load-$Label"
try {
New-Item -ItemType Directory $archive | Out-Null
$dllPath=(Resolve-Path -LiteralPath $Dll).Path
$wrapperPath=(Resolve-Path -LiteralPath $AuditLoadDll).Path
Copy-Item -LiteralPath $dllPath -Destination (Join-Path $runtime 'bwapi-data/AI/Protodd.dll')
Copy-Item -LiteralPath $wrapperPath -Destination (Join-Path $runtime 'bwapi-data/AI/AuditLoad.dll')
$targetModePath=Join-Path $runtime 'bwapi-data/read/TacticalTarget-mode.txt'
[IO.File]::WriteAllText($targetModePath,"$TacticalTargetMode`n",[Text.Encoding]::ASCII)
$targetRuntimeWeights=Join-Path $runtime 'bwapi-data/read/TacticalTarget-weights.bin'
if($TacticalTargetMode -eq 'local-target'){
 Copy-Item -LiteralPath $targetWeightsPath -Destination $targetRuntimeWeights -Force
}elseif(Test-Path -LiteralPath $targetRuntimeWeights -PathType Leaf){
 # This isolated runtime may have been used for a prior opt-in run. Keep its
 # optional input archived with this run, then remove it so the off arm cannot
 # accidentally execute stale model weights.
 $staleWeightsArchive=Join-Path $archive 'TacticalTarget-weights-prior.bin'
 Move-Item -LiteralPath $targetRuntimeWeights -Destination $staleWeightsArchive
}
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
map = $Map
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
[IO.File]::WriteAllText((Join-Path $runtime 'bwapi-data/read/AuditLoad-max-frame.txt'),'1200',[Text.Encoding]::ASCII)
foreach($name in 'Protodd-learning-mode.txt','Policy-mode.txt'){'frozen' | Set-Content (Join-Path $runtime "bwapi-data/read/$name") -Encoding ascii}
foreach($name in 'LearnedMacro-mode.txt','ProductionDemand-mode.txt'){'off' | Set-Content (Join-Path $runtime "bwapi-data/read/$name") -Encoding ascii}
$staleLog=Join-Path $runtime 'bwapi-data/write/Protodd.log'
if(Test-Path -LiteralPath $staleLog){throw "Preserve or archive the existing Protodd.log before load testing: $staleLog"}
$result=Join-Path $runtime 'bwapi-data/write/load-scenario.csv'
$frameTiming=Join-Path $runtime 'bwapi-data/write/full-callback-us.bin'
$callbackTiming=Join-Path $runtime 'bwapi-data/write/callback-timing.csv'
foreach($output in @($result,$frameTiming,$callbackTiming)){if(Test-Path -LiteralPath $output){throw "Unarchived load result: $output"}}
$engineManifest=Get-Content -LiteralPath (Join-Path $runtime '.protodd-runtime-profile.json') -Raw | ConvertFrom-Json
[ordered]@{dll=$dllPath;dll_sha256=(Get-FileHash $dllPath).Hash;wrapper_sha256=(Get-FileHash $wrapperPath).Hash;map=$Map;map_sha256=(Get-FileHash $mapPath).Hash;engine_sha256=$engineManifest.engine_dll_sha256;profile=$Profile;frame_limit=1200;frame_limit_mode_sha256=(Get-FileHash (Join-Path $runtime 'bwapi-data/read/AuditLoad-max-frame.txt')).Hash;tactical_target_mode=$TacticalTargetMode;tactical_target_weights_sha256=$targetWeightsSha256;started=[DateTime]::UtcNow.ToString('o');purpose='measure full playing-DLL and lifecycle/unit callback latency under a stock-engine stress fixture; no win labels'} | ConvertTo-Json | Set-Content (Join-Path $archive 'receipt.json')
& (Join-Path $PSScriptRoot 'start-owned-starcraft.ps1') -Runtime $runtime -RuntimeLock $runtimeLock
$deadline=[DateTime]::UtcNow.AddMinutes(10)
do {
 Start-Sleep -Milliseconds 500
 $done=(Test-Path $result) -and [bool](Select-String -Path $result -Pattern '^DONE,')
} while(-not $done -and [DateTime]::UtcNow -lt $deadline)
if(-not $done){throw 'Load fixture did not complete; inspect exact owned process'}
& (Join-Path $PSScriptRoot 'stop-owned-starcraft.ps1') -Runtime $runtime
Copy-Item (Join-Path $runtime 'bwapi-data/bwapi.ini') (Join-Path $archive 'bwapi.ini')
Copy-Item -LiteralPath $mapPath -Destination (Join-Path $archive (Split-Path -Leaf $mapPath))
Copy-Item -LiteralPath (Join-Path $runtime 'bwapi-data/read/AuditLoad-max-frame.txt') -Destination (Join-Path $archive 'AuditLoad-max-frame.txt')
Copy-Item -LiteralPath $targetModePath -Destination (Join-Path $archive 'TacticalTarget-mode.txt')
if($TacticalTargetMode -eq 'local-target'){
 Copy-Item -LiteralPath $targetRuntimeWeights -Destination (Join-Path $archive 'TacticalTarget-weights.bin')
 if((Get-FileHash -LiteralPath (Join-Path $archive 'TacticalTarget-weights.bin') -Algorithm SHA256).Hash.ToLowerInvariant() -ne $targetWeightsSha256){
  throw 'Archived tactical target weights differ from the preflight input'
 }
}
foreach($name in @('load-scenario.csv','full-callback-us.bin','callback-timing.csv','Protodd.log')) {
 $source=Join-Path $runtime "bwapi-data/write/$name"
 if(-not (Test-Path -LiteralPath $source)){throw "Expected load output is missing: $source"}
 Move-Item -LiteralPath $source -Destination (Join-Path $archive $name)
}

function Get-NearestRank([long[]]$Values,[double]$Percentile) {
 if($Values.Count -eq 0){return 0}
 $sorted=@($Values | Sort-Object)
 $index=[Math]::Max(0,[Math]::Ceiling($Percentile*$sorted.Count)-1)
 return [long]$sorted[$index]
}
$frameBytes=[IO.File]::ReadAllBytes((Join-Path $archive 'full-callback-us.bin'))
if(($frameBytes.Length % 8) -ne 0){throw 'Full-frame callback timing file is not a whole number of 64-bit samples'}
$frameValues=[Collections.Generic.List[long]]::new()
for($offset=0;$offset -lt $frameBytes.Length;$offset+=8){$frameValues.Add([BitConverter]::ToInt64($frameBytes,$offset))}
$callbackRows=@(Import-Csv (Join-Path $archive 'callback-timing.csv'))
$onFrameRows=@($callbackRows | Where-Object callback -eq 'onFrame')
if($onFrameRows.Count -ne $frameValues.Count){throw "onFrame binary/CSV sample counts differ: $($frameValues.Count) vs $($onFrameRows.Count)"}
for($index=0;$index -lt $frameValues.Count;$index++){
 if([long]$onFrameRows[$index].duration_us -ne $frameValues[$index]){
  throw "onFrame binary/CSV sample differs at index $index"
 }
}
$callbackSummary=@(foreach($group in ($callbackRows | Group-Object callback)){
 $values=[long[]]@($group.Group | ForEach-Object {[long]$_.duration_us})
 [pscustomobject][ordered]@{
  callback=$group.Name
  samples=$values.Count
  p50_us=Get-NearestRank $values 0.50
  p95_us=Get-NearestRank $values 0.95
  p99_us=Get-NearestRank $values 0.99
  max_us=if($values.Count){($values|Measure-Object -Maximum).Maximum}else{0}
  thrown=@($group.Group|Where-Object threw -eq '1').Count
  over_42ms=@($values|Where-Object {$_ -gt 42000}).Count
  over_55ms=@($values|Where-Object {$_ -gt 55000}).Count
  over_1000ms=@($values|Where-Object {$_ -gt 1000000}).Count
 }
})
$callbackSummary | Export-Csv -NoTypeInformation -Encoding UTF8 (Join-Path $archive 'callback-summary.csv')
$performanceRows=@(Select-String -LiteralPath (Join-Path $archive 'Protodd.log') -Pattern '^PERF_SUMMARY,')
if($performanceRows.Count -ne 1){throw "Expected exactly one Protodd PERF_SUMMARY row, found $($performanceRows.Count)"}
$performanceFields=$performanceRows[0].Line -split ','
if($performanceFields.Count -ne 8){throw 'Protodd PERF_SUMMARY row has an unexpected field count'}
$performanceSummary=[ordered]@{
 samples=[long]$performanceFields[1]
 moving_average_ms=[double]::Parse($performanceFields[2],[Globalization.CultureInfo]::InvariantCulture)
 peak_ms=[double]::Parse($performanceFields[3],[Globalization.CultureInfo]::InvariantCulture)
 over_42ms=[long]$performanceFields[4]
 over_55ms=[long]$performanceFields[5]
 over_1000ms=[long]$performanceFields[6]
 over_10000ms=[long]$performanceFields[7]
}
if(($performanceSummary.Values | Where-Object {$_ -lt 0}).Count){throw 'Protodd PERF_SUMMARY contains negative values'}
$targetActivationRows=@(Select-String -LiteralPath (Join-Path $archive 'Protodd.log') -Pattern '^TACTICAL_TARGET,')
$targetSummaryRows=@(Select-String -LiteralPath (Join-Path $archive 'Protodd.log') -Pattern '^TACTICAL_TARGET_SUMMARY,')
$targetCandidateScores=0L
$targetModelGatePassed=$TacticalTargetMode -eq 'off'
if($TacticalTargetMode -eq 'local-target'){
 if($targetActivationRows.Count -eq 1 -and $targetActivationRows[0].Line -eq 'TACTICAL_TARGET,loaded=1,control=1' -and
    $targetSummaryRows.Count -eq 1 -and $targetSummaryRows[0].Line -match 'candidateScores=(\d+)'){
  $targetCandidateScores=[long]$Matches[1]
  $targetModelGatePassed=$targetCandidateScores -gt 0
 }
}
$callbackGateIssues=[Collections.Generic.List[string]]::new()
if($callbackRows.Count -eq 0){$callbackGateIssues.Add('no external callback samples were recorded')}
if($frameValues.Count -eq 0){$callbackGateIssues.Add('no onFrame samples were recorded')}
if(@($callbackRows | Where-Object callback -eq 'onStart').Count -ne 1){$callbackGateIssues.Add('expected exactly one onStart sample')}
if(@($callbackRows | Where-Object callback -eq 'onEnd').Count -ne 1){$callbackGateIssues.Add('expected exactly one onEnd sample')}
if($callbackRows | Where-Object threw -eq '1'){$callbackGateIssues.Add('one or more bot callbacks threw')}
if($callbackSummary | Where-Object over_55ms -gt 0){$callbackGateIssues.Add('a bot callback exceeded 55 ms')}
if($frameValues | Where-Object {$_ -gt 55000}){$callbackGateIssues.Add('an onFrame callback exceeded 55 ms')}
if($performanceSummary.over_55ms -gt 0 -or $performanceSummary.over_1000ms -gt 0 -or
   $performanceSummary.over_10000ms -gt 0){$callbackGateIssues.Add('Protodd PERF_SUMMARY reports a timeout-threshold overrun')}
$callbackGatePassed=$callbackGateIssues.Count -eq 0
$memoryRecords=@(foreach($line in (Get-Content (Join-Path $archive 'load-scenario.csv') | Where-Object {$_ -match '^MEMORY,'})){
 $fields=$line -split ','
 [pscustomobject]@{working_set=[long]$fields[2];peak_working_set=[long]$fields[3];private_usage=[long]$fields[4]}
})
$memorySampleFailures=@(Select-String -LiteralPath (Join-Path $archive 'load-scenario.csv') -Pattern '^MEMORY_FAILED,').Count
$memoryBudget=Get-MatchMemoryBudgetAssessment -Samples $memoryRecords -SampleFailures $memorySampleFailures
$frameArray=[long[]]$frameValues.ToArray()
$frameSummary=[ordered]@{
 map=$Map
 frame_samples=$frameValues.Count
 frame_p50_us=Get-NearestRank $frameArray 0.50
 frame_p95_us=Get-NearestRank $frameArray 0.95
 frame_p99_us=Get-NearestRank $frameArray 0.99
 frame_max_us=if($frameValues.Count){($frameValues|Measure-Object -Maximum).Maximum}else{0}
 frame_over_42ms=@($frameValues|Where-Object {$_ -gt 42000}).Count
 frame_over_55ms=@($frameValues|Where-Object {$_ -gt 55000}).Count
 frame_over_1000ms=@($frameValues|Where-Object {$_ -gt 1000000}).Count
 frame_over_10000ms=@($frameValues|Where-Object {$_ -gt 10000000}).Count
 callback_rows=$callbackRows.Count
 callback_throws=@($callbackRows|Where-Object threw -eq '1').Count
 callback_over_42ms=@(($callbackRows|Where-Object {[long]$_.duration_us -gt 42000}).Count)
 callback_over_55ms=@(($callbackRows|Where-Object {[long]$_.duration_us -gt 55000}).Count)
 onframe_binary_csv_exact=$true
 callback_gate_passed=$callbackGatePassed
 callback_gate_issues=@($callbackGateIssues.ToArray())
 protodd_performance_summary=$performanceSummary
 peak_working_set_bytes=if($memoryRecords.Count){($memoryRecords|Measure-Object peak_working_set -Maximum).Maximum}else{0}
 peak_private_usage_bytes=if($memoryRecords.Count){($memoryRecords|Measure-Object private_usage -Maximum).Maximum}else{0}
 memory_samples=$memoryRecords.Count
 memory_budget=$memoryBudget
 tactical_target=[ordered]@{requested_mode=$TacticalTargetMode;weights_sha256=$targetWeightsSha256;activation_rows=$targetActivationRows.Count;summary_rows=$targetSummaryRows.Count;candidate_scores=$targetCandidateScores;gate_passed=$targetModelGatePassed}
}
$frameSummary | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $archive 'summary.json') -Encoding UTF8
Get-Content (Join-Path $archive 'callback-summary.csv')
Get-Content (Join-Path $archive 'summary.json')
Get-Content (Join-Path $archive 'load-scenario.csv')
if(-not $callbackGatePassed){throw "Load fixture failed the callback safety gate: $($callbackGateIssues -join ', ')"}
if(-not $targetModelGatePassed){throw 'Load fixture did not activate the requested tactical target model or score any candidates'}
if(-not $memoryBudget.within_budget){throw "Load fixture failed the documented process memory budget: $($memoryBudget.issues -join ', ')"}
} finally {
 Exit-MatchRuntimeLock -Lock $runtimeLock
}
