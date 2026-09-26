# Local training execution — 24 September 2026

**Latest continuation:** the [population-goal cycle](production-goal-experiment-20260924.md)
completed capacity/development training, a separate nine-game train check and
fixed 24-game validation. The joint model failed development; the narrower
worker model failed validation's per-game recall floor. No new live control or
arena campaign followed. Verified status:
`build/strength-first-20260924/production-goal-status.json`.

**Live integration continuation:** native history and production feedback now
pass real-game checks; a bounded local Probe/Zealot/Dragoon control trial passed
its fixed screen. The full-game comparison with matched seeds completed with
candidate 0/2 wins versus reference 1/2, failing advancement. The local paired
supervisor has stopped; no 72-game campaign was launched. Current evidence is in
[production-live-integration-20260924.md](production-live-integration-20260924.md)
and `build/strength-first-20260924/production-integration-status.json`.
Historical offline failures below remain rejected; tournament control is off.

This records execution of the active [strength-first plan](strength-first-plan.md).
All jobs use local compute. Experimental control is limited to isolated local
development campaigns; no experimental model has tournament control.
The consolidated machine-readable checklist is
`build/strength-first-20260924/training-plan-status.json`, regenerated with
`python -m training.local_training_review` after inspecting jobs. It verifies
completed jobs' frozen sources and checkpoint steps/sample counts, and records
dependent gates separately from completed research experiments.

**Earlier offline update:** concurrent production-demand training passes
development and 24-game fixed-weight confirmation. Win32 model-only numerical
parity also passes; live input/execution integration and playing-strength gates
remain. See [production-demand-experiment-20260924.md](production-demand-experiment-20260924.md)
and `build/strength-first-20260924/production-training-status.json` for current
evidence. The failed experiments below remain preserved historical results.

**Prior cycle status at 07:52 UTC:** all four bounded group/macro runs have completed;
the larger group model and both economy classifiers failed development. The
group fit exited normally after its final audit. No associated training or arena
process remains, and no continuation fit is queued. The research cycle is
finished; the full strength/promotion plan is **not complete**.

## Completed baseline and loss review

The frozen DLL is `build/robust-training-20260922/baseline-Protodd.dll`, SHA-256
`0ec21a0176d092d6c1b001498d392073b9b941d72f0e1802ad7a38dfef79106e`.

| Campaign | Opponent | Consistent normal results | Excluded |
|---|---|---:|---:|
| baseline-01 | BananaBrain, PvP | 0 wins / 4 games | 0 |
| baseline-01 | McRaveZ, PvZ | 0 wins / 4 games | 0 |
| baseline-01 | Iron, PvT | 0 games | 4 never detected game state |
| baseline-pvt-02 | UABTerran, PvT | 0 wins / 4 games | 0 |

Both campaigns are under `build/strength-first-20260924`. They use Benzene and
Destination, both host sides, the same frozen candidate DLL and manager builds.
The replacement opponent uses BWAPI 4.4. The original failed Iron reports are
preserved. All managers were stopped after their campaigns finished and no game
process remained. Both report sides were normal, with consistent outcomes and
no timeout-counter violations for the 12 completed games. Archived legal unit
sightings and own-unit losses near enemy forces establish actual opponent
activity. This is development evidence, not a tournament strength estimate or a
paired comparison against a new candidate. Host pairing does not ensure equal
random seeds; `MATCH` records preserve seeds/map hashes for later comparisons.

Reproducible timelines and original log line references:

- `build/strength-first-20260924/baseline-01-loss-review-v2.json`
- `build/strength-first-20260924/baseline-pvt-02-loss-review.json`
- Generator: `training/arena_loss_review.py`.

The earlier `baseline-01-loss-review.json` is superseded: its first parser looked
for lifecycle destroy records; this logger records deaths as `LOSS` events.
Use v2, which parses those events and retains their command/nearby-enemy context.
Enemy discovery counts count events, not unique enemy units or kills.

### Selected bottleneck: economy and army readiness before contact

All four PvZ games stay at eight Probes from the frame-1200 snapshot until
frame 3240 (Benzene) or 3360 (Destination). They have no own-unit deaths before
frame 6000. At frame 3600 they have nine Probes and no completed mobile army;
at frame 4800 only 11–12 Probes and 1–2 completed army units. The delay precedes
the decisive enemy attack, so combat losses cannot explain the initial economy
stall. The traces show early construction commitments and saving/placement
states. They establish a repeated symptom; they do not prove which alternative
spending policy wins.

