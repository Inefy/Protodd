$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$scriptPath = Join-Path $repo 'scripts/direct-match.ps1'
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($scriptPath, [ref]$null, [ref]$parseErrors)
if ($parseErrors.Count) { throw 'Direct-match script has syntax errors' }
$function = $ast.Find({param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-MatchCrashReports'
}, $true)
if (-not $function) { throw 'Missing runtime crash detector' }
. ([ScriptBlock]::Create($function.Extent.Text))

$fixturePrefix = [IO.Path]::GetFullPath((Join-Path $repo 'build/direct-match-test-fixtures')).TrimEnd('\') + '\'
$fixtureRoot = [IO.Path]::GetFullPath((Join-Path $fixturePrefix ([guid]::NewGuid().ToString())))
if (-not $fixtureRoot.StartsWith($fixturePrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Fixture path escapes the test directory'
}
$runtimeA = Join-Path $fixtureRoot 'host'
$runtimeB = Join-Path $fixtureRoot 'opponent'
$script:startedAtUtc = [DateTime]::UtcNow.AddMinutes(-1)
try {
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
