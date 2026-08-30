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

Example:

```powershell
python tools/log_analyzer.py bwapi-data/write/AstraBot.log --pretty
```

The interval is intentionally shown alongside raw win rate. Prefer a candidate
only when it improves the intended matchup without materially increasing
crashes or causing a clear regression elsewhere.

## Persistent learning

At game end, `bwapi-data/write/AstraBot.csv` receives aggregate outcomes. On the
next game Astra first looks for `bwapi-data/read/AstraBot.csv`, matching common
tournament read/write isolation, then falls back to the local write copy. If a
tournament forbids persistent learning, omit the CSV from the package; Astra
will deterministically explore as if facing a new opponent.
