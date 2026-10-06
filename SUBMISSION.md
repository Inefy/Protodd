# Protodd tournament submission

Protodd is a Protoss BWAPI 4.4.0 module for StarCraft: Brood War 1.16.1.
`Protodd.dll` is the Release/Win32 competition binary. The `source` directory
contains the complete original source, tests, build system, and documentation.
No BWAPI binaries or other large external libraries are bundled.

## Clean build

Requirements:

- Windows 10 or newer;
- Visual Studio 2022 Build Tools with C++ x86/x64 tools and a Windows SDK;
- CMake 3.24 or newer;
- Python 3.11 or newer and PowerShell;
- Node.js 24 for the registered replay-comparison test;
- an official BWAPI 4.4.0 source checkout.

Install `training/requirements.txt` with CPU-only PyTorch to enable the
optional model-training test group; without it, CMake omits those tests.

From the source directory:

```powershell
./scripts/build-tournament.ps1 -BwapiRoot C:/deps/bwapi
```

The script generates BWAPI's revision header, builds the official BWAPILIB as
Release/Win32, configures Protodd for Win32, builds the DLL, runs the Release
tests, verifies that the PE is x86 and exports `gameInit` and `newAIModule`,
and writes `Protodd.build-manifest.json` beside the DLL. The manifest binds the
binary to the source-file inventory, source commit, CMake cache, compiler,
BWAPILIB, BWAPI source revision, complete feature manifest, and enabled build
flags. This script explicitly selects the tournament profile, which disables
interactive `UserInput`, debug overlays and learned-policy research/upgrade
commands. Local `bwapi-data/read` controls and learned files are excluded from
the package.

## Runtime files

Copy `Protodd.dll` to `bwapi-data/AI/` and select it as the release AI in
`bwapi-data/bwapi.ini`. Protodd reads per-opponent `Protodd-<encoded-alias>.csv`
files from `bwapi-data/read/` and its own local `bwapi-data/write/` directory,
then writes a cumulative snapshot with a unique bounded outcome ID for each
completed game. Versioned reversible field encoding keeps punctuation and
control characters distinct; map history includes the BWAPI map hash. Repeated
snapshot imports are idempotent, and history replacement is atomic. It also
writes `Protodd.log`. It requires no network, GPU, registry setting, environment
variable, or absolute runtime path.

The bot must play Protoss. Tournament aliases are treated as opaque opponent
keys; Protodd does not attempt to infer real bot identities.

## Package integrity

After a successful build, run `./scripts/package-tournament.ps1`. The packager
checks the DLL hash, the exact inventoried source files, and the source snapshot
against the adjacent build manifest before it writes the archive. Runtime
learning files, logs, maps, StarCraft assets, and BWAPI binaries are excluded.

CI extracts the archive into an empty directory and rebuilds the Win32 module
against the official BWAPI 4.4.0 source checkout. That verifies the submitted
source and toolchain instructions; actual game loading still needs qualification
with a native StarCraft runtime.
