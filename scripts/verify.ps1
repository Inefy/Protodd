param(
    [switch]$SkipAdapter,
    [string]$BwapiRoot = "build/_deps/bwapi-src"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
try {
    $zigPath = python -c "import pathlib, ziglang; print(pathlib.Path(ziglang.__file__).parent / 'zig.exe')"
    if (-not (Test-Path -LiteralPath $zigPath)) {
        throw "The portable verifier requires ziglang: python -m pip install --user ziglang"
    }

    New-Item -ItemType Directory -Force -Path "build/verify" | Out-Null
    $coreSources = @(
        Get-ChildItem -LiteralPath "src/core" -Filter "*.cpp" | ForEach-Object FullName
    )
    $coreSources += (Resolve-Path "tests/test_main.cpp").Path
    & $zigPath c++ -std=c++20 -Iinclude -Wall -Wextra -Wpedantic -Wconversion `
        -Wshadow -Werror -Wno-nullability-completeness @coreSources `
        -o build/verify/astra_tests.exe
    if ($LASTEXITCODE -ne 0) { throw "Core compilation failed" }
    & build/verify/astra_tests.exe
    if ($LASTEXITCODE -ne 0) { throw "Core tests failed" }

    python tools/log_analyzer.py --self-test
    if ($LASTEXITCODE -ne 0) { throw "Log analyzer tests failed" }

    if (-not $SkipAdapter) {
        $candidateA = Join-Path $BwapiRoot "bwapi/include/BWAPI.h"
        $candidateB = Join-Path $BwapiRoot "include/BWAPI.h"
        if (Test-Path -LiteralPath $candidateA) {
            $bwapiInclude = Split-Path -Parent $candidateA
        } elseif (Test-Path -LiteralPath $candidateB) {
            $bwapiInclude = Split-Path -Parent $candidateB
        } else {
            throw "BWAPI headers not found under '$BwapiRoot'. Pass -SkipAdapter or -BwapiRoot."
        }

        $adapterFlags = @(
            '-target', 'x86-windows-msvc', '-std=c++20', '-fms-extensions',
            '-Iinclude', '-isystem', $bwapiInclude, '-Isrc/bwapi',
            '-Wall', '-Wextra', '-Wpedantic', '-Wconversion', '-Wshadow', '-Werror',
            '-Wno-nullability-completeness', '-Wno-unknown-pragmas',
            '-Wno-deprecated-this-capture', '-Wno-division-by-zero',
            '-Wno-unused-command-line-argument', '-Wno-language-extension-token'
        )
        foreach ($source in Get-ChildItem -LiteralPath "src/bwapi" -Filter "*.cpp") {
            $object = Join-Path "build/verify" ($source.BaseName + ".obj")
            & $zigPath c++ @adapterFlags -c $source.FullName -o $object
            if ($LASTEXITCODE -ne 0) { throw "Adapter compilation failed: $($source.Name)" }
        }
    }

    git diff --check
    if ($LASTEXITCODE -ne 0) { throw "Whitespace validation failed" }
    Write-Output "AstraBot verification passed"
}
finally {
    Pop-Location
}

