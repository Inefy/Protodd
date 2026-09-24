# AIIDE 2026: training for the strongest deployable Protoss bot

## Active revision — 24 September 2026

Follow the [strength-first plan](strength-first-plan.md). The user asked for a
reassessment and adjustments: prioritize demonstrated playing strength, coherent
learned decision scopes and actual group execution. The six-slot replay packet
model is an offline baseline, not the mandatory next tournament controller.
The frozen reference campaign has started; loss diagnosis and the action/sequence
contract precede another large fit. Full-game learning and all promotion gates
remain goals. The earlier priorities and dated implementation status below are
historical and are superseded where they conflict with this revision.

## Original plan and implementation history

Plan prepared 22 September 2026 after auditing the current data, training, runtime,
and evaluation code. This is the implementation priority for the replay-learning
work. It refines [the original roadmap](replay-learning-plan.md) for AIIDE 2026.
Changes below are proposed unless explicitly marked completed.

Current priority supersedes the earlier A0/A2-first delivery order below:
make the GPU-trained whole-game Protoss policy the tournament candidate. The
existing bot remains an executable comparison and command fallback while the
learned controller is brought through legality, latency and paired strength
gates. Spend the remaining development time on action quality, whole-game
coverage and a CPU-deployable student instead of extending the shallow macro
policy. If the comprehensive controller misses a gate, record the evidence and
select the strongest validated build before the submission deadline.

### Direction update — whole-game GPU policy as primary controller

The requested tournament candidate is now the replay-trained whole-game policy,
with the existing deterministic controllers providing legality, placement,
pathing and bounded fallbacks. Train the comprehensive teacher on GPU; compile
a numerically checked CPU evaluator or distilled student into the submitted
Windows/BWAPI bot. The published AIIDE machines have no competition GPU, so
GPU inference cannot be a tournament dependency. The learned policy must own
explicit unit/action scopes; the scripted bot cannot silently overwrite its
orders. Promotion still requires paired strength campaigns and frame-time tests.

The v3.2 full-prefix release is extracting 12,563 frozen Protoss train and
validation games. `training.whole_game_fit` supports BF16 minibatches,
high-MMR/player-diverse sampling, separate verified held-out releases, and
source-hashed reports. The 24-game-per-matchup fit and the 64-game-per-matchup
width-512 fit are completed development experiments, not tournament model
selection: the latter predicted only 3/72 held-out action kinds correctly in
its bounded report. A longer action-weighted continuation is running. The
deterministic export and Win32 C++ evaluator have passed
replay-observation numerical parity. We can embed exact model bytes in the DLL;
the resource probe verifies both bytes and inference. A paired local shadow
campaign passed after fixing a terminal zero-unit case, with worst complete
callbacks below 31 ms on this PC. Learned command ownership and strength
validation remain the main blockers to making the GPU-trained policy the
tournament controller. A typed intent decoder and BWAPI legal-command adapter
now run in shadow mode. The first paired intent campaign exposed incompatible
action/target combinations in the smoke student and one 56.98 ms callback. The
adapter does not issue learned commands. A staggered-inference build is being
measured separately under the same live CPU load.

### Implementation update — 22 September 2026

The training foundation is implemented and passed all 25 Win32 Release CTest suites:

- **T01 complete, data release frozen:** retired the old auto-training
  entry point; verified archived source pins; resumed extraction-only work using
  `training.data_release`. Original payloads, cohort, schema and validation are
  preserved. All 30,019 replay audits are terminal and 18,679 qualified games are usable.
- **T02 implemented:** bounded metadata/label audit with final-test isolation.
  A three-training-game pilot verified 985 accepted commands versus 529 retained
  non-wait labels; this is a partial pilot, not an estimate for the whole corpus.
- **T03 implemented:** every-frame legal memory, unchanged feature cadence,
  staggered shadow inference and preemptive runtime-load checks. The exact Win32
  DLL builds in `build/runtime-parity-audit`; no learned command control is enabled.
- **T04 complete, packing finished:** frozen-release-bound float32 tensor shards
  and deterministic game/perspective sequence sampling with consumption-based
  resume state. There are 7,086,438 training and 902,351 validation rows. Final-test
  samples remain unopened by packing/training.
