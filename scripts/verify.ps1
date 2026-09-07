param(
    [switch]$SkipAdapter,
    [string]$BwapiRoot = "build/_deps/bwapi-src",
    [string]$ZigPath = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
try {
    if (-not $ZigPath) {
        $ZigPath = python -c "import importlib.util, pathlib; s = importlib.util.find_spec('ziglang'); print(pathlib.Path(s.origin).parent / 'zig.exe' if s else '')"
    }
    if (-not $ZigPath -or -not (Test-Path -LiteralPath $ZigPath)) {
        throw "Pass -ZigPath to zig.exe, or install it for the active Python: python -m pip install --user ziglang"
    }

    New-Item -ItemType Directory -Force -Path "build/verify" | Out-Null
    $coreSources = @(
        Get-ChildItem -LiteralPath "src/core" -Filter "*.cpp" | ForEach-Object FullName
    )
    $coreSources += (Resolve-Path "tests/test_main.cpp").Path
    & $zigPath c++ -std=c++20 -Iinclude -Wall -Wextra -Wpedantic -Wconversion `
        -Wshadow -Werror -Wno-nullability-completeness @coreSources `
        -o build/verify/protodd_tests.exe
    if ($LASTEXITCODE -ne 0) { throw "Core compilation failed" }
    & build/verify/protodd_tests.exe
    if ($LASTEXITCODE -ne 0) { throw "Core tests failed" }

    python tools/log_analyzer.py --self-test
    if ($LASTEXITCODE -ne 0) { throw "Log analyzer tests failed" }

    python tools/direct_report.py --self-test
    if ($LASTEXITCODE -ne 0) { throw "Direct-match report tests failed" }

    python tools/decision_report.py --self-test
    if ($LASTEXITCODE -ne 0) { throw "Decision observer tests failed" }

    python tests/test_ladder.py
    if ($LASTEXITCODE -ne 0) { throw "Ladder tests failed" }

    python tools/ladder.py audit
    if ($LASTEXITCODE -ne 0) { throw "Ladder privacy audit failed" }

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
    Write-Output "Protodd verification passed"
}
finally {
    Pop-Location
}