A later frame-by-frame review of all four archived PvZ logs identified the
immediate cause of the Probe pause: `planPvZ` intentionally caps the worker
target at eight until two completed Zealots or a completed Cannon exist. At
frame 2400, each game had an idle Nexus, eight Probes, and 248–264 minerals;
the plan requested only eight Probes. Higher-priority structure commitments
also affected spending, but changing reservations alone would not have created
an unmet Probe goal. A later source-pinned paired development test raised the
early cap to ten after the first Pylon. It produced two extra Probes at frame
3,000 in all four pairs, but both versions lost 0/4 and the candidate had no
larger mobile army at frame 6,000. See
[pvz-worker-cap-20260925.md](pvz-worker-cap-20260925.md). This does not
establish a strength gain.

PvT also fields only one completed army unit at frame 4800 in all four games,
with the Core incomplete and roughly 320 gas banked. PvP game 3 has one army
unit against three visible enemies at frame 4800, then loses 18 Probes and both
early combat units before frame 6000. Worker-defense behavior contributes to
that collapse, but the earlier readiness gap is already present.

The first scoped policy should learn **persistent economy/production priorities**:
worker production, supply, mobile army, gas/technology and expansion. It must
commit a bounded intent, bind a legal producer/builder, reserve resources once,
observe acceptance/completion, and release or cancel on death, changed need or
expiry. One owner per scope is required; repeatedly issuing human click packets
is not sufficient. Existing placement/pathing remain execution services.

Before local control, scenario fixtures must cover duplicate proposals, occupied
producer, insufficient resources, actor death, failed placement, cancellation and
fallback. Require no double reservation or duplicate execution and correct
success/failure feedback. Training targets must come from qualified human train
games or explicitly marked training episodes, never these evaluation traces.
Acceptance compares a frozen learned candidate against the reference on matched
games: shorter pre-contact worker stalls and earlier mobile army without worse
reliability/supply blocks, followed by a positive gameplay effect. These diagnostic
metrics cannot replace wins or the 72-game strength gate. No behavior patch was
made to the frozen baseline during this review.

## Completed capacity experiments

The original 600-update whole-network diagnostic fitted 48 training windows but
failed: 29/80 historical signatures, 1/37 position hits and 0/98 development
signatures. The follow-up conditioning audit found teacher-context kind accuracy
80/80, but even the best mixture mean was within 64 pixels for only 1/37 targets.
This motivated changing the position objective and actor conditioning.

`GroupCommandModel` adds trainable entity relations and local map features,
predicts a bounded owned actor set and conditions arguments on its pooled
representation. Position supervision uses a 32×32 coarse cell plus a 4×4 subcell
with identical training/inference decoding. Later training includes generated
earlier commands. Each window reconstructs eight prior cadence observations
from zero in both fitting and auditing; native runtime parity is not established.

The source-pinned run in `build/group-capacity-20260924` completed 1,800 updates,
7,200 presentations and all 48 unique train windows. Output is
`artifacts/replay-learning/whole-game-group-capacity-20260924`.

| Metric | Group model, training | Separate development games |
|---|---:|---:|
| Historical command signatures | 66/80 | 1/98 |
| Positions within 64px | 37/37 | 0/53 |
| Position median error | 22.6px | 1,814px |
| Attack / attack-move kind matches | 5/5; 10/10 | 1/4; 0/7 |
| Exactly confirmed actor sets | 65/80 | 12/98 |
| All observed fields correct, including actor set/extra arguments | 58/80 | 1/98 |

The predeclared capacity gate passed. This proves the architecture can learn
the fitted examples; the development results do **not** establish generalization.
Partial actor labels are reported as known positives, known negatives and unknown
memberships; the observed-field audit is not proof of unseen arguments or actual
execution. Historical signature metrics retain their old definition.

### Numerical and loss audit

The capacity fit's final minibatch loss (0.185 over its last 50 updates) differed
from the final mean over all 48 fixed training windows (4.621). A fixed-checkpoint
audit reproduced the recorded FP32 command counts exactly. BF16 produced the
same 66 signature and 37 position matches, with teacher loss 4.618; precision
rounding does not explain this discrepancy. Batched teacher, generated and
scheduled-context objectives also retain the high loss.

One window accounts for most of the final fixed-set loss: game
`049a7a0f5ccaa6f274ecaa93a5634a8b72160fa71b3a36ada7153dc4d7e1e81d`, cadence
frame 22896, sequence 4658. It was presented 133 times. Its spell command at
frame 22904 has target-entity cross-entropy 217.268 of total window loss 217.373;
other supervised heads are small. This identifies a fitted model failure, not
evidence that the target label is invalid. The other 47 windows have loss below
one. The audit does not establish when this target error developed, and does
not justify dropping the example or declaring the model numerically fixed.

Report: `artifacts/replay-learning/whole-game-group-objective-outlier-audit-20260924.json`.
No optimizer or new fit was used. The historical capacity pass and all command
errors remain recorded; no thresholds were changed.

