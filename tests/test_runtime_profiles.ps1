$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Import-Module (Join-Path $repo 'scripts/RuntimeProfiles.psm1') -Force
$stock = Get-StockRuntimeProfile -RepositoryRoot $repo
$patched = Get-PatchedRuntimeProfile -RepositoryRoot $repo
if ($stock.dll_sha256 -eq $patched.dll_sha256) {
    throw 'Stock certification and patched diagnostic profiles must use distinct engine DLLs'
}
$stockRoot = Resolve-MatchRuntimeRoot -Profile 'stock-certification' -RepositoryRoot $repo
$stockSetRoot = Resolve-MatchRuntimeRoot -Profile 'stock-certification' -RepositoryRoot $repo -RuntimeSet 'isolation-check'
$patchedRoot = Resolve-MatchRuntimeRoot -Profile 'patched-diagnostic' -RepositoryRoot $repo
$patchedSetRoot = Resolve-MatchRuntimeRoot -Profile 'patched-diagnostic' -RepositoryRoot $repo -RuntimeSet 'isolation-check'
$stockLogs = Resolve-MatchArchiveRoot -Profile 'stock-certification' -RepositoryRoot $repo -RuntimeSet 'isolation-check'
$patchedLogs = Resolve-MatchArchiveRoot -Profile 'patched-diagnostic' -RepositoryRoot $repo -RuntimeSet 'isolation-check'
if ($stockRoot -eq $patchedRoot -or $stockSetRoot -eq $patchedSetRoot -or
    $stockLogs -eq $patchedLogs -or
    -not $stockSetRoot.StartsWith((Join-Path $repo 'build/stock-certification') + '\', [StringComparison]::OrdinalIgnoreCase) -or
    -not $patchedSetRoot.StartsWith((Join-Path $repo 'build/patched-diagnostic') + '\', [StringComparison]::OrdinalIgnoreCase) -or
    -not $patchedLogs.StartsWith((Join-Path $repo 'build/direct-logs/patched-diagnostic') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Named runtime or result sets are not isolated by profile'
}
$collisionNameRoot = Resolve-MatchRuntimeRoot -Profile 'patched-diagnostic' -RepositoryRoot $repo -RuntimeSet 'stock-certification'
if ($collisionNameRoot -eq $stockRoot -or $collisionNameRoot -eq $stockSetRoot) {
    throw 'A patched runtime set can collide with the stock certification profile root'
}
$unsafeSetRejected = $false
try { [void](Resolve-MatchRuntimeRoot -Profile 'patched-diagnostic' -RepositoryRoot $repo -RuntimeSet '..') }
catch { $unsafeSetRejected = $true }
if (-not $unsafeSetRejected) { throw 'Runtime profile accepted a path-traversing set name' }
$prepPatchedScript = Join-Path $repo 'scripts/prepare-patched-runtime.ps1'
$parseErrors = $null
[void][Management.Automation.Language.Parser]::ParseFile($prepPatchedScript, [ref]$null, [ref]$parseErrors)
if ($parseErrors.Count) { throw 'Patched runtime preparation script has syntax errors' }
$temporary = Join-Path ([IO.Path]::GetTempPath()) ("protodd-stock-profile-" + [Guid]::NewGuid().ToString('N') + '.dll')
try {
    $extracted = Export-StockRuntimeDll -RepositoryRoot $repo -Destination $temporary
    $actual = (Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $stock.dll_sha256 -or $extracted.archive_sha256 -ne $stock.archive_sha256) {
        throw 'Stock runtime extraction did not preserve the official archive and DLL hashes'
    }
    Write-Output "Runtime profile checks passed: stock=$($stock.dll_sha256) patched=$($patched.dll_sha256); named runtime and result sets are isolated"
} finally {
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
}
