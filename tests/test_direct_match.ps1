$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$provenanceModule = Join-Path $repo 'scripts/MatchProvenance.psm1'
Import-Module $provenanceModule -Force
$ownershipModule = Join-Path $repo 'scripts/MatchProcessOwnership.psm1'
Import-Module $ownershipModule -Force
$scriptPath = Join-Path $repo 'scripts/direct-match.ps1'
$sourceText = [IO.File]::ReadAllText($scriptPath)
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($scriptPath, [ref]$null, [ref]$parseErrors)
if ($parseErrors.Count) { throw 'Direct-match script has syntax errors' }
foreach ($requiredText in @(
    '[switch]$MeasureFullCallbacks',
    "requires the stock-certification runtime profile",
    "bwapi-data/AI/AuditLoad.dll",
    "AuditLoad-max-frame.txt",
    'audit_load_artifacts_complete',
    'MatchProcessOwnership.psm1',
    'cleanup_complete',
    'process_cleanup = $cleanupStatus',
    'Match cleanup is incomplete',
    'Resolve-MatchRuntimeSet',
    'runtime_set_mode = if ($autoRuntimeSet)',
    'Enter-MatchRuntimeLock',
    'Exit-MatchRuntimeLock',
    '[string]$MapSourcePath',
    'Resolve-MatchMapPath -RuntimeRoot $runtimeA'
    'Start-HeadlessStarCraftLauncher',
    'headless = $true',
    'native_diagnostics = $nativeDiagnostics'
)) {
    if (-not $sourceText.Contains($requiredText)) {
        throw "Direct-match full-callback audit integration is missing: $requiredText"
    }
}
$sessionStart = [DateTime]::SpecifyKind([DateTime]'2026-10-07T02:50:00', [DateTimeKind]::Utc)
$processSnapshot = @(
    [pscustomobject]@{ ProcessId = 303; ParentProcessId = 302; CreationDate = $sessionStart.AddSeconds(3) },
    [pscustomobject]@{ ProcessId = 404; ParentProcessId = 999; CreationDate = $sessionStart.AddSeconds(2) },
    [pscustomobject]@{ ProcessId = 202; ParentProcessId = 101; CreationDate = $sessionStart.AddSeconds(2) },
    [pscustomobject]@{ ProcessId = 101; ParentProcessId = 100; CreationDate = $sessionStart.AddSeconds(1) },
    [pscustomobject]@{ ProcessId = 505; ParentProcessId = 100; CreationDate = $sessionStart.AddSeconds(-5) },
    [pscustomobject]@{ ProcessId = 7; ParentProcessId = 100; CreationDate = $sessionStart.AddSeconds(2) }
)
$ownedProcesses = @(Get-OwnedStarCraftProcesses -Processes $processSnapshot `
    -RootProcessIds @(100, 302) -BaselineProcessIds @(7) -StartedAtUtc $sessionStart)
$ownedProcessIds = @($ownedProcesses | Select-Object -ExpandProperty process_id)
if (($ownedProcessIds -join ',') -ne '101,202,303') {
    throw "StarCraft ownership filter included or omitted the wrong process IDs: $($ownedProcessIds -join ',')"
}
$emptyProcessInventory = @(Get-OwnedStarCraftProcesses -Processes @() `
    -RootProcessIds @() -StartedAtUtc $sessionStart)
