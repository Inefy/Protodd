param([string]$Label='candidate',[string]$Dll='build/audit-validation-20260926/Protodd.candidate.dll')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$root=Join-Path $repo 'build/audit-validation-20260926'
$runtime=Join-Path $root 'runtime'
if($Label -notmatch '^[a-zA-Z0-9-]+$'){throw 'Invalid label'}
if(Get-Process StarCraft -ErrorAction SilentlyContinue){throw 'Existing game; refusing duplicate'}
$archive=Join-Path $root "load-$Label"
if(Test-Path $archive){throw 'Existing load result'}
New-Item -ItemType Directory $archive | Out-Null
Copy-Item -LiteralPath $Dll -Destination (Join-Path $runtime 'bwapi-data/AI/Protodd.dll')
Copy-Item (Join-Path $repo 'build/test-review-20260926-win32/Release/AuditLoad.dll') (Join-Path $runtime 'bwapi-data/AI/AuditLoad.dll')
$ini=Get-Content (Join-Path $runtime 'bwapi-data/bwapi.ini') -Raw
$ini=$ini.Replace('AuditScenario.dll','AuditLoad.dll') -replace 'maps/audit/[^\r\n]+','maps/audit/load.scx'
[IO.File]::WriteAllText((Join-Path $runtime 'bwapi-data/bwapi.ini'),$ini)
'on' | Set-Content (Join-Path $runtime 'bwapi-data/read/CallbackAudit-mode.txt') -Encoding ascii
foreach($name in 'Protodd-learning-mode.txt','Policy-mode.txt'){'frozen' | Set-Content (Join-Path $runtime "bwapi-data/read/$name") -Encoding ascii}
foreach($name in 'LearnedMacro-mode.txt','ProductionDemand-mode.txt'){'off' | Set-Content (Join-Path $runtime "bwapi-data/read/$name") -Encoding ascii}
$result=Join-Path $runtime 'bwapi-data/write/load-scenario.csv'
if(Test-Path $result){throw 'Unarchived load result'}
[ordered]@{dll=(Resolve-Path $Dll).Path;dll_sha256=(Get-FileHash $Dll).Hash;wrapper_sha256=(Get-FileHash (Join-Path $runtime 'bwapi-data/AI/AuditLoad.dll')).Hash;map_sha256=(Get-FileHash (Join-Path $runtime 'maps/audit/load.scx')).Hash;started=[DateTime]::UtcNow.ToString('o');purpose='complete actual playing DLL callback under preplaced 200-supply combat; no win labels'} | ConvertTo-Json | Set-Content (Join-Path $archive 'receipt.json')
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
foreach($file in Get-ChildItem (Join-Path $runtime 'bwapi-data/write') -File) {
 $destination=[IO.Path]::GetFullPath((Join-Path $archive $file.Name))
 if(-not $file.FullName.StartsWith($runtime+'\') -or -not $destination.StartsWith($archive+'\')){throw 'Archive path outside validation root'}
 Move-Item -LiteralPath $file.FullName -Destination $destination
}
Get-Content (Join-Path $archive 'load-scenario.csv')
