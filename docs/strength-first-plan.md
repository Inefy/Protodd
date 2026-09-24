# Strength-first development plan — 24 September 2026

This is the active direction after the user's request to reassess the path to
the strongest possible bot and make adjustments. It supersedes the priority of
making the existing six-slot replay packet model the sole next tournament
candidate. The goal remains learned improvement across the whole game, Protoss
first. Architecture and training volume are means; demonstrated playing strength
under the actual CPU/BWAPI constraints determines what ships.

## Assessment

The current path has produced useful data and deployment infrastructure, but
continuing small frozen-backbone command-head probes is a poor next investment.
The available evidence does not establish that the current model can be rescued
by another threshold, head, or larger pass over the same objective. This is a
resource-aware engineering judgment, not proof of an optimal architecture.

| Finding | Consequence and adjustment |
|---|---|
| The full fit, extraction, and command audit are complete. The best recent position-arbitrated diagnostic has 241/28,715 audited signatures (0.84%), 395/20,829 position hits (1.90%), 240/7,965 non-right-click kind matches (3.01%), attack 0/331 and attack-move 1/2,040. | Preserve the checkpoint and fixed 256px current-order position rule as offline baselines. Whole-game model control remains blocked. Passing the old small numeric floors would only permit further testing, not establish competence. |
| The full fit used 7,200 games, but retained only 183,649 windows across 300 groups, with 525,005 window presentations and 76,800 updates. Actual unique windows consumed by gradients were not logged. There is a two-window-per-game-per-category cap. | Report actual exposure and learning curves. A selected game is not an epoch over its complete trajectory. Before scaling, show that a coherent small training set can be learned and generalizes to separate development games. |
| In three already-used, omitted-train development games, 1,388/4,434 scored-slot commands selected multiple actors (31.3%). Those commands contain 10,306 unit-command assignments. Runtime multi-slot decoding sets `maxActors=1`. | Even perfect one-actor choices can cover at most 4,434/10,306 assignments (43.0%) for this cohort. Implement coherent actor sets or squad intents and measure set coverage, extra actors and actual execution. These are cohort-specific contract counts, not estimated corpus accuracy. |
| The audit accepts any selected actor as actor-correct. Its signature checks kind, mode, target, position, unit type and delay, but does not fully check actor sets, queue flags, technology, upgrade or every command argument. | Keep historical metrics unchanged for comparison. Add a versioned semantic/action-contract audit; do not describe the existing signature as proof of complete command reproduction. |
| The encoder averages the terrain convolution into one global vector; entity embeddings have no entity-to-entity attention. Most subsequent heads freeze that representation. | Test retained spatial features and bounded actor/target relations with trainable relevant encoder layers. Another classifier on identical frozen features is not the default experiment. |
| Training burns in at most eight earlier cadence rows from zero memory; evaluation uses continuous memory or periodic resets every eight decisions. | Choose one documented sequence/memory contract and use it consistently in training, offline evaluation and runtime. The periodic reset result does not demonstrate robust long-term memory. |
| Free-running decoder outputs are evaluated on replay states; model actions do not change those states. The same 24-game validation has informed repeated experiments. | Replay audits check legal inputs and decoder errors, not recovery from the bot's own actions. Treat this validation as a regression/development benchmark; select fresh evaluation cohorts before candidate selection and keep final test sealed. |
| Existing live reports show major economic, production and engagement failures, with several proposed fixes still losing paired games. | Prioritize the largest reproducible causes of losses. Require matched gameplay evidence and ablations before retaining a change. Avoid endless unmeasured heuristic tuning as well as endless offline probes. |

Reproducible exposure/actor report:
`artifacts/replay-learning/whole-game-strategy-audit-20260924.json`, produced by
`training.whole_game_strategy_audit`. It pins the fit log, run specification,
release identity, relevant source files and verified shard receipts. It reads
three **train** games, opens no final test data, and fits no model. Full model
experiment history remains in [whole-game-model.md](whole-game-model.md).

## Target architecture and training direction

Use a hierarchy of learned decisions with explicit ownership and persistent
execution. Keep the existing legal observation, production, placement, pathing,
command arbitration and fallback infrastructure while replacing decision scopes
when evidence supports doing so. A macro-only model is not the completed goal.

| Scope | Learned decision | Execution and evidence |
|---|---|---|
| Economy and production | Persistent spending priorities, production/composition, worker/gas allocation, expansion and technology | Bind producers/builders, reserve resources, retry or cancel correctly; measure income, supply blocks, idle producers, army/tech timing and wins. |
| Scouting and strategy | Information targets, uncertainty, defend/attack/retreat, squad objectives and reinforcement | Own a defined squad or goal scope; measure useful scouting, defense response, force concentration and held-out games. |
| Tactics | Actor sets, engagement choice, target/focus fire, movement regions and ability use | Local relative geometry and legal targets; test group coverage, illegal/extra actors, engagement outcomes, then full games. |
| Long-term memory | Beliefs and persistent plans over coherent sequences | Same recurrent state semantics during fitting and play; burn-in/truncated backprop and decision feedback. |

