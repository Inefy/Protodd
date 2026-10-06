$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$provenanceModule = Join-Path $repo 'scripts/MatchProvenance.psm1'
Import-Module $provenanceModule -Force
$scriptPath = Join-Path $repo 'scripts/direct-match.ps1'
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($scriptPath, [ref]$null, [ref]$parseErrors)
if ($parseErrors.Count) { throw 'Direct-match script has syntax errors' }
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
    'Direct-match crash detection regression passed'
} finally {
    $resolved = [IO.Path]::GetFullPath($fixtureRoot)
    if (-not $resolved.StartsWith($fixturePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to remove a fixture outside its test directory'
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue
}
