$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$scriptPath = Join-Path $repo 'scripts/direct-match.ps1'
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($scriptPath, [ref]$null, [ref]$parseErrors)
if ($parseErrors.Count) { throw 'Direct-match script has syntax errors' }
foreach ($name in @('Get-MatchOwnedStarCraftIdentities', 'Get-MatchCrashReports', 'Wait-ForMatchCrashReports', 'Save-MatchCrashReportBytes')) {
    $function = $ast.Find({param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $true)
    if (-not $function) { throw "Missing crash report helper: $name" }
    . ([ScriptBlock]::Create($function.Extent.Text))
}

$fixturePrefix = [IO.Path]::GetFullPath((Join-Path $repo 'build/direct-match-test-fixtures')).TrimEnd('\') + '\'
$fixtureRoot = [IO.Path]::GetFullPath((Join-Path $fixturePrefix ([guid]::NewGuid().ToString())))
if (-not $fixtureRoot.StartsWith($fixturePrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Fixture path escapes the test directory'
}
$runtimeA = Join-Path $fixtureRoot 'host'
$runtimeB = Join-Path $fixtureRoot 'opponent'
$script:startedAtUtc = [DateTime]::UtcNow.AddMinutes(-1)
$script:crashReportObservations = @{}
try {
    $launchTime = [DateTime]::UtcNow
    $runnerPath = Join-Path $fixtureRoot 'host/injectory_x86.exe'
    $gamePath = Join-Path $fixtureRoot 'host/StarCraft.exe'
    $processRows = @(
        [pscustomobject]@{id=501; parent_id=1; path=$runnerPath; started_utc=$launchTime}
        [pscustomobject]@{id=600; parent_id=501; path=$gamePath; started_utc=$launchTime.AddSeconds(1)}
        [pscustomobject]@{id=601; parent_id=1; path=(Join-Path $fixtureRoot 'unrelated/StarCraft.exe'); started_utc=$launchTime.AddSeconds(1)}
        [pscustomobject]@{id=602; parent_id=999; path=$gamePath; started_utc=$launchTime.AddSeconds(1)}
    )
    $launcher = [pscustomobject]@{id=501; path=$runnerPath; started_utc=$launchTime; expected_game_path=$gamePath}
    $owned = @(Get-MatchOwnedStarCraftIdentities -Processes $processRows -Launchers @($launcher))
    if ($owned.Count -ne 1 -or $owned[0].id -ne 600) {
        throw 'Process ownership resolver included an unrelated game or missed the launched descendant'
    }
    $unverifiable = @(Get-MatchOwnedStarCraftIdentities -Processes @($processRows | Where-Object id -ne 501) -Launchers @($launcher))
    if ($unverifiable.Count -ne 0) { throw 'Process ancestry without a verified launcher root was accepted' }

    foreach ($runtime in @($runtimeA, $runtimeB)) {
        New-Item -ItemType Directory -Path (Join-Path $runtime 'Errors') -Force | Out-Null
    }
    $old = Join-Path $runtimeA 'Errors/old.txt'
    Set-Content -LiteralPath $old -Value 'EXCEPTION: old crash' -Encoding ascii
    [IO.File]::SetLastWriteTimeUtc($old, $script:startedAtUtc.AddMinutes(-1))
    Set-Content -LiteralPath (Join-Path $runtimeB 'Errors/warning.txt') -Value 'Palette warning' -Encoding ascii
    $audit = Wait-ForMatchCrashReports -TimeoutMilliseconds 1000 -StabilityMilliseconds 50
    if (-not $audit.complete -or $audit.crashes.Count -ne 0) {
        throw 'Historical crashes or stable warnings were counted as fresh crashes or incomplete reports'
    }

    $opponentCrash = Join-Path $runtimeB 'Errors/fresh.txt'
    [IO.File]::WriteAllText($opponentCrash, "EXCEPTION: 0xE06D7363`n")
    $audit = Wait-ForMatchCrashReports -TimeoutMilliseconds 1000 -StabilityMilliseconds 50
    if (-not $audit.complete -or $audit.crashes.Count -ne 1 -or $audit.crashes[0].side -ne 'opponent') {
        throw 'Fresh opponent crash was not identified from a stable snapshot'
    }

    $hostCrash = Join-Path $runtimeA 'Errors/fresh.txt'
    [IO.File]::WriteAllText($hostCrash, "EXCEPTION: 0xC0000005`n")
    $audit = Wait-ForMatchCrashReports -TimeoutMilliseconds 1000 -StabilityMilliseconds 50
    if (-not $audit.complete -or $audit.crashes.Count -ne 2 -or @($audit.crashes | Where-Object side -eq 'protodd').Count -ne 1) {
        throw 'Fresh host crash was not identified correctly'
    }

    $partialPath = Join-Path $runtimeA 'Errors/writer.txt'
    $writer = [IO.File]::Open($partialPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $partial = [Text.Encoding]::UTF8.GetBytes('EXCEPT')
        $writer.Write($partial, 0, $partial.Length)
        $writer.Flush()
        $audit = Wait-ForMatchCrashReports -TimeoutMilliseconds 120 -StabilityMilliseconds 50
        if ($audit.complete -or @($audit.pending | Where-Object { $_.file.Name -eq 'writer.txt' }).Count -ne 1) {
            throw 'A locked partial report was not classified as incomplete'
        }
        $final = [Text.Encoding]::UTF8.GetBytes("ION: writer finalized`n")
        $writer.Write($final, 0, $final.Length)
        $writer.Flush()
    } finally { $writer.Dispose() }
    $audit = Wait-ForMatchCrashReports -TimeoutMilliseconds 1000 -StabilityMilliseconds 50
    $finalized = @($audit.crashes | Where-Object { $_.file.Name -eq 'writer.txt' })
    if (-not $audit.complete -or $finalized.Count -ne 1 -or
        [Text.Encoding]::UTF8.GetString($finalized[0].bytes) -ne "EXCEPTION: writer finalized`n") {
        throw 'Finalized writer report was not archived from its complete stable bytes'
    }
    $archive = Join-Path $fixtureRoot 'archived.txt'
    Save-MatchCrashReportBytes -Path $archive -Bytes $finalized[0].bytes
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $finalized[0].sha256) {
        throw 'Archived crash report bytes did not match the stable snapshot hash'
    }
    'Direct-match stable crash report regression passed'
} finally {
    $resolved = [IO.Path]::GetFullPath($fixtureRoot)
    if (-not $resolved.StartsWith($fixturePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to remove a fixture outside its test directory'
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue
}
