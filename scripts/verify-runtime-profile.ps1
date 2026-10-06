param(
    [Parameter(Mandatory=$true)][string]$Runtime,
    [ValidateSet('patched-diagnostic', 'stock-certification')]
    [string]$Profile
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Import-Module (Join-Path $PSScriptRoot 'RuntimeProfiles.psm1') -Force
$runtimePath = [IO.Path]::GetFullPath($Runtime)
$info = Get-RuntimeProfileInfo -Profile $Profile -RepositoryRoot $repo
$engine = Join-Path $runtimePath 'bwapi-data/BWAPI.dll'
$markerPath = Join-Path $runtimePath '.protodd-runtime-profile.json'
if (-not (Test-Path -LiteralPath $engine -PathType Leaf) -or
    -not (Test-Path -LiteralPath $markerPath -PathType Leaf)) {
    throw "Runtime is not initialized as a verified profile: $runtimePath"
}
$marker = Get-Content -LiteralPath $markerPath -Raw | ConvertFrom-Json
$actualHash = (Get-FileHash -LiteralPath $engine -Algorithm SHA256).Hash.ToLowerInvariant()
if ($marker.schema -ne 'protodd-runtime-profile-v1' -or $marker.profile -ne $Profile -or
    $marker.engine_dll_sha256 -ne $actualHash -or $actualHash -ne $info.dll_sha256) {
    throw "Runtime profile marker or engine hash is invalid: $runtimePath"
}
"RUNTIME_PROFILE=$Profile"
"BWAPI_DLL_SHA256=$actualHash"
"SOURCE=$($info.source)"
