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
    [string]$BotDll = "build/tournament/Release/Protodd.dll",
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
$existingStarCraft = @(Get-Process -Name StarCraft -ErrorAction SilentlyContinue)
if ($existingStarCraft.Count -gt 0) {
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
if (-not (Test-Path -LiteralPath $resolvedDll -PathType Leaf)) {
    throw "Bot DLL not found: $resolvedDll"
}
foreach ($runtime in @($runtimeA, $runtimeB)) {
    & (Join-Path $PSScriptRoot 'restore-match-runtime.ps1') -Runtime $runtime
}
$mapPath = [System.IO.Path]::GetFullPath((Join-Path $runtimeA $Map))
$runtimePrefix = $runtimeA.TrimEnd('\') + '\'
if (-not $mapPath.StartsWith($runtimePrefix, [System.StringComparison]::OrdinalIgnoreCase) -or
    -not (Test-Path -LiteralPath $mapPath -PathType Leaf)) {
    throw "Map must exist inside the host runtime: $mapPath"
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

$script:gameLaunchers = @()
$script:ownedStarCraft = @()
$script:processOwnershipWarnings = @()
$script:proxyLaunched = @()
function Get-MatchOwnedStarCraftIdentities {
    param([object[]]$Processes, [object[]]$Launchers)
    $owned = @()
    foreach ($launcher in $Launchers) {
        $root = @($Processes | Where-Object {
            $_.id -eq $launcher.id -and
            [string]::Equals($_.path, $launcher.path, [StringComparison]::OrdinalIgnoreCase) -and
            [Math]::Abs(($_.started_utc - $launcher.started_utc).TotalSeconds) -le 0.5
        }) | Select-Object -First 1
        if (-not $root) { continue }
        $descendants = @([int]$launcher.id)
        $changed = $true
        while ($changed) {
            $changed = $false
            foreach ($process in $Processes) {
                if ($descendants -contains [int]$process.id -or $descendants -notcontains [int]$process.parent_id) { continue }
                $descendants += [int]$process.id
                $changed = $true
            }
        }
        foreach ($process in $Processes) {
            if ($descendants -notcontains [int]$process.id -or
                -not [string]::Equals($process.path, $launcher.expected_game_path, [StringComparison]::OrdinalIgnoreCase) -or
                $process.started_utc -lt $launcher.started_utc) { continue }
            $owned += [pscustomobject]@{
                id=[int]$process.id; path=$process.path; started_utc=$process.started_utc
                launcher_id=[int]$launcher.id
            }
        }
    }
    @($owned | Sort-Object id -Unique)
}
function Get-MatchProcessSnapshot {
    $processes = @(Get-CimInstance -ClassName Win32_Process -ErrorAction Stop | ForEach-Object {
        if (-not $_.ExecutablePath -or -not $_.CreationDate) { return }
        [pscustomobject]@{
            id=[int]$_.ProcessId; parent_id=[int]$_.ParentProcessId
            path=[IO.Path]::GetFullPath([string]$_.ExecutablePath)
            started_utc=[System.Management.ManagementDateTimeConverter]::ToDateTime($_.CreationDate).ToUniversalTime()
        }
    })
    if ($processes.Count -eq 0) { throw 'Win32_Process returned no usable process identities' }
    $processes
}
function Assert-MatchProcessIdentityCapability {
    try {
        $processes = @(Get-MatchProcessSnapshot)
        $self = @($processes | Where-Object id -eq $PID | Select-Object -First 1)
        if ($self.Count -eq 0 -or -not $self[0].path -or
            $null -eq $self[0].started_utc -or $self[0].parent_id -lt 0) {
            throw 'The current runner process identity is not readable'
        }
    } catch {
        throw "Cannot start a match safely: required Win32_Process identity read is unavailable: $($_.Exception.Message)"
    }
}
function Invoke-MatchLaunchPlan {
    param([Parameter(Mandatory)][scriptblock]$LaunchPlan)
    Assert-MatchProcessIdentityCapability
    & $LaunchPlan
}
function Update-MatchOwnedStarCraft {
    try {
        $processes = @(Get-MatchProcessSnapshot)
        $found = @(Get-MatchOwnedStarCraftIdentities -Processes $processes -Launchers $script:gameLaunchers)
        foreach ($identity in $found) {
            if (@($script:ownedStarCraft | Where-Object { $_.id -eq $identity.id -and $_.started_utc -eq $identity.started_utc }).Count -eq 0) {
                $script:ownedStarCraft += $identity
            }
        }
    } catch {
        $warning = "Could not verify StarCraft process ancestry; no unverified process will be terminated: $($_.Exception.Message)"
        if ($script:processOwnershipWarnings -notcontains $warning) { $script:processOwnershipWarnings += $warning }
    }
}
function Test-MatchOwnedProcessInstance {
    param([object]$Identity)
    try {
        $process = Get-Process -Id $Identity.id -ErrorAction Stop
        if ($process.ProcessName -ne 'StarCraft') { return $null }
        $path = [IO.Path]::GetFullPath([string]$process.Path)
        if (-not [string]::Equals($path, $Identity.path, [StringComparison]::OrdinalIgnoreCase)) { return $null }
        $started = $process.StartTime.ToUniversalTime()
        if ([Math]::Abs(($started - $Identity.started_utc).TotalSeconds) -gt 0.5) { return $null }
        return $process
    } catch { return $null }
}
function Get-CurrentOwnedStarCraft {
    Update-MatchOwnedStarCraft
    foreach ($identity in $script:ownedStarCraft) {
        $process = Test-MatchOwnedProcessInstance -Identity $identity
        if ($process) { $process }
    }
}
function Close-LaunchedStarCraft {
    # Only close process instances previously proven to descend from one of
    # this run's exact injector processes and to match the expected runtime path.
    foreach ($identity in $script:ownedStarCraft) {
        $present = Get-Process -Id $identity.id -ErrorAction SilentlyContinue
        if (-not $present) { continue }
        $process = Test-MatchOwnedProcessInstance -Identity $identity
        if (-not $process) {
            $warning = "Could not revalidate owned StarCraft PID $($identity.id); left it untouched."
            if ($script:processOwnershipWarnings -notcontains $warning) { $script:processOwnershipWarnings += $warning }
            continue
        }
        [void]$process.CloseMainWindow()
    }

    $gracePeriod = [DateTime]::UtcNow.AddSeconds(3)
    while ([DateTime]::UtcNow -lt $gracePeriod) {
        $remaining = @(Get-CurrentOwnedStarCraft)
        if ($remaining.Count -eq 0) { return }
        Start-Sleep -Milliseconds 200
    }

    foreach ($identity in $script:ownedStarCraft) {
        $process = Test-MatchOwnedProcessInstance -Identity $identity
        if (-not $process) { continue }
        Stop-Process -InputObject $process -Force -ErrorAction SilentlyContinue
    }
}
function Close-LaunchedProxy {
    foreach ($identity in @($script:proxyLaunched)) {
        try {
            $process = Get-Process -Id $identity.id -ErrorAction Stop
            $path = [IO.Path]::GetFullPath([string]$process.Path)
            $started = $process.StartTime.ToUniversalTime()
            if ([string]::Equals($path, $identity.path, [StringComparison]::OrdinalIgnoreCase) -and
                [Math]::Abs(($started - $identity.started_utc).TotalSeconds) -le 2) {
                Stop-Process -InputObject $process -Force -ErrorAction SilentlyContinue
            }
        } catch { }
    }
}
function Start-MatchGameRunner {
    param([string]$RuntimePath)
    $runnerPath = Join-Path $RuntimePath 'injectory_x86.exe'
    $expectedGamePath = [IO.Path]::GetFullPath((Join-Path $RuntimePath 'StarCraft.exe'))
    $runner = Start-Process -FilePath $runnerPath `
        -ArgumentList '--launch','StarCraft.exe','--inject','bwapi-data\BWAPI.dll','wmode.dll' `
        -WorkingDirectory $RuntimePath -WindowStyle Hidden -PassThru
    try { $runnerStartedUtc = $runner.StartTime.ToUniversalTime() }
    catch {
        $warning = "Could not verify injector PID $($runner.Id) start identity under '$RuntimePath'; left any game process untouched."
        $script:processOwnershipWarnings += $warning
        throw $warning
    }
    $script:gameLaunchers += [pscustomobject]@{
        id=$runner.Id; path=[IO.Path]::GetFullPath($runnerPath)
        started_utc=$runnerStartedUtc; expected_game_path=$expectedGamePath
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(6)
    do {
        Update-MatchOwnedStarCraft
        if (@($script:ownedStarCraft | Where-Object { $_.launcher_id -eq $runner.Id }).Count -gt 0) { return }
        if ([DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
    } while ([DateTime]::UtcNow -lt $deadline)
    $warning = "Could not prove StarCraft ownership for launcher PID $($runner.Id) under '$RuntimePath'; left any unverified game process untouched."
    $script:processOwnershipWarnings += $warning
    throw $warning
}
function Get-MatchCrashReports {
    param([int]$StabilityMilliseconds = 300)
    if (-not $script:crashReportObservations) {
        $script:crashReportObservations = @{}
    }
    foreach ($side in @(@('protodd', $runtimeA), @('opponent', $runtimeB))) {
        $errorRoot = Join-Path $side[1] 'Errors'
        foreach ($file in @(Get-ChildItem -LiteralPath $errorRoot -Filter '*.txt' -File -ErrorAction SilentlyContinue)) {
            if ($file.LastWriteTimeUtc -lt $script:startedAtUtc) { continue }
            $key = [IO.Path]::GetFullPath($file.FullName)
            try {
                # Exclusive access proves the exception handler is not writing
                # while this snapshot is read. A lock or read error stays pending.
                $stream = [IO.File]::Open($key, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
                try {
                    $memory = [IO.MemoryStream]::new()
                    try {
                        $stream.CopyTo($memory)
                        $bytes = $memory.ToArray()
                    } finally { $memory.Dispose() }
                } finally { $stream.Dispose() }
                $hasher = [Security.Cryptography.SHA256]::Create()
                try { $sha = [BitConverter]::ToString($hasher.ComputeHash($bytes)).Replace('-', '') }
                finally { $hasher.Dispose() }
                $now = [DateTime]::UtcNow
                $previous = $script:crashReportObservations[$key]
                if (-not $previous -or $previous.sha256 -ne $sha -or $previous.length -ne $bytes.Length -or
                    $previous.last_write_utc -ne $file.LastWriteTimeUtc -or $previous.locked) {
                    $script:crashReportObservations[$key] = [pscustomobject]@{
                        sha256=$sha; length=$bytes.Length; last_write_utc=$file.LastWriteTimeUtc
                        observed_utc=$now; bytes=$bytes; locked=$false
                    }
                    continue
                }
                $previous.bytes = $bytes
                if (($now - $previous.observed_utc).TotalMilliseconds -lt $StabilityMilliseconds) { continue }
                $report = [Text.Encoding]::UTF8.GetString($bytes)
                if ($report -match '(?m)^EXCEPTION:') {
                    [pscustomobject]@{
                        side=$side[0]; file=$file; bytes=$bytes; sha256=$sha
                        last_write_utc=$file.LastWriteTimeUtc
                    }
                }
            } catch {
                $key = [IO.Path]::GetFullPath($file.FullName)
                if (-not $script:crashReportObservations.ContainsKey($key)) {
                    $script:crashReportObservations[$key] = [pscustomobject]@{
                        sha256=$null; length=$file.Length; last_write_utc=$file.LastWriteTimeUtc
                        observed_utc=[DateTime]::UtcNow; bytes=$null; locked=$true
                    }
                } else {
                    $script:crashReportObservations[$key].locked = $true
                    $script:crashReportObservations[$key].observed_utc = [DateTime]::UtcNow
                }
            }
        }
    }
}

function Wait-ForMatchCrashReports {
    param([int]$TimeoutMilliseconds = 5000, [int]$StabilityMilliseconds = 300)
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMilliseconds)
    $crashes = @()
    $pending = @()
    do {
        $crashes = @(Get-MatchCrashReports -StabilityMilliseconds $StabilityMilliseconds)
        $pending = @()
        foreach ($side in @(@('protodd', $runtimeA), @('opponent', $runtimeB))) {
            $errorRoot = Join-Path $side[1] 'Errors'
            foreach ($file in @(Get-ChildItem -LiteralPath $errorRoot -Filter '*.txt' -File -ErrorAction SilentlyContinue)) {
                if ($file.LastWriteTimeUtc -lt $script:startedAtUtc) { continue }
                $key = [IO.Path]::GetFullPath($file.FullName)
                $observation = $script:crashReportObservations[$key]
                $stable = $observation -and -not $observation.locked -and
                    $observation.sha256 -and
                    (([DateTime]::UtcNow - $observation.observed_utc).TotalMilliseconds -ge $StabilityMilliseconds)
                if (-not $stable) {
                    $pending += [pscustomobject]@{
                        side=$side[0]; file=$file; bytes=$(if ($observation) { $observation.bytes } else { $null })
                        sha256=$(if ($observation) { $observation.sha256 } else { $null })
                        locked=[bool](-not $observation -or $observation.locked)
                        length=$file.Length; last_write_utc=$file.LastWriteTimeUtc
                    }
                }
            }
        }
        if ($pending.Count -eq 0) { break }
        if ([DateTime]::UtcNow -ge $deadline) { break }
        Start-Sleep -Milliseconds 50
    } while ($true)
    [pscustomobject]@{ complete=($pending.Count -eq 0); crashes=$crashes; pending=$pending }
}

function Save-MatchCrashReportBytes {
    param([string]$Path, [byte[]]$Bytes)
    $temporary = "$Path.$([guid]::NewGuid().ToString('N')).tmp"
    try {
        [IO.File]::WriteAllBytes($temporary, $Bytes)
        [IO.File]::Move($temporary, $Path)
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
    }
}

$logPath = Join-Path $writeRoot "Protodd.log"
$result = $null
$frameLimitReached = $false
$wallClockTimedOut = $false
$gameProcessExited = $false
$lastObservedFrame = -1
$traceBeforeCleanup = ""
$runtimeCrashes = @()
$runtimeCrashReports = @()
$runtimeCrashReportStatus = 'complete'
$script:crashReportObservations = @{}
$script:startedAtUtc = [DateTime]::UtcNow
$startedUtc = $script:startedAtUtc.ToString('o')
$script:observerProcess = $null
try {
    Invoke-MatchLaunchPlan -LaunchPlan {
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
            $script:observerProcess = Start-Process -FilePath (Get-Command python).Source `
                -ArgumentList $observerArguments -WorkingDirectory $repoPath -WindowStyle Hidden -PassThru `
                -RedirectStandardOutput (Join-Path $archiveRoot "$Label.observer.out") `
                -RedirectStandardError (Join-Path $archiveRoot "$Label.observer.err")
            "LIVE_OBSERVER=http://127.0.0.1:$observerPort"
        }
        Start-MatchGameRunner -RuntimePath $runtimeA
        if ($resolvedOpponentType -eq "Proxy") {
            $proxy = Start-Process -FilePath $env:ComSpec `
                -ArgumentList @('/c', 'call', 'bwapi-data\AI\run_proxy.bat') `
                -WorkingDirectory $runtimeB -WindowStyle Hidden -PassThru
            $script:proxyLaunched += [pscustomobject]@{
                id=$proxy.Id; path=[IO.Path]::GetFullPath([string]$env:ComSpec)
                started_utc=$proxy.StartTime.ToUniversalTime()
            }
            Start-Sleep -Seconds 2
        }
        Start-MatchGameRunner -RuntimePath $runtimeB
    }

    $deadline = if ($TimeoutSeconds -gt 0) {
        [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    } else { $null }
    while ($true) {
        if (@(Get-MatchCrashReports).Count -gt 0) { break }
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
        $activeGameIds = @(Get-CurrentOwnedStarCraft | Select-Object -ExpandProperty Id)
        if ($activeGameIds.Count -lt $script:ownedStarCraft.Count) {
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
    $observedTerminalResult = $result
    # Close every launched writer before waiting for stable report snapshots.
    Close-LaunchedStarCraft
    Close-LaunchedProxy
    $crashAudit = Wait-ForMatchCrashReports -TimeoutMilliseconds 5000 -StabilityMilliseconds 300
    foreach ($crash in @($crashAudit.crashes)) {
        $saved = Join-Path $archiveRoot "$Label-$($crash.side)-crash-$($crash.file.Name)"
        Save-MatchCrashReportBytes -Path $saved -Bytes $crash.bytes
        $runtimeCrashes += [ordered]@{side=$crash.side; report=$saved; sha256=$crash.sha256}
        $runtimeCrashReports += [ordered]@{side=$crash.side; status='complete'; report=$saved; sha256=$crash.sha256; bytes=$crash.bytes.Length}
    }
    foreach ($pending in @($crashAudit.pending)) {
        $saved = $null
        if ($null -ne $pending.bytes) {
            $saved = Join-Path $archiveRoot "$Label-$($pending.side)-incomplete-$($pending.file.Name)"
            Save-MatchCrashReportBytes -Path $saved -Bytes $pending.bytes
        }
        $runtimeCrashReports += [ordered]@{
            side=$pending.side; status='incomplete'; source=$pending.file.FullName; report=$saved
            sha256=$pending.sha256; bytes=$pending.length; locked=$pending.locked
            last_write_utc=$pending.last_write_utc.ToString('o')
        }
    }
    if (-not $crashAudit.complete) { $runtimeCrashReportStatus = 'incomplete' }
    if ($runtimeCrashes.Count -gt 0 -or $runtimeCrashReportStatus -eq 'incomplete') { $result = $null }
    $terminationReason = if ($runtimeCrashReportStatus -eq 'incomplete') {
        'runtime-crash-report-incomplete'
    } elseif ($runtimeCrashes.Count -gt 0) {
        'runtime-crash'
    } elseif ($script:processOwnershipWarnings.Count -gt 0 -and $script:ownedStarCraft.Count -eq 0) {
        'process-ownership-unverified'
    } elseif ($result) {
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
        observed_terminal_result = $observedTerminalResult
        runtime_crashes = $runtimeCrashes
        runtime_crash_reports = $runtimeCrashReports
        runtime_crash_report_status = $runtimeCrashReportStatus
        process_cleanup_warnings = @($script:processOwnershipWarnings)
        host_bwapi_sha256 = (Get-FileHash -LiteralPath (Join-Path $runtimeA 'bwapi-data/BWAPI.dll')).Hash
        opponent_bwapi_sha256 = (Get-FileHash -LiteralPath (Join-Path $runtimeB 'bwapi-data/BWAPI.dll')).Hash
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
    if (-not $result -and (Test-Path -LiteralPath $logPath)) {
        $cleanupTerminal = @(Get-Content -LiteralPath $logPath -Tail 24 -ErrorAction SilentlyContinue |
            Where-Object { $_ -match '^END,(win|loss),(\d+)$' } |
            Select-Object -Last 1)
        if ($cleanupTerminal.Count -gt 0) {
            $record.cleanup_result_ignored = [string]$cleanupTerminal[0]
        }
    }
    if ($null -ne $script:observerProcess) {
        Stop-Process -InputObject $script:observerProcess -ErrorAction SilentlyContinue
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
    $record | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $archiveRoot "$Label.json") -Encoding utf8
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
    if ($script:processOwnershipWarnings.Count -gt 0) {
        throw "Could not verify safe game-process ownership; unverified processes were left untouched. See the match record for '$Label'"
    }
    if ($runtimeCrashes.Count -gt 0) {
        throw "Match aborted after a runtime crash; see the archived crash reports for '$Label'"
    }
    if ($runtimeCrashReportStatus -eq 'incomplete') {
        throw "Match report writer did not finish cleanly; see the archived incomplete report evidence for '$Label'"
    }
    if ($frameLimitReached) {
        throw "Match reached the in-game frame limit of $FrameLimit without a terminal result"
    }
    if ($wallClockTimedOut) {
        throw "Match did not produce a terminal result within $TimeoutSeconds seconds"
    }
    throw "Match did not produce a terminal result before the game process exited"
}
"RESULT=$result"
