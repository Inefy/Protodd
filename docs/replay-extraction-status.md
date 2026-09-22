# Replay extraction and training run

## Current workflow — 22 September 2026

**The first full A0 GPU training run is complete.** It ran all 20 epochs / 34,620
updates and passed full FP32 validation and compiled C++ export checks. It launched
22 September at 09:34:57 UTC (07:04:57 Newfoundland time) and finished at
09:38:48 UTC (07:08:48 local), about 3 minutes 51 seconds including preparation
and export. Worker PID 52108 has exited normally; training is no longer running.

The best checkpoint is epoch 20. Matchup/game-balanced validation cross-entropy
fell from the masked frequency control's **0.731180 to 0.534551**, a **26.9%**
reduction. The final binary's SHA-256 is
`25996fb91f5007aec8194825e7feab77db41bdd492169a05745ccd93970effcf`.
C++ parity passed on all 424 selected validation cases, with maximum logit error
`5.73e-5`. Final-test examples remain unopened by training/evaluation.

This remains a reference model, not a deployment candidate. Overall validation
top-1 is 84.54%, versus 84.15% for the frequency control; 84.15% of validation rows
are `wait`. On the 142,997 non-wait rows, top-1 is only **4.94%**, although top-3 is
72.24%. Next experiments must address event timing and wait/action imbalance and
report non-wait performance, rather than celebrating aggregate accuracy or just
increasing width. The existing intent executor work is still necessary.

The trained Win32 model's local synthetic CPU benchmark measured 5.49 ms median,
5.81 ms p99 and 5.95 ms maximum over 256 predictions. It excludes feature encoding
and the rest of the bot and does not validate tournament-host latency. Preserve
CPU optimization and whole-frame budget checks before deployment; see the run's
`cpu_benchmark.json`.

| Stage | Final data counts |
|---|---|
| Playback audit | 30,019 processed; 29,136 matched; 883 quarantined; zero pending |
| Qualified extraction | 18,679 usable games; 367 quarantined; zero unresolved |
| Training tensors | 7,086,438 rows from 11,183 games |
| Validation tensors | 902,351 rows from 1,380 games |
| Final test | Excluded from tensor preparation and training/evaluation |

The A0 run is `artifacts/replay-learning/protoss-a0-gpu-20260922/`:

- Configuration: `training/configs/protoss-a0-local.json`; normalized settings,
  dataset/code hashes and environment are saved in the run's `config.json`.
- Progress: `status.json`. Phases distinguish hash verification, baseline,
  GPU cache loading, optimizer training, validation, export and completion.
- Logs: `build/replay-extractor/protoss-a0-gpu-20260922.{stdout,stderr}.log`.
- Recovery: `latest.pt` every 250 updates; `best.pt` on improved validation;
  original training source under `source/`.
- Result: `LearnedMacro.bin`, history, full validation, frequency control,
  stratified Python/C++ export parity and final `manifest.json`.

The RTX 5070 Ti uses BF16 autocast and caches training inputs at that same
precision. The 100-update real-data cache/stream comparison produced identical
model bytes and validation metrics, with about 48x higher **training throughput**
(407,183 versus 8,481 examples/sec). This ratio excludes cache loading, validation
and export. Peak tensor allocation was 8,424 MiB; desktop and driver allocations
are additional. The pilot's exported model passed C++ parity on 424 stratified
validation cases. Evidence: `build/replay-extractor/cuda-cache-parity-benchmark-20260922.json`.
All 25 Win32 Release CTest suites pass, including eight new trainer tests.

The full experiment used 1,731 updates per sample-equivalent epoch and completed
all 20 epochs; patience four did not stop it early. Every epoch evaluated all
902,351 validation rows in FP32. Balanced sampling is with replacement.
Status ETA estimates training updates only; validation/checkpoint/export time
is additional, and early stopping can shorten the run.

For an interrupted copy of this experiment, resume using its frozen code. The
completed run does not need a restart:

```powershell
./build/model-venv/Scripts/python.exe -u artifacts/replay-learning/protoss-a0-gpu-20260922/source/resume.py
```

The old automatic launcher remains retired. Extraction-only status deliberately
retains `extraction_complete_training_held`; it is historical stage state, not
the current trainer status. The stopped game campaign remains stopped. Learned
command execution, recurrent models and playing-strength evaluation remain
separate work in the [AIIDE plan](aiide-2026-training-plan.md). This run is a
shadow-only reference model and does not yet establish a stronger playing bot.

