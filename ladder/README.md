# Local bot ladder

This directory contains the tracked configuration template for repeatable
Protodd experiments. All third-party bots, maps, Tournament Manager files,
results, replays, reports, and machine-specific settings live in ignored
subdirectories. They are local test inputs, not part of Protodd.

The ladder uses
[StarcraftAITournamentManager](https://github.com/davechurchill/StarcraftAITournamentManager)
to execute games. The repository's `tools/ladder.py` prepares its bot folders,
settings, and JSONL schedule, then reads either its raw `results.txt` or its
generated `html/results/detailed_results_json.js`.

## 1. Initialize the private workspace

```powershell
./scripts/ladder.ps1 init
```

This creates `ladder/ladder.local.json` plus these Git-ignored locations:

- `ladder/bots/` — opponent binaries, source trees, read/write data, and
  provenance manifests.
- `ladder/manager/` — the unpacked Tournament Manager distribution.
- `ladder/maps/` — the local map archive and any map data.
- `ladder/runs/` — immutable run snapshots, raw results, replays, and logs.
- `ladder/reports/` — generated JSON, Markdown, HTML, and CSV analysis.

Run `./scripts/ladder.ps1 audit` at any time to verify that none of those paths
is tracked and all remain covered by `.gitignore`. The same audit runs in the
normal verifier and CI. Git's explicit `-f` option can bypass any ignore rule,
so never force-add ladder assets.

## 2. Install the runner and maps locally

Unpack Tournament Manager so that
`ladder/manager/server/server.jar` and its normal `server/`, `client/`, and
required-file layout exist. Put a legal Tournament Manager-compatible map
archive at `ladder/maps/maps.zip`. Keep the archive's internal paths aligned
with the `maps` values in `ladder.local.json`.

Tournament Manager requires two configured clients for a match. Its upstream
documentation recommends one client per physical machine or VM. Each client
needs Brood War 1.16.1 and the matching BWAPI/TournamentModule files. These
licensed game files are deliberately not managed by this repository.

## 3. Add open-source opponents

Check each bot's license and tournament packaging instructions. Import either
its complete Tournament Manager bot folder, its `AI/` directory, or its DLL:

```powershell
./scripts/ladder.ps1 add-bot `
  --name Iron `
  --race Terran `
  --type dll `
  --bwapi-version BWAPI_412 `
  --source C:/downloads/Iron `
  --source-url https://example.invalid/upstream `
  --version exact-release-or-commit `
  --license MIT
```

The command copies the bot into `ladder/bots/Iron`, verifies Tournament
Manager's required `AI/Iron.dll` layout, records provenance and a content hash,
and adds only a local entry to `ladder.local.json`. Proxy bots also require
`AI/run_proxy.bat`.

Never point the ladder directly at a mutable download directory. Import a
specific release or commit so a later run can be reproduced.

## 4. Prepare and run a batch

Build the exact Protodd DLL, choose an even number of rounds, then prepare a
named run:

```powershell
./scripts/build-tournament.ps1 -BwapiRoot C:/deps/bwapi-4.4.0
./scripts/ladder.ps1 prepare --label baseline-main
```

Preparation copies the local Tournament Manager into
`ladder/runs/baseline-main/tournament`, removes its bundled bot pool from that
copy, stages Protodd and the configured opponents, and writes:

- a deterministic 1-vs-all `server/games.txt` schedule;
- alternating host order for every opponent across rounds;
- competition time limits in `server/server_settings.json`;
- a `manifest.json` with the Git commit, dirty state, DLL hash, opponent hashes,
  maps, and complete batch size.

Start `tournament/server/run_server.bat` inside that run and connect two
Tournament Manager clients. A clean Git commit is strongly recommended for
baseline and candidate runs.

## 5. Generate the report

After the batch completes, include Protodd's collected log when available:

```powershell
./scripts/ladder.ps1 report `
  ladder/runs/baseline-main/tournament/server/results.txt `
  --manifest ladder/runs/baseline-main/manifest.json `
  --protodd-log ladder/runs/baseline-main/tournament/server/bots/Protodd/write/Protodd.log `
  --output ladder/reports/baseline-main
```

The output includes:

- `index.html` — local dashboard;
- `report.md` and `report.json` — human- and machine-readable summaries;
- `games.csv` — one row per Tournament Manager game;
- `telemetry-games.csv` — one row per Protodd log game when logs were passed.

Win rates always include Wilson 95% intervals. Fewer than 30 games is marked
`insufficient`, 30–99 is `directional`, and 100+ is `strong`. A segment is only
called winning or losing when its interval excludes 50%. Crashes, frame
timeouts, incomplete matches, map splits, opponent splits, trend, supply-block
snapshots, late high bank, dangerous fight entry, enemy uncertainty, and
runtime spikes are reported separately.

## 6. Compare a candidate with the baseline

Use the same opponents, versions, maps, number of games, and time limits:

```powershell
./scripts/ladder.ps1 compare `
  ladder/reports/baseline-main/report.json `
  ladder/reports/candidate-change/report.json `
  --output ladder/reports/baseline-vs-candidate.json
```

The comparison reports the overall percentage-point change and a Newcombe
score-based 95% interval, plus opponent-level deltas. Treat `inconclusive` as a
request for more games, not evidence of no effect. Any Protodd crash or severe
frame timeout is a release blocker regardless of win rate.
