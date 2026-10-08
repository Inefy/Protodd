$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$root = Join-Path ([IO.Path]::GetTempPath()) ("protodd-build-profile-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root -Force | Out-Null

function Invoke-CMakeConfigure {
    param(
        [Parameter(Mandatory)][string]$Directory,
        [string[]]$Options = @()
    )
    $arguments = @('-S', $repo, '-B', $Directory,
        '-DPROTODD_BUILD_TESTS=OFF', '-DPROTODD_BUILD_BWAPI_MODULE=OFF') + $Options
    $priorErrorActionPreference = $ErrorActionPreference
    try {
        # Expected-invalid profile probes emit CMake errors on stderr. Capture
        # those diagnostics as text so a nonzero configure result stays data.
        $ErrorActionPreference = 'Continue'
        $output = & cmake @arguments 2>&1 | Out-String
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $priorErrorActionPreference
    }
    return [pscustomobject]@{ ExitCode = $exitCode; Output = $output }
}

try {
    $tournamentDir = Join-Path $root 'tournament-default'
    $tournament = Invoke-CMakeConfigure -Directory $tournamentDir
    if ($tournament.ExitCode -ne 0) { throw "Default profile configure failed:`n$($tournament.Output)" }
    $tournamentCache = Join-Path $tournamentDir 'CMakeCache.txt'
    foreach ($entry in @(
        'PROTODD_TOURNAMENT_PROFILE:BOOL=ON',
        'PROTODD_DEVELOPER_PROFILE:BOOL=OFF',
        'PROTODD_WHOLE_GAME_RESEARCH_AUTHORITY:BOOL=OFF'
    )) {
        if (-not (Select-String -LiteralPath $tournamentCache -Pattern ('^' + [regex]::Escape($entry) + '$') -Quiet)) {
            throw "Tournament defaults do not record $entry"
        }
    }

    $developerDir = Join-Path $root 'developer'
    $developer = Invoke-CMakeConfigure -Directory $developerDir -Options @(
        '-DPROTODD_TOURNAMENT_PROFILE=OFF',
        '-DPROTODD_DEVELOPER_PROFILE=ON',
        '-DPROTODD_WHOLE_GAME_RESEARCH_AUTHORITY=ON'
    )
    if ($developer.ExitCode -ne 0) { throw "Explicit developer profile configure failed:`n$($developer.Output)" }

    $profiles = @(
        [pscustomobject]@{ Name = 'native-standard'; Control = 'OFF'; Hybrid = 'OFF'; AllIn = 'OFF'; Weight = '' },
        [pscustomobject]@{ Name = 'native-allin'; Control = 'OFF'; Hybrid = 'OFF'; AllIn = 'ON'; Weight = '' },
        [pscustomobject]@{ Name = 'hybrid-standard'; Control = 'ON'; Hybrid = 'ON'; AllIn = 'OFF'; Weight = '' },
        [pscustomobject]@{ Name = 'hybrid-allin'; Control = 'ON'; Hybrid = 'ON'; AllIn = 'ON'; Weight = '' }
    )
    foreach ($profile in $profiles) {
        $profileDir = Join-Path $root $profile.Name
        $configured = Invoke-CMakeConfigure -Directory $profileDir -Options @(
            "-DPROTODD_WHOLE_GAME_CONTROL=$($profile.Control)",
            "-DPROTODD_WHOLE_GAME_HYBRID=$($profile.Hybrid)",
            "-DPROTODD_WHOLE_GAME_EVALUATION_BUILD=$($profile.Hybrid)",
            "-DPROTODD_NATIVE_ALLIN_OPENING=$($profile.AllIn)"
        )
        if ($configured.ExitCode -ne 0) {
            throw "Independent opening/controller profile configure failed for $($profile.Name):`n$($configured.Output)"
        }
        $profileCache = Join-Path $profileDir 'CMakeCache.txt'
        foreach ($entry in @(
            "PROTODD_NATIVE_ALLIN_OPENING:BOOL=$($profile.AllIn)",
            "PROTODD_WHOLE_GAME_CONTROL:BOOL=$($profile.Control)",
            "PROTODD_WHOLE_GAME_HYBRID:BOOL=$($profile.Hybrid)",
            "PROTODD_WHOLE_GAME_EVALUATION_BUILD:BOOL=$($profile.Hybrid)",
            'PROTODD_WHOLE_GAME_WEIGHTS:FILEPATH='
        )) {
            if (-not (Select-String -LiteralPath $profileCache -Pattern ('^' + [regex]::Escape($entry) + '$') -Quiet)) {
                throw "$($profile.Name) configure failed independent-factor assertion: $entry"
            }
        }
    }

    $conflictDir = Join-Path $root 'conflicting-profiles'
    $conflict = Invoke-CMakeConfigure -Directory $conflictDir -Options @(
        '-DPROTODD_TOURNAMENT_PROFILE=ON',
        '-DPROTODD_DEVELOPER_PROFILE=ON'
    )
    if ($conflict.ExitCode -eq 0 -or $conflict.Output -notmatch 'Choose either the tournament profile or the developer profile') {
        throw 'CMake accepted simultaneous tournament and developer profiles'
    }

    $researchDir = Join-Path $root 'tournament-research'
    $research = Invoke-CMakeConfigure -Directory $researchDir -Options @(
        '-DPROTODD_WHOLE_GAME_RESEARCH_AUTHORITY=ON'
    )
    if ($research.ExitCode -eq 0 -or $research.Output -notmatch 'tournament profile cannot enable learned research authority') {
        throw 'CMake accepted learned research authority in the tournament profile'
    }

    $unprofiledDir = Join-Path $root 'unprofiled-module'
    $priorErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $unprofiled = & cmake -S $repo -B $unprofiledDir `
            -DPROTODD_BUILD_TESTS=OFF -DPROTODD_BUILD_BWAPI_MODULE=ON `
            -DPROTODD_TOURNAMENT_PROFILE=OFF -DPROTODD_DEVELOPER_PROFILE=OFF 2>&1 | Out-String
        $unprofiledExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $priorErrorActionPreference
    }
    if ($unprofiledExitCode -eq 0 -or $unprofiled -notmatch 'require an explicit tournament or developer profile') {
        throw 'CMake accepted a BWAPI module without an explicit profile'
    }

    $pvzConflictDir = Join-Path $root 'pvz-conflict'
    $pvzConflict = Invoke-CMakeConfigure -Directory $pvzConflictDir -Options @(
        '-DPROTODD_PVZ_GATEWAY_OPENING=ON',
        '-DPROTODD_PVZ_REPLAY_OPENING=ON'
    )
    if ($pvzConflict.ExitCode -eq 0 -or
        $pvzConflict.Output -notmatch 'Unsupported combination of PvZ strategy interventions') {
        throw 'CMake accepted two incompatible PvZ strategy interventions'
    }

    $cannonPrerequisiteDir = Join-Path $root 'cannon-screen-without-base'
    $cannonPrerequisite = Invoke-CMakeConfigure -Directory $cannonPrerequisiteDir -Options @(
        '-DPROTODD_PVZ_POWERED_CANNON_SCREEN=ON'
    )
    if ($cannonPrerequisite.ExitCode -eq 0 -or
        $cannonPrerequisite.Output -notmatch 'Powered Cannon screen requires the replay PvZ opening') {
        throw 'CMake accepted a dependent PvZ screen without its validated base feature'
    }

    $hybridPrerequisiteDir = Join-Path $root 'hybrid-without-controller'
    $hybridPrerequisite = Invoke-CMakeConfigure -Directory $hybridPrerequisiteDir -Options @(
        '-DPROTODD_WHOLE_GAME_HYBRID=ON'
    )
    if ($hybridPrerequisite.ExitCode -eq 0 -or
        $hybridPrerequisite.Output -notmatch 'Hybrid control requires a weighted local evaluation controller') {
        throw 'CMake accepted hybrid command authority without its controller prerequisites'
    }

    $registry = Get-Content -LiteralPath (Join-Path $repo 'docs/feature-registry.json') -Raw | ConvertFrom-Json
    $releaseFeatures = @($registry.options | Where-Object { $_.tournament_value -eq 'ON' })
    if (@($registry.options).Count -ne 37 -or
        -not ($releaseFeatures.id -contains 'PROTODD_TOURNAMENT_PROFILE') -or
        ($registry.options | Where-Object { $_.promotion_status -eq 'unpromoted-opt-in' -and
            $_.tournament_value -ne 'OFF' })) {
        throw 'Feature registry omitted a CMake option or enabled an unpromoted tournament option'
    }

    $buildScript = Join-Path $repo 'scripts/build-tournament.ps1'
    $outsideBuild = [IO.Path]::GetFullPath((Join-Path $repo 'outside-tournament-build'))
    $priorErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $escapedBuild = & pwsh -NoProfile -File $buildScript -BuildDirectory $outsideBuild 2>&1 | Out-String
        $escapedBuildExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $priorErrorActionPreference
    }
    if ($escapedBuildExitCode -eq 0 -or $escapedBuild -notmatch 'Tournament build directory must stay inside') {
        throw 'Tournament build script accepted an output directory outside build/'
    }

    Write-Output 'Tournament and developer profile configuration checks passed'
} finally {
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}
