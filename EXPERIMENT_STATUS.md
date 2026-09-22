# Experimental three-race learning work

## Priority update — 22 September 2026

Implementing the [AIIDE 2026 plan](docs/aiide-2026-training-plan.md). The automatic
GPU launcher is retired. A new extraction-only worker preserves/rechecks the old
artifacts and publishes a frozen release; tensor preparation and dataset analysis
are separate stages with final-test isolation. The live observation-memory
cadence now matches replay sampling, and shadow inference can be skipped before
exceeding the frame budget. Ladder reports separate strategic outcomes from
operational tournament points and block promotion on candidate failures.

Validation, extraction and tensor preparation are complete. The user explicitly
authorized the A0 full-corpus GPU run, launched 22 September at 09:34:57 UTC using
`training/configs/protoss-a0-local.json` and completed at 09:38:48 UTC. All 20 epochs
/ 34,620 updates finished. Final status is
`artifacts/replay-learning/protoss-a0-gpu-20260922/status.json`. It trained on
7,086,438 rows and evaluated all 902,351 validation rows per epoch. No trainer is
still running for this experiment.

The new trainer adds exact resume, frozen-source recovery, atomic checkpoints,
balanced draws, CUDA BF16, full FP32 validation and C++ export checks. GPU caching
produced identical model bytes and validation to streaming in a 100-update
real-data pilot, with 407,183 versus 8,481 examples/sec during training. The
throughput comparison excludes startup, validation and export. All 25 Win32
Release CTest suites pass, including the new cache/resume/isolation tests.

Best epoch 20 reduced matchup/game-balanced validation CE from 0.731180 to
0.534551 (26.9%). The final model passes C++ parity on 424 validation inputs.
However, non-wait top-1 is only 4.94%; aggregate accuracy is dominated by waiting.
Treat this as the A0 comparison reference and prioritize event timing / wait-action
imbalance before learned control. Local synthetic Win32 inference p99 is 5.81 ms;
whole-frame timing on tournament-class hardware remains unverified.

Learned command authority, recurrent models and match strength evaluation remain
outstanding. The model is shadow-only and the game campaign remains stopped.
Use the [current run status](docs/replay-extraction-status.md), not the historical
auto-launch notes below, for live paths and restart commands.

## Prior work and preserved evidence

The user stopped the test campaign on 19 September 2026. Scheduled follow-up is paused. No games should restart without a new request.

The current priority is the [replay learning plan](docs/replay-learning-plan.md),
starting with Protoss on this PC before Terran and Zerg. The user authorized
validation followed by local GPU training. On 20 September, the macro-v2 native
extractor, legal observation memory, accepted-command labels, frozen holdouts and
an automatic validation-to-CUDA pipeline were implemented. The model has 598
inputs, 46 macro intents and 1,710,126 parameters; it remains shadow-only.

The 30,019-file command audit is complete. Native/reference playback agrees on
27 sampled games across nine map names and all Protoss matchups. Legal extraction
also matches those checkpoints and has passed fog/detection, hidden-state,
command-acceptance and causal-label checks. A six-game real-data import pilot and
a v2 CUDA/export smoke test passed. All 18 x64 and 19 Win32 CTest suites pass.

**Full-corpus playback validation and extraction are now running in background.**
The validator was upgraded from four to eight workers at 8,035 completed replays,
preserving every result and all training input pins. A fixed 17-game benchmark
showed 20.6% higher throughput with identical checkpoint hashes and pass/fail
results; the new scheduling runner is `tools.validate_parallel` (PID 656 at launch).
Human-data GPU optimization has not started at this snapshot; the pipeline will
launch it automatically once the complete corpus audit and dataset gates pass.
The candidate qualified ladder cohort contains 19,046 games with frozen
chronological/player/map holdouts. Divergent or invalid replays are quarantined;
large failure rates stop the launch. See the [run status, logs and evidence](docs/replay-extraction-status.md)
and [model workflow](training/README.md).

Validation compares simulators, not authoritative StarCraft playback. Shared
simulation errors remain possible and all manifests retain that limitation.
Training completion will not establish a playing-strength improvement. No model
has been deployed and the stopped StarCraft campaign has not restarted.

This change adds basic Terran and Zerg controllers, shared tabular policy learning, reviewed-outcome training tools, frozen evaluation support, diagnostics, and regression tests. It also includes Protoss detection and conservative combat-estimation fixes, plus proxy-launcher validation.

These are experimental bots, not validated competitive improvements. The Terran policy pilot scored 0/2 for each of the empty, Q-trained and episode-return policies. The detector comparison scored 0/4 for both control and candidate. Opponent openings varied despite matched packages and requested seeds. Zerg production became functional in both immediate counting-fix tests, but both games were losses.

Earlier Terran supply planning is the latest unvalidated change. Its game campaign was stopped at the user's request; partial games must not be counted as results. All ten Release CTest suites passed before that campaign began. Local original logs, replays, frozen binaries and partial-run archives remain under the ignored build/strength-20260918 directory. Game assets, third-party binaries and machine-specific tournament files are not included in Git.

Do not train from frozen evaluation traces, count unpaired reports, or treat opponent crashes/runtime-limit outcomes as strategic wins. Thirty historical proxy wins remain suspect and are not strength evidence.
