param([string]$BwapiRoot = 'build/_deps/bwapi-src', [switch]$Deploy)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

function Get-ZipEntrySha256 {
    param([string]$ArchivePath, [string]$EntryName)
    $archive = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        $entry = $archive.GetEntry($EntryName)
        if (-not $entry) { throw "BWAPI package does not contain the expected runtime entry '$EntryName'" }
        $stream = $entry.Open()
        try {
            $sha = [Security.Cryptography.SHA256]::Create()
            try { return ([BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '')) }
            finally { $sha.Dispose() }
        } finally { $stream.Dispose() }
    } finally { $archive.Dispose() }
}

function Deploy-BwapiRuntimeArtifacts {
    param(
        [Parameter(Mandatory)][string]$DllPath,
        [Parameter(Mandatory)][string[]]$RuntimeTargets,
        [Parameter(Mandatory)][string]$PackagePath,
        [Parameter(Mandatory)][string]$BackupRoot,
        [scriptblock]$BeforeReplace,
        [scriptblock]$AfterReplace,
        [scriptblock]$BeforeRollback
    )
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if (-not (Test-Path -LiteralPath $DllPath -PathType Leaf)) { throw "BWAPI DLL not found: $DllPath" }
    if (-not (Test-Path -LiteralPath $PackagePath -PathType Leaf)) { throw "Required BWAPI package not found: $PackagePath" }
    $sourceHash = (Get-FileHash -LiteralPath $DllPath -Algorithm SHA256).Hash
    # Validate the canonical archive and its expected entry before creating or
    # replacing any runtime destination.
    [void](Get-ZipEntrySha256 -ArchivePath $PackagePath -EntryName 'bwapi-data/BWAPI.dll')
    $packageOriginalHash = (Get-FileHash -LiteralPath $PackagePath -Algorithm SHA256).Hash
    foreach ($target in $RuntimeTargets) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    }
    New-Item -ItemType Directory -Path $BackupRoot -Force | Out-Null

    $items = @()
    foreach ($target in $RuntimeTargets) {
        $items += [pscustomobject]@{ Target=$target; Kind='dll'; Original=(Test-Path -LiteralPath $target -PathType Leaf) }
    }
    $items += [pscustomobject]@{ Target=$PackagePath; Kind='zip'; Original=$true }
    $id = [guid]::NewGuid().ToString('N')
    $committed = [Collections.Generic.List[object]]::new()
    $keepTemporary = $false
    try {
        foreach ($item in $items) {
            $item | Add-Member -NotePropertyName Stage -NotePropertyValue (Join-Path (Split-Path -Parent $item.Target) ".protodd-$id.stage")
            $item | Add-Member -NotePropertyName Rollback -NotePropertyValue (Join-Path (Split-Path -Parent $item.Target) ".protodd-$id.rollback")
            $item | Add-Member -NotePropertyName ReplaceBackup -NotePropertyValue (Join-Path (Split-Path -Parent $item.Target) ".protodd-$id.replaced")
            $item | Add-Member -NotePropertyName RollbackRecovery -NotePropertyValue (Join-Path (Split-Path -Parent $item.Target) ".protodd-$id.rollback-recovery")
            if ($item.Original) {
                $currentHash = (Get-FileHash -LiteralPath $item.Target -Algorithm SHA256).Hash
                $saved = Join-Path $BackupRoot "$currentHash.$(if ($item.Kind -eq 'zip') { 'zip' } else { 'dll' })"
                if (-not (Test-Path -LiteralPath $saved -PathType Leaf)) { Copy-Item -LiteralPath $item.Target -Destination $saved }
                if ((Get-FileHash -LiteralPath $saved -Algorithm SHA256).Hash -ne $currentHash) {
                    throw "Backup verification failed for $($item.Target)"
                }
                $item | Add-Member -NotePropertyName Backup -NotePropertyValue $saved
                Copy-Item -LiteralPath $item.Target -Destination $item.Rollback
                if ((Get-FileHash -LiteralPath $item.Rollback -Algorithm SHA256).Hash -ne $currentHash) {
                    throw "Rollback copy verification failed for $($item.Target)"
                }
            }
            if ($item.Kind -eq 'dll') {
                Copy-Item -LiteralPath $DllPath -Destination $item.Stage
            } else {
                Copy-Item -LiteralPath $PackagePath -Destination $item.Stage
                $archive = [IO.Compression.ZipFile]::Open($item.Stage, [IO.Compression.ZipArchiveMode]::Update)
                try {
                    $entry = $archive.GetEntry('bwapi-data/BWAPI.dll')
                    if (-not $entry) { throw 'Staged BWAPI package lost its expected runtime entry' }
                    $entry.Delete()
                    [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $DllPath, 'bwapi-data/BWAPI.dll') | Out-Null
                } finally { $archive.Dispose() }
            }
            $stageHash = if ($item.Kind -eq 'dll') {
                (Get-FileHash -LiteralPath $item.Stage -Algorithm SHA256).Hash
            } else {
                Get-ZipEntrySha256 -ArchivePath $item.Stage -EntryName 'bwapi-data/BWAPI.dll'
            }
            if ($stageHash -ne $sourceHash) { throw "Staged BWAPI artifact hash mismatch for $($item.Target)" }
        }
        # Recheck the package after staging to catch concurrent edits.
        if ((Get-FileHash -LiteralPath $PackagePath -Algorithm SHA256).Hash -ne $packageOriginalHash) {
            throw 'Required BWAPI package changed during deployment staging'
        }
        for ($i = 0; $i -lt $items.Count; $i++) {
            $item = $items[$i]
            if ($BeforeReplace) { & $BeforeReplace $i $item.Target }
            if ($item.Original) { [IO.File]::Replace($item.Stage, $item.Target, $item.ReplaceBackup) }
            else { [IO.File]::Move($item.Stage, $item.Target) }
            $committed.Add($item)
            if ($AfterReplace) { & $AfterReplace $i $item.Target }
            $actualHash = if ($item.Kind -eq 'dll') {
                (Get-FileHash -LiteralPath $item.Target -Algorithm SHA256).Hash
            } else {
                Get-ZipEntrySha256 -ArchivePath $item.Target -EntryName 'bwapi-data/BWAPI.dll'
            }
            if ($actualHash -ne $sourceHash) { throw "Post-replacement hash verification failed for $($item.Target)" }
            if (Test-Path -LiteralPath $item.ReplaceBackup) { Remove-Item -LiteralPath $item.ReplaceBackup -Force }
        }
    } catch {
        $failure = $_
        $rollbackErrors = [Collections.Generic.List[string]]::new()
        for ($i = $committed.Count - 1; $i -ge 0; $i--) {
            $item = $committed[$i]
            try {
                if ($BeforeRollback) { & $BeforeRollback $i $item.Target }
                if ($item.Original) { [IO.File]::Replace($item.Rollback, $item.Target, $item.RollbackRecovery) }
                elseif (Test-Path -LiteralPath $item.Target) { Remove-Item -LiteralPath $item.Target -Force }
                if ($item.Original) {
                    $restoredHash = (Get-FileHash -LiteralPath $item.Target -Algorithm SHA256).Hash
                    $backupHash = (Get-FileHash -LiteralPath $item.Backup -Algorithm SHA256).Hash
                    if ($restoredHash -ne $backupHash) { throw "restored hash $restoredHash differs from backup $backupHash" }
                    if (Test-Path -LiteralPath $item.RollbackRecovery) { Remove-Item -LiteralPath $item.RollbackRecovery -Force }
                }
            } catch { $rollbackErrors.Add("$($item.Target): $($_.Exception.Message)") }
        }
        if ($rollbackErrors.Count -gt 0) {
            $keepTemporary = $true
            throw "Deployment failed: $($failure.Exception.Message). Rollback also failed; recovery files preserved. $($rollbackErrors -join '; ')"
        }
        throw $failure
    } finally {
        if (-not $keepTemporary) {
            foreach ($item in $items) {
                foreach ($path in @($item.Stage, $item.Rollback, $item.ReplaceBackup, $item.RollbackRecovery)) {
                    if ($path -and (Test-Path -LiteralPath $path)) { Remove-Item -LiteralPath $path -Force }
                }
            }
        }
    }
}

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
"BWAPI_RUNTIME=$dll"
"SHA256=$((Get-FileHash -LiteralPath $dll).Hash)"
if ($Deploy) {
    if (Get-Process StarCraft -ErrorAction SilentlyContinue) {
        throw 'StarCraft started during the build; stop it before deploying'
    }
    # All targets and the canonical package are staged and checked before the
    # transaction replaces anything. Historical backups remain immutable.
    $required = Join-Path $repo 'ladder/manager/server/required/Required_BWAPI_440.zip'
    $targets = @('direct-template', 'match-runtime-a', 'match-runtime-b') | ForEach-Object {
        Join-Path $repo "build/$_/bwapi-data/BWAPI.dll"
    }
    Deploy-BwapiRuntimeArtifacts -DllPath $dll -RuntimeTargets $targets -PackagePath $required `
        -BackupRoot (Join-Path $repo 'build/bwapi-runtime-backup')
}
