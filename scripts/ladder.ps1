param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$LadderArguments
)

$ErrorActionPreference = "Stop"
$repoPath = Split-Path -Parent $PSScriptRoot
Push-Location $repoPath
try {
    python tools/ladder.py @LadderArguments
    if ($LASTEXITCODE -ne 0) { throw "Ladder command failed" }
}
finally {
    Pop-Location
}
