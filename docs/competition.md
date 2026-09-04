# Competition and tuning workflow

A strong bot is an empirical system. Treat every strategic or micro change as
an experiment over a map and opponent matrix, not as a verdict from one replay.

## Baseline gate

1. Run `scripts/verify.ps1` before packaging.
2. Build `Release|Win32` and test the exact DLL in a clean BWAPI 4.4.0 setup.
3. Run mirrored batches with the same map list and seeds for baseline and
   candidate builds. Use at least 100 games per important matchup before
   trusting a small win-rate movement.
4. Separate crashes/timeouts from losses. A crash is a release blocker even if
   aggregate win rate improves.
5. Inspect replays for correlated failure modes: supply blocks, unspent bank,
   missing detection, worker collapse, bad fight entry, stalled production,
   and pathing traps.
6. Keep matchup, map, opponent version, starting location, and commit hash in
   every batch record. Never merge a tuning change on aggregate win rate alone
   when one matchup or crash rate regresses materially.

## Metrics

`tools/log_analyzer.py` consumes one or more `AstraBot.log` files and reports:

- wins, losses, and Wilson 95% win-rate interval;
- game length distribution;
- opening-style results;
- average bank, fight ratio, and uncertainty from periodic snapshots.
- peak frame time, AIIDE 42/55 ms threshold counts, caught errors, and
  automatic load-shedding incidents.

Example:

```powershell
python tools/log_analyzer.py bwapi-data/write/AstraBot.log --pretty
```

The interval is intentionally shown alongside raw win rate. Prefer a candidate
only when it improves the intended matchup without materially increasing
crashes or causing a clear regression elsewhere.

## Automated local ladder

For repeated games against open-source opponents, use the repository's
[local ladder](../ladder/README.md). It prepares a deterministic 1-vs-all
Tournament Manager batch and records the commit and hashes of every binary.
Its report adds opponent, enemy-race, and map splits; first-half/second-half
trend; Wilson intervals; incomplete-game accounting; runtime failures; and a
per-game CSV suitable for replay triage.

Always compare a candidate to a baseline made with the same opponent binaries,
maps, rounds, host ordering, and tournament settings. The `compare` command's
95% interval must exclude zero before it labels the candidate a likely
improvement or regression. Segment-level data still matters: an aggregate gain
can hide a PvT, PvZ, PvP, opponent, or map regression.

## Runtime budget

Astra measures every callback. A frame at 28 ms temporarily disables local
combat simulation and reduces navigation/scouting cadence; a frame at 40 ms
enters an emergency tier with a smaller command budget. Macro, workers,
detection, and retreat control continue. The bot returns to full quality after
the cooldown window rather than permanently degrading after a transient spike.

`PERF_SUMMARY` is written before each `END` record. Any nonzero `over_55ms`,
`over_1s`, `over_10s`, or caught-error count is a release blocker even when the
batch win rate rises.

## Persistent learning

At game end, `bwapi-data/write/AstraBot-<encoded-alias>.csv` receives cumulative
outcomes for the current opponent alias. Astra merges the corresponding read
and local write snapshots without double-counting common history. Separate
filenames prevent AIIDE's round transfers from overwriting other opponents'
updates. The alias is encoded as hexadecimal bytes, without identifying the
real bot. Clearing both read and write history starts a fresh learning run;
omitting initial data alone does not disable learning during a tournament.

Direct local tests record a `.json` outcome manifest before StarCraft is
closed. Use `tools/direct_report.py` with those manifests: an interrupted game
is incomplete even if shutdown subsequently writes `END,loss` to a raw trace.
The direct-match helper preserves a raw trace and archives the pre-cleanup
trace separately. Its default resets learning; `-PreserveLearning` retains it.
Use `-Seed <integer>` to request BWAPI's seed override on both clients, then
check the manifest's observed seed and the initial base positions before
treating runs as paired. Matching a requested seed alone is not proof of
deterministic behavior from an independently randomized opponent.

The direct helper also accepts `-OpponentName BananaBrain -OpponentRace Protoss`
after that bot is imported into the local ladder. It loads the named DLL and
copies its complete AI folder. The current direct runtime requires BWAPI 4.4.0
opponents. All AI component hashes are recorded; reports separate differing
opponent binaries, configurations, and map hashes when those fields exist.
