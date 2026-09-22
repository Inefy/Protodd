# Protoss macro model

This implements the first trainable model and its deployment contract. It is a
configurable two-hidden-layer ReLU network, defaulting to 1024/1024 neurons:
**598 inputs, 46 macro intents, 1,710,126 parameters**. CPU inference lives in the
portable C++ core and compiles into the Win32 DLL. GPU training runs separately.

The implementation includes dataset validation, streaming training, validation
metrics, a masked frequency baseline, binary export, Python/C++ parity tests, and
opt-in shadow predictions. The native v2 extractor now produces legal player
observations and accepted macro labels from `.rep` files through the playback
adapter. Enemy-belief models, recurrent/spatial policies, and learned command
authority remain later work in the [replay roadmap](../docs/replay-learning-plan.md).

The [Remastered playback adapter and comparison gate](../docs/replay-playback.md)
now resolve the observed Backrooms/KnockOut terrain failures. They validate
spectator checkpoints. The extractor adds native visibility/detection filtering,
shared observed-enemy memory, pre-action features, and command acceptance checks.
The [current run](../docs/replay-extraction-status.md) has completed validation,
extraction and tensor preparation, and the first full-corpus A0 GPU run completed
all 20 epochs with compiled export parity. Non-wait recall remains too low for control.
Since 22 September, extraction and tensor preparation are separate stages;
the old automatic CUDA launcher is retired. See the
[AIIDE implementation plan](../docs/aiide-2026-training-plan.md). This is an
experimental baseline: agreement between simulators does not establish fidelity
to the original StarCraft game, and all outputs remain shadow-only.

## Local setup

Run commands from the repository root using Python 3.11. Keep the training
environment isolated under the ignored build directory:

```powershell
python -m venv build/model-venv
./build/model-venv/Scripts/python.exe -m pip install --upgrade pip
./build/model-venv/Scripts/python.exe -m pip install -r training/requirements.txt --index-url https://download.pytorch.org/whl/cu128
```

