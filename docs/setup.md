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
MSVC ABI, runs the Release tests, and prints the DLL digest. The output is
`build/tournament/Release/Protodd.dll`.

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
analysis. The bot never enables complete-map information or user input.

## Tournament package

Run `./scripts/package-tournament.ps1`. The resulting
`artifacts/Protodd-AIIDE-2026.zip` includes the Release DLL, complete source,
manifest, and exact build instructions. It deliberately excludes StarCraft
assets, BWAPI binaries, debug symbols, logs, and maps.

For large local round robins, use the official
`davechurchill/StarcraftAITournamentManager` with two configured clients. It
records normal results separately from crashes, non-starts, and frame-limit
timeouts; feed the collected `Protodd.log` files to `tools/log_analyzer.py`.
