param([string]$BwapiRoot = 'build/_deps/bwapi-src', [switch]$Deploy)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if ($Deploy -and (Get-Process StarCraft -ErrorAction SilentlyContinue)) {
    throw 'Cannot deploy BWAPI while StarCraft is running'
}
$root = if ([IO.Path]::IsPathRooted($BwapiRoot)) { [IO.Path]::GetFullPath($BwapiRoot) }
        else { [IO.Path]::GetFullPath((Join-Path $repo $BwapiRoot)) }
$source = Join-Path $root 'bwapi/BWAPI/Source/BWAPI/GameImpl.cpp'
$original = 'return getLatencyFrames() - (this->getFrameCount() - lastTurnFrame);'
$patched = 'return std::max(0, getLatencyFrames() - (this->getFrameCount() - static_cast<int>(lastTurnFrame)));'
$text = [IO.File]::ReadAllText($source)
if ($text.Contains($original)) {
    [IO.File]::WriteAllText($source, $text.Replace($original, $patched))
} elseif (-not $text.Contains($patched)) {
    throw 'Unrecognized BWAPI latency implementation; refusing to patch it'
}
# The official 4.4 source predates the C++17 filesystem namespace.
$pathHeader = Join-Path $root 'bwapi/Util/Source/Util/Path.h'
$pathText = [IO.File]::ReadAllText($pathHeader)
$modernPathText = $pathText.Replace('std::experimental::filesystem', 'std::filesystem')
if ($modernPathText -ne $pathText) { [IO.File]::WriteAllText($pathHeader, $modernPathText) }

# Compile the actual patched vendor method against controlled turn/frame data.
# A stale turn must never become an unsigned, enormous vector allocation.
$method = [regex]::Match([IO.File]::ReadAllText($source),
    'int GameImpl::getRemainingLatencyFrames\(\) const\s*\{[^}]+\}')
if (-not $method.Success) { throw 'Could not extract the patched latency method' }
$check = Join-Path $repo 'build/bwapi-latency-regression'
New-Item -ItemType Directory -Path $check -Force | Out-Null
$harness = @"
#include <algorithm>
#include <iostream>
#include <vector>
struct GameImpl {
    int latency = 4, frame = 100;
    unsigned long lastTurnFrame = 100;
    int getLatencyFrames() const { return latency; }
    int getFrameCount() const { return frame; }
    int getRemainingLatencyFrames() const;
};
$($method.Value)
int main() {
    GameImpl game;
    for (int delay : {0, 1, 4, 5, 19, 20, 100, 100000}) {
        game.frame = 100 + delay;
        const int remaining = game.getRemainingLatencyFrames();
        const int expected = delay < 4 ? 4 - delay : 0;
        if (remaining != expected) return 1;
        std::vector<std::vector<int>> buffer;
        buffer.resize(static_cast<std::size_t>(remaining + 15));
        if (buffer.size() > 19) return 2;
    }
    std::cout << "BWAPI latency regression passed (normal and stale network turns)\n";
}
"@
[IO.File]::WriteAllText((Join-Path $check 'test.cpp'), $harness)
@'
cmake_minimum_required(VERSION 3.24)
project(BwapiLatencyRegression LANGUAGES CXX)
add_executable(latency_regression test.cpp)
target_compile_features(latency_regression PRIVATE cxx_std_17)
'@ | Set-Content -LiteralPath (Join-Path $check 'CMakeLists.txt') -Encoding ascii
& cmake -S $check -B "$check/build" -G 'Visual Studio 17 2022' -A Win32
if ($LASTEXITCODE -ne 0) { throw 'Latency regression configuration failed' }
& cmake --build "$check/build" --config Release
if ($LASTEXITCODE -ne 0) { throw 'Latency regression compilation failed' }
& "$check/build/Release/latency_regression.exe"
if ($LASTEXITCODE -ne 0) { throw 'Patched BWAPI latency regression failed' }

$vswhere = 'C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ tools are required' }
$msbuild = Join-Path $vs 'MSBuild/Current/Bin/MSBuild.exe'
# BWAPI's legacy projects use /Zp1 for StarCraft structures. Load Windows
# declarations at their required packing before returning to that layout.
$windowsHeader = Join-Path $check 'windows-default-pack.h'
$windowsHeaderText = @'
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#pragma pack(push, 8)
#include <Windows.h>
#pragma pack(pop)
'@
if (-not (Test-Path -LiteralPath $windowsHeader) -or [IO.File]::ReadAllText($windowsHeader).Trim() -ne $windowsHeaderText.Trim()) {
    [IO.File]::WriteAllText($windowsHeader, $windowsHeaderText)
}
$savedCl = $env:CL
$env:CL = "$savedCl /FI`"$windowsHeader`""
Push-Location (Join-Path $root 'bwapi')
try {
    & cscript //nologo revisionUpdate.vbs
    if ($LASTEXITCODE -ne 0) { throw 'BWAPI revision header generation failed' }
    & $msbuild 'BWAPI/BWAPI.vcxproj' /m /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v143 /p:PostBuildEventUseInBuild=false /verbosity:minimal
    if ($LASTEXITCODE -ne 0) { throw 'Patched BWAPI runtime build failed' }
} finally { Pop-Location; $env:CL = $savedCl }
$dll = Join-Path $root 'bwapi/BWAPI/Release/BWAPI.dll'
if (-not (Test-Path -LiteralPath $dll)) { throw "Patched DLL not found: $dll" }
$dllHash = (Get-FileHash -LiteralPath $dll).Hash.ToLowerInvariant()
"BWAPI_RUNTIME=$dll"
"SHA256=$dllHash"
if ($Deploy) {
    if (Get-Process StarCraft -ErrorAction SilentlyContinue) {
        throw 'StarCraft started during the build; stop it before publishing a diagnostic profile'
    }
    $profileRoot = Join-Path $repo 'build/patched-diagnostic-template/bwapi-data'
    New-Item -ItemType Directory -Path $profileRoot -Force | Out-Null
    Copy-Item -LiteralPath $dll -Destination (Join-Path $profileRoot 'BWAPI.dll') -Force
    $sourceCommit = (& git -C $root rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -ne 0) { $sourceCommit = 'source-archive' }
    $sourcePatch = & git -C $root diff HEAD --binary 2>$null
    $patchBytes = [Text.Encoding]::UTF8.GetBytes(($sourcePatch -join "`n"))
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $patchHash = [BitConverter]::ToString($sha.ComputeHash($patchBytes)).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
    $profile = [ordered]@{
        schema = 'protodd-patched-runtime-v1'
        profile = 'patched-diagnostic'
        source_commit = ([string]$sourceCommit).Trim()
        source_dirty = @(& git -C $root status --porcelain 2>$null).Count -gt 0
        patch_sha256 = $patchHash
        dll_sha256 = $dllHash
        published_utc = [DateTime]::UtcNow.ToString('o')
    }
    $manifestPath = Join-Path $repo 'build/patched-runtime-profile.json'
    [IO.File]::WriteAllText($manifestPath, ($profile | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
    "PATCHED_DIAGNOSTIC_PROFILE=$profileRoot"
    "PROFILE_MANIFEST=$manifestPath"
}