The pinned PyTorch 2.7.1/CUDA 12.8 combination was tested with this PC's RTX 5070 Ti.
For CPU-only machines use the official `/whl/cpu` index. These indexes and package
versions are documented in [PyTorch's installation archive](https://pytorch.org/get-started/previous-versions/).
The training environment is 64-bit; it is not bundled into the game DLL.

Build the portable tools and enable the PyTorch-dependent tests:

```powershell
cmake -S . -B build/model-core -G "Visual Studio 17 2022" -A x64 -DPROTODD_BUILD_BWAPI_MODULE=OFF "-DPython3_EXECUTABLE=$((Resolve-Path build/model-venv/Scripts/python.exe).Path)"
cmake --build build/model-core --config Release --parallel 4
ctest --test-dir build/model-core -C Release --output-on-failure
```

Other C++20 platforms can use their normal CMake generator. Without PyTorch,
the C++ and standard-library dataset tests still run; the training/parity suite
is registered only when the configured Python can import torch and numpy. CI
installs the CPU packages so that suite runs there too.

## One feature contract for replay extraction and live play

`ObservationEncoder::encode(GameState)` is the source of feature computation.
Use the same C++ encoder in the replay perspective adapter. `model_tool schema`
exports the schema; `schema_v2.json` is its checked-in snapshot, compared with the
compiled tool in tests. Feature names, ordering, scales, and intent names feed a
64-bit fingerprint checked by the trainer and C++ loader. Bump the schema version
when changing feature semantics or the intent/mask contract, even if names stay the same.
V2 has fingerprint `6c97c484f101bb26`. It removes latency and recently attacked
worker inputs because replay/live parity was not established for them; these are
not silently zero-filled. The old v1 schema is historical and its weights are incompatible.

Inputs include own bank, income, doubled supply, unit/queue counts, technology,
observed enemy types, and remembered enemy counts/ages. Static scaling and clipping
to [0,16] happen once in C++; Python consumes those encoded values without further
normalization. Global replay IDs, enemy resources/queues/technology, private unit
fields, and hidden positions are excluded. Known enemy race comes from the legal
adapter: v2 derives race from observed enemy units and ignores resolved replay-header race.

**The encoder expects a legal GameState.** It cannot detect that an extractor
fabricated an unseen enemy as remembered. Reconstruction of visibility, detection,
observed deaths, and memory must pass the extractor's perspective tests. Tests here
verify that changing private enemy fields and IDs does not change encoded features.
The native integration self-test also perturbs hidden enemy state and checks
fog, cloak/detection, and rejected/accepted/repeated commands.

Encode each player's observations chronologically every 24 frames. Reset the
encoder per player/game. Duplicate-frame calls return
the same vector; backward time resets its income history. First samples explicitly
mark missing history. This baseline uses observed enemy memory and short income
history, not a recurrent hidden state or a complete sequence of prior decisions.

The named actions cover one next macro intent: train a unit, build a structure,
expand with a Nexus, research, upgrade, or wait. Archon merges and location/producer
selection need later executors. `learnedIntentMask` checks structural prerequisites
and technology progress; it permits saving for a currently unaffordable intent.
It does not promise current producer availability, placement, or immediate command
legality. Generate training masks with that same function. Before eventual control,
the executor must check the actual command with BWAPI.

V2 predicts the first accepted mapped macro request in `[frame, frame + 24)`;
ties follow recorded command order. Full windows without one receive `wait`.
Repeated build orders are excluded; incomplete end windows and windows whose
label is unavailable in the pre-action structural mask are dropped and counted.
Build labels require a matching producer transition because the engine's build
wrapper can return success for a rejected request. Multi-intent decoding,
cancellation/persistence, worker/gas
allocation outputs, spatial targets, and richer history are future model versions.

## Prepared-data contract

### Audit raw downloads first

`training.replay_audit` performs the command/metadata stage on completed `.rep`
files. Install the pinned [screp parser](https://github.com/icza/screp/tree/v1.13.4)
locally, then run the Protoss audit (PvP, TvP, ZvP are the defaults):

```powershell
$env:GOBIN = Join-Path (Get-Location) 'build/replay-tools'
go install github.com/icza/screp/cmd/screp@v1.13.4
./build/model-venv/Scripts/python.exe -m training.replay_audit --root artifacts/cwal-dataset --parser build/replay-tools/screp.exe --output artifacts/replay-learning/audit-protoss --workers 4
```

Use `--limit-per-matchup 20` for a pilot or `--resume` with the same output to add
new downloads/retry failed files. Each invocation snapshots completed files;
it does not wait for the downloader or ingest `.part` files. Cached results use
file size and modification time; use a new output directory to re-audit unchanged
files against revised catalog claims. The parser executable/version and auditor
hash are recorded, and a changed auditor/parser refuses resume.

The output contains `config.json`, resumable `audit.sqlite`, and `summary.json`.
It records byte and map hashes, format/unit limits, player slots, command counts,
parser errors, exact-byte duplicates, catalog claims, and quarantine reasons.
Catalog names/races must identify exactly one slot before a claim is attached.
Opponent MMR is never inferred from the primary player's MMR, and source pro tags
remain unverified claims. The pipeline groups alternate recordings by map hash,
timestamp and player names/races before freezing splits.
Macro request counts include failed/repeated clicks; they are not accepted labels.

**An audit is not a training dataset.** All rows retain `training_ready: false`.
The trainer still needs the perspective-safe observations below. The
[run and evidence](../docs/replay-extraction-status.md) records the extraction
checks, current pipeline, and remaining fidelity limitations.

### Import validated observations

Keep raw replays and extracted artifacts under `artifacts/`. The training importer
expects a manifest and JSONL observations produced by a validated extractor. A
manifest looks like this (abbreviated; train and validation both need real samples):

```json
{
  "schema": "protodd-macro-v2",
  "fingerprint": "6c97c484f101bb26",
  "source": "human_replay",
  "extractor": "your-validated-extractor-version",
  "audit_id": "your-extraction-audit-id",
  "validation": {"playback": true, "perspective": true, "actions": true},
  "games": [{
    "game_id": "canonical-game-id",
    "duplicate_group": "canonical-duplicate-group",
    "replay_sha256": "64-lowercase-hex-characters",
    "split": "train",
    "races": ["P", "T"],
    "player_quality": ["verified_pro", "unknown"],
    "valid_through_frame": 24000
  }]
}
```

The booleans record completed audits, not an instruction to skip validation.
Each JSONL row has exactly these fields:

```json
{
  "game_id": "canonical-game-id",
  "perspective": 0,
  "frame": 4800,
  "action_frame": 4804,
  "features": ["598 numeric values from ObservationEncoder"],
  "allowed_actions": ["wait", "train_probe", "build_pylon", "train_dragoon"],
  "action": "train_dragoon",
  "confidence": 1.0
}
```

The examples use placeholders and are not runnable sample data. `frame` denotes
the pre-action observation; `action_frame` is the aligned request/acceptance label,
not the eventual unit completion. Avoid including action effects in the input.
Both must lie inside the validated prefix. Wait labels need a defined observation
window and must not be inferred from missing or corrupt commands.

Game IDs and replay hashes must be unique. Duplicate groups cannot cross splits;
perspectives inherit their game's split. Build chronological/player/map holdouts
upstream: this importer verifies supplied assignments and does not choose splits.
Unknown-quality perspectives are rejected even if their opponent is a pro.
Pro samples receive weight 1; qualified ladder samples weight 0.5; confidence
multiplies that weight. Sampling loss also divides by samples per game so long
games and both PvP perspectives do not automatically dominate the objective.

```powershell
./build/model-venv/Scripts/python.exe -m training.prepare --manifest artifacts/replay-learning/v2/manifest.json --samples artifacts/replay-learning/v2/samples.jsonl --output artifacts/replay-learning/v2/macro.sqlite
```

Preparation creates a new SQLite file atomically and refuses overwrites. It rejects
unknown actions, masked labels, nonfinite/out-of-range features, duplicate decisions,
bad timing, ambiguous perspective quality, and incompatible schemas. SQLite stores
float32 feature blobs with indexed split assignments. Training streams records
through a bounded 8,192-sample shuffle buffer rather than materializing the corpus
in RAM. It currently uses a single data-reader process.

Synthetic fixtures require `source: synthetic_test` and `--allow-synthetic`. That
source is preserved in all training artifacts. Bot evaluation traces are rejected.
These gates cannot establish that supplied hashes, labels, or audit claims are
honest; retain the extractor report and independently inspect golden replays.

## GPU training from frozen tensors

The full replay corpus uses `training.train_shards`. Run from the repository root:

```powershell
./build/model-venv/Scripts/python.exe -u -m training.train_shards --config training/configs/protoss-a0-local.json
```

That configuration names an existing experiment; do not start a duplicate. New
experiments require a new output directory. CUDA/BF16 is explicit, with no silent
CPU fallback. The A0 reference has 1024/1024 hidden widths, batch size 4,096, AdamW,
500 warmup steps followed by cosine decay to 10% of the initial learning rate,
gradient clipping, and up to 20 sample-equivalent epochs with patience four.

Sampling is uniform over matchup, game, qualified perspective, then row, with
replacement. An epoch is `ceil(training_rows / batch_size)` draws of a batch,
not a promise to visit every row exactly once. Confidence/quality weights remain
raw; inverse game length is not applied a second time. Every validation example
is evaluated each epoch in FP32. Selection uses the mean of each matchup's mean
per-game cross-entropy, so the largest matchup cannot dominate selection. Reports
retain sample/game/matchup metrics, per-action recall, wait/non-wait metrics,
Brier score, illegal unmasked proposals and a masked frequency control.

The local configuration keeps all training inputs on the GPU in BF16, the same
conversion already performed by autocast before the first linear layer. Original
FP32 shards are unchanged and remain the validation/export reference. Only
training rows are cached. This needs about 8.08 GiB for features and metadata;
the allocator must have a further 2 GiB free before enabling it. The real-data
100-update pilot produced **identical model bytes and full validation metrics**
with caching enabled and disabled. Training throughput rose from 8,481 to 407,183
examples/sec in that pilot, excluding startup, validation and export. Peak tensor
allocation was 8,424 MiB; desktop/driver memory is additional. Results vary with
workload and hardware. `data_cache: "stream"` uses bounded pinned-memory prefetch
on smaller GPUs; `auto` checks capacity. FP32 training always streams.

`status.json` records phase, committed step, checkpoint step, throughput, loader
wait, memory and a training-only ETA. It updates every 15 seconds and at phase
boundaries. `latest.pt` atomically saves model, optimizer, schedule position,
random states, consumed sampling cursor, partial-epoch totals and history every
250 updates and at epoch boundaries. `best.pt` records the best validated model.
Prefetched samples never advance the consumed cursor. Duplicate writers are
blocked by an OS lock. Resume rejects changed data, settings, source, runtime
versions or parity tool. Only load trusted local checkpoints.

After an interruption, use the preserved source snapshot and original settings:

```powershell
./build/model-venv/Scripts/python.exe -u artifacts/replay-learning/protoss-a0-gpu-20260922/source/resume.py
```

While repository source and configuration remain identical, the original command
also accepts `--resume`. Do not resume while the process in `status.json` is alive.
CPU and CUDA/BF16 interrupted runs are tested against uninterrupted runs, including
epoch-boundary interruptions and recovery of a deleted export using frozen source.

Completion creates `LearnedMacro.bin`, `history.json`, `frequency_baseline.json`,
`export_parity.json` and `manifest.json`. The exact exported FP32 model receives
full validation and C++ comparison stratified by matchup, action and game phase.
Final-test labels are never read. All outputs remain shadow-only: this A0 experiment
establishes a comparison point for A1/temporal candidates, not playing strength.

## Legacy SQLite training, model sizes, and evaluation

The earlier trainer below remains for small fixtures. The full corpus uses the
resumable tensor trainer above.

```powershell
./build/model-venv/Scripts/python.exe -m training.train --dataset artifacts/replay-learning/v2/macro.sqlite --output artifacts/models/macro-v2-run01 --hidden 1024 1024 --epochs 20 --device cuda
```

Use `--hidden 256 256`, `512 512`, or larger widths to compare capacity. Both widths
must be 1-2048 and total parameters at most eight million. These are deployment
bounds, not claims about optimal playing strength. `--device auto` selects CUDA
when available; an explicitly requested unavailable CUDA device fails clearly.

Training uses masked cross-entropy, AdamW, gradient clipping, deterministic seeds,
and validation early stopping. The best checkpoint is selected by average per-game
validation cross-entropy. Reports include natural sample-weighted and per-game
metrics, matchup slices, separate wait/macro/per-action metrics, top-1/top-3 agreement,
Brier score, and the unmasked illegal
proposal rate. Small candidate sets can inflate top-k accuracy; read it with loss
and action masks. A training-only masked frequency baseline provides context.
Reproducibility across different PyTorch versions or CPU/GPU hardware is not promised.

Each new run directory contains:

- `LearnedMacro.bin`: best validation checkpoint in the bounded C++ binary format.
- `config.json`: architecture, seeds, device, library versions, schemas, source
  hashes/commit/dirty status, and input dataset hashes.
- `history.json`: epoch losses, validation metrics, and elapsed time.
- `manifest.json`: model SHA-256, selected-model and baseline validation metrics,
  synthetic-data flag, and explicit unvalidated-strength/shadow-only status.

An interrupted run may contain a checkpoint but has no final manifest. Treat it
as incomplete. The trainer does not resume optimizer state. Training never reads final-test
samples. After selecting a frozen candidate, evaluate the test split explicitly:

```powershell
./build/model-venv/Scripts/python.exe -m training.evaluate --dataset artifacts/replay-learning/v2/macro.sqlite --model artifacts/models/macro-v2-run01/LearnedMacro.bin --split test
./build/model-core/Release/model_tool.exe benchmark artifacts/models/macro-v2-run01/LearnedMacro.bin
```

The benchmark warms up then measures 256 CPU predictions using synthetic features.
It excludes feature extraction, logging, and other game managers. It does not
replace a whole-frame latency or playing-strength evaluation.

## Shadow use in the bot

Build the DLL through the existing tournament build workflow. For a future,
explicitly started shadow experiment, place the chosen binary at
`bwapi-data/read/LearnedMacro.bin` in that isolated runtime and put `shadow` in
`bwapi-data/read/LearnedMacro-mode.txt`. Preserve the run manifest and hashes with
the frozen bot package. Missing/empty mode or `off` disables this feature.

`MODEL` records report load/failure status and schema. `MODEL_SHADOW` records report
frame, named intent, masked probability, candidate mask, and elapsed microseconds.
Predictions run at most once per 24 frames. The existing controller retains
authority; there is deliberately no learned-control mode yet. A corrupt model,
invalid observation, or shadow update exceeding 10 ms disables inference for that
game. The probability is an uncalibrated model score, not guaranteed confidence.

Legal enemy memory now updates every frame, including brief sightings between
feature samples. Feature encoding stays on 24-frame boundaries; shadow inference
runs seven frames later to avoid competing with strategy/diagnostic work. Reduced
or emergency runtime load suppresses that inference before it executes, without
skipping observation history or replaying a stale sample later. Logs include the
observation frame and inference frame. This delayed schedule is currently for
shadow evaluation; learned command timing still needs an execution contract.

## Separate validation, extraction, preparation, and training

`training.validate_corpus --config <json>` runs four bounded playback workers,
persists each result in `validation.sqlite`, and reports progress/ETA in
`summary.json`. The legacy `training.replay_pipeline` entry point now exits without
launching anything. `training.data_release` resumes verified v2 extraction and
waits for the complete corpus audit, then publishes a frozen data release. It has
no training command or automatic GPU phase. Both its destination and shared legacy
extraction directory are protected by process-lifetime locks.

For the existing run, original source files were archived under
`artifacts/replay-learning/source-snapshots/protoss-v2-20260920`. Their original
hashes are checked against the old identity. Only changes to the retired CLI body
are allowed in the extraction module; helper/import changes fail. Trainer-only
files are retained as historical provenance instead of dependencies of extraction.
Decoder/extractor/evidence/schema/cohort/assets and validation pins remain enforced.

```powershell
./build/model-venv/Scripts/python.exe -u -m training.data_release --config build/replay-extractor/training-pipeline-config.json --output artifacts/replay-learning/protoss-data-release-20260922 --legacy-sources artifacts/replay-learning/source-snapshots/protoss-v2-20260920 --follow
```

This is a restart command, not an instruction to create a second active worker.
The stage checks both compressed payload hashes, checkpoint identity and frozen
cohort membership before reusing a game. It publishes `manifest.json`,
`sources.json`, and finally `release.json`; `status.json` reports progress every
15 seconds. The final status is `extraction_complete_training_held`.

Analyze training data without opening final-test result or target files:

```powershell
./build/model-venv/Scripts/python.exe -m training.analyze_dataset --input artifacts/replay-learning/protoss-training-v2-20260920 --output artifacts/replay-learning/NEW-audit --mode labels --max-games 100
```

Default mode is metadata and the default split is train. Validation requires
`--include-validation`; final-test label analysis is unavailable. A bounded pilot
is explicitly partial, not a corpus estimate. Reports include action retention,
same-window losses, phase/matchup counts, unavailable evidence and integrity errors.

Prepare immutable float32 tensors once a release exists:

```powershell
./build/model-venv/Scripts/python.exe -m training.pack_dataset --manifest artifacts/replay-learning/protoss-data-release-20260922/manifest.json --games-root artifacts/replay-learning/protoss-training-v2-20260920/games --output artifacts/replay-learning/protoss-tensors-20260922 --wait-for-release
```

Human-data packing requires the frozen `release.json` and `sources.json` hashes.
It packs train and validation only, never opening final-test result/sample files.
Typed memory-mapped shards preserve game/perspective boundaries, actual frame
gaps, masks, targets and raw confidence/quality weights. `training.shards` provides
a reproducible matchup/game/perspective sequence sampler; speculative reads do
not advance its consumed resume cursor. Progress is `<output>.status.json`.
Incomplete `.partial` outputs cannot be used for training or silently overwritten.
The tensor trainer above now consumes these shards directly.

The completed audit used eight workers through `tools.validate_parallel`, which
resumes the original database and unchanged validation checks. Its fixed-replay
benchmark improved throughput by 20.6% with identical checkpoint hashes and
pass/fail results. All previously completed results and the running extraction
pipeline were preserved; see the [scheduling workflow](../docs/replay-playback.md#increasing-throughput-without-changing-validation).

The cohort requires an exact-player source MMR claim of at least 2,000, a parsed
1v1 game, and a duration of at least three minutes. Pro tags remain unverified.
Splits hold out the latest 10% by time, Radeon maps, and a stable 5% of player
identities; the preceding 10% by time is validation. Both PvP perspectives always
share a split. Games with unknown-quality Protoss perspectives cannot contribute
those perspectives. The frozen candidate cohort has 19,046 games: 11,408 train,
1,420 validation, and 6,218 test, before playback/extraction exclusions.

Training requires every corpus replay to have a terminal audit result, at least
95% matching playback, at least 95% usable qualified games, at least 100 games per
split, unchanged pinned inputs, and no held-out player/map in training. Each
extracted game must independently match its validated checkpoint hash. Failures
are quarantined. The pipeline stops if a gate fails and leaves the reason in
`status.json`; it never lowers the gates automatically.

The old queued CUDA configuration (1024/1024, batch 512, 20 epochs) remains retired.
The user explicitly authorized the new A0 tensor experiment on 22 September.
Extraction/preparation never launch training, final-test evaluation, deployment,
or a game campaign themselves.

## Validation on 20 September 2026

- V2 default-width synthetic CUDA training/export and Win32 C++ parity passed on
  the RTX 5070 Ti; this smoke test is not human-replay training.
- Small synthetic training learned the fixture; Python and Win32 C++ logits,
  masked probabilities, and selected actions matched within numerical tolerance.
- All 19 Win32 Release and 18 x64 CTest suites passed, including schema, data isolation,
  malformed model, feature privacy, inference, and training tests.
- The historical v1 Release/Win32 benchmark of the 1,712,174-parameter model measured
  0.657 ms median, 0.8252 ms p99, and 0.9357 ms maximum over 256 measured calls on
  this PC. This used untrained synthetic weights/features, not match observations.

The extractor matched all 27 reference replays and audited 19,595 observations
with 2,959 accepted macro labels. A separate six-game real-data extraction/import
pilot produced 3,655 valid rows. Full-corpus validation and extraction are running;
human-data CUDA training was queued behind their gates at that historical snapshot.
The 22 September staged workflow above supersedes that automatic launch. No model has been installed
into an active game runtime and no StarCraft test campaign was restarted.
