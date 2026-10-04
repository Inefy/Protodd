$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$repo = Split-Path -Parent $PSScriptRoot
$scriptPath = Join-Path $repo 'scripts/build-bwapi-runtime.ps1'
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($scriptPath, [ref]$null, [ref]$parseErrors)
if ($parseErrors.Count) { throw 'BWAPI runtime build script has syntax errors' }
foreach ($name in @('Get-ZipEntrySha256', 'Deploy-BwapiRuntimeArtifacts')) {
    $function = $ast.Find({param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $true)
    if (-not $function) { throw "Missing deployment helper: $name" }
    . ([ScriptBlock]::Create($function.Extent.Text))
}

$fixturePrefix = [IO.Path]::GetFullPath((Join-Path $repo 'build/bwapi-runtime-deploy-test-fixtures')).TrimEnd('\') + '\'
$fixtureRoot = [IO.Path]::GetFullPath((Join-Path $fixturePrefix ([guid]::NewGuid().ToString())))
if (-not $fixtureRoot.StartsWith($fixturePrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Fixture path escapes the deployment test directory'
}
function New-DeployFixture {
    param([string]$Root, [switch]$MissingEntry)
    New-Item -ItemType Directory -Path $Root -Force | Out-Null
    $dll = Join-Path $Root 'source.dll'
    [IO.File]::WriteAllBytes($dll, [Text.Encoding]::UTF8.GetBytes('new runtime bytes'))
    $targets = @()
    foreach ($index in 0..2) {
        $runtime = Join-Path $Root "runtime-$index/bwapi-data"
        New-Item -ItemType Directory -Path $runtime -Force | Out-Null
        $target = Join-Path $runtime 'BWAPI.dll'
        [IO.File]::WriteAllBytes($target, [Text.Encoding]::UTF8.GetBytes("old runtime $index"))
        $targets += $target
    }
    $packageDir = Join-Path $Root 'required'
    New-Item -ItemType Directory -Path $packageDir -Force | Out-Null
    $package = Join-Path $packageDir 'Required_BWAPI_440.zip'
    $archive = [IO.Compression.ZipFile]::Open($package, [IO.Compression.ZipArchiveMode]::Create)
    try {
        $entryName = if ($MissingEntry) { 'other/file.txt' } else { 'bwapi-data/BWAPI.dll' }
        $entry = $archive.CreateEntry($entryName)
        $stream = $entry.Open()
        try {
            $bytes = [Text.Encoding]::UTF8.GetBytes('old packaged runtime')
            $stream.Write($bytes, 0, $bytes.Length)
        } finally { $stream.Dispose() }
    } finally { $archive.Dispose() }
    [pscustomobject]@{Root=$Root; Dll=$dll; Targets=$targets; Package=$package; Backup=(Join-Path $Root 'backup')}
}
function Get-Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
try {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $happy = New-DeployFixture -Root (Join-Path $fixtureRoot 'happy')
    $sourceHash = Get-Hash $happy.Dll
    Deploy-BwapiRuntimeArtifacts -DllPath $happy.Dll -RuntimeTargets $happy.Targets `
        -PackagePath $happy.Package -BackupRoot $happy.Backup
    foreach ($target in $happy.Targets) {
        if ((Get-Hash $target) -ne $sourceHash) { throw "Runtime target was not replaced and verified: $target" }
    }
    if ((Get-ZipEntrySha256 -ArchivePath $happy.Package -EntryName 'bwapi-data/BWAPI.dll') -ne $sourceHash) {
        throw 'Canonical package entry does not match the deployed DLL'
    }
    if (@(Get-ChildItem -LiteralPath $happy.Backup -File).Count -ne 4) {
        throw 'Original DLL and package backups were not preserved'
    }

    $missingPackage = New-DeployFixture -Root (Join-Path $fixtureRoot 'missing-package')
    $before = @($missingPackage.Targets | ForEach-Object { Get-Hash $_ })
    $missingPackage.Package = Join-Path $missingPackage.Root 'missing.zip'
    try {
        Deploy-BwapiRuntimeArtifacts -DllPath $missingPackage.Dll -RuntimeTargets $missingPackage.Targets `
            -PackagePath $missingPackage.Package -BackupRoot $missingPackage.Backup
        throw 'Missing canonical package was accepted'
    } catch {
        if ($_.Exception.Message -eq 'Missing canonical package was accepted') { throw }
    }
    if (@($missingPackage.Targets | ForEach-Object { Get-Hash $_ }) -join ',' -ne ($before -join ',')) {
        throw 'Missing package changed a runtime destination before validation'
    }

    $missingEntry = New-DeployFixture -Root (Join-Path $fixtureRoot 'missing-entry') -MissingEntry
    $before = @($missingEntry.Targets | ForEach-Object { Get-Hash $_ })
    try {
        Deploy-BwapiRuntimeArtifacts -DllPath $missingEntry.Dll -RuntimeTargets $missingEntry.Targets `
            -PackagePath $missingEntry.Package -BackupRoot $missingEntry.Backup
        throw 'Package missing the expected entry was accepted'
    } catch {
        if ($_.Exception.Message -eq 'Package missing the expected entry was accepted') { throw }
    }
    if (@($missingEntry.Targets | ForEach-Object { Get-Hash $_ }) -join ',' -ne ($before -join ',')) {
        throw 'Missing package entry changed a runtime destination before validation'
    }

    $rollback = New-DeployFixture -Root (Join-Path $fixtureRoot 'rollback')
    $beforeTargets = @($rollback.Targets | ForEach-Object { Get-Hash $_ })
    $beforePackage = Get-Hash $rollback.Package
    $injected = { param($index, $target) if ($index -eq 1) { throw 'injected replacement failure' } }
    try {
        Deploy-BwapiRuntimeArtifacts -DllPath $rollback.Dll -RuntimeTargets $rollback.Targets `
            -PackagePath $rollback.Package -BackupRoot $rollback.Backup -BeforeReplace $injected
        throw 'Injected replacement failure was not raised'
    } catch {
        if ($_.Exception.Message -eq 'Injected replacement failure was not raised') { throw }
        if ($_.Exception.Message -notmatch 'injected replacement failure') { throw }
    }
    if (@($rollback.Targets | ForEach-Object { Get-Hash $_ }) -join ',' -ne ($beforeTargets -join ',') -or
        (Get-Hash $rollback.Package) -ne $beforePackage) {
        throw 'A failed deployment did not restore all original hashes'
    }

    $postReplace = New-DeployFixture -Root (Join-Path $fixtureRoot 'post-replace-hash-failure')
    $beforeTargets = @($postReplace.Targets | ForEach-Object { Get-Hash $_ })
    $beforePackage = Get-Hash $postReplace.Package
    $tamperAfterReplace = {
        param($index, $target)
        if ($index -eq 1) { [IO.File]::WriteAllText($target, 'tampered after atomic replacement') }
    }
    try {
        Deploy-BwapiRuntimeArtifacts -DllPath $postReplace.Dll -RuntimeTargets $postReplace.Targets `
            -PackagePath $postReplace.Package -BackupRoot $postReplace.Backup -AfterReplace $tamperAfterReplace
        throw 'Post-replacement hash mismatch was not detected'
    } catch {
        if ($_.Exception.Message -eq 'Post-replacement hash mismatch was not detected') { throw }
        if ($_.Exception.Message -notmatch 'Post-replacement hash verification failed') { throw }
    }
    if (@($postReplace.Targets | ForEach-Object { Get-Hash $_ }) -join ',' -ne ($beforeTargets -join ',') -or
        (Get-Hash $postReplace.Package) -ne $beforePackage) {
        throw 'Post-replacement hash failure did not restore all original hashes'
    }

    $rollbackFailure = New-DeployFixture -Root (Join-Path $fixtureRoot 'rollback-failure')
    $rollbackFailureOriginal = Get-Hash $rollbackFailure.Targets[0]
    $injectedRollbackFailure = { param($index, $target) throw 'injected rollback failure' }
    try {
        Deploy-BwapiRuntimeArtifacts -DllPath $rollbackFailure.Dll -RuntimeTargets $rollbackFailure.Targets `
            -PackagePath $rollbackFailure.Package -BackupRoot $rollbackFailure.Backup `
            -BeforeReplace $injected -BeforeRollback $injectedRollbackFailure
        throw 'Injected rollback failure was not reported'
    } catch {
        if ($_.Exception.Message -eq 'Injected rollback failure was not reported') { throw }
        if ($_.Exception.Message -notmatch 'Rollback also failed; recovery files preserved') { throw }
    }
    $recovery = @(Get-ChildItem -LiteralPath $rollbackFailure.Root -Filter '.protodd-*.rollback' -File -Recurse)
    if ($recovery.Count -lt 1) { throw 'Rollback failure did not preserve adjacent recovery copies' }
    $preservedOriginal = @($recovery | Where-Object { (Get-Hash $_.FullName) -eq $rollbackFailureOriginal })
    if ($preservedOriginal.Count -lt 1) { throw 'Preserved rollback recovery copy did not contain the original destination bytes' }
    'BWAPI runtime deployment transaction regression passed'
} finally {
    $resolved = [IO.Path]::GetFullPath($fixtureRoot)
    if (-not $resolved.StartsWith($fixturePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to remove deployment fixtures outside the test directory'
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue
}
