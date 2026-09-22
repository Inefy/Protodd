# Whole-game training and verification

Active objective: test the games themselves and train all aspects of play to
produce a robust bot. Protoss comes first, followed by Terran and Zerg. This goal
is not complete when a replay predictor finishes fitting or a smoke game passes.
Local compute is authorized; cloud spending still requires a cost proposal.

## Coverage required before claiming completion

| Domain | Training/control work required | Verification required |
|---|---|---|
| Economy | Mining, worker/gas allocation, expansion, supply and production use | Income, idle-worker/producer time, supply blocks and losses under harassment |
| Macro/technology | Event timing, intent choice, persistent command execution, tech transitions and upgrades | Accepted-command feedback, resource contention, prerequisite/placement failures; non-wait recall and full games |
| Scouting and belief | Exploration, coverage, enemy-memory uncertainty, detection and information value | Fog/cloak/brief-sighting fixtures, unseen opponents and maps, scouting ablations |
| Army strategy | Composition, reinforcement, attack/retreat, defending multiple bases | Diverse rush/timing/economy opponents; paired game outcomes and failed-fight diagnostics |
| Micro and tactics | Movement, targeting, kiting, focus fire, spell use, transport and harassment | Unit/scenario fixtures, game engagements and each unit/ability's supported behavior |
| Long-term decisions | Temporal observations, opponent adaptation and delayed outcome learning | Causal sequence tests and independently adjudicated training episodes |
| Robust execution | CPU inference, time limits, pathfinding, recovery and legal observations | Exact compiled package, complete frame timings, crashes, stalls and tournament-host tests |
| Transfer | All matchups; Protoss first, then own Terran and Zerg | Race-specific models/controllers; maps, opponents and seeds excluded from training |

The current v2 replay data contains only macro intents, so it cannot supply all
these targets. Expand legal observation/action recording and the training models
for the missing domains, and use separately marked interactive training games.
Do not relabel macro imitation as full-game training. Retain a sealed final test
pool and never train from evaluation traces. Wins require healthy consistent
reports from both players; one bot's END message cannot establish that an opponent
did not crash. Hardware/runtime failures remain operational failures.

## Evidence and current work — 22 September 2026

- A0 macro reference completed 20 epochs; CPU export verified. Non-wait macro
  recall is only 4.94%; action timing remains an open issue.
- Rebuilt the current Win32 Protoss/Terran/Zerg DLLs. Frozen Protoss baseline:
  `build/robust-training-20260922/baseline-Protodd.dll`, SHA-256
  `0ec21a0176d092d6c1b001498d392073b9b941d72f0e1802ad7a38dfef79106e`.
- Actually played Protoss versus UABTerran on Benzene, requested/observed seed
  220922. Single-sided diagnostic result: loss at frame 10,914, with no caught or
  logging errors. Maximum own army three versus 23 observed enemy army units;
  first core 4,944, first Dragoon 5,688, first range 7,560, base breach 7,152.
  This exposes early production/defense weakness; it is not paired reward evidence.
  Manifest/log: `build/direct-logs/robust-20260922-baseline-001.{json,log}`.
- Added `training.arena` to prepare immutable training/development/final-test
  campaigns without relying on the missing 18 September workspace. It verifies
  runnable JARs, runtime paths, map archives, opponent packages and both host sides,
  preserves required opponent read data, and fingerprints all initial artifacts.
- Prepared `build/robust-training-20260922/development-01`: 12 development games,
  insanitybot / BananaBrain / McRaveZ, Benzene / Destination, both host sides.
  `processes.json` records the actual server/client handles after launch. Inspect
  those handles and logs, rather than treating the manifest as proof of progress.
- Three arena tests pass: immutable inputs/host pairing, rejection of placeholder
  clients, and rejection of one-sided/crashed outcome evidence.

Inspect the campaign:

```powershell
./build/model-venv/Scripts/python.exe -m training.arena inspect build/robust-training-20260922/development-01
```

Next: verify the tournament runner through actual paired results, classify game
failures, fix early production/defense, implement temporal/event macro training
and learned intent execution, then expand data/control heads for the remaining
domains in the coverage table. Prepare separate training campaigns with
`--purpose training`; development/final-test campaigns remain frozen. The arena
does not automatically certify activity or train from unchecked results.
