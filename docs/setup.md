# Build and local match setup

## Requirements

- Windows 10 or 11.
- StarCraft: Brood War 1.16.1 in a path separate from StarCraft Remastered.
- BWAPI 4.4.0 from the official release.
- Visual Studio 2022 with Desktop development with C++, the x86 toolchain, and
  CMake. BWAPI's project files request the old `v141_xp` toolset; if it is not
  installed, retarget the `BWAPILIB` project to the installed x86 toolset.

## Build BWAPI's static interface library

BWAPI 4.4.0 intentionally does not provide a universal prebuilt `.lib`, because
MSVC toolset changes can break C++ ABI compatibility.

1. Clone or unpack `bwapi/bwapi` at tag `v4.4.0`.
2. Open `bwapi/bwapi.sln` in Visual Studio.
3. Select `Release` and `Win32`.
4. Build the `BWAPILIB` project.
5. Locate the resulting `BWAPILIB.lib` (normally under a `Release` directory).

## Build AstraBot

```powershell
cmake --preset tournament `
  -DBWAPI_ROOT=C:/deps/bwapi-4.4.0 `
  -DBWAPI_LIBRARY=C:/deps/bwapi-4.4.0/bwapi/Release/BWAPILIB.lib
cmake --build --preset tournament-release
ctest --preset tournament-release
```

The output is `AstraBot.dll`. Keep the bot and BWAPI library on the same BWAPI
version (`v4.4.0`) and MSVC runtime.

## Run a local game

1. Install BWAPI 4.4.0 into the StarCraft 1.16.1 directory.
2. Copy `AstraBot.dll` to `bwapi-data/AI/AstraBot.dll`.
3. Merge `bwapi-data/bwapi.ini.example` from this repository into the local
   BWAPI configuration. Set the map path to a legal melee map.
4. Enable `BWAPI 4.4.0 Injector [RELEASE]` in Chaoslauncher and start the game.

AstraBot must run as Protoss. Logs are written to
`bwapi-data/write/AstraBot.log`; replays should be retained for regression
analysis. The bot never enables complete-map information or user input.

## Tournament package

Package only the release DLL and any tournament-approved read/write data. Do
not include StarCraft assets, BWAPI DLLs supplied by the tournament, debug
symbols, source checkout, local logs, or maps. Test the exact archive in a clean
StarCraft/BWAPI installation before submission.

