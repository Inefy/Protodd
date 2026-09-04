param(
    [string]$DllPath = "build/tournament/Release/AstraBot.dll",
    [string]$OutputPath = "artifacts/AstraBot-AIIDE-2026.zip"
)

$ErrorActionPreference = "Stop"
$repoPath = Split-Path -Parent $PSScriptRoot
$artifactRoot = [System.IO.Path]::GetFullPath((Join-Path $repoPath "artifacts"))
$stagingRoot = [System.IO.Path]::GetFullPath((Join-Path $artifactRoot "staging/AstraBot"))
$resolvedDll = if ([System.IO.Path]::IsPathRooted($DllPath)) {
    [System.IO.Path]::GetFullPath($DllPath)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $repoPath $DllPath))
}
$resolvedOutput = if ([System.IO.Path]::IsPathRooted($OutputPath)) {
    [System.IO.Path]::GetFullPath($OutputPath)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $repoPath $OutputPath))
}
$artifactPrefix = $artifactRoot.TrimEnd('\') + '\'
if (-not $stagingRoot.StartsWith($artifactPrefix, [System.StringComparison]::OrdinalIgnoreCase) -or
    -not $resolvedOutput.StartsWith($artifactPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Package staging and output must stay inside $artifactRoot"
}
if (-not (Test-Path -LiteralPath $resolvedDll)) {
    throw "Build the Release|Win32 DLL first: $resolvedDll"
}

if (Test-Path -LiteralPath $stagingRoot) {
    Remove-Item -LiteralPath $stagingRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $stagingRoot -Force | Out-Null
$sourceRoot = Join-Path $stagingRoot "source"
New-Item -ItemType Directory -Path $sourceRoot -Force | Out-Null

Copy-Item -LiteralPath $resolvedDll -Destination (Join-Path $stagingRoot "AstraBot.dll")
foreach ($file in @("README.md", "LICENSE", "CMakeLists.txt", "CMakePresets.json")) {
    Copy-Item -LiteralPath (Join-Path $repoPath $file) -Destination $sourceRoot
}
foreach ($directory in @("cmake", "include", "src", "tests", "tools", "docs", "bwapi-data")) {
    Copy-Item -LiteralPath (Join-Path $repoPath $directory) -Destination $sourceRoot -Recurse
}
# Runtime helper caches can be present after verification, but they are not
# source and should never inflate or contaminate the tournament submission.
Get-ChildItem -LiteralPath $sourceRoot -Directory -Filter "__pycache__" -Recurse |
    Remove-Item -Recurse -Force
Get-ChildItem -LiteralPath $sourceRoot -File -Include "*.pyc", "*.pyo" -Recurse |
    Remove-Item -Force
New-Item -ItemType Directory -Path (Join-Path $sourceRoot "scripts") -Force | Out-Null
foreach ($script in @("build-tournament.ps1", "verify.ps1", "ladder.ps1",
                      "direct-match.ps1")) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $script) `
        -Destination (Join-Path $sourceRoot "scripts/$script")
}
New-Item -ItemType Directory -Path (Join-Path $sourceRoot "ladder") -Force | Out-Null
foreach ($file in @("README.md", "ladder.example.json")) {
    Copy-Item -LiteralPath (Join-Path $repoPath "ladder/$file") `
        -Destination (Join-Path $sourceRoot "ladder/$file")
}
Copy-Item -LiteralPath (Join-Path $repoPath "SUBMISSION.md") -Destination $stagingRoot

$manifest = [ordered]@{
    bot = "AstraBot"
    race = "Protoss"
    bwapi = "4.4.0"
    git_commit = (& git -C $repoPath rev-parse HEAD).Trim()
    source_dirty = -not [string]::IsNullOrWhiteSpace(
        ((& git -C $repoPath status --porcelain) -join "`n"))
    dll_sha256 = (Get-FileHash -LiteralPath $resolvedDll -Algorithm SHA256).Hash
    packaged_utc = [DateTime]::UtcNow.ToString("o")
}
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stagingRoot "manifest.json") `
    -Encoding utf8

$outputDirectory = Split-Path -Parent $resolvedOutput
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
if (Test-Path -LiteralPath $resolvedOutput) {
    Remove-Item -LiteralPath $resolvedOutput -Force
}
Compress-Archive -LiteralPath $stagingRoot -DestinationPath $resolvedOutput -CompressionLevel Optimal
Remove-Item -LiteralPath $stagingRoot -Recurse -Force

$packageHash = (Get-FileHash -LiteralPath $resolvedOutput -Algorithm SHA256).Hash
Write-Output "Tournament package: $resolvedOutput"
Write-Output "SHA256: $packageHash"
