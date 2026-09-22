# Remastered replay playback

The native replay adapter uses the pinned Remastered OpenBW fork plus a local
terrain overlay. **Classic MPQ terrain is unsuitable for these Remastered maps.**
Using the old CV5/VF4 tables caused Backrooms to fail during initial unit placement
and KnockOut to simulate incorrect construction, economy and unit production.
Replacing only those terrain files resolved both failures. Commands and terrain
tile IDs are preserved; do not remove troublesome map units or ignore load errors.

## Components

- `training/replay_assets.py` extracts the twelve nonempty CV5/VF4 files for six
  ladder tilesets from the pinned local `sim.pack.gz`. It checks the pack hash,
  requires the complete set, and writes a per-file hash manifest. The native loader
  requires these files, including when its requests use `Tileset/` capitalization;
  it never silently substitutes classic terrain for them.
- `tools/replay_decoder` uses screp v1.13.4 to decode `seRS` replay sections,
  preserving raw commands and the 3,400-unit LMTS limits. The adapter translates
  map display strings (`STRx` to `STR`) and Remastered map headers (206 to 205,
  64 to 63); supported legacy headers 59/63/205 stay unchanged. Gameplay map chunks
  such as `UNIT` and `MTXM` stay byte-for-byte identical. This internal uncompressed
  `.raw` format is not a StarCraft replay and never replaces the downloaded file.
- `tools/replay_native` compiles a separate x64 checkpoint executable against
  OpenBW commit `86fc7f6da3c7a01542471f2ea303eb4bde0f9f04`. CMake rejects a different
  or modified engine checkout. No third-party engine changes are required.
- `training/playback_check.py` verifies the terrain manifest, copies and hashes
  each replay, and runs isolated native and bwsim processes with time limits.
  `training/compare_replay.mjs` checks every requested checkpoint, including frame
  zero and the exact final frame. Missing checkpoints, runtime errors and state
  differences produce a quarantine record and a nonzero batch exit status.

Comparison includes all twelve players' bank and race-specific supply, and each
HUD unit's index, type, owner, completion/hidden flags, position, HP, shields,
energy, remaining construction time and production queue. The reference HUD
omits turret subunits and Scanner Sweep; those exclusions are explicit in reports.
The `hidden` flag is a simulation property, **not** a player's fog-of-war view.

Each run saves incremental `results.jsonl`, a final `summary.json`, and `config.json`
with terrain, replay-tool, reference-runtime and base-MPQ identities. Original
replays are unchanged. Failed games cannot be treated as successes simply because
the engine reaches the final frame. Incomplete batches lack `complete: true` in
their summary; inspect incremental records instead of treating them as finished.

## Local setup

