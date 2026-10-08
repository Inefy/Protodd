param(
    [string]$SchedulePath = 'artifacts/goal-20261005/t114-match-identity-20261007/matched-schedule-dev-confirmation-v2.json',
    [switch]$Execute
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Push-Location $repo
try {
    $resolvedSchedule = [IO.Path]::GetFullPath($SchedulePath)
    if (-not (Test-Path -LiteralPath $resolvedSchedule -PathType Leaf)) {
        throw "T114 schedule not found: $resolvedSchedule"
    }
    $validator = Join-Path $repo 'tools/t114_evaluate.py'
    & python $validator --schedule $resolvedSchedule --repo-root $repo --validate-only
    if ($LASTEXITCODE -ne 0) { throw 'T114 schedule/package/opponent validation failed' }
    $schedule = Get-Content -LiteralPath $resolvedSchedule -Raw | ConvertFrom-Json
    $scheduleRoot = Split-Path -Parent $resolvedSchedule
    $profileRoot = Join-Path $repo "build/stock-certification"
    $archiveRoot = Join-Path $repo "build/direct-logs/$($schedule.shared_runtime.runtime_profile)"
    $directMatch = Join-Path $repo 'scripts/direct-match.ps1'
    $prepareRuntime = Join-Path $repo 'scripts/prepare-stock-runtime.ps1'
    $packageDllRoot = Join-Path $repo 'build/t114-execution-arm-dlls'

    $baseline = @(Get-Process -Name StarCraft -ErrorAction SilentlyContinue)
    if ($baseline.Count -gt 0) {
        throw "Refusing T114 execution while StarCraft is running: $($baseline.Id -join ',')"
    }

    $armDlls = @{}
    foreach ($armName in @('reference', 'candidate')) {
        $arm = $schedule.arms.$armName
        $dllDirectory = Join-Path $packageDllRoot $arm.name
        $dllPath = Join-Path $dllDirectory 'Protodd.dll'
        if (Test-Path -LiteralPath (Join-Path $dllDirectory 'Protodd.build-manifest.json')) {
            throw "Unexpected repository build manifest beside archived DLL: $dllPath"
        }
        if (Test-Path -LiteralPath $dllPath -PathType Leaf) {
            $actual = (Get-FileHash -LiteralPath $dllPath -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($actual -ne $arm.dll_sha256.ToLowerInvariant()) {
                throw "Existing extracted $armName DLL differs from the frozen package"
            }
        } elseif ($Execute) {
            New-Item -ItemType Directory -Path $dllDirectory -Force | Out-Null
            Add-Type -AssemblyName System.IO.Compression.FileSystem
            $packagePath = Join-Path $scheduleRoot $arm.package_path
            $archive = [System.IO.Compression.ZipFile]::OpenRead($packagePath)
            try {
                $entry = $archive.GetEntry('Protodd.dll')
                if ($null -eq $entry) { throw "Package has no root Protodd.dll: $packagePath" }
                [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $dllPath)
            } finally { $archive.Dispose() }
            $actual = (Get-FileHash -LiteralPath $dllPath -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($actual -ne $arm.dll_sha256.ToLowerInvariant()) {
                throw "Extracted $armName DLL differs from the frozen package"
            }
        }
        $armDlls[$armName] = $dllPath
    }

    $plannedRuns = @()
    foreach ($pair in $schedule.pairs) {
        foreach ($run in $pair.runs) {
            $opponent = $schedule.opponents.PSObject.Properties[$pair.opponent].Value
            $map = $schedule.maps.PSObject.Properties[$pair.map].Value
            $runtimeRoot = Join-Path $profileRoot $run.runtime_set
            $runArchiveRoot = Join-Path $archiveRoot $run.runtime_set
            $resultPath = Join-Path $runArchiveRoot "$($run.label).json"
            if ((Test-Path -LiteralPath $runtimeRoot) -or (Test-Path -LiteralPath $runArchiveRoot)) {
                throw "Scheduled runtime/archive set already exists; refusing reuse: $($run.runtime_set)"
            }
            if (Test-Path -LiteralPath $resultPath) {
                throw "Scheduled match result already exists; refusing overwrite: $resultPath"
            }
            $plannedRuns += [pscustomobject]@{
                pair_id = $pair.pair_id
                arm = $run.arm
                dll = $armDlls[$run.arm]
                opponent_name = $opponent.name
                opponent_race = $opponent.race
                map = $map.relative_path
                map_source = [IO.Path]::GetFullPath((Join-Path $scheduleRoot $map.evidence_path))
                seed = [int]$pair.seed
                runtime_set = $run.runtime_set
                result_path = $resultPath
            }
        }
    }

    "Validated $($plannedRuns.Count) ordered arm attempts for $($schedule.schedule_id)."
    if (-not $Execute) {
        foreach ($run in $plannedRuns) {
            "PLAN $($run.pair_id) $($run.arm) $($run.opponent_name) $($run.map) seed=$($run.seed) set=$($run.runtime_set)"
        }
        'Dry run only. Pass -Execute to start the scheduled matches.'
        return
    }

    foreach ($run in $plannedRuns) {
        $live = @(Get-Process -Name StarCraft -ErrorAction SilentlyContinue)
        if ($live.Count -gt 0) {
            throw "Refusing to continue while StarCraft is running: $($live.Id -join ',')"
        }
        "Starting $($run.pair_id) $($run.arm)"
        & $prepareRuntime -RuntimeSet $run.runtime_set
        & $directMatch `
            -OpponentName $run.opponent_name `
            -OpponentRace $run.opponent_race `
            -Map $run.map `
            -MapSourcePath $run.map_source `
            -Label $run.pair_id `
            -RuntimeSet $run.runtime_set `
            -Seed $run.seed `
            -HostRace $schedule.shared_runtime.host_race `
            -BotDll $run.dll `
            -RuntimeProfile $schedule.shared_runtime.runtime_profile `
            -FrameLimit $schedule.shared_runtime.frame_limit `
            -FrameMilliseconds $schedule.shared_runtime.frame_milliseconds `
            -TimeoutSeconds $schedule.shared_runtime.timeout_seconds `
            -AllowUnmanifestedDll `
            -SaveReplay
        if (-not (Test-Path -LiteralPath $run.result_path -PathType Leaf)) {
            throw "Direct-match produced no immutable result record: $($run.result_path)"
        }
        $record = Get-Content -LiteralPath $run.result_path -Raw | ConvertFrom-Json
        if ($run.arm -eq 'candidate' -and $record.status -ne 'completed') {
            $crashSides = @($record.runtime_crashes | ForEach-Object { [string]$_.side })
            $opponentOnlyCrash = $crashSides.Count -gt 0 -and @($crashSides | Where-Object { $_ -ne 'opponent' }).Count -eq 0
            if (-not $opponentOnlyCrash) {
                throw "Candidate attempt is incomplete or failed; stopping per frozen rule: $($run.pair_id)"
            }
        }
        "Finished $($run.pair_id) $($run.arm): status=$($record.status) result=$($record.result)"
    }

    $evaluationPath = Join-Path $scheduleRoot 'development-evaluation.json'
    & python $validator --schedule $resolvedSchedule --repo-root $repo `
        --archive-root $archiveRoot --output $evaluationPath | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'T114 evaluation failed after schedule execution' }
    $evaluation = Get-Content -LiteralPath $evaluationPath -Raw | ConvertFrom-Json
    "T114 evaluation: $($evaluation.analysis_status); valid pairs=$($evaluation.completed_valid_pairs)/$($evaluation.scheduled_pairs)"
} finally {
    Pop-Location
}
