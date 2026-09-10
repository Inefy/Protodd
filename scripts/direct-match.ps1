param(
    [ValidateSet("Zerg", "Terran", "Protoss", "Random")]
    [string]$OpponentRace = "Zerg",
    [string]$OpponentName = "",
    [ValidateSet("Auto", "Dll", "Proxy")]
    [string]$OpponentType = "Auto",
    [string]$OpponentCharacterName = "",
    [ValidateSet('', 'PvP_nzcore', 'PvP_zcore', 'PvP_zzcore', 'PvP_zcorez',
        'PvP_10/12gatedt', 'PvP_2gatedtexpo', 'PvP_2gatereaver', 'PvP_3gaterobo',
        'PvP_3gatespeedzeal', 'PvP_12nexus', 'PvP_4gategoon', 'PvP_9/9gate',
        'PvP_9/9proxygate', 'PvP_10/12gate')]
    [string]$OpponentOpening = "",
    [ValidateSet('', 'Terran_MarineRush', 'Terran_TankPush', 'Terran_4RaxMarines', 'Terran_VultureRush')]
    [string]$OpponentStrategy = "",
    [string]$Map = "maps/aiide/(2)Benzene.scx",
    [string]$Label = "direct-match",
    # Retained as an explicit opt-in emergency failsafe for local debugging.
    # Zero means no wall-clock cutoff.
    [ValidateRange(0, 2147483646)]
    [int]$TimeoutSeconds = 0,
    # A normal game may take much longer in wall-clock time than its in-game
    # clock.  The default is therefore an in-game limit only: 86,400 frames
    # at Brood War's 24 frames per second is one hour.
    [ValidateRange(1, 2147483646)]
    [int]$FrameLimit = 86400,
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
if ($OpponentStrategy -and ($OpponentName -notmatch '^UAB(?:Terran)?$' -or $OpponentRace -ne 'Terran')) {
    throw "OpponentStrategy currently supports only UAlbertaBot's Terran strategy profiles"
}
$moduleName = "$OpponentName.dll"
$opponentRoot = Join-Path $repoPath "ladder/bots/$OpponentName/AI"
$opponentMetadataPath = Join-Path (Split-Path -Parent $opponentRoot) ".astra-ladder.json"
if (-not (Test-Path -LiteralPath (Join-Path $opponentRoot $moduleName) -PathType Leaf)) {
    throw "Opponent DLL not found: $opponentRoot/$moduleName"
}
$proxyLauncherPath = Join-Path $opponentRoot "run_proxy.bat"
$resolvedOpponentType = if ($OpponentType -ne "Auto") {
    $OpponentType
} elseif ($OpponentName -match '^UAB(?:Protoss|Terran|Zerg)$') {
    # These packages contain an optional client executable, but the race-fixed
    # DLLs export a complete AIModule and are registered as DLL opponents.
    "Dll"
} elseif (Test-Path -LiteralPath $proxyLauncherPath -PathType Leaf) {
    "Proxy"
} else {
    "Dll"
}
if ($resolvedOpponentType -eq "Proxy" -and
    -not (Test-Path -LiteralPath $proxyLauncherPath -PathType Leaf)) {
    throw "Proxy opponent requires a run_proxy.bat launcher: $proxyLauncherPath"
}
if ([string]::IsNullOrWhiteSpace($OpponentCharacterName)) {
    $OpponentCharacterName = $OpponentName
    if (Test-Path -LiteralPath $opponentMetadataPath -PathType Leaf) {
        try {
            $metadataName = (Get-Content -LiteralPath $opponentMetadataPath -Raw |
                ConvertFrom-Json).name
            if ($metadataName -match '^[A-Za-z0-9_-]{1,24}$') {
                $OpponentCharacterName = $metadataName
            }
        } catch {
            # The package name remains a safe fallback for malformed metadata.
        }
    }
}
if ($OpponentCharacterName -notmatch '^[A-Za-z0-9_-]{1,24}$') {
    throw "Opponent character name must be 1-24 letters, digits, underscores, or hyphens"
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
if ($OpponentStrategy) {
    # Runtime-only override: leave the installed opponent package untouched.
    $uabConfigPath = Join-Path $runtimeB 'bwapi-data/AI/UAlbertaBot_Config.txt'
    if (-not (Test-Path -LiteralPath $uabConfigPath -PathType Leaf)) {
        throw "UAlbertaBot strategy configuration was not staged: $uabConfigPath"
    }
    $uabConfig = [System.IO.File]::ReadAllText($uabConfigPath)
    $strategyMatch = [regex]::Match($uabConfig, '(?m)^(\s*"Terran"\s*:\s*")[^"]+("\s*,\s*)$')
    if (-not $strategyMatch.Success) {
        throw "Could not locate UAlbertaBot's default Terran strategy setting"
    }
    $uabConfig = $uabConfig.Substring(0, $strategyMatch.Index) +
        $strategyMatch.Groups[1].Value + $OpponentStrategy + $strategyMatch.Groups[2].Value +
        $uabConfig.Substring($strategyMatch.Index + $strategyMatch.Length)
    [System.IO.File]::WriteAllText($uabConfigPath, $uabConfig)
    "OPPONENT_STRATEGY=$OpponentStrategy"
}
$runtimeOpponentMetadataPath = Join-Path $runtimeB "bwapi-data/.astra-ladder.json"
if (Test-Path -LiteralPath $opponentMetadataPath -PathType Leaf) {
    Copy-Item -LiteralPath $opponentMetadataPath -Destination $runtimeOpponentMetadataPath -Force
} elseif (Test-Path -LiteralPath $runtimeOpponentMetadataPath -PathType Leaf) {
    # Do not let the previous opponent's display identity survive into a bot
    # package that does not provide metadata.
    Remove-Item -LiteralPath $runtimeOpponentMetadataPath -Force
}

function Ensure-CharacterProfile {
    param([string]$RuntimePath, [string]$CharacterName)

    $characterRoot = Join-Path $RuntimePath "characters"
    foreach ($extension in @("mpc", "spc")) {
        $destination = Join-Path $characterRoot "$CharacterName.$extension"
        if (Test-Path -LiteralPath $destination -PathType Leaf) { continue }
        $template = Get-ChildItem -LiteralPath $characterRoot -Filter "*.$extension" -File |
            Select-Object -First 1
        if (-not $template) {
            throw "No .$extension character template exists in $characterRoot"
        }
        Copy-Item -LiteralPath $template.FullName -Destination $destination
    }
}
Ensure-CharacterProfile -RuntimePath $runtimeA -CharacterName "AstraBot"
Ensure-CharacterProfile -RuntimePath $runtimeB -CharacterName $OpponentCharacterName
if ($OpponentOpening) {
    # Runtime-only configuration: the installed ladder opponent is unchanged.
    Add-Content -LiteralPath (Join-Path $runtimeB 'bwapi-data/AI/Configuration.txt') `
        -Value "`nPvP_opening=$OpponentOpening" -Encoding ascii
}
$runtimeConfiguration = Join-Path $runtimeB 'bwapi-data/AI/Configuration.txt'
$runtimeConfigurationHash = if (Test-Path -LiteralPath $runtimeConfiguration) {
    (Get-FileHash -LiteralPath $runtimeConfiguration -Algorithm SHA256).Hash
} else { $null }
$runtimeStrategyConfiguration = Join-Path $runtimeB 'bwapi-data/AI/UAlbertaBot_Config.txt'
$runtimeStrategyConfigurationHash = if (Test-Path -LiteralPath $runtimeStrategyConfiguration) {
    (Get-FileHash -LiteralPath $runtimeStrategyConfiguration -Algorithm SHA256).Hash
} else { $null }

$seedConfiguration = if ($Seed -ge 0) { "seed_override = $Seed" } else { "" }
$hostIni = @"
[ai]
ai = bwapi-data/AI/Protodd.dll
ai_dbg = bwapi-data/AI/Protodd.dll
tournament =

[auto_menu]
auto_menu = LAN
character_name = AstraBot
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
character_name = $OpponentCharacterName
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
$frameLimitReached = $false
$wallClockTimedOut = $false
$gameProcessExited = $false
$lastObservedFrame = -1
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
    if ($resolvedOpponentType -eq "Proxy") {
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
    $launched = @($launched | Sort-Object -Unique)

    $deadline = if ($TimeoutSeconds -gt 0) {
        [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    } else { $null }
    while ($true) {
        if (Test-Path -LiteralPath $logPath) {
            try {
                $recentLines = @(Get-Content -LiteralPath $logPath -Tail 24 -ErrorAction Stop)
                $terminal = $recentLines |
                    Where-Object { $_ -match '^END,(win|loss),(\d+)$' } |
                    Select-Object -Last 1
                if ($terminal) {
                    $result = [string]$terminal
                    break
                }
                foreach ($line in $recentLines) {
                    if ($line -match '^STATE,(\d+),') {
                        $lastObservedFrame = [int]$Matches[1]
                    }
                }
                if ($lastObservedFrame -ge $FrameLimit) {
                    $frameLimitReached = $true
                    break
                }
            } catch {
                # The game briefly holds an exclusive write lock while flushing.
            }
        }
        if ($null -ne $deadline -and [DateTime]::UtcNow -ge $deadline) {
            $wallClockTimedOut = $true
            break
        }
        $activeGameIds = @(Get-Process -Id $launched -ErrorAction SilentlyContinue |
            Where-Object { $_.ProcessName -eq 'StarCraft' } |
            Select-Object -ExpandProperty Id)
        if ($activeGameIds.Count -lt $launched.Count) {
            $gameProcessExited = $true
            break
        }
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
    $terminationReason = if ($result) {
        'completed'
    } elseif ($frameLimitReached) {
        'frame-limit'
    } elseif ($wallClockTimedOut) {
        'wall-clock-timeout'
    } elseif ($gameProcessExited) {
        'game-process-exited'
    } else {
        'runner-stopped'
    }
    $traceRecords = @($traceBeforeCleanup -split "`r?`n" |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $traceLines = $traceRecords.Count
    $traceLastRecord = @($traceRecords | Select-Object -Last 1)
    $record = [ordered]@{
        label = $Label
        status = $(if ($result) { 'completed' } else { 'incomplete' })
        result = $result
        termination_reason = $terminationReason
        started_utc = $startedUtc
        finished_utc = [DateTime]::UtcNow.ToString('o')
        bot_sha256 = $sourceDllHash
        opponent = $OpponentName
        opponent_type = $resolvedOpponentType.ToLowerInvariant()
        opponent_character_name = $OpponentCharacterName
        opponent_race = $OpponentRace
        opponent_sha256 = (Get-FileHash -LiteralPath (Join-Path $opponentRoot $moduleName)).Hash
        opponent_metadata_sha256 = $(if (Test-Path -LiteralPath $opponentMetadataPath -PathType Leaf) {
            (Get-FileHash -LiteralPath $opponentMetadataPath -Algorithm SHA256).Hash
        } else { $null })
        opponent_components_sha256 = $opponentComponents
        opponent_opening_requested = $OpponentOpening
        opponent_strategy_requested = $OpponentStrategy
        opponent_runtime_configuration_sha256 = $runtimeConfigurationHash
        opponent_runtime_strategy_configuration_sha256 = $runtimeStrategyConfigurationHash
        opponent_runtime_learning_reset = -not [bool]$PreserveLearning
        map = $Map
        map_sha256 = (Get-FileHash -LiteralPath $mapPath).Hash
        timeout_seconds = $(if ($TimeoutSeconds -gt 0) { $TimeoutSeconds } else { $null })
        frame_limit = $FrameLimit
        frame_limit_reached = $frameLimitReached
        last_observed_frame = $lastObservedFrame
        trace_bytes = [System.Text.Encoding]::UTF8.GetByteCount($traceBeforeCleanup)
        trace_lines = $traceLines
        trace_last_record = $(if ($traceLastRecord.Count -gt 0) { $traceLastRecord[0] } else { $null })
        boot_observed = [bool]($traceBeforeCleanup -match '(?m)^BOOT,')
        start_observed = [bool]($traceBeforeCleanup -match '(?m)^START,')
        seed_requested = $(if ($Seed -ge 0) { $Seed } else { $null })
        seed_observed = $observedSeed
        learning_preserved = [bool]$PreserveLearning
    }
    Close-LaunchedStarCraft
    Close-LaunchedProxy
    if (-not $result -and (Test-Path -LiteralPath $logPath)) {
        $cleanupTerminal = @(Get-Content -LiteralPath $logPath -Tail 24 -ErrorAction SilentlyContinue |
            Where-Object { $_ -match '^END,(win|loss),(\d+)$' } |
            Select-Object -Last 1)
        if ($cleanupTerminal.Count -gt 0) {
            $record.cleanup_result_ignored = [string]$cleanupTerminal[0]
        }
    }
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
    if ($frameLimitReached) {
        throw "Match reached the in-game frame limit of $FrameLimit without a terminal result"
    }
    if ($TimeoutSeconds -gt 0) {
        throw "Match did not produce a terminal result within $TimeoutSeconds seconds"
    }
    throw "Match did not produce a terminal result before the game process exited"
}
"RESULT=$result"
