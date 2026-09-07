param(
    [ValidateSet("Zerg", "Terran", "Protoss", "Random")]
    [string]$OpponentRace = "Zerg",
    [string]$OpponentName = "",
    [ValidateSet('', 'PvP_nzcore', 'PvP_zcore', 'PvP_zzcore', 'PvP_zcorez',
        'PvP_10/12gatedt', 'PvP_2gatedtexpo', 'PvP_2gatereaver', 'PvP_3gaterobo',
        'PvP_3gatespeedzeal', 'PvP_12nexus', 'PvP_4gategoon', 'PvP_9/9gate',
        'PvP_9/9proxygate', 'PvP_10/12gate')]
    [string]$OpponentOpening = "",
    [string]$Map = "maps/aiide/(2)Benzene.scx",
    [string]$Label = "direct-match",
    [int]$TimeoutSeconds = 480,
    [ValidateRange(0, 1000)]
    [int]$FrameMilliseconds = 0,
    [ValidateRange(-1, 2147483646)]
    [int]$Seed = -1,
    [string]$BotDll = "build/protodd-tournament/Release/Protodd.dll",
    [switch]$PreserveLearning,
    [switch]$NoObserver
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
$baselineStarCraftIds = @(Get-Process -Name StarCraft -ErrorAction SilentlyContinue |
    Select-Object -ExpandProperty Id)
if ($baselineStarCraftIds.Count -gt 0) {
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
if ($OpponentOpening -and ($OpponentName -ne 'BananaBrain' -or
    $OpponentRace -ne 'Protoss' -or $OpponentOpening -notmatch '^PvP_[A-Za-z0-9/]+$')) {
    throw "OpponentOpening requires BananaBrain Protoss and a PvP opening name"
}
$moduleName = "$OpponentName.dll"
$opponentRoot = Join-Path $repoPath "ladder/bots/$OpponentName/AI"
if (-not (Test-Path -LiteralPath (Join-Path $opponentRoot $moduleName) -PathType Leaf)) {
    throw "Opponent DLL not found: $opponentRoot/$moduleName"
}
$opponentComponents = [ordered]@{}
$opponentFiles = @(Get-ChildItem -LiteralPath $opponentRoot -File -Recurse | Sort-Object FullName)
foreach ($file in $opponentFiles) {
    $relative = $file.FullName.Substring($opponentRoot.Length).TrimStart('\', '/')
    $opponentComponents[$relative] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
}

New-Item -ItemType Directory -Path $archiveRoot -Force | Out-Null
$writeRoot = Join-Path $runtimeA "bwapi-data/write"
if (Test-Path -LiteralPath (Join-Path $archiveRoot "$Label.json")) {
    throw "A match record already exists for label '$Label'; choose a new label"
}
$archiveFiles = @(Join-Path $writeRoot "Protodd.log")
if (-not $PreserveLearning) {
    foreach ($dataDirectory in @($writeRoot, (Join-Path $runtimeA "bwapi-data/read"))) {
        $archiveFiles += @(Get-ChildItem -LiteralPath $dataDirectory -Filter 'Protodd*.csv' -File |
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

# Opponents also learn from runtime read/write files. Archive both sides by
# default so an A/B test does not quietly train the opponent between games.
if (-not $PreserveLearning) {
    foreach ($dataKind in @('read', 'write')) {
        $opponentData = Join-Path $runtimeB "bwapi-data/$dataKind"
        foreach ($file in @(Get-ChildItem -LiteralPath $opponentData -File -Recurse)) {
            $relative = $file.FullName.Substring($opponentData.Length).TrimStart('\', '/')
            $saved = [System.IO.Path]::GetFullPath(
                (Join-Path $archiveRoot "$Label-opponent-before/$dataKind/$relative"))
            if (-not $file.FullName.StartsWith($buildPrefix, [System.StringComparison]::OrdinalIgnoreCase) -or
                -not $saved.StartsWith($buildPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "Opponent history archive must remain inside the build workspace"
            }
            New-Item -ItemType Directory -Path (Split-Path -Parent $saved) -Force | Out-Null
            Move-Item -LiteralPath $file.FullName -Destination $saved
        }
    }
}

$sourceDllHash = (Get-FileHash -LiteralPath $resolvedDll -Algorithm SHA256).Hash
$deployedDll = Join-Path $runtimeA "bwapi-data/AI/Protodd.dll"
Copy-Item -LiteralPath $resolvedDll -Destination $deployedDll -Force
$deployedDllHash = (Get-FileHash -LiteralPath $deployedDll -Algorithm SHA256).Hash
if ($sourceDllHash -ne $deployedDllHash) {
    throw "Deployed bot does not match the requested DLL"
}
"MATCH_DLL=$resolvedDll"
"DLL_SHA256=$sourceDllHash"
foreach ($file in $opponentFiles) {
    $relative = $file.FullName.Substring($opponentRoot.Length).TrimStart('\', '/')
    $destination = Join-Path $runtimeB "bwapi-data/AI/$relative"
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $file.FullName -Destination $destination -Force
}
if ($OpponentOpening) {
    # Runtime-only configuration: the installed ladder opponent is unchanged.
    Add-Content -LiteralPath (Join-Path $runtimeB 'bwapi-data/AI/Configuration.txt') `
        -Value "`nPvP_opening=$OpponentOpening" -Encoding ascii
}
$runtimeConfiguration = Join-Path $runtimeB 'bwapi-data/AI/Configuration.txt'
$runtimeConfigurationHash = if (Test-Path -LiteralPath $runtimeConfiguration) {
    (Get-FileHash -LiteralPath $runtimeConfiguration -Algorithm SHA256).Hash
} else { $null }

$seedConfiguration = if ($Seed -ge 0) { "seed_override = $Seed" } else { "" }
$hostIni = @"
[ai]
ai = bwapi-data/AI/Protodd.dll
ai_dbg = bwapi-data/AI/Protodd.dll
tournament =

[auto_menu]
auto_menu = LAN
character_name = FIRST
pause_dbg = OFF
lan_mode = Local PC
auto_restart = OFF
map = $Map
game = ProtoddUAB
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
$proxyLaunched = @()
function Get-TrackedStarCraftIds {
    $ids = @($script:launched)
    # StarCraft can fork a child after the injection wrapper was sampled. Only
    # include processes created after this match started and never touch a
    # pre-existing game owned by the user.
    $ids += @(Get-Process -Name StarCraft -ErrorAction SilentlyContinue |
        Where-Object {
            $fresh = $false
            try {
                $startTime = $_.StartTime
                $fresh = $null -ne $startTime -and
                         $startTime.ToUniversalTime() -ge $script:startedAtUtc
            } catch {
                $fresh = $false
            }
            $script:baselineStarCraftIds -notcontains $_.Id -and $fresh
        } |
        Select-Object -ExpandProperty Id)
    @($ids | Sort-Object -Unique)
}
function Close-LaunchedStarCraft {
    # Never use AppActivate or SendKeys here: focus can change while a game
    # exits, sending Alt+F4/Enter to an unrelated application or Windows.
    foreach ($id in @(Get-TrackedStarCraftIds)) {
        $process = Get-Process -Id $id -ErrorAction SilentlyContinue
        if (-not $process -or $process.ProcessName -ne 'StarCraft') { continue }
        [void]$process.CloseMainWindow()
    }

    $gracePeriod = [DateTime]::UtcNow.AddSeconds(3)
    while ([DateTime]::UtcNow -lt $gracePeriod) {
        $remaining = @(
            foreach ($id in @(Get-TrackedStarCraftIds)) {
                $process = Get-Process -Id $id -ErrorAction SilentlyContinue
                if ($process -and $process.ProcessName -eq 'StarCraft') { $process }
            }
        )
        if ($remaining.Count -eq 0) { return }
        Start-Sleep -Milliseconds 200
    }

    foreach ($id in @(Get-TrackedStarCraftIds)) {
        $process = Get-Process -Id $id -ErrorAction SilentlyContinue
        if (-not $process -or $process.ProcessName -ne 'StarCraft') { continue }
        Stop-Process -Id $id -Force -ErrorAction SilentlyContinue
    }
}
function Close-LaunchedProxy {
    foreach ($id in @($script:proxyLaunched)) {
        Stop-Process -Id $id -Force -ErrorAction SilentlyContinue
    }
}

$logPath = Join-Path $writeRoot "Protodd.log"
$result = $null
$traceBeforeCleanup = ""
$script:startedAtUtc = [DateTime]::UtcNow
$startedUtc = $script:startedAtUtc.ToString('o')
$observerProcess = $null
try {
    if (-not $NoObserver) {
        # Read-only observer on an ephemeral loopback port. Each match owns
        # its helper and authoritative manifest; no stale cross-match result.
        $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
        $listener.Start()
        $observerPort = $listener.LocalEndpoint.Port
        $listener.Stop()
        $observerArguments = @(
            ('"{0}"' -f (Join-Path $repoPath 'tools/decision_report.py')),
            ('"{0}"' -f $logPath), '--serve', [string]$observerPort, '--manifest',
            ('"{0}"' -f (Join-Path $archiveRoot "$Label.json"))
        )
        $observerProcess = Start-Process -FilePath (Get-Command python).Source `
            -ArgumentList $observerArguments -WorkingDirectory $repoPath -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput (Join-Path $archiveRoot "$Label.observer.out") `
            -RedirectStandardError (Join-Path $archiveRoot "$Label.observer.err")
        "LIVE_OBSERVER=http://127.0.0.1:$observerPort"
    }
    Start-Process -FilePath (Join-Path $runtimeA "injectory_x86.exe") `
        -ArgumentList '--launch','StarCraft.exe','--inject','bwapi-data\BWAPI.dll','wmode.dll' `
        -WorkingDirectory $runtimeA -WindowStyle Hidden
    if (Test-Path -LiteralPath (Join-Path $opponentRoot "run_proxy.bat")) {
        $proxy = Start-Process -FilePath $env:ComSpec `
            -ArgumentList @('/c', 'call', 'bwapi-data\AI\run_proxy.bat') `
            -WorkingDirectory $runtimeB -WindowStyle Hidden -PassThru
        $proxyLaunched += $proxy.Id
        Start-Sleep -Seconds 2
    }
    Start-Sleep -Seconds 3
    $launched += @(Get-Process -Name StarCraft -ErrorAction Stop | Select-Object -ExpandProperty Id)
    Start-Process -FilePath (Join-Path $runtimeB "injectory_x86.exe") `
        -ArgumentList '--launch','StarCraft.exe','--inject','bwapi-data\BWAPI.dll','wmode.dll' `
        -WorkingDirectory $runtimeB -WindowStyle Hidden
    Start-Sleep -Seconds 3
    $launched += @(Get-Process -Name StarCraft -ErrorAction Stop | Select-Object -ExpandProperty Id)

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-Path -LiteralPath $logPath) {
            try {
                $terminal = Get-Content -LiteralPath $logPath -Tail 8 -ErrorAction Stop |
                    Where-Object { $_ -match '^END,(win|loss),(\d+)$' } |
                    Select-Object -Last 1
                if ($terminal) {
                    $result = [string]$terminal
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
        opponent_opening_requested = $OpponentOpening
        opponent_runtime_configuration_sha256 = $runtimeConfigurationHash
        opponent_runtime_learning_reset = -not [bool]$PreserveLearning
        map = $Map
        map_sha256 = (Get-FileHash -LiteralPath $mapPath).Hash
        timeout_seconds = $TimeoutSeconds
        seed_requested = $(if ($Seed -ge 0) { $Seed } else { $null })
        seed_observed = $observedSeed
        learning_preserved = [bool]$PreserveLearning
    }
    Close-LaunchedStarCraft
    Close-LaunchedProxy
    if ($null -ne $observerProcess) {
        Stop-Process -Id $observerProcess.Id -ErrorAction SilentlyContinue
    }
    if ($OpponentName -eq 'BananaBrain') {
        # This is opponent-side diagnostic evidence only. Never replace the
        # pre-cleanup competitive result with a cleanup-generated outcome.
        foreach ($file in @(Get-ChildItem -LiteralPath (Join-Path $runtimeB 'bwapi-data/write') -Filter 'Results_*.txt' -File)) {
            Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $archiveRoot "$Label-opponent-$($file.Name)")
            $last = Get-Content -LiteralPath $file.FullName -Tail 1
            $fields = $last -split ','
            if ($fields.Length -eq 13) { $record.opponent_opening_observed = $fields[5] }
        }
    }
    $record | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $archiveRoot "$Label.json") -Encoding utf8
}

if (Test-Path -LiteralPath $logPath) {
    $archive = Join-Path $archiveRoot "$Label.log"
    Copy-Item -LiteralPath $logPath -Destination (Join-Path $archiveRoot "$Label.raw.log") -Force
    [System.IO.File]::WriteAllText($archive, $traceBeforeCleanup)
    "TRACE=$archive"
    $decisionReport = Join-Path $archiveRoot "$Label.decisions.html"
    python (Join-Path $repoPath "tools/decision_report.py") $archive --output $decisionReport | Out-Null
    if ($LASTEXITCODE -eq 0) { "DECISION_REPORT=$decisionReport" }
    else { Write-Warning "Decision report generation failed; the archived match trace is intact" }
}
if (-not $result) {
    throw "Match did not produce a terminal result within $TimeoutSeconds seconds"
}
"RESULT=$result"
