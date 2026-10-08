param(
    [string]$Dll = 'build/tournament/Release/AuditFaultScenario.dll',
    [string]$Label = 't009-write-denied-v1',
    [string]$RuntimePath = 'build/stock-certification/goal-t009-write-denied-20261007/match-runtime-a',
    [string]$ArchiveParent = 'build/stock-certification/goal-t009-write-denied-20261007'
)

$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildRoot = [IO.Path]::GetFullPath((Join-Path $repo 'build')).TrimEnd('\') + '\'
$runtime = if ([IO.Path]::IsPathRooted($RuntimePath)) {
    [IO.Path]::GetFullPath($RuntimePath)
} else {
    [IO.Path]::GetFullPath((Join-Path $repo $RuntimePath))
}
$archiveRoot = if ([IO.Path]::IsPathRooted($ArchiveParent)) {
    [IO.Path]::GetFullPath($ArchiveParent)
} else {
    [IO.Path]::GetFullPath((Join-Path $repo $ArchiveParent))
}
foreach ($path in @($runtime, $archiveRoot)) {
    if (-not $path.StartsWith($buildRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "T009 ACL test paths must stay under build/: $path"
    }
}
if ($Label -notmatch '^[a-zA-Z0-9-]+$') { throw 'Invalid label' }
if (Get-Process StarCraft -ErrorAction SilentlyContinue) {
    throw 'Existing game; refusing to change runtime ACLs'
}

$writePath = Join-Path $runtime 'bwapi-data/write'
if (-not (Test-Path -LiteralPath $writePath -PathType Container)) {
    throw "Runtime output directory is missing: $writePath"
}
$archivePath = Join-Path $archiveRoot "scenarios-$Label"
if (Test-Path -LiteralPath $archivePath) { throw "Existing audit archive: $archivePath" }
$identity = [Security.Principal.WindowsIdentity]::GetCurrent().User
$originalAcl = Get-Acl -LiteralPath $writePath
$originalSddl = $originalAcl.GetSecurityDescriptorSddlForm(
    [Security.AccessControl.AccessControlSections]::All)
$deniedAcl = Get-Acl -LiteralPath $writePath
$denyRule = [Security.AccessControl.FileSystemAccessRule]::new(
    $identity,
    [Security.AccessControl.FileSystemRights]::Write,
    ([Security.AccessControl.InheritanceFlags]::ContainerInherit -bor
        [Security.AccessControl.InheritanceFlags]::ObjectInherit),
    [Security.AccessControl.PropagationFlags]::None,
    [Security.AccessControl.AccessControlType]::Deny)
$deniedAcl.AddAccessRule($denyRule)
$deniedSddl = $deniedAcl.GetSecurityDescriptorSddlForm(
    [Security.AccessControl.AccessControlSections]::All)
$aclTouched = $false
$probePath = Join-Path $writePath ".t009-acl-probe-$Label.tmp"
$writeWasDenied = $false
$restoredWriteProbe = $false

try {
    $aclTouched = $true
    Set-Acl -LiteralPath $writePath -AclObject $deniedAcl
    try {
        [IO.File]::WriteAllText($probePath, 'must be denied')
    } catch [UnauthorizedAccessException] {
        $writeWasDenied = $true
    } catch [IO.IOException] {
        if (($_.Exception.HResult -band 0xffff) -eq 5) {
            $writeWasDenied = $true
        } else {
            throw
        }
    }
    if (-not $writeWasDenied -or (Test-Path -LiteralPath $probePath)) {
        throw 'The explicit deny ACL did not block a write probe'
    }

    & (Join-Path $PSScriptRoot 'run-audit-scenarios.ps1') `
        -Dll $Dll `
        -Label $Label `
        -Cases @('lifecycle-write-denied') `
        -RuntimePath $runtime `
        -ArchiveParent $archiveRoot
} finally {
    if ($aclTouched) {
        Set-Acl -LiteralPath $writePath -AclObject $originalAcl
        $restoredAcl = Get-Acl -LiteralPath $writePath
        $restoredSddl = $restoredAcl.GetSecurityDescriptorSddlForm(
            [Security.AccessControl.AccessControlSections]::All)
        if ($restoredSddl -ne $originalSddl) {
            throw 'The original output-directory security descriptor was not restored'
        }
        [IO.File]::WriteAllText($probePath, 'restoration verified')
        Remove-Item -LiteralPath $probePath -Force
        $restoredWriteProbe = $true
    }
    if (Test-Path -LiteralPath $probePath) {
        Remove-Item -LiteralPath $probePath -Force
    }
}

$scenarioTrace = Join-Path $archivePath 'lifecycle-write-denied.csv'
if (-not (Test-Path -LiteralPath $scenarioTrace -PathType Leaf)) {
    throw "The access-denied scenario did not archive a trace: $scenarioTrace"
}
$traceHash = (Get-FileHash -LiteralPath $scenarioTrace -Algorithm SHA256).Hash.ToLowerInvariant()
$receipt = [ordered]@{
    scenario = 'lifecycle-write-denied'
    identity_sid = $identity.Value
    denied_right = 'Write (inherited by files and directories)'
    write_probe_denied = $writeWasDenied
    original_acl_restored = $restoredSddl -eq $originalSddl
    write_after_restore = $restoredWriteProbe
    original_sddl = $originalSddl
    denied_sddl = $deniedSddl
    restored_sddl = $restoredSddl
    scenario_trace_sha256 = $traceHash
    runtime_profile = 'stock-certification'
    archive = $archivePath
}
$receiptPath = Join-Path $archivePath 'write-acl-receipt.json'
[IO.File]::WriteAllText($receiptPath, ($receipt | ConvertTo-Json -Depth 5),
    [Text.UTF8Encoding]::new($false))
Get-Content -LiteralPath $scenarioTrace
Get-Content -LiteralPath $receiptPath
