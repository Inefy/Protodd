param(
    [string]$BwapiRoot = "build/_deps/bwapi-src",
    [string]$Configuration = "Release",
    [string]$PlatformToolset = "v143"
)

$ErrorActionPreference = "Stop"
$repoPath = Split-Path -Parent $PSScriptRoot
$bwapiPath = if ([System.IO.Path]::IsPathRooted($BwapiRoot)) {
    [System.IO.Path]::GetFullPath($BwapiRoot)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $repoPath $BwapiRoot))
}

$bwapiProject = Join-Path $bwapiPath "bwapi/BWAPILIB/BWAPILIB.vcxproj"
$revisionScript = Join-Path $bwapiPath "bwapi/revisionUpdate.vbs"
$bwapiLibrary = Join-Path $bwapiPath "bwapi/BWAPILIB/$Configuration/BWAPILIB.lib"
$bwapiHeader = Join-Path $bwapiPath "bwapi/include/BWAPI.h"
if (-not (Test-Path -LiteralPath $bwapiProject) -or
    -not (Test-Path -LiteralPath $revisionScript) -or
    -not (Test-Path -LiteralPath $bwapiHeader)) {
    throw "BwapiRoot must contain the official BWAPI 4.4 source tree: $bwapiPath"
}

$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "Visual Studio Installer (vswhere.exe) was not found"
}
$vsInstall = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsInstall) {
    throw "Install Visual Studio 2022 Build Tools with the C++ x86/x64 workload"
}
$msbuild = Join-Path $vsInstall "MSBuild/Current/Bin/MSBuild.exe"
if (-not (Test-Path -LiteralPath $msbuild)) {
    throw "MSBuild was not found under $vsInstall"
}

Push-Location (Split-Path -Parent $revisionScript)
try {
    & cscript //nologo (Split-Path -Leaf $revisionScript)
    if ($LASTEXITCODE -ne 0) { throw "BWAPI revision header generation failed" }
} finally {
    Pop-Location
}

& $msbuild $bwapiProject /m "/p:Configuration=$Configuration" /p:Platform=Win32 `
    "/p:PlatformToolset=$PlatformToolset" /verbosity:minimal
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $bwapiLibrary)) {
    throw "Official BWAPILIB Release|Win32 build failed"
}

$buildPath = Join-Path $repoPath "build/tournament"
& cmake -S $repoPath -B $buildPath -G "Visual Studio 17 2022" -A Win32 `
    -DPROTODD_BUILD_TESTS=ON -DPROTODD_BUILD_BWAPI_MODULE=ON `
    "-DBWAPI_ROOT=$bwapiPath" "-DBWAPI_LIBRARY=$bwapiLibrary"
if ($LASTEXITCODE -ne 0) { throw "Tournament CMake configuration failed" }
& cmake --build $buildPath --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "Tournament DLL build failed" }
& ctest --test-dir $buildPath -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Tournament Release tests failed" }

$dllPath = Join-Path $buildPath "$Configuration/Protodd.dll"
if (-not (Test-Path -LiteralPath $dllPath)) {
    throw "Expected tournament DLL was not produced: $dllPath"
}
$hash = (Get-FileHash -LiteralPath $dllPath -Algorithm SHA256).Hash
Write-Output "Protodd tournament DLL: $dllPath"
Write-Output "SHA256: $hash"