## Bounded development comparison — completed, failed

The completed fit used 12 omitted-train games per matchup (36 total), three separate
train-development games per matchup (nine total), 512 fixed windows per matchup
for fitting and 128 for development. Budget: 4,800 updates, batch eight, at most
2,400 training seconds, optimizer/RNG/exposure checkpoints every 50 updates.
All relevant parameters train; initialization is random, not the failed model.
The reference is the small actor-ranking model with the already fixed 256px
current-order rule, evaluated on identical development windows and memory history.

Output: `artifacts/replay-learning/whole-game-group-development-20260924`.
Frozen source/logs: `build/group-development-20260924-r2`.
The original launcher failed before fitting because its overly conservative
selection requested more omitted PvZ games than exist (31). The corrected
selection uses 12 train + 3 disjoint development games within that pool.
No duplicate fit or optimizer restart occurred.

The `run.json` pins thresholds before the fit: signatures at least 5% and twice
the reference; position hits at least 10% and 1.5× the reference; lower median
position error; at least 10% actor-and-kind matches for **each** combat kind;
actor precision ≥50%, recall ≥30%; observed-field signatures ≥3%; no regression
in non-right-click counts or any matchup's signatures. These are research
continuation thresholds, not promotion thresholds. `status.json` identifies the
worker PID; launch JSON may name a separate Windows venv wrapper.

Future runs of `whole_game_group_fit` now report the evaluation split and completed
window count every 32 windows. This completed fit used its original frozen source;
its final evaluation legitimately kept one stage marker while the process worked.
The progress-reporting change does not alter model weights, data or loss.

The matched reference on these 384 development windows has 10/781 signatures,
5/449 position hits (median error 1,268px), 25 non-right-click kind matches,
and zero actor-and-kind matches for 33 attacks and 55 attack-moves. Thus the new
candidate must clear meaningful absolute floors as well as improve the reference.

The fit completed all 4,800 updates in 2,301.95 training seconds, consuming all
1,536 fixed windows across 38,400 presentations. Final evaluation completed at
07:51:59 UTC. The optimizer/RNG checkpoint and frozen source are retained.

| Development metric | Fixed reference | Group model |
|---|---:|---:|
| Historical command signatures | 10/781 | 4/781 |
| Positions within 64px | 5/449 | 4/449 |
| Position median error | 1,268px | 1,388px |
| Attack actor-and-kind matches | 0/33 | 3/33 |
| Attack-move actor-and-kind matches | 0/55 | 1/55 |
| Non-right-click kind matches | 25/422 | 48/422 |

The group model's known actor-membership precision is 36.6%, recall 15.5%, and
all observed fields match on 1/781 commands. It produces 124 exactly matching
confirmed actor sets, but total signatures regress in all three matchups. Only
the non-right-click non-regression check passes; all other predeclared checks
fail. Extra combat recognition does not offset the overall command failures.

The training/development gap is large: training signatures 727/3,238, position
hits 943/1,712, median error 22.6px, teacher loss 2.437 versus development loss
12.393. This supports a generalization problem; it does not establish that more
epochs or a larger corpus will fix it. Even the larger training set fails the
capacity thresholds for total signatures and near-position accuracy.

**Decision:** retain this checkpoint for diagnosis, reject it as a candidate,
and do not scale this cell-classifier experiment or advance it to confirmation,
export or controlled games. The small capacity success did not transfer to
other games. The next hypothesis requires explicit review of spatial sharing,
relative geometry and functional scope supervision before another fit.

`whole-game-position-support-20260924.json` diagnoses the fixed sample coverage:
1,712 training position labels occupy 497 of 1,024 coarse cells; 178 cells have
one target. Of 449 development targets, 144 use a cell absent from training,
and 220 have an unseen kind/cell pair. The label-only audit verifies its command
and position denominators against the causal audit. This supports investigating
shared spatial scoring instead of separate output weights per map cell. It is
not proof of why any prediction fails, nor an accuracy ceiling.

## Economy/production learning probes — completed, failed

These are separate CPU experiments on the selected first scope, not duplicate
GPU fits. `training/macro_commitment_probe.py` predicts a production intent in
the next 120 frames and whether one is due. Inputs are the unchanged current
causal macro features. Targets can use future events; windows crossing unknown
gaps/endings are censored, and currently unavailable future intents are not
silently labeled as waiting. Train games and development games are disjoint.