- **T05 implemented and verified; A0 complete:** explicit configuration, CUDA BF16,
  exact optimizer/sampler/RNG resume, frozen-source recovery, atomic checkpoints,
  phase/throughput/memory status, full FP32 validation and stratified C++ export.
  GPU input caching produced identical real-data model bytes and validation to
  streaming, with roughly 48x faster update throughput in a 100-step pilot
  (startup/validation/export excluded). The authorized full local A0 run completed
  all 20 epochs on 22 September; [results and recovery](replay-extraction-status.md).
  Balanced validation CE improved 0.73118 -> 0.53455; all 424 C++ export cases pass.
  Non-wait top-1 is only 4.94% despite 84.54% aggregate top-1. Prioritize event
  timing / wait-action imbalance before selecting larger models for control.
- **T08 partially implemented:** separate strategic/operational outcomes and
  enforced candidate failure blockers. Paired inference, complete frozen-opponent
  campaigns and non-inferiority promotion remain outstanding.

Next implementation work is T06 learned-intent execution, followed by T07
recurrence and controlled A0/A1/A2 comparisons.
These changes have not yet demonstrated increased playing strength. Original-game
fidelity checks, full label coverage analysis and exact tournament-hardware timing
also remain required. The first GPU experiment is complete. The later whole-game
goal authorized fresh game campaigns; current evidence and expanded priorities
are maintained in [whole-game training](robust-training-goal.md).

## 1. Decision and success criterion

Train on GPUs; submit a self-contained CPU bot. Optimize measured win percentage
against a diverse opponent pool while satisfying AIIDE's execution constraints.
Use replay imitation to initialize competent behavior, then correct weaknesses
through training-only game experience. Parameter count and prediction accuracy
are intermediate measurements, not the definition of success.

Protoss is the submission priority. Terran and Zerg follow after the Protoss
learning, execution, and evaluation pipeline proves useful. Do not dilute this
submission by trying to deliver three new race controllers at once.

Build two horizons:

1. **October 14 submission:** reliable learned macro/economy, useful memory,
   executable decisions, robust evaluation, and only the additional learned
   components that improve the frozen tournament candidate.
2. **Longer-term strongest agent:** entity/spatial observations, enemy beliefs,
   scouting and army control, specialized micro, and an opponent league for RL.
   This is a research program; it is not a credible promise for the next 22 days.

Keep a working rules-based candidate throughout. Replace its strategic choices
in explicit scopes when measured improvements justify doing so. Retain its useful
legality, placement, pathing, resource accounting, and command-execution machinery.

## 2. Verified tournament constraints

Checked against the official pages on 22 September 2026:

- Submission is due **October 14, 2026**. Registration closed September 14;
  this plan assumes registration is already complete.
- Official BWAPI, StarCraft 1.16.1, fog of war, and native Windows 10 are required.
- Submit full source and reproducible compilation instructions.
- Design for callbacks under 42 ms. Published losses occur at one frame over
  10 seconds, ten over one second, or 320 over 55 ms.
- The 2026 map pool is undisclosed. Random opponent aliases remain stable;
  reverse identification of opponents is prohibited.