if ($emptyProcessInventory.Count -ne 0) {
    throw 'An empty process inventory should be a verified clean state'
}
$childAfterParentExit = @(
    Get-OwnedStarCraftProcesses -Processes @(
        [pscustomobject]@{ ProcessId = 606; ParentProcessId = 202; CreationDate = $sessionStart.AddSeconds(4) }
    ) -RootProcessIds @(100) -KnownProcessIds $ownedProcessIds -StartedAtUtc $sessionStart
)
if ($childAfterParentExit.Count -ne 1 -or $childAfterParentExit[0].process_id -ne 606) {
    throw 'A restarted StarCraft child was not retained in the owned process tree'
}
$lockRoot = Join-Path ([IO.Path]::GetTempPath()) ('protodd-runtime-lock-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $lockRoot | Out-Null
$firstLock = $null
$reacquiredLock = $null
try {
    $firstLock = Enter-MatchRuntimeLock -RuntimeRoot $lockRoot -Label 'first'
    $secondAcquisitionBlocked = $false
    try {
        $secondLock = Enter-MatchRuntimeLock -RuntimeRoot $lockRoot -Label 'second'
        Exit-MatchRuntimeLock -Lock $secondLock
    } catch {
        $secondAcquisitionBlocked = $_.Exception.Message -match 'already owned by another match'
    }
    if (-not $secondAcquisitionBlocked) {
        throw 'A second match acquired a runtime while the first match held its lock'
    }
    Exit-MatchRuntimeLock -Lock $firstLock
    $firstLock = $null
    $reacquiredLock = Enter-MatchRuntimeLock -RuntimeRoot $lockRoot -Label 'after-release'
} finally {
    Exit-MatchRuntimeLock -Lock $firstLock
    Exit-MatchRuntimeLock -Lock $reacquiredLock
    Remove-Item -LiteralPath $lockRoot -Recurse -Force -ErrorAction SilentlyContinue
}
$function = $ast.Find({param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-MatchCrashReports'
}, $true)
if (-not $function) { throw 'Missing runtime crash detector' }
. ([ScriptBlock]::Create($function.Extent.Text))
$terminalFunction = $ast.Find({param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-MatchTerminalResult'
}, $true)
if (-not $terminalFunction) { throw 'Missing match-result parser' }
. ([ScriptBlock]::Create($terminalFunction.Extent.Text))
if ((Get-MatchTerminalResult -Lines @('END,win,2400') -LogName 'Protodd.log') -ne 'END,win,2400') {
    throw 'Protodd terminal result was not preserved'
}
if ((Get-MatchTerminalResult -Lines @('END,2400,1') -LogName 'RaceBot.log') -ne 'END,win,2400') {
    throw 'RaceBot winner was not normalized to the match result format'
}
if ((Get-MatchTerminalResult -Lines @('END,2400,0') -LogName 'RaceBot.log') -ne 'END,loss,2400') {
    throw 'RaceBot loss was not normalized to the match result format'
}
if (Get-MatchTerminalResult -Lines @('SNAPSHOT,1200,workers=8') -LogName 'RaceBot.log') {
    throw 'A nonterminal snapshot was treated as a completed match'
}

$fixturePrefix = [IO.Path]::GetFullPath((Join-Path $repo 'build/direct-match-test-fixtures')).TrimEnd('\') + '\'
$fixtureRoot = [IO.Path]::GetFullPath((Join-Path $fixturePrefix ([guid]::NewGuid().ToString())))
if (-not $fixtureRoot.StartsWith($fixturePrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Fixture path escapes the test directory'
}
$runtimeA = Join-Path $fixtureRoot 'host'
$runtimeB = Join-Path $fixtureRoot 'opponent'
$script:startedAtUtc = [DateTime]::UtcNow.AddMinutes(-1)
try {
    New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null
    $mapFunction = $ast.Find({param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -eq 'Resolve-MatchMapPath'
    }, $true)
    if (-not $mapFunction) { throw 'Missing isolated-runtime map installer' }
    . ([ScriptBlock]::Create($mapFunction.Extent.Text))
    $mapSource = Join-Path $fixtureRoot 'source-map.scx'
    $mapRuntime = Join-Path $fixtureRoot 'map-runtime'
    [IO.File]::WriteAllBytes($mapSource, [Text.Encoding]::UTF8.GetBytes('verified map bytes'))
    New-Item -ItemType Directory -Path $mapRuntime -Force | Out-Null
    $mapRelativePath = 'maps/aiide2023/(2)Evidence.scx'
    $resolvedMap = Resolve-MatchMapPath -RuntimeRoot $mapRuntime `
        -RelativePath $mapRelativePath -SourcePath $mapSource
    $expectedMap = [IO.Path]::GetFullPath((Join-Path $mapRuntime $mapRelativePath))
    if ($resolvedMap -ne $expectedMap -or
        (Get-FileHash -LiteralPath $resolvedMap -Algorithm SHA256).Hash -ne
            (Get-FileHash -LiteralPath $mapSource -Algorithm SHA256).Hash) {
        throw 'The scheduled map was not copied into the isolated host runtime exactly'
    }
    [void](Resolve-MatchMapPath -RuntimeRoot $mapRuntime -RelativePath $mapRelativePath `
        -SourcePath $mapSource)
    $mapMismatchRejected = $false
    [IO.File]::WriteAllBytes($mapSource, [Text.Encoding]::UTF8.GetBytes('different map bytes'))
    try {
        [void](Resolve-MatchMapPath -RuntimeRoot $mapRuntime -RelativePath $mapRelativePath `
            -SourcePath $mapSource)
    } catch { $mapMismatchRejected = $_.Exception.Message -match 'different bytes' }
    if (-not $mapMismatchRejected) { throw 'A mismatched map silently replaced existing runtime bytes' }
    $mapTraversalRejected = $false
    try {
        [void](Resolve-MatchMapPath -RuntimeRoot $mapRuntime -RelativePath '../outside.scx')
    } catch { $mapTraversalRejected = $_.Exception.Message -match 'relative path inside' }
    if (-not $mapTraversalRejected) { throw 'A map path escaped its isolated host runtime' }

    $provenanceRoot = Join-Path $fixtureRoot 'provenance'
    New-Item -ItemType Directory -Path $provenanceRoot -Force | Out-Null
    $inputFiles = [ordered]@{}
    foreach ($role in @('bot_dll', 'engine_dll', 'map', 'opponent_dll', 'host_ini')) {
        $path = Join-Path $provenanceRoot $role
        [IO.File]::WriteAllText($path, "frozen:$role")
        $inputFiles[$role] = $path
    }
    $configuration = [ordered]@{
        map = 'Python.scx'
        seed_requested = 42
        opponent = [ordered]@{name = 'Iron'; race = 'Terran'}
    }
    $manifest = New-MatchInputManifest -Files $inputFiles -Configuration $configuration
    if (-not (Assert-MatchInputManifest -Manifest $manifest)) {
        throw 'An unchanged match preflight manifest failed verification'
    }
    $preflightPath = Join-Path $provenanceRoot 'match.preflight.json'
    $preflightInfo = Write-MatchInputManifest -Manifest $manifest -Path $preflightPath
    if ((Get-FileHash -LiteralPath $preflightPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne
        $preflightInfo.sha256) {
        throw 'The recorded preflight file hash does not match its contents'
    }
    foreach ($role in $inputFiles.Keys) {
        $original = [IO.File]::ReadAllBytes($inputFiles[$role])
        try {
            [IO.File]::WriteAllText($inputFiles[$role], "substituted:$role")
            $rejected = $false
            try { [void](Assert-MatchInputManifest -Manifest $manifest) }
            catch { $rejected = $_.Exception.Message -match "changed before launch \($role\)" }
            if (-not $rejected) { throw "Preflight did not reject substituted input $role" }
        } finally {
            [IO.File]::WriteAllBytes($inputFiles[$role], $original)
        }
    }
    $mutatedConfiguration = New-MatchInputManifest -Files $inputFiles -Configuration $configuration
    $mutatedConfiguration.configuration.seed_requested = 99
    $configurationRejected = $false
    try { [void](Assert-MatchInputManifest -Manifest $mutatedConfiguration) }
    catch { $configurationRejected = $_.Exception.Message -match 'configuration changed' }
    if (-not $configurationRejected) { throw 'Preflight did not reject a substituted seed/configuration' }
    $duplicatePreflightRejected = $false
    try { [void](Write-MatchInputManifest -Manifest $manifest -Path $preflightPath) }
    catch { $duplicatePreflightRejected = $true }
    if (-not $duplicatePreflightRejected) { throw 'An existing preflight manifest was overwritten' }
    $matchRecordPath = Join-Path $provenanceRoot 'match.json'
    $record = [ordered]@{label='immutable-record'; status='incomplete'; result=$null}
    $recordInfo = Write-MatchRecordImmutable -Record $record -Path $matchRecordPath
    $recordHash = (Get-FileHash -LiteralPath $matchRecordPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($recordHash -ne $recordInfo.sha256) {
        throw 'The atomic match record hash does not match its published bytes'
    }
    $immutableRecordRejected = $false
    try {
        Write-MatchRecordImmutable -Record ([ordered]@{label='replacement'}) -Path $matchRecordPath | Out-Null
    } catch { $immutableRecordRejected = $true }
    $recordHashAfterAttempt = (Get-FileHash -LiteralPath $matchRecordPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if (-not $immutableRecordRejected -or $recordHashAfterAttempt -ne $recordInfo.sha256) {
        throw 'An existing match record was replaced or its content changed'
    }
    if (@(Get-ChildItem -LiteralPath $provenanceRoot -Filter '.match.json.*.tmp' -File).Count -ne 0) {
        throw 'Atomic match record write left a temporary file behind'
    }

    foreach ($runtime in @($runtimeA, $runtimeB)) {
        New-Item -ItemType Directory -Path (Join-Path $runtime 'Errors') -Force | Out-Null
    }
    $old = Join-Path $runtimeA 'Errors/old.txt'
    Set-Content -LiteralPath $old -Value 'EXCEPTION: old crash' -Encoding ascii
    [IO.File]::SetLastWriteTimeUtc($old, $script:startedAtUtc.AddMinutes(-1))
    Set-Content -LiteralPath (Join-Path $runtimeB 'Errors/warning.txt') -Value 'Palette warning' -Encoding ascii
    if (@(Get-MatchCrashReports).Count -ne 0) { throw 'Historical crashes or warnings were counted as fresh crashes' }
    Set-Content -LiteralPath (Join-Path $runtimeB 'Errors/fresh.txt') -Value 'EXCEPTION: 0xE06D7363' -Encoding ascii
    $reports = @(Get-MatchCrashReports)
    if ($reports.Count -ne 1 -or $reports[0].side -ne 'opponent') {
        throw 'Fresh opponent crash was not identified correctly'
    }
    Set-Content -LiteralPath (Join-Path $runtimeA 'Errors/fresh.txt') -Value 'EXCEPTION: 0xC0000005' -Encoding ascii
    $reports = @(Get-MatchCrashReports)
    if ($reports.Count -ne 2 -or @($reports | Where-Object side -eq 'protodd').Count -ne 1) {
        throw 'Fresh host crash was not identified correctly'
    }
'Direct-match crash detection and map placement regressions passed'
} finally {
    $resolved = [IO.Path]::GetFullPath($fixtureRoot)
    if (-not $resolved.StartsWith($fixturePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to remove a fixture outside its test directory'
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue
}