Choose the first learned scope from the loss review, provisionally early
economy/army readiness or engage/retreat. Define its actions and execution before
training it. Different scopes may use different decision rates; avoid forcing
every strategic decision into a human's next 24-frame click packet. Preserve
command-level targets where timing and micro actually need them. Audit mappings
such as right-click versus attack/attack-move using command acceptance and
observed state; do not blindly merge their semantics.

Human replays remain valuable for initialization, representation learning,
sequence supervision, diverse openings and opponent behavior. Then optimize
selected scopes using adjudicated **training** episodes and scenario curricula.
Terminal win/loss is the main full-game objective; economic and combat metrics
diagnose behavior and constrain reward design, not substitutes for wins. Use
diverse frozen opponents and historical selves once the policy is competent
enough to generate useful games. Do not launch full-game RL from random weights
or copy erroneous current-bot actions as unquestioned expert labels.

Rationale from primary sources: [AlphaStar](https://deepmind.google/blog/alphastar-grandmaster-level-in-starcraft-ii-using-multi-agent-reinforcement-learning/)
combined imitation initialization with reinforcement learning and diverse league
opponents. This supports combining objectives, not an assumption that its SC2
results or compute requirements transfer to this PC or Brood War.
[DAgger](https://proceedings.mlr.press/v15/ross11a.html) explains why sequential
imitation must address the states induced by a learner's own decisions.
[TorchCraftAI's module training](https://torchcraft.github.io/TorchCraftAI/docs/module-training.html)
provides a Brood War precedent for evaluating learned modules within a larger
bot. These motivate the staged direction; no external bot code is imported.

## Active work queue

### Execution update — current task

Detailed evidence and current run identities are in
[local-training-results-20260924.md](local-training-results-20260924.md).
The baseline is complete after replacing the unhealthy Iron opponent: 12 normal
games, zero wins; four original launch failures remain excluded. Trace review
selects pre-contact economy/army readiness for the first persistent learned scope.
The new group/spatial architecture passed its small capacity test (66/80 training
signatures, 37/37 position hits), and a bounded 36-train-game / 9-development-game
comparison completed locally. It failed: 4/781 development signatures versus
10/781 for the matched reference, and 4/449 near-position hits versus 5/449.
Both economy/production next-intent classifiers also failed their declared
development checks. The local research runs are complete; no continuation fit
or experimental controller is running. The full plan remains incomplete at
stage 2. Preserve these checkpoints as diagnostics and do not scale or deploy them.
Metadata inventory also establishes that all 1,380 validation games were used by
the earlier macro model; the reserved 24-game command confirmation set is unused
by prior command experiments, not globally fresh. Final-test payloads stay sealed.

### Local training priority — user update, 24 September

The user asked to rely more heavily on training, offered Colab, then explicitly
chose local training. Use the RTX 5070 Ti (16 GB) for this work. No Colab runtime
was started, no project data was uploaded, and no cloud purchase was made.
The next substantive improvements should come from trained decisions with
measurable execution, rather than repeated manual strategy adjustments.

The first local job was `training.whole_game_capacity_fit`, a bounded diagnostic
of the existing full network's ability to learn a small fixed command set. It
unfreezes the whole network from the small actor-rank checkpoint, collects up to
64 windows per game/category (previous full fit: two), then fixes 16 windows per
matchup for training and 16 per matchup for development. These are **48 train
windows and 48 development windows**, not a new large-corpus fit. The three train
and three development games are disjoint games omitted from the original full
fit, previously used for development; they are not fresh validation.

It records exact windows actually used in gradients, sample presentation counts,
before/after teacher loss and free-running decoding, both combat kinds, position
errors and diagnostic actor-set coverage. Train/evaluation both reconstruct eight
prior cadence rows from zero for each window. This matches the diagnostic memory
contract; it does not claim parity with the current continuous-memory runtime.
The budget is 600 updates or 1,200 seconds of training, with optimizer/RNG/sample
count checkpoints every 50 steps. The predeclared capacity check asks for at
least 70% reduction in fixed-set teacher loss, 50% historical audited-signature
accuracy on that training set and some correct predictions for both combat kinds.
These are diagnostic thresholds, never promotion thresholds. Development metrics
are observed before/after, not used to select a checkpoint or tune thresholds.

Current output: `artifacts/replay-learning/whole-game-local-capacity-20260924`.
Frozen source and launch/log records: `build/local-capacity-20260924-r2`.
Use `status.json` for the actual worker PID; Windows' venv launcher PID can differ.
An initial preflight in `build/local-capacity-20260924` stopped before training
because the checker used the receipt's canonical identity hash instead of the
checkpoint's identity-file hash. The corrected check preserves both conventions;
the verified shard reader still validates canonical receipt hashes.

**Completed result:** 600 updates, 2,400 window presentations, all 48 training
windows consumed, 69 model tensors changed. The training phase took about 80
seconds on the local GPU. No second fit is running from this experiment.

| Metric | Before | After |
|---|---:|---:|
| Fixed-train teacher loss | 3.033 | -0.393 |
| Fixed-train audited packet signatures | 0/80 | 29/80 |
| Fixed-train positions within 64px | 0/37 | 1/37 |
| Fixed-train attack / attack-move kind matches | 0/5; 0/10 | 5/5; 1/10 |
| Development teacher loss | 2.985 | 8.715 |
| Development audited packet signatures | 2/98 | 0/98 |
| Development positions within 64px | 0/53 | 0/53 |

The predeclared capacity gate **failed**: 36.25% training signatures is below
50%, position decoding remains poor even on the fitted set, and development
quality regresses. Continuous position-density losses can be negative; the loss
reduction is not a calibrated measure of command success. Do not extend this fit,
promote its checkpoint or scale its objective to the full corpus. This test shows
that the GPU and gradients work, while lower teacher-forced loss is insufficient.
Next prioritize teacher/inference conditioning alignment (including actor sets
and generated earlier commands), position supervision and a trainable spatial/
relational representation. Compare teacher-context and free-decoded errors on
the train cohort before choosing the next architecture experiment. Then require
bounded development improvement and fresh evaluation.

Continue toward
trainable spatial/relational features, actor groups and persistent learned plans,
then separately marked gameplay training and outcome learning. Preserve the
gameplay review below as evidence for which learned scope should improve first.

Follow-up scheduling: the app reports that `whole-game-training-follow-up` no
longer exists, and its local automation configuration is absent. The attempted
update therefore did not apply. No replacement automation was created. The
completed local diagnostic is preserved; this queue currently requires an active
task to continue and should not be described as an unattended training schedule.

### 1. Establish the game baseline and choose one bottleneck — completed

`build/strength-first-20260924/baseline-01` is a new development campaign:
BananaBrain (PvP), McRaveZ (PvZ), Iron (PvT); Benzene and Destination; both host
sides, 12 scheduled games. The frozen reference is
`build/robust-training-20260922/baseline-Protodd.dll`, SHA-256
`0ec21a0176d092d6c1b001498d392073b9b941d72f0e1802ad7a38dfef79106e`.
This is the existing deterministic reference, not a newly trained candidate or
a claim that the old DLL is the strongest current build. All learned modes are
off/frozen. The campaign pins opponent/read inputs, maps, local-client-v5,
local-server-v2 and the schedule; it launched on port 1371.

Inspect `processes.json`, both client logs, server reports and archived telemetry.
Never launch another campaign into the same StarCraft runtimes. Require two
consistent normal reports and reviewed opponent activity for a scored game;
crashes, stalls and timeouts are operational results. Iron's health is to be
established by this campaign. Do not count an unhealthy opponent as a win.

Initial check at 04:26 UTC: game 0 completed against BananaBrain on Benzene,
loss at own frame 19,935; both players reported NORMAL with no crash/timeout or
over-55ms timer counts. The next game is running. Both replays and bot telemetry
are archived under `server/replays` (telemetry in `bot-write`). This is one
structurally valid result, pending the complete campaign and activity review.

Completion check: all 12 scheduled games reported. Eight PvP/PvZ games have
consistent normal reports, all losses. All four Iron games are excluded:
both players reported `GAME_STATE_NEVER_DETECTED`, final frame -1. This is a
PvT launch/compatibility failure, not four strategic losses or wins. Preserve
these reports and repair/replace the PvT reference before claiming three-matchup
coverage. The completed campaign's idle managers are not an active training fit.

After completion, rank losses by the **earliest consequential divergence**:
worker/income loss, supply block, delayed production/tech, unspent resources,
missed information, poor force concentration or losing engagement. Review
traces/replays, not just end-game totals. Select one reproducible bottleneck and
record its hypothesis, baseline and acceptance criterion before editing behavior.
Develop on training/scenario material; use a new matched candidate campaign for
evaluation. The 12 games establish a reference, not statistical proof of a small
strength difference. Verify actual map/start/seed comparability.

### 2. Repair the learning and action contract — in progress

The group capacity/development pair and two exclusive macro-intent variants have
now completed. Development failed. The next work is a reviewed change to the
selected economy scope's supervision and persistent execution: concurrent demands,
producer/resource ownership and observed success feedback. Do not launch another
exclusive next-click classifier or a larger fit of the rejected group model.
Spatial sharing and relative geometry remain hypotheses to test within a bounded
functional task, not established fixes. A new fit needs its own source/data pins
and acceptance criteria before training; the prior thresholds are not relaxed.

Before another full-corpus run:

1. Version actor-set/semantic audit results separately from historical packet
   metrics. Cover all required arguments, availability and execution feedback.
   Do not simply raise `maxActors`: uncalibrated scores could issue bad group orders.
2. Define the first scope's persistent intent and actor binding; write scenario
   fixtures for ownership, cancellations and command success. Preserve causal
   observation boundaries and original-game compatibility checks.
3. Instrument actual unique sampled windows, phase/kind/map coverage, batch sizes,
   training/validation learning curves and exact optimizer resume. Use coherent
   sequences and one matching memory contract.
4. Run one small overfit sanity experiment on train games, then one bounded
   comparison on separate train-development games. Unfreeze/replace the relevant
   representation if necessary; use one compact spatial/relational architecture
   hypothesis, not an unbounded sweep of frozen heads. Record actor-set precision
   and recall, both combat kinds, position near-hit and median error where relevant,
   and the selected scope's functional success. Overfit success is only plumbing.
5. The metadata inventory is complete: there are 1,356 games unused by recorded
   command experiments, but zero globally unused release validation games because
   the macro model evaluated all 1,380. Twenty-four command-confirmation games
   are reserved with payloads unopened. Acquire and deduplicate additional games
   for a globally untouched replay evaluation before making that stronger claim.
   Keep final-test replay payloads and final-test game outcomes sealed.

### 3. Prove a scope improves play, then expand — queued

Once the scoped candidate passes its predeclared offline functional gates:
export and verify Python/Win32 parity, measure complete BWAPI callbacks with
realistic entity counts and concurrent arena load, run shadow mode, then explicitly
bounded local training/control scenarios and matched development games. Each
controlled scope has one owner and a tested fallback. Do not enable the failed
whole-game checkpoint under a new name or route around its existing gates.

Compare the exact frozen candidate against the reference on the same opponent,
map and start distribution; retain per-matchup effects, failures and uncertainty.
Only then expand learning to another scope or scale the training corpus. Introduce
outcome learning from separately marked, healthy training episodes when the
execution loop and episode provenance are reliable. Never train on evaluation
traces or tune a candidate using the final strength campaign.

### 4. Strength and tournament gates — unchanged

Retain the existing 72-game strength gate and all legality, provenance, numerical
parity, complete-callback timing and reliability requirements. The 72 games are
a minimum evidence gate; an inconclusive small advantage is not a promotion.
Report confidence/paired effects and add games if the decision requires them.
Use unseen opponent/map/seed pools where feasible. Ship the strongest **validated**
package; adding a neural controller is not itself a promotion criterion.
Own Terran/Zerg transfer follows robust Protoss coverage. No paid/cloud compute
without the existing cost-proposal authorization process.

## Experiment discipline and recurring work

- No new full-corpus fit until a bounded experiment addresses a demonstrated
  bottleneck and meets its declared development gates. Do not restart the completed
  extraction/full-fit launcher merely because there is idle GPU capacity.
- Stop routine actor-only, kind-only and frozen-feature position/combat variants.
  Revisit a rejected family only with new evidence and a materially different
  hypothesis. Limit a family to two predeclared variants before reviewing it.
- Prioritize completing the baseline and actionable loss review, then the selected
  contract/learning change. Architecture work must state the game behavior it is
  intended to improve and how that improvement will be measured.
- Source-pin every candidate and preserve negative results. The old 24-game audit
  is a regression benchmark; the fixed position rule is not tuned further on it.
- When follow-up runs, inspect processes/artifacts first,
  avoid duplicate games/fits, recover only verified owned work, and update this
  queue plus `whole-game-model.md`. Stay quiet for unchanged/non-actionable state;
  report meaningful results, failures or required user action.

## What this revision has and has not established

Completed: strategy/code review, quantified fit exposure and group-action contract,
12-game healthy reference baseline and loss timelines, validation-exposure inventory,
and a passed capacity test for the new group-command architecture. Its weights are
research artifacts. The subsequent bounded comparison and both macro-intent
probes failed development; the current research cycle is closed. Generalization,
execution, improved win rate and tournament readiness remain unproven. See the
execution report for metrics, verification and the remaining dependency gates.
