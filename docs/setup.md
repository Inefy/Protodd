# Build and local match setup

## Requirements

- Windows 10 or 11.
- StarCraft: Brood War 1.16.1 in a path separate from StarCraft Remastered.
- BWAPI 4.4.0 from the official release.
- Visual Studio 2022 with Desktop development with C++, the x86 toolchain, and
  CMake. BWAPI's project files request the old `v141_xp` toolset; if it is not
  installed, retarget the `BWAPILIB` project to the installed x86 toolset.

## Build the tournament DLL

BWAPI 4.4.0 intentionally does not provide a universal prebuilt `.lib`, because
MSVC toolset changes can break C++ ABI compatibility.

Clone or unpack `bwapi/bwapi` at tag `v4.4.0`, then run:

```powershell
./scripts/build-tournament.ps1 -BwapiRoot C:/deps/bwapi-4.4.0
```

This builds BWAPI's static interface library and Protodd with the same x86
MSVC ABI, runs the Release tests, verifies the DLL architecture and exports,
and writes a source/configuration-bound build manifest beside
`build/tournament/Release/Protodd.dll`.

The script forces the tournament profile: `UserInput` and interactive overlays
are disabled, and learned-policy research/upgrade commands are rejected.
The build manifest and the startup `FEATURE_MANIFEST` record the active
profile; tournament packages exclude all `bwapi-data/read` controls and learned
files.

For local inspection, configure a separate developer DLL against the BWAPILIB
already built above:

```powershell
cmake -S . -B build/developer -G "Visual Studio 17 2022" -A Win32 `
  -DPROTODD_BUILD_TESTS=OFF -DPROTODD_BUILD_BWAPI_MODULE=ON `
  -DPROTODD_TOURNAMENT_PROFILE=OFF -DPROTODD_DEVELOPER_PROFILE=ON `
  -DPROTODD_WHOLE_GAME_RESEARCH_AUTHORITY=OFF `
  "-DBWAPI_ROOT=C:/deps/bwapi-4.4.0" `
  "-DBWAPI_LIBRARY=C:/deps/bwapi-4.4.0/bwapi/BWAPILIB/Release/BWAPILIB.lib"
cmake --build build/developer --config Release --target protodd_bwapi
```

That profile enables `/debug`, the overlays, and BWAPI `UserInput`. Learned
research authority remains off unless explicitly enabled for an isolated
developer evaluation. Never package a developer DLL.

## Run a local game

1. Install BWAPI 4.4.0 into the StarCraft 1.16.1 directory.
2. Copy `Protodd.dll` to `bwapi-data/AI/Protodd.dll`.
3. Merge `bwapi-data/bwapi.ini.example` from this repository into the local
   BWAPI configuration. Set the map path to a legal melee map.
4. Enable `BWAPI 4.4.0 Injector [RELEASE]` in Chaoslauncher and start the game,
   or use STARTcraft's Injectory runner with a legally installed Brood War
   1.16.1 directory.

Protodd must run as Protoss. Logs are written to
`bwapi-data/write/Protodd.log`; replays should be retained for regression
analysis. The tournament profile never enables complete-map information or
user input.

## Tournament package

Run `./scripts/package-tournament.ps1` after building. It refuses to package a
stale DLL or source/configuration drift. The resulting
`artifacts/Protodd-AIIDE-2026.zip` includes the Release DLL, inventoried source,
build manifest, and exact build instructions. It excludes StarCraft assets,
BWAPI binaries, debug symbols, learning files, logs, and maps. CI rebuilds the
archive from an empty directory against the official BWAPI 4.4.0 source.

For large local round robins, use the official
`davechurchill/StarcraftAITournamentManager` with two configured clients. It
records normal results separately from crashes, non-starts, and frame-limit
timeouts; feed the collected `Protodd.log` files to `tools/log_analyzer.py`.

## Direct matches after a tournament campaign

Tournament Manager deletes `bwapi-data`, `characters`, and `maps` from its
StarCraft runtimes during cleanup. The default
`scripts/direct-match.ps1` profile is `patched-diagnostic`; it restores missing
files and requires the DLL, source inventory, and CMake cache to match the
build manifest. Run records are stored under a profile-specific directory and
include both clients' engine hashes, map/opponent hashes, seed, settings, and
learning-state hashes. Older binaries require the explicit
`-AllowUnmanifestedDll` diagnostic option.

For example, after building the tournament DLL:

```powershell
./scripts/direct-match.ps1 -OpponentRace Protoss -Map 'maps/aiide/(4)Python.scx' -Label uab-protoss-check
```

Use `-OpponentRace Terran` or `Zerg` for the other race-fixed UAlbertaBot
packages. Match records and decision reports are saved under
`build/direct-logs`. `scripts/restore-match-runtime.ps1 -Runtime <absolute-path>`
also restores an idle runtime before launching it manually.

To qualify against the unmodified release engine, run
`./scripts/prepare-stock-runtime.ps1` once while StarCraft is stopped. It makes
separate host/opponent runtime copies under `build/stock-certification`, omits
mutable game data, and installs the engine DLL from the preserved official
BWAPI archive only after checking its archive and DLL hashes. Then pass
`-RuntimeProfile stock-certification` to `direct-match.ps1`. The stock and
patched profiles use separate runtimes and result folders; a profile marker
prevents later restoration from silently switching a runtime's engine.

For a named paired evaluation, prepare each profile from the existing local
runtime pair while StarCraft is stopped:

```powershell
./scripts/prepare-patched-runtime.ps1 -RuntimeSet evaluation-01
./scripts/prepare-stock-runtime.ps1 -RuntimeSet evaluation-01
```

Run `direct-match.ps1` with the same `-RuntimeSet evaluation-01` and the same
bot DLL/build manifest for each profile, changing only
`-RuntimeProfile patched-diagnostic` versus `stock-certification`. Named
patched runtimes live under `build/patched-diagnostic/<name>`, stock runtimes
under `build/stock-certification/<name>`, and results under
`build/direct-logs/<profile>/<name>`. The launcher refuses missing named pairs
instead of silently creating or merging campaign state.

## Local BWAPI latency crash repair

The bundled BWAPI 4.4 runtime can return negative remaining latency after a
stale network turn. Its latency-compensation buffer converts `remaining + 15`
to an unsigned vector size; values below zero cause `std::length_error` and
terminate StarCraft. This occurred in the UAlbertaBot Terran process on
Destination during the 4 October 2026 test.

```powershell
./scripts/build-bwapi-runtime.ps1 -Deploy
```

The repair clamps remaining latency to zero, compiles and checks the actual
patched vendor method against normal and delayed turn data, and rebuilds the
Win32 engine. The script also adapts the legacy source to C++17 filesystem and
loads Windows headers at their required packing when building the old packed
StarCraft projects with the current SDK.

`-Deploy` requires StarCraft to be stopped. It publishes the patched build to
the isolated `build/patched-diagnostic-template` profile and writes its hash
manifest. It does not replace the official Tournament Manager archive or alter
existing campaign runtimes. The stock-certification profile always verifies
against the separately preserved official archive.
Direct matches archive fresh crash reports from either side and exclude those
games from completed win/loss results.