Use Go 1.26, CMake/MSVC x64, Python 3.11+, Node 24.5+, and locally installed game
MPQs. The reference runtime setup is documented in
[extraction status](replay-extraction-status.md#reproduce-the-capability-probe).
The following commands run from the repository root; existing pinned checkouts
can be reused. Keep all game assets and generated data under ignored build output.

```powershell
git clone https://github.com/alexpineda/openbw.git build/replay-openbw-remastered
git -C build/replay-openbw-remastered checkout 86fc7f6da3c7a01542471f2ea303eb4bde0f9f04
go build -C tools/replay_decoder -o ../../build/replay-native/replay_decode.exe .
cmake -S tools/replay_native -B build/replay-native -A x64 "-DOPENBW_SOURCE_DIR=$((Resolve-Path build/replay-openbw-remastered).Path)"
cmake --build build/replay-native --config Release --parallel 2
python -m training.replay_assets --pack build/headless-bwsim/sim.pack.gz --output build/replay-modern-assets
```

The asset setup requires a new output directory. Subsequent checks reuse and
verify the prepared directory. Each comparison also requires a new report directory.
Use your Node 24.5+ executable explicitly if the default `node` is older:

```powershell
python -m training.playback_check --node path/to/node.exe --backend build/headless-bwsim --native build/replay-native/Release/replay_checkpoints.exe --decoder build/replay-native/replay_decode.exe --mpq build/match-runtime-a --assets build/replay-modern-assets --output artifacts/replay-learning/playback-new-run --interval 24 --replay-list training/playback_regressions.json
```

A replay list is a JSON array of local paths. The checked-in regression list names
downloaded examples; it contains no game/replay assets. You may also pass paths as
positional arguments. The map-sample run uses an interval of 240 frames; the original
failure regressions use 24. Use `--interval 1` to narrow a divergence to a frame.

Asset-free tests:

```powershell
go test -C tools/replay_decoder ./...
python tests/test_replay_playback.py
node --test tests/test_replay_comparison.mjs
```

## Increasing throughput without changing validation

`tools.validate_parallel` resumes an existing, pinned full-corpus audit with a
different worker count. It calls the unchanged `training.playback_check.check_one`
for every pending replay. Decoder, engines, assets, checkpoint interval, compared
fields and timeouts still come from the original verified identity. Completed
SQLite rows, including quarantines, remain intact; altered replay identities or
unexpected corpus entries fail closed. New scheduling provenance is saved in a
separate `parallel-execution-<pid>.json`, leaving the original identity and the
running training pipeline's pins unchanged.

Benchmark against the same previously checked sample, including a known divergent
game, before changing concurrency:

```powershell
python -u -m tools.benchmark_validation --config build/replay-native/full-validation-config.json --reference artifacts/replay-learning/playback-fixes-20260920.json --output artifacts/replay-learning/parallel-benchmark-new-run --workers 4 8
```

The benchmark requires identical pass/fail results, both checkpoint hashes,
checkpoint counts, first differences and compared fields at every worker count.
It records wall time and throughput in `report.json`; background workloads and
the mix of replay durations affect the estimate.

After stopping only the old validator (and any of its in-flight child processes),
back up the committed SQLite database and start the replacement:

```powershell
python -u -m tools.validate_parallel --config build/replay-native/full-validation-config.json --workers 8
```

Use `Start-Process -WindowStyle Hidden` with separate logs for background operation.
Keep the existing extraction/training process running. Never run two validators
against the same output directory. At most eight replays run concurrently in
this example; timed-out checks are retried once after the parallel workers drain,
using the same timeout and validation checks. This prevents congestion alone from
prematurely quarantining a game. Changed inputs are still rejected before the
final completed summary is published. No check is skipped or sampled more sparsely.

## Limits of the gate

Local validation on 20 September 2026: all three original cases matched 1,780
checkpoints at 24-frame intervals. A broader 27-game sample covered all nine map
names and all three Protoss matchups, matching 1,652 checkpoints at 240-frame
intervals after the three version-64 cases were rerun. Final decoder output hashes
were checked against the successful runs for every sampled replay. Evidence is in
`artifacts/replay-learning/playback-fixes-20260920.json`; the detailed
[readiness report](replay-extraction-status.md) distinguishes historical failures
and current results. All 15 CTest suites and the Go conversion tests pass.

Matching bwsim checkpoints is stronger than loading, deterministic stepping or
matching the final bank, but it is not an authoritative StarCraft fidelity audit.
Both engines may share simulation bugs. Only the reported fields and checkpoints
are compared; turret internals, Scanner Sweep and fog/detection are not certified.
Unsupported tilesets (Ashworld/Installation), limits, malformed maps and oversized
legacy string tables fail closed. New formats/assets need an explicit adapter
change and regression checks. Other modern replay extensions are not applied by
the native adapter; a gameplay effect must be caught by comparison or a separate
authoritative audit before treating a model as suitable for promotion.

Reports deliberately keep `training_ready: false` and
`authoritative_game_validated: false`. This tool does not feed spectator snapshots
to `training.prepare`. The separate v2 extractor now implements legal observations
and accepted-action labels and requires exact checkpoint-hash parity for every
game. Full-corpus validation and extraction feed an automatic experimental CUDA
run; authoritative game checks remain outstanding. See the
[run status and launch gates](replay-extraction-status.md) for that distinction.
