param(
    [string]$Reference='build/strength-first-20260924/production-paired-reference-07',
    [string]$Candidate='build/strength-first-20260924/production-paired-candidate-07',
    [string]$Status='build/strength-first-20260924/production-paired-07.status.json'
)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repo
$referencePath=(Resolve-Path -LiteralPath $Reference).Path
$candidatePath=(Resolve-Path -LiteralPath $Candidate).Path
$statusPath=[IO.Path]::GetFullPath((Join-Path $repo $Status))
$python=Join-Path $repo 'build/model-venv/Scripts/python.exe'
$lock=[IO.File]::Open($statusPath+'.lock',[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
function Publish([string]$stage,[string]$detail='') {
    $data=[ordered]@{stage=$stage;updated=[DateTime]::UtcNow.ToString('o');pid=$PID;reference=$referencePath;candidate=$candidatePath;detail=$detail;promotion_allowed=$false}
    $temp=$statusPath+'.tmp'
    $data|ConvertTo-Json|Set-Content -LiteralPath $temp
    Move-Item -LiteralPath $temp -Destination $statusPath -Force
}
function Wait-Results([string]$run) {
    $results=Join-Path $run 'server/results.jsonl'
    while($true) {
        if(Test-Path -LiteralPath $results) {
            try {$rows=@(Get-Content -LiteralPath $results|Where-Object {$_}|ForEach-Object {$_|ConvertFrom-Json})} catch {$rows=@()}
            if($rows.Count -ge 4){break}
        }
        Start-Sleep -Seconds 5
    }
    # Never replace or interrupt another live game to make a queued run fit.
    while(Get-Process StarCraft -ErrorAction SilentlyContinue){Start-Sleep -Seconds 5}
}
function Stop-OwnedLaunchers([string]$run) {
    foreach($job in (Get-Content -LiteralPath (Join-Path $run 'processes.json') -Raw|ConvertFrom-Json)) {
        $process=Get-Process -Id $job.pid -ErrorAction SilentlyContinue
        $expected=if($job.started -is [DateTime]){$job.started.ToUniversalTime()}else{([DateTimeOffset]::Parse($job.started)).UtcDateTime}
        if($process -and $process.ProcessName -eq 'java' -and [Math]::Abs(($process.StartTime.ToUniversalTime()-$expected).TotalMilliseconds) -lt 10){Stop-Process -Id $job.pid -Force}
    }
}
try {
    if(Test-Path -LiteralPath (Join-Path $candidatePath 'processes.json')){throw 'Candidate already has an owner; refusing duplicate launch'}
    Publish 'waiting-reference'
    Wait-Results $referencePath
    $health=(& $python -m training.arena inspect $referencePath|Out-String)|ConvertFrom-Json
    if($LASTEXITCODE -ne 0 -or @($health.structurally_valid).Count -ne 2 -or @($health.excluded).Count){throw 'Reference games failed health/adjudication; inspect before candidate dispatch'}
    Stop-OwnedLaunchers $referencePath
    Publish 'starting-candidate'
    & (Join-Path $repo 'scripts/start-arena.ps1') -Run $candidatePath
    Publish 'candidate-running'
    Wait-Results $candidatePath
    Stop-OwnedLaunchers $candidatePath
    Publish 'reviewing'
    $report=Join-Path $repo 'build/strength-first-20260924/production-paired-07-report.json'
    & $python -m training.production_paired_review $candidatePath $referencePath $report
    if($LASTEXITCODE -ne 0){throw 'Paired review failed; preserved all game artifacts'}
    $result=Get-Content -LiteralPath $report -Raw|ConvertFrom-Json
    Publish 'complete' ("Pilot advance_to_72="+$result.advance_to_72+"; no promotion or further games launched automatically")
} catch {
    Publish 'failed' $_.Exception.Message
    throw
} finally {$lock.Dispose()}
