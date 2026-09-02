# AstraBot

AstraBot is an original, competition-oriented StarCraft: Brood War AI. It plays
Protoss through BWAPI 4.4.0 and is designed around partial information,
opponent adaptation, deterministic behavior, and replay-driven testing.

This repository starts from a clean-room architecture. It does not copy a
tournament bot or depend on code whose license restricts competition entry.

## What is implemented

- Matchup-specific PvT, PvZ, and PvP plans with reactive anti-rush, anti-air,
  and anti-cloak transitions.
- Bayesian opening recognition under fog of war and decaying enemy memory.
- Economy saturation, gas policy, worker defense, evacuation, and transfers.
- Resource-reserving macro planner with production, technology, expansion, and
  supply goals, queued-production accounting, and automatic prerequisite repair.
- Base-aware construction, safe reachable expansions, projected-supply timing,
  and mineral-line saturation transfers across completed bases.
- Ground/air/detection influence maps, risk-aware scouting, combat evaluation,
  local squads, detector escorts, coordinated focus fire, unit-specific kiting,
  surrounds, cloak preservation, Storm, Stasis, Feedback, and ammunition upkeep.
- A legal-information-only BWAPI adapter with command deduplication and
  staggered frame scheduling, a fair per-tick command budget, measured
  frame-time load shedding, and callback exception containment.
- UCB-based opponent/map learning across games, using tournament-safe read and
  write directories to explore and exploit four opening styles.
- Portable deterministic regression scenarios and strict-warning compilation.

## Build the portable core

Requirements: CMake 3.24+ and a C++20 compiler.

```powershell
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

For the same strict, portable gate used during development (core tests, log
tests, adapter translation-unit checks, and whitespace validation), run:

```powershell
./scripts/verify.ps1
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
required to run matches. The official 4.4.0 source distribution does not ship
a compiler-independent static library; see [the setup guide](docs/setup.md) for
the exact `BWAPILIB` build and configuration flow.

## Competition principles

- 1v1 melee, Protoss, legal BWAPI information only.
- Deterministic decisions for reproducible regressions.
- Matchup-specific openings with evidence-driven transitions.
- Deadline-aware frame scheduling and command deduplication.
- Persistent opponent learning only where tournament rules permit writes.

See [the architecture](docs/architecture.md) for the complete system design.
Use [the competition workflow](docs/competition.md) to benchmark changes and
the included log analyzer to compare batches rather than individual games.

## License

MIT. See [LICENSE](LICENSE).