- Evaluate the tournament's frame limit, fastest execution, and persistent-file
  transfers faithfully. [Official AIIDE rules](https://davechurchill.ca/starcraft/aiide/)

The published machines have i5-12500T CPUs, 8 GB RAM, Windows 10, and **no dedicated
GPU support**. Submission includes a compiled bot, source, and build instructions.
Our DLL and frozen weights must load without Python, CUDA, network access, or
machine-specific paths. Additional model files are part of the package; the
submission page's small-package example is not an explicit universal model-size
limit. [Official hardware and submission instructions](https://davechurchill.ca/starcraft/compete/)

Do not assume training on an H100 allows an H100-sized model to play at AIIDE.
The deployed model must earn its place on the actual CPU/time/memory budget.

## 3. Current state and findings

### Work preserved

Snapshot around 06:56 UTC, before holding the launcher:

| Item | Observed state |
|---|---|
| Playback audit | 30,000 / 30,019 processed; 19 pending; 29,123 matched; 877 quarantined |
| Qualified cohort | 19,046 games and 19,046 currently qualified perspectives |
| Extracted | 18,676 games; 364 quarantined; 6 still unresolved |
| Extracted rows | 11,910,667; includes train, validation, and test |
| Frozen split, before extraction exclusions | 11,408 train / 1,420 validation / 6,218 test games |
| Cohort matchups, before exclusions | 4,506 PvT / 4,704 PvZ / 9,836 PvP |
| Existing model | 598 inputs; 1024 x 1024 hidden layers; 46 outputs; 1,710,126 parameters |
| Local training stack | RTX 5070 Ti, 16,303 MiB; torch 2.7.1+cu128; CUDA available; BF16 reported supported |
| Free C: space during inspection | Approximately 485 GB decimal |

These are qualified ladder perspectives with an exact-player source MMR claim
of at least 2000. They are **not independently verified professional players**.
The cohort explicitly records that professional tags are unverified. Do not
advertise a pro-only dataset or count 11.9 million correlated rows as independent
games. The large final-test allocation comes from combined player, map, and time
holdouts; preserve it instead of moving it into training.

**Completed in this planning task:** stopped only the idle automatic extraction /
training launcher (PID 40220) so its old hardcoded job cannot begin during planning.
The validator (PID 656) remained running. Completed outputs were preserved;
remaining extraction is also held until an extraction-only continuation exists.
The hold is recorded in
`artifacts/replay-learning/protoss-training-v2-20260920/training-launch-hold.json`.
That file is an annotation, not an implemented software interlock. The process
stop is what prevents launch. Do not blindly restart the old auto-training command.

### Why a larger version of today's model is insufficient

| Finding in the repository | Required response |
|---|---|
| [ModelRuntime](../src/bwapi/ModelRuntime.cpp) supports only off/shadow and logs predictions. | Implement action ownership and execution; weights alone currently change no decisions. |
| [Extractor](../tools/replay_native/extract.cpp) keeps the first mapped accepted macro action in each 24-frame window. | Measure discarded simultaneous actions; introduce richer action/timing supervision where justified. |
| The 46 actions omit worker allocation, producer/location choice, merges, scouting, tactics and micro. | Add targets and matching executors in stages. |
| [ObservationEncoder](../src/core/ObservationEncoder.cpp) stores aggregates without unit/base geometry. | Existing rows support temporal macro experiments; spatial control needs new extraction. |
| Extractor memory updates every simulation frame; live [ProtoddModule](../src/bwapi/ProtoddModule.cpp) invokes model observation every 24 frames. | Repair missed brief sightings before deployment parity claims. |
| [Dataset loader](../training/dataset.py) reads SQLite rows, shuffles only 8,192 at a time and constructs batches in Python. | Prepack sequential tensor shards and use explicit sampling/prefetch. |
| Per-game weights are normalized inside game-heavy batches. | Define the sampling objective explicitly; inverse-length weights can cancel within a batch. |
| [Trainer](../training/train.py) lacks full optimizer resume, AMP and step-level telemetry. | Add reliable checkpoints and measured GPU optimization before long experiments. |
| [LearnedPolicy](../include/protodd/LearnedPolicy.hpp) only supports a bounded two-layer MLP; action masks are 64-bit. | Version the recurrent/multi-head export format and runtime; do not just enlarge Python. |
| [Ladder](../tools/ladder.py) includes crash-assigned outcomes in aggregate win rate. | Separate strategic improvement, tournament score and reliability; enforce promotion rules. |

Native checkpoint extraction and the comparison backend share OpenBW ancestry.
Their agreement is valuable, but it is not independent original-StarCraft proof.
The checked fields do not fully cover visibility/detection, research, spell state,
or command acceptance. Preserve the existing evidence and add targeted
authoritative-game comparisons before promoting learned control.

## 4. Mandatory work before the first substantial GPU run

Small correctness/profiling experiments follow these gates; full corpus sweeps
wait for them. Live strength evaluation is required for promotion, not before an
initial offline prototype can be trained in isolation.

### G0: finish and freeze data without auto-training

- Separate validation, extraction, tensor preparation, training, and export into
  explicit stages with independent immutable manifests and restart points.
- Resume the remaining extraction without launching training. The old pipeline
  hashes every `training/*.py` and checks them again before launch. Preserve its
  pinned source/binaries and results; a versioned continuation must verify and
  reuse completed outputs rather than edit pins or ignore hash failures.
- Reuse the completed replay/checkpoint audit when its inputs are unchanged.
  A changed extractor receives a new identity/output and its own parity tests.
- Implement a real launch gate in the new orchestrator, and an accurate status
  record for validation, extraction, preparation, fitting and evaluation.
- Publish a frozen data release with file hashes, exclusions, valid prefixes,
  qualified perspectives and split counts. Keep raw replay archives immutable.

Acceptance: resumable extraction-only mode, no implicit training subprocess,
identical reused hashes, every eligible game resolved, and no frozen-test access
by fitting/tuning jobs. Playback and extraction thresholds remain enforced.

### G1: determine what the dataset actually teaches

Produce one report, with bounded streaming passes rather than repeated full scans:

- Matchup, map family, player, source rating, date, game length, phase and outcome
  coverage; quarantine reasons and whether exclusions concentrate in a mechanic.
- Supported accepted commands versus retained targets; same-window action loss,
  repeated commands, dropped mask windows, wait rate and rare-tech support.
- Separate missing/unsupported supervision from actual no-macro-action windows.
  Do not infer deliberate saving simply because no mapped command appeared.
- Stratify errors and command coverage by PvT/PvZ/PvP. The current corpus is
  heavily PvP; set explicit matchup weighting rather than accepting row volume.
- Audit aliases/duplicate recordings and source identity uncertainty. Both
  perspectives and alternate recordings stay in one split. Do not repartition
  the frozen test based on outcomes or model performance.
- Obtain a separate short-game/rush dataset: current cohort excludes games under
  three minutes. Distinguish actual quick losses from disconnects/aborts.

Deliverables: `data-card.json`, `label-coverage.json`, split/quality/exclusion
tables, and a small set of reviewed replay examples for each label failure.
Use training examples for development review; keep final-test outcomes sealed.

### G2: fix replay/live contracts and make decisions executable

- Update legal enemy memory each frame or from reliable visibility events;
  schedule model inference separately. Test brief sightings between inference
  ticks, cloak/detection changes, unseen deaths, and re-observed empty locations.
- Add a persistent intent executor and a decision arbiter. Each scope has exactly
  one decision owner; suppress competing scripted goals when learning owns it.
- Track intent IDs, producer/target binding, reserved resources, issue/acceptance,
  actual start/completion, retry, timeout, cancellation and supersession.
- Structural availability permits saving for a Nexus or technology. Check actual
  affordability, placement and command legality again at execution time.
- Cancel superseded reservations and stale `MacroPlanner` pending goals. Avoid
  spending the same bank twice or issuing a new build every model tick.
- Retain auditable fallbacks for invalid models, stale decisions, unsupported
  actions and essential emergencies. Log overrides; do not hide a policy that
  appears safe only because scripted control silently does all its work.

Acceptance: meaningful integration scenarios for supply, worker production,
simultaneous producers, saving, cancellation, detection and recovery; full logs
show whether the proposed decision actually happened. No unintended commands
in shadow mode. No privileged state reaches deployed features.

### G3: establish trustworthy measurement

Freeze a current-bot package and an opponent/map matrix before comparing models.
Implement game provenance, strategic versus operational outcome reporting,
pair/cluster IDs, enforced runtime/crash blockers, and an untouched final pool.
The current headline win-rate report is insufficient for checkpoint promotion.

## 5. Immediate model experiments using existing v2 data

Avoid re-extracting the entire corpus just to test history. Existing rows include
game, perspective and frame identity; action audit files retain mapped command
events. Preserve real frame gaps, and join only actions strictly before the
current observation as inputs. Do not pass the target action or future commands
as previous-action history. During live play, feed back actually accepted actions,
including scripted emergency overrides, rather than the model's last argmax.

| Experiment | Architecture | Purpose / deployment |
|---|---|---|
| A0 | Existing 1.71M MLP with corrected training infrastructure | Reproducible baseline; existing CPU export |
| A1 | Wider compatible MLP, about 5.52M parameters | Isolate capacity gain; benchmark CPU before adopting |
| A2 | Scalar encoder + GRU, initially hidden size 256 then 512, with prior-action/time features | Test useful decision history; new C++ recurrent export/runtime |
| A3 | A stronger recurrent/temporal teacher, roughly 10–30M parameters as an initial search range | Test whether more capacity improves the same legal observation task; distill if useful |

These are experiment candidates, not a claim that a particular size is optimal.
For this tournament, implement a small set of well-tested C++ operators for A2
and benchmark it early. A general Python or GPU inference service is not a
submission dependency. Do not let a custom transformer engine consume the
remaining development schedule.

For recurrence, start with 32–64-step training sequences and explicit burn-in;
test longer history only after memory/throughput and validation justify it.
Reset at game/perspective boundaries. Use actual frame deltas, missing-window
masks and causal padding; recurrent state can persist beyond a training window
during live play. Never mix the two players' hidden states.

Use an explicit sampler: choose matchup, game, eligible perspective, then a
contiguous sequence. Define whether the objective is game-balanced or naturally
distributed; avoid applying inverse-game weights twice. Report both distributions
at evaluation. Rare-action sampling is an ablation with measured timing and
calibration effects, not a blanket deletion of wait examples.

Current action logs can support coverage analysis and some ordered bundle
relabeling. They do not contain all target coordinates, unmapped commands or
complete action lifecycles. Sequential masks must reflect actual availability
at each action; do not fabricate intermediate game states from coarse rows.
Keep A0/A2 on the exact original task for an architecture-only comparison before
combining improved labels and improved architecture.

Train with masked behavior-cloning loss, AdamW, gradient clipping, warmup and
decay; choose hyperparameters on validation. Include action timing/event loss
only when the labels and executor share its semantics. Never discard all losing
games or assume a winner's every decision was good. Mask uncertain terminal
outcomes rather than manufacturing value labels from replay-ending behavior.

Use a staged search: small representative train subsets to debug, bounded
equal-budget trials to shortlist, full-data validation for finalists, then at
least three seeds for finalists where time allows. Compare under equal update
and wall-clock budgets, and record the tradeoff. The full final test stays sealed.

## 6. GPU efficiency with measured quality preservation

The immediate bottleneck may be data delivery rather than the GPU. At about
12 million rows, 598 float32 features alone occupy roughly 28.7 GB decimal.
Do not load the entire dataset plus training state into this PC's 32 GB RAM.

1. Convert validated rows once into checksummed, typed, memory-mapped shards
   with sequence/game indexes, compact masks and metadata. Retain float32 source
   feature precision initially. SQLite remains the catalog, not the per-row hot
   training path. Verify shard outputs against the importer on representative
   and boundary fixtures.
2. Use a bounded loader with measured worker count, prefetch, pinned host buffers
   and asynchronous transfers. Avoid multiplying dataset copies under Windows
   process spawning. Cache training-only class counts and baseline statistics.
3. Benchmark BF16 autocast on the installed CUDA stack, preserving appropriate
   float32 parameters/reductions and finite checks. Keep a float32 reference.
   Evaluate logits/loss, short-run convergence and downstream validation before
   adoption. Mixed precision is not mathematically identical training.
4. Benchmark fused optimization and compilation only where supported; keep an
   eager fallback. PyTorch 2.7.1/Windows support must be checked against the actual
   installed versions rather than assumed from newest documentation.
5. Tune microbatch size and sequence length. Start MLP sweeps at 512/1024/2048;
   sequence batches need their own measurements. Accumulation changes effective
   batch/update cadence; adjust learning-rate experiments accordingly.
6. Aggregate metrics on device, reduce unnecessary per-batch synchronization,
   and vectorize validation. Preserve finite/error checks and auditable reference
   evaluation. Do not promise utilization improvements before profiling.
7. Save atomic resumable checkpoints: model, optimizer, scheduler/scaler, RNG,
   sampler cursor, epoch/step, best score, patience, and data/config hashes.
   Record consumed rather than merely prefetched sample position; either preserve
   in-flight loader state or checkpoint at a deterministic drained boundary.
   Test interrupted-versus-uninterrupted continuation.
8. Report every 15–30 seconds: phase, steps, examples/sec, data-wait time, GPU
   utilization, allocated/reserved peak VRAM, loss, gradient health, checkpoint
   time, and phase-specific ETA. This prevents another nearly silent long run.

Reserve roughly 3–4 GiB for desktop/driver/variance initially; benchmark a peak
training allocation around 11–12 GiB before raising it. This is an operating
target, not a model-capacity prediction. Optimizer state is only one VRAM cost:
activations, entity count, sequence length, attention, workspaces and batch size
can dominate. Use activation checkpointing only if its time/memory tradeoff helps.

Primary implementation references: [PyTorch performance tuning](https://docs.pytorch.org/tutorials/recipes/recipes/tuning_guide.html)
and [automatic mixed precision](https://docs.pytorch.org/tutorials/recipes/recipes/amp_recipe.html).
Every speed option must pass data-equivalence or numerical/convergence checks
appropriate to the change. Faster epochs alone do not justify reduced playing strength.

## 7. Richer replay extraction and what to train next

Create a v3 schema alongside v2. Start with a representative pilot across
matchups, maps, durations and difficult mechanics; prove perspective/action
parity before a bulk extraction. Keep observations and privileged targets in
separate streams and access paths.

| Priority | Learn | Additional evidence/data | Success measurement |
|---|---|---|---|
| 1 | Production, workers/gas, expansion and technology timing | Producer queues/progress, resource saturation, accepted action sequences, target base, execution lifecycle | Realized intents, supply blocks, idle producers, savings-aware bank, workers and expansion completion, then wins |
| 1 | Enemy beliefs and detection deadlines | Legal observation history; hidden tech/army/expansion truth as targets only; censored future horizons | Calibration, rare-threat recall, false alarms and useful warning time |
| 2 | Scouting and map information | Base/region graph, exploration/fog, seen enemy positions, scout destinations and returns | Information acquired, scout losses and downstream match performance |
| 2 | Army positioning, commitment and retreat | Legal unit positions/attributes, terrain, routes, threat memory and combat events | Objective success, losses after commits, counterattack exposure, match wins |
| 3 | Storm, Reaver/Shuttle, Dragoon and detector micro | Selected combat windows every 3–6 frames; legal targets, cooldown/energy fields and spell events | Targeted scenario success plus integration games and CPU cost |

Suggested longer-term model: scalar economy encoder + visible/remembered entity
encoder + base/terrain encoder + recurrent strategic state, with separate macro,
belief, scout and tactical heads. Preserve uncertainty and age for remembered
entities. Use player-local first-observed identifiers; replay-global allocation
IDs and unseen enemy fields must not become features. Cap/pool entities
deterministically with documented semantics and test crowded endgames.

Use ordered action bundles or producer-conditioned choices with a stop token;
support duration/commitment/cancel semantics. A meaningful action includes
producer/target and successful execution, not just its macro class.

Auxiliary losses can predict hidden tech probabilities/counts, threat arrival at
15/30/60 seconds, own production milestones and validated outcomes. Label masks
must respect truncated replay horizons. Train any privileged critic separately;
the actor, recurrent state and deployed preprocessing use legal observations only.

Do not extrapolate absent information: v2 cannot reconstruct unit geometry or
full combat state from counts. Dense full-corpus micro extraction is postponed
until a pilot measures storage and useful label yield. A larger GPU cannot repair
missing or biased labels.

## 8. Distillation and tournament runtime

The tournament model can be learned directly or distilled from a stronger
teacher. The teacher also consumes legal history; its action probabilities and
auxiliary predictions can supervise the student alongside real actions. Avoid
teaching impossible oracle behavior from privileged actor inputs.

Train and compare CPU candidates around 1–6M parameters first; this is a search
range, not a permanent strength cap. Preserve useful recurrence and information
before increasing width. Larger candidates remain eligible if they pass real
hardware timing and improve games. Distillation/quantization can lose strength;
retain float32 and teacher baselines and require equivalent deployment tests.

CPU targets, explicitly proposed rather than measured:

- Aim for added macro inference p99 below 2 ms on representative target-class
  hardware, with bounded memory allocation and no GPU/Python dependency.
- Measure feature encoding, memory maintenance, inference, execution and the
  entire callback together at p50/p95/p99/p99.9/max, including large armies.
- Stagger model inference from strategy/diagnostic spikes and check frame-budget
  state before running it. Continue existing intents or use bounded fallback when
  overloaded; a timing check after inference cannot prevent that frame's overrun.
- Treat any new >55 ms callbacks as a local promotion concern; retain the
  tournament's exact time-loss counters. No new crashes, illegal commands,
  nonfinite decisions, or unexplained recurrent-state contamination.
- Store frozen weights as packaged read-only assets or embedded resources, not
  only in tournament learning folders that may be empty or replaced. Keep path
  handling relative and source/build instructions reproducible.
- Test the exact Release/Win32 DLL, model hashes and configuration on clean
  Windows 10. Our faster desktop is not proof of i5-12500T performance.

Retain legal alias-based adaptation through cumulative per-alias files and the
tournament transfer cycle. Start from generic priors; do not infer an alias's
real bot identity. Evaluate fresh-state robustness and permitted within-tournament
adaptation separately. Do not add online full-network training to the submission.

## 9. Strength evaluation and promotion

Use the official round-robin score as the end goal, while diagnosing why it
changes. Log separately:

1. Normal strategic wins/losses and official score-limit outcomes.
2. Operational tournament score, including our crashes/timeouts as losses.
3. Failure incidence for both sides; opponent crashes are not evidence of better
   strategic decisions and cannot compensate for our new failures.

Freeze complete baseline/candidate and opponent packages, configurations,
map hashes, model versions, and both sides' persistent learning snapshots.
Record actual spawns and pair/cluster IDs. Requested seeds alone do not prove
deterministic execution. Use the same schedule and starting conditions where
feasible and stratify PvT/PvZ/PvP, map type and opponent strategy.

Build separate pools for training, development and final evaluation. Challenge
suites cover early rushes/proxies, cloak, tank lines, chokes, containment,
harassment, expansion defense, worker recovery, spell units and lifted/island
cleanup. Base threats on legal information and verified game mechanics.
Sample old tournament maps plus diverse held-out community maps; do not
special-case map names or assume the hidden AIIDE pool is known.

Run small legality/smoke batches first, then at least 100 games per matchup per
arm as coarse screening when throughput permits. This is not enough to resolve
small gains. Predeclare practical improvement/noninferiority margins and use a
power calculation plus pairing/cluster-aware intervals for promotion. Around
50% wins, detecting a five-percentage-point difference can require roughly
1,600 games per arm under a simple independent-game approximation; paired
designs and heterogeneous opponents change the requirement. Do not repeatedly
peek until significance appears. An inconclusive candidate remains experimental.

Select candidates using validation and development games. Freeze the selection
before final evaluation; never train on final evaluation traces. If the final
pool is subsequently used to guide development, retire it and establish another
untouched pool for the next claim.

Diagnose failure at each boundary: observation → belief → proposal → arbitration
→ command → realized result. Report action timing, non-wait performance, per-action
recall/calibration, realized-intent rate, overrides, supply blocks, savings-aware
bank, detection lead time and combat losses. Require credible win-rate evidence,
important-matchup protection and runtime correctness before promotion.

## 10. Beyond imitation: controlled games and reinforcement learning

After a competent executable policy exists, collect training-only games against
diverse frozen bots and prior checkpoints. Human demonstrations omit states our
bot will reach after its own mistakes. Use those states to identify data gaps,
targeted expert/teacher supervision, and scenario/RL training. Do not label a
new bot state with the next action from an unrelated human replay.

Start RL with bounded macro/tactical choices, recurrent PPO as a simple candidate,
and an imitation/KL anchor to preserve useful behavior. Verify that proposed
actions and actual executed actions align; log overrides and use a consistent
environment action interface. Keep terminal win/loss central. Any shaping must
be justified and tested for stalling, farming or material-trade exploits.

Maintain a league of older checkpoints, diverse scripted opponents and specialist
exploiters. Track a payoff matrix and prioritize weaknesses while retaining broad
coverage. Avoid self-play solely against the latest checkpoint. Bound actor-policy
lag and keep training data/version provenance if collection becomes asynchronous.

Offline RL on human data is a separate ablation only after validated transition,
reward and action contracts exist. Its estimates need real-game confirmation;
recorded outcomes do not identify the result of unchosen actions. Replay playback
cannot become interactive training merely by replacing one command: the recorded
continuation then ceases to describe the same game.

Benchmark actual authoritative-game throughput before sizing RL compute. If an
accelerated simulator is used, validate branching and mechanics on targeted cases
and confirm every promoted policy in original StarCraft. CPU actor throughput
may dominate the budget even when the learner is GPU-fast.

The relevant precedent is the combination of imitation and diverse league
training in [AlphaStar](https://deepmind.google/blog/alphastar-grandmaster-level-in-starcraft-ii-using-multi-agent-reinforcement-learning/).
[AlphaStar Unplugged](https://arxiv.org/abs/2308.03526) also studies offline methods
beyond cloning. Both concern StarCraft II: they motivate experiments, not a claim
that their strength or compute transfers to Brood War. For Brood War-specific
implementation ideas, [TorchCraftAI](https://github.com/TorchCraft/TorchCraftAI)
provides modular agents and training examples; audit compatibility and licensing
before reuse. Full-game RL is beyond the committed October minimum.

## 11. Compute and spending decisions

Use the local 5070 Ti for pipeline validation, A0/A2, initial teacher pilots,
and small searches. Do not rent compute while input loading, faulty targets, or
missing live execution are the binding constraint.

Only after measuring steady-state useful examples/sec and validation progress,
compare one larger-GPU pilot. Published Runpod dedicated-instance prices checked
22 September 2026 give illustrative GPU-only costs:

| Instance | VRAM | Posted hourly price, USD | 24 hours | 100 hours |
|---|---:|---:|---:|---:|
| RTX 5090 | 32 GB | $0.99 | $23.76 | $99 |
| A100 PCIe | 80 GB | $1.59 | $38.16 | $159 |
| H100 SXM | 80 GB | $3.49 | $83.76 | $349 |

Source: [Runpod pricing](https://www.runpod.io/pricing). Availability/tier, storage,
data transfer, CPU game workers and taxes can change total cost. These are budget
examples, not predictions of job duration or quotes locked for purchase.

If the full teacher fits locally and progresses quickly, stay local. If capacity
or sequence length is constrained, compare 32 GB and 80 GB options. H100 is a
candidate for a larger teacher, not a default requirement or guaranteed best
value. Measure cost per useful update and per validated improvement. No cloud
instance is provisioned by this plan.

Estimate jobs using measured throughput:

`hours = training_steps * seconds_per_step / 3600 + validation + checkpoint/I/O time`

`cost = GPU_hours * rate + CPU_rollout_hours * rate + storage/transfer`

For league training, size CPU workers and match duration first; an idle GPU next
to too few game workers does not accelerate learning. Store the measurements and
offer a capped pilot budget before committing to a long rented run.

## 12. Delivery order and deadline decisions

Dates are target windows, contingent on correctness and observed throughput.
Keep an installable candidate throughout; do not gamble the submission on a late
architecture rewrite.

| Target window | Deliverable | Exit decision |
|---|---|---|
| Sep 22–24 | G0/G1, memory-cadence fix, data report, frozen baseline, phase telemetry | Trustworthy immutable data and no auto-launch; reproducible real-data fixture |
| Sep 24–27 | Shards/sampler/resume/profiling; bounded macro executor; recurrent C++ proof | A0 smoke training and CPU inference work; decisions execute correctly |
| Sep 27–Oct 1 | A0/A1/A2 comparisons, teacher pilot if useful; development matches | Choose an executable candidate using evidence; target label/execution failures |
| Oct 2 | Feature go/no-go | If recurrence/teacher lacks deployment evidence, ship the best measured simpler controller; no speculative dependencies |
| Oct 2–7 | Focused macro/belief improvements, unknown-map tests, adversarial scenarios, finalist comparisons | Stronger candidate without matchup/runtime regression; v3 additions only if ready |
| Oct 8–10 | Final candidate selection, sealed match pool, exact-package stress tests | Freeze model, code, configs and reproducible build |
| Oct 11–13 | Clean Windows 10 and target-class CPU checks, packaging and recovery buffer | Submission archive ready before the deadline |
| Oct 14 | Submission deadline | Only necessary verified fixes; no new research architecture |

Until Oct 2, a v3 pilot can run alongside A0/A2 development if engineering capacity
permits. Do not make the tournament depend on full spatial extraction, large-scale
RL, or new Terran/Zerg controllers. After submission, expand those tracks using
the same legal-observation and strength-evaluation contracts.

### Implementation backlog, in dependency order

| ID | Proposed code/artifact work | Done when |
|---|---|---|
| T01 | Stage orchestration and immutable release manifests around `training/replay_pipeline.py` | Extraction-only restart verified, old outputs reused, explicit training gate |
| T02 | `training/analyze_dataset.py` and data/label reports | Coverage, split leakage checks and action-loss accounting published |
| T03 | Memory scheduling in `ProtoddModule` / `ModelRuntime` | Brief-sighting and fog/detection parity fixtures pass |
| T04 | `training/pack_dataset.py`, typed shards and sequence sampler | Same features/targets, causal sequences, reproducible sampling |
| T05 | Trainer configs, AMP/profiler, checkpoint/resume and status | Correctness/resume checks and local throughput benchmark complete |
| T06 | `IntentExecutor` / arbiter + macro-planner integration | Persistent legal intents, no competing ownership, fallback and execution telemetry |
| T07 | Recurrent model and versioned C++ export/runtime | Stratified Python/C++ parity and target-class CPU budget pass |
| T08 | Ladder accounting, pairing, promotion manifest and scenarios | Frozen baseline comparison cannot promote crashes or unsupported win claims |
| T09 | Bounded A0/A1/A2/teacher experiment matrix | Validation and live evidence choose finalist; no test contamination |
| T10 | Clean-build submission package and exact-artifact validation | DLL + weights + source compile/run on clean Windows 10 |
| R01 | V3 observations, truth/targets and action lifecycle pilot | Authoritative parity and useful labels demonstrated before bulk extraction |
| R02 | Belief/scouting/tactics heads and executors | Each adds value in isolated and combined game tests |
| R03 | Training league, faithful interactive environment and RL | Measured rollout throughput and robust gains on untouched opponents |

T01–T05 are the pre-training preparation priority. T06–T08 proceed alongside
offline pilots and are mandatory before a trained candidate can claim improved
playing strength. T09/T10 determine what actually goes to AIIDE.

## 13. What this plan does not assume

It does not assume the current replays are all professional, that every extracted
row is independent, that a bigger model wins, that compressed inference preserves
strength automatically, or that simulator agreement proves all original-game
mechanics. It does not promise an AIIDE win from a single training run.

The strongest practical path is an executable, measured learning loop: correct
legal observations, useful targets, capable temporal models, reliable commands,
and repeated tests against diverse opponents. Scale compute where those tests
show that compute is the remaining limit.
