$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (Get-Process StarCraft -ErrorAction SilentlyContinue) {
    Write-Output 'SKIP: StarCraft is running; the isolated runtime recovery regression requires exclusive engine access'
    exit 77
}
$profileTemplate = Join-Path $repo 'build/patched-diagnostic-template/bwapi-data/BWAPI.dll'
$initialTemplate = Join-Path $repo 'build/direct-template/bwapi-data/BWAPI.dll'
if (-not (Test-Path -LiteralPath $profileTemplate -PathType Leaf) -and
    -not (Test-Path -LiteralPath $initialTemplate -PathType Leaf)) {
    Write-Output 'SKIP: local BWAPI runtime template is not part of the source archive'
    exit 77
}

Import-Module (Join-Path $repo 'scripts/RuntimeProfiles.psm1') -Force
$profile = Get-PatchedRuntimeProfile -RepositoryRoot $repo
$fixturePrefix = [IO.Path]::GetFullPath((Join-Path $repo 'build/runtime-recovery-tests')).TrimEnd('\') + '\'
$fixtureName = [guid]::NewGuid().ToString('N')
$fixtureRoot = [IO.Path]::GetFullPath((Join-Path $fixturePrefix $fixtureName))
if (-not $fixtureRoot.StartsWith($fixturePrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Runtime recovery fixture escaped its isolated build directory'
}
$runtime = Join-Path $fixtureRoot 'runtime'
$restoreScript = Join-Path $repo 'scripts/restore-match-runtime.ps1'

try {
    New-Item -ItemType Directory -Path (Join-Path $runtime 'bwapi-data') -Force | Out-Null
    foreach ($name in @('StarCraft.exe', 'injectory_x86.exe', 'wmode.dll')) {
        [IO.File]::WriteAllText((Join-Path $runtime $name), 'fixture')
    }
    Copy-Item -LiteralPath $profile.path -Destination (Join-Path $runtime 'bwapi-data/BWAPI.dll')

    # Fail one directory-creation call to model a transient filesystem error.
    # Restore must surface it and be retryable after the fault is removed.
    $global:protoddRecoveryFailurePath = [IO.Path]::GetFullPath(
        (Join-Path $runtime 'bwapi-data/read'))
    $global:protoddRecoveryFailureArmed = $true
    function global:New-Item {
        [CmdletBinding()]
        param(
            [string]$Path,
            [string]$LiteralPath,
            [string]$ItemType,
            [switch]$Force
        )
        $candidate = if ($LiteralPath) { $LiteralPath } else { $Path }
        if ($global:protoddRecoveryFailureArmed -and $candidate -and
            [IO.Path]::GetFullPath($candidate).Equals(
                $global:protoddRecoveryFailurePath, [StringComparison]::OrdinalIgnoreCase)) {
            $global:protoddRecoveryFailureArmed = $false
            throw 'Injected transient filesystem directory-creation failure'
        }
        Microsoft.PowerShell.Management\New-Item @PSBoundParameters
    }
    $restoreFailed = $false
    try {
        & $restoreScript -Runtime $runtime -Profile 'patched-diagnostic' | Out-Null
    } catch {
        $restoreFailed = $true
    } finally {
        Remove-Item -LiteralPath Function:\global:New-Item -Force -ErrorAction SilentlyContinue
    }
    Remove-Variable -Name protoddRecoveryFailurePath, protoddRecoveryFailureArmed -Scope Global -ErrorAction SilentlyContinue
    if (-not $restoreFailed) { throw 'Runtime restore ignored an injected filesystem failure' }

    & $restoreScript -Runtime $runtime -Profile 'patched-diagnostic' | Out-Null
    $markerPath = Join-Path $runtime '.protodd-runtime-profile.json'
    $marker = Get-Content -LiteralPath $markerPath -Raw | ConvertFrom-Json
    if ($marker.profile -ne 'patched-diagnostic' -or
        (Get-FileHash (Join-Path $runtime 'bwapi-data/BWAPI.dll') -Algorithm SHA256).Hash.ToLowerInvariant() -ne $profile.dll_sha256) {
        throw 'Runtime recovery did not restore the pinned engine profile'
    }

    # Simulate Tournament Manager cleanup after a successful restore. The
    # second restore must refill missing support files without replacing the
    # already verified engine or profile marker.
    Remove-Item -LiteralPath (Join-Path $runtime 'bwapi-data/AI') -Recurse -Force
    Remove-Item -LiteralPath (Join-Path $runtime 'characters') -Recurse -Force
    $map = Get-ChildItem -LiteralPath (Join-Path $runtime 'maps') -Recurse -Filter '*.scx' |
        Select-Object -First 1
    if (-not $map) { throw 'Initial runtime restore did not extract any tournament maps' }
    Remove-Item -LiteralPath $map.FullName -Force
    & $restoreScript -Runtime $runtime -Profile 'patched-diagnostic' | Out-Null
    if (-not (Test-Path -LiteralPath (Join-Path $runtime 'bwapi-data/AI'))) {
        throw 'Cleanup recovery did not recreate the AI directory'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $runtime 'characters'))) {
        throw 'Cleanup recovery did not recreate character data'
    }
    if (-not (Test-Path -LiteralPath $map.FullName -PathType Leaf)) {
        throw 'Cleanup recovery did not restore the removed map'
    }

    # A verified marker prevents a substituted engine from being silently
    # accepted. Restoring the original pinned file allows recovery again.
    [IO.File]::WriteAllText((Join-Path $runtime 'bwapi-data/BWAPI.dll'), 'bad engine')
    $tamperRejected = $false
    try {
        & $restoreScript -Runtime $runtime -Profile 'patched-diagnostic' | Out-Null
    } catch {
        $tamperRejected = $true
    }
    if (-not $tamperRejected) { throw 'Runtime restore accepted a changed engine after profile verification' }
    Copy-Item -LiteralPath $profile.path -Destination (Join-Path $runtime 'bwapi-data/BWAPI.dll') -Force
    & $restoreScript -Runtime $runtime -Profile 'patched-diagnostic' | Out-Null

    'Runtime filesystem-failure and cleanup-recovery regression passed'
} finally {
    $resolved = [IO.Path]::GetFullPath($fixtureRoot)
    if (-not $resolved.StartsWith($fixturePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to remove a fixture outside its build directory'
    }
    if (Test-Path -LiteralPath $resolved) {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
