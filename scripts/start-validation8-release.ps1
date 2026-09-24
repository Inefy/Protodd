$ErrorActionPreference = 'Stop'
$workspaceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $workspaceRoot
$python = Join-Path $workspaceRoot 'build/model-venv/Scripts/python.exe'
$statusFile = Join-Path $workspaceRoot 'build/robust-training-20260922/whole-game-validation8.status.json'
$output = 'artifacts/replay-learning/whole-game-release-validation8-v32c-20260923'
$quality = 'artifacts/replay-learning/whole-game-quality-validation8-v32c-20260923.json'

function Write-Status($stage, $detail, $code) {
    @{ stage = $stage; detail = $detail; exit_code = $code;
       recorded_utc = (Get-Date).ToUniversalTime().ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath $statusFile -Encoding utf8
}

try {
    if (Test-Path -LiteralPath $output) {
        throw "Validation release output already exists: $output"
    }
    Write-Status 'extracting' 'Eight full-prefix train and validation games per matchup' $null
    & $python -m training.whole_game_release `
        --release artifacts/replay-learning/protoss-data-release-20260922 `
        --root artifacts/cwal-dataset `
        --extractor build/replay-whole-game-v31/Release/replay_extract_v3.exe `
        --selftest build/replay-whole-game-v31/Release/replay_extract_v3_selftest.exe `
        --decoder build/replay-native/replay_decode.exe `
        --reference build/replay-native/Release/replay_checkpoints.exe `
        --mpq build/match-runtime-a --assets build/replay-modern-assets `
        --output $output --per-matchup 8 --workers 1
    if ($LASTEXITCODE -ne 0) {
        throw "Validation replay extraction exited with code $LASTEXITCODE."
    }
    Write-Status 'indexing' 'Building exact-MMR quality index for this release identity' $null
    & $python -m training.whole_game_quality `
        --identity "$output/identity.json" `
        --audit artifacts/replay-learning/audit-protoss-20260920/audit.sqlite `
        --output $quality
    if ($LASTEXITCODE -ne 0) {
        throw "Validation quality indexing exited with code $LASTEXITCODE."
    }
    Write-Status 'complete' "Release: $output; quality: $quality" 0
} catch {
    Write-Status 'failed' $_.Exception.Message 1
    Write-Error $_
    exit 1
}