The notes below preserve the original run history; their old automatic-launch
command is retired and must not be used to restart the current workflow.

## Historical snapshot — 20 September 2026

Full validation and extraction are running locally. **Human-data GPU optimization
has not started yet:** the background pipeline will start it automatically after
all 30,019 corpus replays have audit results and the dataset passes its gates.
The stopped StarCraft match campaign remains stopped. The first model is an
experimental, shadow-only Protoss macro policy; no playing-strength gain is claimed.

## Active jobs and outputs

- Validation: `tools.validate_parallel`, eight workers, PID 656 after the
  scheduling upgrade on 21 September UTC. It resumed all 8,035 completed results
  from the original four-worker process (PID 22288, now stopped).
  Config: `build/replay-native/full-validation-config.json`.
  Progress: `artifacts/replay-learning/full-validation-20260920/summary.json`.
  Per-replay results: `validation.sqlite` in the same directory.
  Current logs: `build/replay-native/parallel-validation.{stdout,stderr}.log`.
  Historical logs: `build/replay-native/full-validation.{stdout,stderr}.log`.
  Scheduling provenance: `parallel-execution-656.json` in the validation output.
- Extraction and automatic CUDA launch: `training.replay_pipeline`, PID 40220 at
  launch. Config: `build/replay-extractor/training-pipeline-config.json`.
  Output: `artifacts/replay-learning/protoss-training-v2-20260920/`.
  `status.json` reports phase, sample counts and failures; `cohort.json` freezes
  eligibility and splits; `identity.json` pins inputs. Logs are
  `build/replay-extractor/training-pipeline.{stdout,stderr}.log`.
- Once training starts, `training.stdout.log` contains epoch metrics and
  `training.stderr.log` contains errors. The best model and its manifests go
  under `model/`. `model/export-parity.json` checks it against the Win32 evaluator.

At startup the full audit estimated approximately 37 hours. The later eight-worker
upgrade improved throughput by 20.6% on a fixed 17-replay benchmark (113.812 seconds
with four workers versus 94.375 seconds with eight). Both runs reproduced every
reference result and checkpoint hash, including a known divergent game. Background
workloads and replay mix affect actual throughput; use the live summary for the
current estimate. Extraction, dataset preparation and CUDA training add
work. Keep the PC powered on and awake. A stopped validation/extraction job can
resume with its unchanged config and inputs; never run concurrent copies against
one output directory. Interrupted optimizer runs do not yet resume automatically.

```powershell
python -u -m tools.validate_parallel --config build/replay-native/full-validation-config.json --workers 8
python -u -m training.replay_pipeline --config build/replay-extractor/training-pipeline-config.json
```

