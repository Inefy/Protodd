param(
    [string]$Weights = "",
    [string]$BuildDirectory = "build/trained-controller",
    [string]$BwapiRoot = "build/_deps/bwapi-src",
    [string]$Python = "",
    [switch]$Exclusive,
    [switch]$StandardOpenings
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
function Resolve-RepoPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $repo $Path))
}
$weightsPath = Resolve-RepoPath $Weights
$buildPath = Resolve-RepoPath $BuildDirectory
$bwapiPath = Resolve-RepoPath $BwapiRoot
if ([string]::IsNullOrWhiteSpace($Weights)) {
    throw "Trained weights are not included in the source archive; pass -Weights with an exported weights.bin"
}
$pythonPath = if ([string]::IsNullOrWhiteSpace($Python)) {
    (Get-Command python -CommandType Application -ErrorAction Stop).Source
} else { Resolve-RepoPath $Python }
if (-not (Test-Path -LiteralPath $pythonPath -PathType Leaf)) {
    throw "Python executable was not found: $pythonPath"
}
$manifestPath = Join-Path (Split-Path -Parent $weightsPath) "manifest.json"
$hybrid = if ($Exclusive) { "OFF" } else { "ON" }
$allin = if ($StandardOpenings) { "OFF" } else { "ON" }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$weightsHash = (Get-FileHash -LiteralPath $weightsPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($manifest.schema -ne "protodd-whole-game-multislot-weights-v2" -or
    $manifest.maximum_slots -ne 6 -or $manifest.weights_sha256 -ne $weightsHash) {
    throw "Expected an intact exported six-slot model with a matching manifest"
}
& cmake -S $repo -B $buildPath -G "Visual Studio 17 2022" -A Win32 `
    "-DPROTODD_BUILD_BWAPI_MODULE=ON" "-DPROTODD_BUILD_TESTS=ON" `
    "-DBWAPI_ROOT=$bwapiPath" "-DPython3_EXECUTABLE=$pythonPath" `
    "-DPROTODD_WHOLE_GAME_WEIGHTS=$weightsPath" "-DPROTODD_WHOLE_GAME_CONTROL=ON" `
    "-DPROTODD_WHOLE_GAME_EVALUATION_BUILD=ON" "-DPROTODD_WHOLE_GAME_HYBRID=$hybrid" `
    "-DPROTODD_NATIVE_ALLIN_OPENING=$allin"
if ($LASTEXITCODE -ne 0) { throw "Trained-controller configuration failed" }
& cmake --build $buildPath --config Release --parallel 8
if ($LASTEXITCODE -ne 0) { throw "Trained-controller build failed" }
& ctest --test-dir $buildPath -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Trained-controller tests failed" }
if ((Get-FileHash -LiteralPath $weightsPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $weightsHash) {
    throw "Model weights changed during the build"
}
$dllPath = Join-Path $buildPath "Release/ProtoddEvaluation.dll"
Write-Output "Local evaluation controller: $dllPath"
Write-Output "Hybrid control: $hybrid"
Write-Output "Native all-in opening: $allin"
Write-Output "Embedded weights SHA256: $weightsHash"
Write-Output "DLL SHA256: $((Get-FileHash -LiteralPath $dllPath -Algorithm SHA256).Hash)"
