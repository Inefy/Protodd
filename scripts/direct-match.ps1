param(
    [ValidateSet("Zerg", "Terran", "Protoss")]
    [string]$OpponentRace = "Zerg",
    [string]$OpponentName = "",
    [string]$Map = "maps/aiide/(2)Benzene.scx",
    [string]$Label = "direct-match",
    [int]$TimeoutSeconds = 480,
    [ValidateRange(0, 1000)]
    [int]$FrameMilliseconds = 0,
    [ValidateRange(-1, 2147483646)]
    [int]$Seed = -1,
    [string]$BotDll = "build/tournament/Release/AstraBot.dll",
    [switch]$PreserveLearning
)

$ErrorActionPreference = "Stop"
$repoPath = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$runtimeA = [System.IO.Path]::GetFullPath((Join-Path $repoPath "build/match-runtime-a"))
$runtimeB = [System.IO.Path]::GetFullPath((Join-Path $repoPath "build/match-runtime-b"))
$archiveRoot = [System.IO.Path]::GetFullPath((Join-Path $repoPath "build/direct-logs"))
$buildPrefix = [System.IO.Path]::GetFullPath((Join-Path $repoPath "build")).TrimEnd('\') + '\'

foreach ($path in @($runtimeA, $runtimeB, $archiveRoot)) {
    if (-not $path.StartsWith($buildPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Direct-match paths must stay inside $buildPrefix"
    }
}
if (Get-Process -Name StarCraft -ErrorAction SilentlyContinue) {
    throw "Refusing to start: a StarCraft process is already running"
}
if ($Label -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') {
    throw "Label may contain only letters, digits, dots, underscores, and hyphens"
}

$resolvedDll = if ([System.IO.Path]::IsPathRooted($BotDll)) {
    [System.IO.Path]::GetFullPath($BotDll)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $repoPath $BotDll))
}
$mapPath = [System.IO.Path]::GetFullPath((Join-Path $runtimeA $Map))
$runtimePrefix = $runtimeA.TrimEnd('\') + '\'
if (-not $mapPath.StartsWith($runtimePrefix, [System.StringComparison]::OrdinalIgnoreCase) -or
    -not (Test-Path -LiteralPath $mapPath -PathType Leaf)) {
    throw "Map must exist inside the host runtime: $mapPath"
}
if (-not (Test-Path -LiteralPath $resolvedDll -PathType Leaf)) {
    throw "Bot DLL not found: $resolvedDll"
}

if ([string]::IsNullOrWhiteSpace($OpponentName)) { $OpponentName = "UAB$OpponentRace" }
if ($OpponentName -notmatch '^[A-Za-z0-9_-]+$') {
    throw "Opponent name must identify a local ladder bot without path separators"
}
$moduleName = "$OpponentName.dll"
$opponentRoot = Join-Path $repoPath "ladder/bots/$OpponentName/AI"
if (-not (Test-Path -LiteralPath (Join-Path $opponentRoot $moduleName) -PathType Leaf)) {
    throw "Opponent DLL not found: $opponentRoot/$moduleName"
}
$opponentComponents = [ordered]@{}
$opponentFiles = @(Get-ChildItem -LiteralPath $opponentRoot -File -Recurse | Sort-Object FullName)
foreach ($file in $opponentFiles) {
    $relative = [System.IO.Path]::GetRelativePath($opponentRoot, $file.FullName)
    $opponentComponents[$relative] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
}

New-Item -ItemType Directory -Path $archiveRoot -Force | Out-Null
$writeRoot = Join-Path $runtimeA "bwapi-data/write"
if (Test-Path -LiteralPath (Join-Path $archiveRoot "$Label.json")) {
    throw "A match record already exists for label '$Label'; choose a new label"
}
$archiveFiles = @(Join-Path $writeRoot "AstraBot.log")
if (-not $PreserveLearning) {
    foreach ($dataDirectory in @($writeRoot, (Join-Path $runtimeA "bwapi-data/read"))) {
        $archiveFiles += @(Get-ChildItem -LiteralPath $dataDirectory -Filter 'AstraBot*.csv' -File |
            Select-Object -ExpandProperty FullName)
    }
}
foreach ($existing in $archiveFiles) {
    $name = Split-Path -Leaf $existing
    if (Test-Path -LiteralPath $existing) {
        $stamp = [DateTime]::UtcNow.ToString("yyyyMMdd-HHmmssfff")
        Move-Item -LiteralPath $existing -Destination (Join-Path $archiveRoot "previous-$stamp-$name")
    }
}

$sourceDllHash = (Get-FileHash -LiteralPath $resolvedDll -Algorithm SHA256).Hash
$deployedDll = Join-Path $runtimeA "bwapi-data/AI/AstraBot.dll"
Copy-Item -LiteralPath $resolvedDll -Destination $deployedDll -Force
$deployedDllHash = (Get-FileHash -LiteralPath $deployedDll -Algorithm SHA256).Hash
if ($sourceDllHash -ne $deployedDllHash) {
    throw "Deployed bot does not match the requested DLL"
}
"MATCH_DLL=$resolvedDll"
"DLL_SHA256=$sourceDllHash"
foreach ($file in $opponentFiles) {
    $relative = [System.IO.Path]::GetRelativePath($opponentRoot, $file.FullName)
    $destination = Join-Path $runtimeB "bwapi-data/AI/$relative"
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $file.FullName -Destination $destination -Force
}

$seedConfiguration = if ($Seed -ge 0) { "seed_override = $Seed" } else { "" }
$hostIni = @"
[ai]
ai = bwapi-data/AI/AstraBot.dll
ai_dbg = bwapi-data/AI/AstraBot.dll
tournament =

[auto_menu]
auto_menu = LAN
character_name = FIRST
pause_dbg = OFF
lan_mode = Local PC
auto_restart = OFF
map = $Map
game = AstraUAB
mapiteration = SEQUENCE
race = Protoss
enemy_count = 1
enemy_race = $OpponentRace
game_type = MELEE
wait_for_min_players = 2
wait_for_max_players = 2
wait_for_time = 1000

[config]
holiday = OFF
shared_memory = ON
console_attach_on_startup = FALSE
console_alloc_on_startup = FALSE
console_attach_auto = FALSE
console_alloc_auto = FALSE

[window]
windowed = ON
left = 0
top = 0
width = 640
height = 480

[starcraft]
sound = OFF
speed_override = $FrameMilliseconds
$seedConfiguration
drop_players = ON

[paths]
log_path = bwapi-data/logs
"@
$joinIni = @"
[ai]
ai = bwapi-data/AI/$moduleName
ai_dbg = bwapi-data/AI/$moduleName
tournament =

[auto_menu]
auto_menu = LAN
character_name = FIRST
pause_dbg = OFF
lan_mode = Local PC
auto_restart = OFF
map =
game = JOIN_FIRST
mapiteration = SEQUENCE
race = $OpponentRace
enemy_count = 1
enemy_race = Protoss
game_type = MELEE
wait_for_min_players = 2
wait_for_max_players = 2
wait_for_time = 1000

[config]
holiday = OFF
shared_memory = ON
console_attach_on_startup = FALSE
console_alloc_on_startup = FALSE
console_attach_auto = FALSE
console_alloc_auto = FALSE

[window]
windowed = ON
left = 680
top = 0
width = 640
height = 480

[starcraft]
sound = OFF
speed_override = $FrameMilliseconds
$seedConfiguration
drop_players = ON

[paths]
log_path = bwapi-data/logs
"@
[System.IO.File]::WriteAllText((Join-Path $runtimeA "bwapi-data/bwapi.ini"), $hostIni)
[System.IO.File]::WriteAllText((Join-Path $runtimeB "bwapi-data/bwapi.ini"), $joinIni)

$launched = @()
function Close-LaunchedStarCraft {
    # Never use AppActivate or SendKeys here: focus can change while a game
    # exits, sending Alt+F4/Enter to an unrelated application or Windows.
    foreach ($id in $script:launched) {
        $process = Get-Process -Id $id -ErrorAction SilentlyContinue
        if (-not $process -or $process.ProcessName -ne 'StarCraft') { continue }
        [void]$process.CloseMainWindow()
    }

    $gracePeriod = [DateTime]::UtcNow.AddSeconds(3)
    while ([DateTime]::UtcNow -lt $gracePeriod) {
        $remaining = @(
            foreach ($id in $script:launched) {
                $process = Get-Process -Id $id -ErrorAction SilentlyContinue
                if ($process -and $process.ProcessName -eq 'StarCraft') { $process }
            }
        )
        if ($remaining.Count -eq 0) { return }
        Start-Sleep -Milliseconds 200
    }

    foreach ($id in $script:launched) {
        $process = Get-Process -Id $id -ErrorAction SilentlyContinue
        if (-not $process -or $process.ProcessName -ne 'StarCraft') { continue }
        Stop-Process -Id $id -Force -ErrorAction SilentlyContinue
    }
}

$logPath = Join-Path $writeRoot "AstraBot.log"
$result = $null
$traceBeforeCleanup = ""
$startedUtc = [DateTime]::UtcNow.ToString('o')
try {
    Start-Process -FilePath (Join-Path $runtimeA "injectory_x86.exe") `
        -ArgumentList '--launch','StarCraft.exe','--inject','bwapi-data\BWAPI.dll','wmode.dll' `
        -WorkingDirectory $runtimeA -WindowStyle Hidden
    Start-Sleep -Seconds 3
    $launched = @(Get-Process -Name StarCraft -ErrorAction Stop | Select-Object -ExpandProperty Id)
    Start-Process -FilePath (Join-Path $runtimeB "injectory_x86.exe") `
        -ArgumentList '--launch','StarCraft.exe','--inject','bwapi-data\BWAPI.dll','wmode.dll' `
        -WorkingDirectory $runtimeB -WindowStyle Hidden
    Start-Sleep -Seconds 3
    $launched = @(Get-Process -Name StarCraft -ErrorAction Stop | Select-Object -ExpandProperty Id)

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-Path -LiteralPath $logPath) {
            try {
                $terminal = Get-Content -LiteralPath $logPath -Tail 8 -ErrorAction Stop |
                    Where-Object { $_ -match '^END,(win|loss),(\d+)$' } |
                    Select-Object -Last 1
                if ($terminal) {
                    $result = $terminal
                    break
                }
            } catch {
                # The game briefly holds an exclusive write lock while flushing.
            }
        }
        if (-not (Get-Process -Id $launched -ErrorAction SilentlyContinue)) { break }
        Start-Sleep -Seconds 1
    }
} finally {
    if (Test-Path -LiteralPath $logPath) {
        try { $traceBeforeCleanup = Get-Content -LiteralPath $logPath -Raw -ErrorAction Stop }
        catch { $traceBeforeCleanup = "" }
    }
    # Capture the competitive outcome before closing StarCraft. onEnd(false)
    # during cleanup can write END,loss even when the match merely timed out.
    $observedSeed = $null
    if ($traceBeforeCleanup -match '(?m)^MATCH,seed=(\d+),') {
        $observedSeed = [long]$Matches[1]
    }
    $record = [ordered]@{
        label = $Label
        status = $(if ($result) { 'completed' } else { 'incomplete' })
        result = $result
        started_utc = $startedUtc
        finished_utc = [DateTime]::UtcNow.ToString('o')
        bot_sha256 = $sourceDllHash
        opponent = $OpponentName
        opponent_race = $OpponentRace
        opponent_sha256 = (Get-FileHash -LiteralPath (Join-Path $opponentRoot $moduleName)).Hash
        opponent_components_sha256 = $opponentComponents
        map = $Map
        map_sha256 = (Get-FileHash -LiteralPath $mapPath).Hash
        timeout_seconds = $TimeoutSeconds
        seed_requested = $(if ($Seed -ge 0) { $Seed } else { $null })
        seed_observed = $observedSeed
        learning_preserved = [bool]$PreserveLearning
    }
    $record | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $archiveRoot "$Label.json") -Encoding utf8
    Close-LaunchedStarCraft
}

if (Test-Path -LiteralPath $logPath) {
    $archive = Join-Path $archiveRoot "$Label.log"
    Copy-Item -LiteralPath $logPath -Destination (Join-Path $archiveRoot "$Label.raw.log") -Force
    [System.IO.File]::WriteAllText($archive, $traceBeforeCleanup)
    "TRACE=$archive"
}
if (-not $result) {
    throw "Match did not produce a terminal result within $TimeoutSeconds seconds"
}
"RESULT=$result"
