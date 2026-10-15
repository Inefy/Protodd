# Protodd — AIIDE 2026

Protoss bot for StarCraft: Brood War 1.16.1 and official BWAPI 4.4.0.
`Protodd.dll` is the Release/Win32 binary. Complete source and the MIT license
are in `source/`; `manifest.json` records the binary and source hashes.

## Run

Use Windows 10, official BWAPI 4.4.0, and the
[Microsoft Visual C++ v14 x86 Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist)
(MSVC 14.41 or newer). Copy `Protodd.dll` to `bwapi-data/AI/`, set
`ai = bwapi-data/AI/Protodd.dll` under `[ai]` in `bwapi-data/bwapi.ini`, and
select Protoss. No additional bot data files are required.

The bot uses the standard `bwapi-data/read/` and `bwapi-data/write/` directories
for persistent learning and logs. Python, Node.js, and a GPU are not required
at runtime.

## Build

Install Visual Studio 2022 Build Tools (C++ x86/x64 tools, v143, and a Windows
SDK), CMake 3.24+, PowerShell 7, Python 3.11+, and Git. Put `cmake`, `python`,
and `git` on PATH. Obtain an official
[BWAPI 4.4.0](https://github.com/bwapi/bwapi/releases/tag/v4.4.0) Git checkout:

```powershell
git clone --branch v4.4.0 https://github.com/bwapi/bwapi.git C:/deps/bwapi
```

From `source/`, run in PowerShell, replacing the example BWAPI path:

```powershell
./scripts/build-tournament.ps1 -BwapiRoot C:/deps/bwapi
```

The BWAPI directory must contain `bwapi/BWAPILIB/BWAPILIB.vcxproj`.
The script builds official BWAPILIB and the tournament bot, runs tests, and
writes `build/tournament/Release/Protodd.dll`. No environment variables or
additional C++ libraries are required. Optional replay/model tests run only
when their Node.js/Python dependencies are installed.