These are resume commands, not instructions to start duplicates of the active jobs.
The original validation identity, comparison code, engines, interval, timeouts,
extracted samples and all 24 pinned training inputs remain unchanged. A backup of
all results before the switch is `before-parallel-20260921.sqlite`; the benchmark
report is `artifacts/replay-learning/parallel-benchmark-20260921/report.json`.
See [scheduling details](replay-playback.md#increasing-throughput-without-changing-validation).
On Windows, launch background jobs with `Start-Process -WindowStyle Hidden` and
separate stdout/stderr logs. Changed code/assets/tools require a new pinned run.

## Completed checks

- All 30,019 PvP/TvP/ZvP files passed command/metadata parsing using screp 1.13.4;
  exact byte duplicates: zero. The corpus uses modern 1.21+ format and 3,400-unit
  limits. Source quality is attributed to exact player slots; pro tags are not
  verified pro identity. Command parsing alone never grants training readiness.
- Fixed the native backend's classic-terrain mismatch using pinned Remastered
  CV5/VF4 files and explicit map header/string conversion. The three original
  regressions match 1,780 checkpoints at 24-frame intervals, and a version-64 map
  regression matches 382. All 27 sampled map-name/matchup cases across nine map
  names match 1,652 checkpoints at 240-frame intervals. See
  `artifacts/replay-learning/playback-fixes-20260920.json` and the
  [playback implementation](replay-playback.md).
- Built the native legal-observation extractor with the same C++ encoder used by
  the bot. It matched the validated playback hashes on all 27 cases, including
  19,595 legal rows and 2,959 non-wait labels. The audit checks every sample's
  timing, action mask and first accepted command. Native integration tests cover
  hidden-state perturbation, fog, cloak/detection, unaffordable commands, and
  repeated builds. Evidence:
  `artifacts/replay-learning/extractor-audit-v2-20260920.json`.
- Fixed an extractor-specific desync: action execution and simulation now reuse
  the same persistent engine function object and its unit-finder cache generation.
  Every extracted game must still reproduce the full validated checkpoint hash.
- A separate six-game extraction/import pilot passed, producing 3,655 qualified
  rows in train/validation/test partitions. Evidence:
  `build/replay-pipeline-pilot-v2/result.json`.
- All 18 x64 and 19 Win32 CTest suites pass, along with the Go decoder tests.
  The v2 1,710,126-parameter model completed synthetic CUDA training and Win32
  export parity on the RTX 5070 Ti. Evidence:
  `build/model-cuda-v2-validation-20260920/result.json` and `export-parity.json`.

## Dataset contract and launch gates

Schema `protodd-macro-v2` has 598 inputs, 46 actions and fingerprint
`6c97c484f101bb26`. Unreconstructed latency and attacked-worker features were
removed explicitly; they are not filled with guessed values. Enemy race comes
from legal observations rather than resolved replay metadata. Both adapters use
`MacroEnemyMemory` for visible/detected enemy observations, witnessed removal,
and last-seen memory. Privileged enemy bank, tech, positions and queues never
enter the feature vector.

Own economy, gathered resources/income, doubled supply, gas-worker state,
unit/production counts and research/upgrades come from native player state.
Observations precede actions every 24 frames. A label is the first accepted mapped
macro request within the next 24 frames in recorded order, otherwise wait.
Unaccepted/repeated commands are excluded. Partial final windows and labels whose
prerequisites were absent at observation time are discarded and counted.

The frozen candidate cohort contains **19,046 qualified ladder games**: 11,408
train, 1,420 validation and 6,218 test before playback/extraction exclusions. Each
contributing Protoss player needs its own source MMR claim of at least 2,000;
unknown opponents cannot inherit that quality. Games must be at least three
minutes. Semantic duplicate groups and both PvP perspectives stay together.
Chronological cutoffs, Radeon maps and stable player holdouts are fixed before
extraction. The trainer never reads final-test samples for optimization or selection.

GPU training requires completed corpus validation, at least 95% passing playback,
at least 95% usable qualified games, at least 100 games per split, unchanged
pinned inputs and no map/player holdout leakage. Individual failures are
quarantined; broad failures stop the job. Current audit failures are visible in
`validation.sqlite`, and extraction failures in each game's `result.json`.

CUDA uses a 1024/1024 network, batch size 512 and at most 20 epochs with early
stopping. Reports distinguish wait, macro actions, individual actions, matchups,
and game-balanced validation performance. An exported-model comparison to the
Win32 C++ evaluator runs before the pipeline reports completion. No model is
installed into a game runtime automatically.

## Remaining limits

Checkpoint agreement between the native and bwsim simulators is **not an
authoritative StarCraft fidelity audit**; shared bugs and between-checkpoint
mismatches remain possible. Neither the full-corpus comparison nor the legal-view
self-tests establish full parity with original-game playback. All data/model
manifests explicitly retain `authoritative_game_validated: false`. This run can
support experimental imitation training, not a claim of certified replay fidelity
or a competitive improvement. An authoritative replay/BWAPI trace comparison and
controlled playing-strength evaluation are still required before promotion.

The full corpus audit is still in progress, so the 27-game sample must not be
presented as evidence that every downloaded replay works. Divergent games are
excluded. Unsupported formats/tilesets fail closed. Archon merges, simultaneous
macro outputs, spatial placement, worker allocation, combat micro and recurrent
belief models are later work; this first policy only proposes macro intents.

## Reproduce the capability probe

Install the research runtime at the pinned revision under ignored build output:

```powershell
git clone https://github.com/Gooseheaded/headless-bwsim.git build/headless-bwsim
git -C build/headless-bwsim checkout d8a1015464afbab9fc1d3d7426fc7e8952ce5e85
npm.cmd --prefix build/headless-bwsim ci --ignore-scripts --no-audit --no-fund
npm.cmd --prefix build/headless-bwsim run build
node training/probe_replay_backend.mjs build/headless-bwsim artifacts/replay-learning/backend-probe.json path/to/replay.rep
```

Use Node **24.5 or newer** for the probe's Memory64 runtime. This PC's default Node
is 22, so the run used the bundled Codex Node 24.19 executable explicitly. The
probe refuses to overwrite its report. Keep third-party engine/game artifacts
local under ignored build output. No raw replay is uploaded by this workflow.
