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
    $output = & cmake @arguments 2>&1 | Out-String
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $output }
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
    $unprofiled = & cmake -S $repo -B $unprofiledDir `
        -DPROTODD_BUILD_TESTS=OFF -DPROTODD_BUILD_BWAPI_MODULE=ON `
        -DPROTODD_TOURNAMENT_PROFILE=OFF -DPROTODD_DEVELOPER_PROFILE=OFF 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0 -or $unprofiled -notmatch 'require an explicit tournament or developer profile') {
        throw 'CMake accepted a BWAPI module without an explicit profile'
    }

    $buildScript = Join-Path $repo 'scripts/build-tournament.ps1'
    $outsideBuild = [IO.Path]::GetFullPath((Join-Path $repo 'outside-tournament-build'))
    $escapedBuild = & pwsh -NoProfile -File $buildScript -BuildDirectory $outsideBuild 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0 -or $escapedBuild -notmatch 'Tournament build directory must stay inside') {
        throw 'Tournament build script accepted an output directory outside build/'
    }

    Write-Output 'Tournament and developer profile configuration checks passed'
} finally {
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}
