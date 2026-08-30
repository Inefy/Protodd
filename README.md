# AstraBot

AstraBot is an original, competition-oriented StarCraft: Brood War AI. It plays
Protoss through BWAPI 4.4.0 and is designed around partial information,
opponent adaptation, deterministic behavior, and replay-driven testing.

This repository starts from a clean-room architecture. It does not copy a
tournament bot or depend on code whose license restricts competition entry.

## Status

The repository is under active construction. The portable game-state model,
test harness, build contract, and BWAPI module boundary are established first;
strategy, macro, scouting, combat evaluation, and micro are built on that core.

## Build the portable core

Requirements: CMake 3.24+ and a C++20 compiler.

```powershell
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The `dev` preset uses Ninja. Any generator works if configured manually:

```powershell
cmake -S . -B build/dev -DASTRA_BUILD_BWAPI_MODULE=OFF
cmake --build build/dev
ctest --test-dir build/dev --output-on-failure
```

## Build the tournament DLL

Install Visual Studio with the C++ workload, unpack BWAPI 4.4.0, then configure
the 32-bit preset with the local dependency path:

```powershell
cmake --preset tournament -DBWAPI_ROOT=C:/deps/BWAPI
cmake --build --preset tournament-release
ctest --preset tournament-release
```

Copy `AstraBot.dll` into `StarCraft/bwapi-data/AI/` and select it in
`bwapi-data/bwapi.ini`. StarCraft 1.16.1 and the tournament's BWAPI injector are
required to run matches.

## Competition principles

- 1v1 melee, Protoss, legal BWAPI information only.
- Deterministic decisions for reproducible regressions.
- Matchup-specific openings with evidence-driven transitions.
- Deadline-aware frame scheduling and command deduplication.
- Persistent opponent learning only where tournament rules permit writes.

See [the architecture](docs/architecture.md) for the complete system design.

## License

MIT. See [LICENSE](LICENSE).
