# AstraBot tournament submission

AstraBot is a Protoss BWAPI 4.4.0 module for StarCraft: Brood War 1.16.1.
`AstraBot.dll` is the Release/Win32 competition binary. The `source` directory
contains the complete original source, tests, build system, and documentation.
No BWAPI binaries or other large external libraries are bundled.

## Clean build

Requirements:

- Windows 10 or newer;
- Visual Studio 2022 Build Tools with C++ x86/x64 tools and a Windows SDK;
- CMake 3.24 or newer;
- an official BWAPI 4.4.0 source checkout.

From the source directory:

```powershell
./scripts/build-tournament.ps1 -BwapiRoot C:/deps/bwapi
```

The script generates BWAPI's revision header, builds the official BWAPILIB as
Release/Win32, configures AstraBot for Win32, builds the DLL, runs the Release
tests, and prints the DLL's SHA-256 digest.

## Runtime files

Copy `AstraBot.dll` to `bwapi-data/AI/` and select it as the release AI in
`bwapi-data/bwapi.ini`. Astra only reads `bwapi-data/read/AstraBot.csv` and
writes `bwapi-data/write/AstraBot.csv` plus `AstraBot.log`. It requires no
network, GPU, registry setting, environment variable, or absolute runtime path.

The bot must play Protoss. Tournament aliases are treated as opaque opponent
keys; Astra does not attempt to infer real bot identities.