| Experiment | Train / development rows | Consumed presentations | Development event precision / recall | Correct event intent | Probe / Pylon / Gateway recall |
|---|---:|---:|---:|---:|---:|
| 36 train games, all phases | 23,561 / 6,078 | 614,400 | 59.9% / 74.1% | 26.7% (frequency reference 36.4%) | 52.7% / 4.5% / 4.0% |
| 300 train games, frames <7,200, weighted types | 86,079 / 2,544 | 1,228,800 | 65.8% / 73.8% | 30.1% (frequency reference 58.2%) | 27.4% / 5.6% / 46.7% |

All selected training rows were consumed in both runs. The first fit completed
1,200 updates; the second completed 2,400. The larger cohort was selected using
metadata before its fit, excluding the same nine development games. The second
variant used training-only bounded inverse-square-root class weights to test
infrastructure learning. Both failed predeclared intent/infrastructure gates.
The larger early-game result has different denominators and is not a paired
improvement over the first experiment. No model was exported or enabled.

Artifacts:

- `artifacts/replay-learning/macro-commitment-development-20260924`
- `artifacts/replay-learning/macro-commitment-early-development-20260924`
- Frozen launch/source records under the correspondingly named `build` directories.

Stop this exclusive next-intent classifier family after these two variants.
Further work needs a reviewed supervision/execution hypothesis, such as concurrent
production demands and persistent intent success feedback, rather than another
threshold change or more epochs on this classifier. The loss-review requirement
for a persistent owner/executor remains unmet; these predictions have no route
to live control.

## Memory contract and CPU inference preflight — completed diagnostic

`RollingGroupInference` caches each fixed model's observation encoding, then
rebuilds recurrent memory from zero over the preceding eight encodings. It
accepts no teacher tokens or labels. Tests confirm exact output equality with
training-style history reconstruction through window eviction and gaps, game
resets, duplicate-frame idempotence, and rejection after weights change.
The new `encode_step` refactor also removes duplicate terrain convolution work.
The active training job continues to use its original frozen source.

The refactor has bit-for-bit output parity with the frozen capacity model on
the tested tensors. The CPU-only preflight uses synthetic 128×128 maps and two
PyTorch CPU threads under concurrent GPU training. For the final cached-terrain
version, p95 model inference was approximately 18.2ms / 13.1ms / 17.3ms / 46.9ms
at 64 / 128 / 256 / 512 entities; the largest observed sample was 76.1ms. Twenty
timed samples per size and concurrent load are insufficient to claim stable
latency or a controlled speedup. Feature extraction, BWAPI callbacks, arbitration,
dispatch and logging are excluded. Neither Win32 parity nor the full callback
gate has been run or passed.

Reports: `whole-game-group-runtime-preflight-20260924.json` and
`whole-game-group-runtime-preflight-cached-terrain-20260924.json` under
`artifacts/replay-learning`. A promising teacher needs a measured compact native
implementation/student; increasing the model size before resolving this budget
would be a poor next step.

## Evaluation inventory

`artifacts/replay-learning/whole-game-evaluation-inventory-20260924.json` pins
prior run metadata, alias/duplicate identities and the full macro-model history.
Of 1,380 validation games, 1,356 are unused by the recorded whole-game command
experiments. **All 1,380 were evaluated by the earlier macro model** on 902,351
validation rows. There are zero globally untouched validation games in this
release. A deterministic 24-game command-confirmation cohort (eight/matchup) is
reserved from the 1,356, with payloads unopened. It is disjoint from the new fit
and earlier command validation, but must not be called globally fresh.

New, deduplicated games are required for a globally untouched replay evaluation.
The final-test payloads remain sealed. After a development pass, freeze the model
and confirmation criteria before inspecting the reserved command cohort; then
complete export/Win32 parity, full callback timing, shadow/scenario evaluation,
matched games and the 72-game strength gate. A failure blocks dependent stages;
lowering thresholds or enabling tournament control is not completion.

## Verification and remaining work

The 22 focused tests for group decoding/loss, recurrent runtime equivalence,
development gates, observed-field auditing and macro target construction passed.
The final Python syntax checks passed, and `training.local_training_review`
verified all four completed jobs against their frozen source hashes and
checkpoint exposure records. Its output records every failed development check.

The next prerequisite is a functioning learned economy/production scope with
persistent ownership and observed execution feedback, plus supervision that
represents concurrent demands. The two exclusive next-intent classifiers and
the absolute cell-classifier command fit are rejected as candidate paths in
their present form. Further work must establish a materially different bounded
hypothesis and test its functional behavior before another fit or scale-up.

Still uncompleted: a passing development candidate, frozen disjoint confirmation,
the learned scope executor and its failure scenarios, new-model native export/
Win32 parity, full callback timing, matched candidate games and the 72-game
strength gate. No playing-strength improvement has been demonstrated in this
research cycle. These prerequisites are not bypassed by marking the plan done.
