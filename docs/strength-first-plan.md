# Strength-first development plan — 24 September 2026

## Current checkpoint — 27 September 2026

The latest default source includes the clearer in-game status overlay, combat
and scouting correctness fixes, and several bounded strategy changes. The
experimental PvZ early-splash and PvP fog-detection rules are build options
that remain **off** in the default bot. The overlay improves diagnosis; it is
not evidence of stronger play. At this checkpoint all 41 development test
suites pass and the default Win32 BWAPI DLL builds successfully.

A fresh, structurally healthy four-game development baseline against
BananaBrain (PvP) and McRaveZ (PvZ) on Benzene lost 0/4. This is a narrow
opponent/map sample, not an overall ladder win-rate estimate. It reinforces
three concrete bottlenecks:

1. **Detection can lose the economy while the army survives.** In one PvP
   game, the first visible Dark Templar arrived at frame 9,707, the
   Observatory completed at 10,370, and all Probes were gone by 10,800 while
   13 combat units remained. The existing quiet-tech-gap check missed a
   scouted two-Gateway army, and a six-Dragoon rule deferred detection.
2. **The opening screen and base transition still break under pressure.**
   Another PvP game had 13 combat units around its natural completion, then
   lost its two Reavers and most of the army to the first major attack. An
   expansion or tech timing is useful only when the army can hold the base.
3. **Reactive PvZ splash arrives too late.** In the frozen early-splash
   comparison both arms lost 0/4. The eligible Hydra signal often arrived
   after the army had already collapsed; one late Reaver died before firing.
   More late Reaver priority is not the next useful variant.

Current order of work:

1. Finish the same-input, four-game-per-arm PvP fog-detection screen on
   Benzene and Destination. Check Observer completion **before** the first
   Dark Templar, Probe survival, combat strength, and adjudicated wins. Keep
   the option off unless those results justify promotion. The first reference
   attempt produced one valid loss, then its manager and client processes
   exited before game 1 without a recorded exception. The candidate has not
   started. Reprepare both arms for a complete comparison; do not treat the
   partial result as a win-rate screen.
2. Run the prepared current-default PvT baseline against UABTerran so the
   next intervention is chosen from all three matchups rather than another
   PvZ guess.
3. Attack the earliest repeatable loss mechanism: opening mobile defense,
   protected tech/expansion transitions, army cohesion and detector coverage.
   Choose one bounded change at a time and compare frozen binaries in healthy
   matched games on both starting sides.
4. Promote a trained tactic only after its action contract, legal runtime
   execution and matched live outcome are verified. The existing offline
   target/command metrics and failed screens do not establish a win gain.

The target is a stronger full-game bot. No current result establishes a
major win-rate gain or tournament readiness.

## Manual priority update — 26 September UTC

A subsequent deep correctness audit fixed twelve issues spanning upgrade
prerequisites and producer selection, navigation, melee targeting, Storm
friendly fire, threat maps, unavailable workers/scouts and command identity.
See [the audit and validation record](deep-performance-audit-20260926.md).
These remain development changes awaiting controlled engine/live evaluation;
the concurrent available-tech composition comparison uses its earlier frozen
DLLs and cannot validate this patch set.

The fresh three-arm Pylon-grace comparison completed twelve healthy games:
all arms lost 0/4, and frame-8400 army was essentially unchanged. Delayed
Pylon construction improved, but two candidate games missed a Core. Preserve
the isolated candidate; it is not an established strength improvement.

The recent manual work addressed a native-reproducible production accounting
failure: army shares assigned to unstarted tech suppress every affordable
available unit once opening quotas are met. The bounded correction uses the
mix supported by committed tech, after existing worker/tech reservations.
Native and Win32 regression checks passed. The completed twelve-game live
screen failed its frozen army-growth gate: candidate 5.75 at frame 8400 versus
5.25/5.0 in the same-DLL reference arms, and all arms lost 0/4. Other gates
passed, but this does not qualify a larger campaign or a strength claim. See
[available-composition-20260926.md](available-composition-20260926.md).
The correction remains unvalidated development source; its frozen experiment
is preserved. Do not combine it with the archived placement rule or promote it
based on this screen. Hourly requests remain paused.

A subsequent manual test review passed all 37 development suites and four
Win32 checks. It also reproduced and corrected the missing Robotics Support
Bay prerequisite for Reavers, which could reserve funds for impossible units.
The frozen composition comparison is unchanged. Validate the Reaver correction
separately after that run; see [test-review-20260926.md](test-review-20260926.md).

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

### PvT Core bridge and paired games — active

The [PvT Core bridge experiment](pvt-core-bridge-20260925.md) addresses a
repeated idle-Gateway window during Core construction. Its two-pair Steamhammer
screen passed functional army, worker, Dragoon-timing and runtime checks, but
both versions lost 0/2. Fresh four-pair development games on Benzene and
Destination are running under the owned sequential supervisor in
`build/bridge-zealot-generalization-20260925`. No tournament mode changed.

The [worker spending training pilots](worker-outcome-training-20260925.md)
stopped before treatment because UABTerran crashed on apparent Protodd wins.
Those games supply no qualified intervention win labels; do not restart the
unlaunched conditions or fit a worker policy from them.

### Latest population-goal cycle — completed, confirmation failed

The [population-goal experiment](production-goal-experiment-20260924.md) tested
36 omitted train games, nine development games and a separate nine-game train
holdout. The joint population/priority network failed development. A learned
worker-only linear policy passed the separate holdout, then failed unchanged
24-game validation: two games missed the per-game recall floor, and late PvP
worker-growth recall was 65.1%. No new model was exported or given control.

This two-variant cycle is closed. The next dependency is outcome-labelled
training scenarios for worker spending/recovery with qualified persistent
ownership, matched training interventions and actual army/economy trade-offs.
Do not repeat similar population-head fits or relax the failed gates. All jobs
from this cycle have exited; no arena campaign is queued. The earlier live
integration below remains evidence for its original three-unit scope only.

### Latest live integration

The production model now passes causal live-history checks, native persistent
unit-production feedback, complete callback timing, and a two-game bounded
Probe/Zealot/Dragoon control screen. The screen preserved Probe count but had
4.0 versus 4.5 reference army starts; stronger gameplay is not established.
The seed-matched full-game pilot completed: candidate 0/2 wins, reference 1/2.
Runtime and execution checks passed, but the win-improvement gate failed.
The supervisor and games have stopped; no 72-game campaign was launched.
Next, test sustained worker/composition goals and spending priorities on bounded
training material, with readiness checks throughout the controlled window.
See [production-live-integration-20260924.md](production-live-integration-20260924.md).
Building-control cancellation and the 72-game strength gate remain unqualified.
No tournament mode was changed. The earlier continuation below is historical.

### Earlier offline execution update

**Continuation:** the new concurrent production-demand policy passed its
capacity check, bounded 300-game training / nine-game development comparison,
and frozen confirmation on 24 reserved validation games. Confirmation macro F1
is 56.88%; all predeclared checks passed. A standalone Win32 export matches every
decoded quantity on 7,139 rows and runs at 0.0708ms p95 for model inference alone.
Twenty-one focused tests pass, including the offline commitment reference.
See [production-demand-experiment-20260924.md](production-demand-experiment-20260924.md)
for provenance, criteria, limitations and the current remaining work. The old
single-action classifiers below remain rejected; this uses all accepted action
logs, concurrent quantities and strictly past own-command history.

The current blocker is native/live input and persistent execution integration,
then full callback timing and gameplay evidence. Assimilator recall is only
7.35% in confirmation, so an offline pass cannot authorize unrestricted macro
control. No model has live or tournament control. The confirmation games were
used by the historical macro model and are not globally untouched.

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
The 25 September PvZ same-DLL repeat control found pre-treatment state
divergence in all four seed-matched pairs and large late-game duration
variance. Treat seed matching as a variance reduction tool, not an exact
counterfactual; calibrate a metric against same-DLL repeats and use more
independent seeds before accepting a small gameplay effect. See
[pvz-repeat-control-20260925.md](pvz-repeat-control-20260925.md).
The source-pinned 25 September build-lease diagnostic then found many accepted
Pylon orders canceled while their Probes were still travelling. A bounded
18-second travel extension failed its frozen functional screen and was retired;
the observation-only diagnostics remain. Investigate builder selection and
near-site obstruction from the archived ACTION/ENTITY/BUILDLEASE traces before
another placement change. See
[pvz-build-lease-diagnostic-20260925.md](pvz-build-lease-diagnostic-20260925.md).
The subsequent native selector diagnostic found 11 accepted Pylon orders that
ended in a hard release despite a site-eligible Probe at least 256 pixels
closer. A bounded site-builder handoff candidate reduced hard releases in two
seed-matched games but failed its frozen Pylon-construction floor; the game
with fewer Pylons had no handoff exposure. The rule was retired. Continue
with a controlled near-footprint retry diagnosis, then use repeated independent
seeds for any later gameplay effect.

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

### 3. Prove a scope improves play, then expand — native preflight passed; integration pending

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

### PvZ gateway-first opening pilot, 26 September 2026

A source-frozen four-game matched pilot against McRaveZ on Benzene and
Destination tested moving the first Gateway ahead of the Forge/Cannon and
allowing up to 11 opening Probes. All four pairs had healthy games and identical
seed/map/host inputs. The first Zealot arrived 884–1,016 frames earlier, but
the completed mobile army at frame 6,000 changed by only +1, 0, -1, and 0.
Both variants lost all four games. One candidate game never completed a
Cybernetics Core before defeat. The pilot failed its frozen functional gate;
`PROTODD_PVZ_GATEWAY_OPENING` stays OFF and no larger campaign is justified.
The archived machine report is
`build/pvz-gateway-opening-20260926/pilot-v2-report.json`.

The next useful hypothesis is a continuous opening spending schedule: keep
Gateway production active through the Forge/Cannon response, then make the
Cybernetics Core transition on time. Check issued unit/build orders and paid
resource commitments, not only the first defender timing. Preserve the
pressure fallback while testing this; one early unit cannot establish a
stronger opening by itself.

The bounded follow-up added a six-completed-Zealot and committed-Core
checkpoint before the natural, still behind the OFF-by-default opening flag.
All four candidate games were healthy and had matching seed/map/host records.
The Core completed 1,476–3,214 frames earlier, and mobile army at frame 6,000
rose by 1, 2, 2, and 2 units. Neither variant won a game. The arena endpoint
changed from port 1385 to 1386, so the frozen review's exact common-input
hash check failed; the diagnostic rows are archived separately in
`build/pvz-gateway-opening-20260926/pilot-v3-diagnostic.json`. This is useful
mechanism evidence, not a passing promotion result. No larger run is warranted
from it. The next loss review should examine why a stronger early screen still
collapses to the later Zerg army, including hydralisk splash and air coverage.

The follow-up six-Zealot pressure window did not establish a gain. Its first
attempt aborted after a launcher crash, and the retry exposed an expansion
override that recalled the army immediately after a forward order. After
repairing that override, the four-game pilot had three decisive losses and
one 30,000-frame cap with both bots reporting no win. The frozen health and
pairing gate failed, so that experiment stays OFF. See
`build/pvz-gateway-opening-20260926/pilot-v5-report.json`.

A separate default-on squad cohesion repair was tested against the prior
four-game reference with identical non-DLL inputs. In the one reference game
with understrength MainArmy attack orders during enemy contact, the count fell
from four to zero. No early losses were added; completed mobile army at frame
8,400 changed by 0, -1, +1, and 0. Both versions lost all four games. Its
functional gate passed, but its strength gate did not, so this is a verified
movement repair rather than a measured win-rate gain. See
`build/squad-cohesion-20260926/pilot-report.json`.

The next frozen pilot tested Hydra response spending. The reference repeatedly
reserved an Observatory and Observer against Hydralisks with no Lurker in sight,
while it produced no Reaver in four losses. A separate candidate reserved the
Robotics/Support Bay/Reaver chain against ground mass and reserved detection
only after observing a Lurker. Core tests passed. All four matched candidate
games were healthy but followed different Zerg openings, never reached a Hydra
response, and lost. The predeclared exposure gate failed, so the change was
restored to the frozen source state and remains an archived hypothesis rather
than a win-rate improvement. See `build/hydra-splash-20260926/pilot-report.json`.

Those four games exposed a more immediate placement bottleneck. The bot could
reserve minerals for its second Cannon, then choose an unpowered expansion as
the least defended Nexus and return without examining the powered main. One
loss had repeated `build-no-location-placement-search` reports from frame
8,833, but its second Cannon only began at frame 10,822 after the expansion
was lost. The next isolated candidate skips unpowered Nexuses when selecting
where to build defensive Cannons or Batteries. All four matched games were
healthy and the second Cannon started 3,070–4,737 frames earlier, but total
no-location failures fell only from 83 to 45 because one candidate game made
39 late attempts under attack. Both versions lost 0/4; the predeclared
functional gate failed. The source remains a development candidate while the
failure is repaired, with its exact evidence in
`build/powered-defense-20260926/pilot-report.json`.

The next isolated candidate requests a Pylon at the natural as soon as its
Nexus is committed and no ground attack is approaching. In the placement
candidate, the natural Nexus began around frames 6,400–6,700, yet its first
nearby Pylon appeared only around 9,100–9,700 in three games and never in the
fourth. The separate four-game comparison advanced natural power by at least
1,000 frames in two games, but its frozen gate required three; it delayed the
second Cannon too much in two games and still lost 0/4. See
`build/natural-power-20260926/pilot-report.json`.

The failure has a concrete executor cause. In one match the natural Pylon was
requested at frame 6,985 and an accepted build order was issued at 7,123. At
frame 7,315 the Probe was still moving, the site was legal and reachable, yet
the generic eight-second Pylon hard lease canceled the command. This happened
repeatedly; construction finally began at frame 9,160. The next isolated
pilot gives only that planned remote natural Pylon an 18-second travel lease,
while keeping the stalled-builder escape. Its frozen comparison and review
are under `build/natural-power-lease-20260926`. The four-game review passed
its functional gate: natural Pylon starts advanced at least 1,000 frames in
three games, moving-builder hard lease expiries fell from 32 to 3, second
Cannon timing stayed within the declared limit, and frame-8,400 mobile army
was unchanged in every pair. Both versions still lost all four games. This is
an executor repair, not evidence of improved win rate; the remaining priority
is to produce a timely counter to Hydra and Mutalisk mass without losing the
early defensive screen. See `build/natural-power-lease-20260926/pilot-report.json`.

A second Hydra response pilot used the natural-power-lease version as its
reference. Seeing Hydralisks or their Den committed a Robotics/Support Bay/
Reaver chain after a small defensive screen, while skipping optional Stargate
spending unless Zerg air tech was seen. All four matched games were healthy
and three candidate games saw Hydras. Support Bay began at least 720 frames
earlier in two exposed games, but only one Reaver arrived within 4,800 frames
of the first Hydra. Both versions lost all four games, so the frozen functional
gate failed. In one loss, 12 Hydras arrived while Protodd still lacked a
completed Cybernetics Core; reactive splash tech was too late. The change was
restored to the lease baseline. See `build/hydra-commit-20260926/pilot-report.json`.

The next isolated pilot moved that Core checkpoint earlier, once a committed
PvZ natural had two completed Cannons and four completed Zealots and no ground
attack was approaching. All four matched games were healthy, but Core began at
least 1,000 frames earlier in only one game. The second Cannon was delayed
1,437 frames in another, beyond the declared 720-frame limit. Both versions
lost all four games. The checkpoint was restored to the lease baseline; see
`build/core-checkpoint-20260926/pilot-report.json`. The next investigation
should measure when Gateways are idle, how many Zealots are completed before
the first large Hydra wave, and whether the two-base economy funds more mobile
units without delaying defensive power.

The reference logs show that at frame 8,400, two Gateways were idle in two
games with five completed Zealots and 152–166 minerals banked. The minute-based
Zealot target was already satisfied. The next isolated candidate raises that
target to at least eight only after the second Cannon has started. Its frozen
four-game comparison and defensive timing gate are under
`build/fortified-zealot-20260926`. All four matched games were healthy. The
candidate had one more completed army unit at frame 8,400 in three games,
second-Cannon delays of 60, 3, 368, and -391 frames, and no additional early
losses. Its declared functional gate passed, but both versions lost all four
games. Core construction was delayed by over 2,000 frames in several candidate
games, so this remains a production finding rather than a strength-validated
opening. See `build/fortified-zealot-20260926/pilot-report.json`.

The follow-up candidate reserves the first Core at priority 100 after the
second Cannon starts and five Zealots are complete, ahead of further Zealot
cycles. Its source, reference inputs, and pass criteria are frozen under
`build/balanced-fortress-20260927`. All four matched games were healthy and
completed army at frame 8,400 rose by one in three games; Cannon two stayed
within the limit. But the Core never started in one game under sustained
emergency pressure, the frozen functional gate failed, and both versions lost
all four games. The combined opening was restored to the lease baseline. See
`build/balanced-fortress-20260927/pilot-report.json`.

The next priority is the emergency hold itself. In the exposed failure,
Protodd had five completed Zealots, one completed Cannon, an unfinished second
Cannon and no Core at frame 8,400 while the emergency plan consumed production
capacity. Ground pressure kept the planned quiet-window tech checkpoint from
firing. A useful follow-up must measure worker allocation, Pylon survivability,
Gateway cycles, and whether defensive buildings finish and remain powered
through the first large Hydra wave. Improvements must win healthy paired games
before they can be called strength gains.

A new isolated pilot asks for a third Cannon after the first two are committed
at a two-base PvZ defense. The existing placement code chooses the least
defended powered Nexus, breaking ties toward the enemy, so the added structure
should reinforce the forward natural. Its frozen four-game inputs and pass
criteria, including a Core-delay limit, are under `build/third-cannon-20260927`.
All four matched games were healthy. Cannon three began before frame 9,500 in
three games, but Cannon two was delayed 2,807 frames in the fourth, and both
versions lost all four games. The frozen functional gate failed; the added
goal was restored to the lease baseline. See the frozen pilot report. Further
static defense work should address natural placement and construction timing
directly, not only the total Cannon count.

These four-game screens are diagnostic, not causal win-rate estimates. The
arena fixes map, seed, host and all non-DLL inputs, but early actions can still
diverge before a conditional candidate rule fires. In the third-Cannon game 0,
the initial Nexus had a different unit ID and early Pylon/Zealot timings had
already diverged before Protodd possessed two Cannons. Future claims need a
larger, disjoint multi-opponent campaign with confidence intervals and replay
review, especially when a candidate changes an opening timing by only one or
two production cycles.

The trained Protodd target ranker received its first live four-game PvZ screen
against a newly run heuristic reference with identical non-DLL inputs,
including the frozen v2 weights and mode file. The candidate loaded the model
and scored legal targets in all four games; its functional integration gate
passed. Both arms lost all four games, so the trained scorer stays opt-in and
unpromoted. See `build/trained-target-screen-20260927/pilot-report.json` and
`docs/trained-tactics-20260926.md`. The next trained-tactics step is to compare
model and heuristic choices on the exact same live legal candidate sets,
identify harmful disagreements, and fit or gate a narrower decision before
another win-rate trial. Broad tactical control is not justified by this screen.

The same-set target diagnostic under `build/target-disagreement-20260927`
recorded 652 disagreements in 3,581 live decisions (18.2%) across two healthy
losses. There were no broad worker/building-versus-combat switches or immediate
threat abandonments by its defined counters. The model remains opt-in; the next
trained step is a replay-level review of within-category choices before
another fit or outcome screen.

The scouting review found a separate timing gap: after the opening Probe left
the enemy main, a follow-up Probe repeatedly visited the enemy natural. In one
reference loss the enemy main was last seen near frame 3,598, the natural was
rechecked around frame 6,720, and the Hydralisk Den was not discovered until
around frame 8,880. A bounded stale-main priority change is under a frozen
four-game development comparison in `build/scout-main-tech-20260927`.
That comparison was healthy but failed its functional gate: zero of four
candidate games revisited the main before frame 8,000, one extra Probe died
before frame 8,400, and both arms lost all four games. The stale-main score
bonus was reverted. Travel traces also revealed that Destination's nearest
enemy "natural" marker had no resources. The working source now excludes such
markers and keeps a timed-out follow-up Probe leased until it returns home;
those two fixes are under a separate frozen comparison in
`build/scout-safe-return-20260927`.
The four-game safe-return comparison was healthy and issued follow-up return
commands in all four games. It failed its development gate: one extra Probe
died before frame 8,400 and both arms lost 0/4. The reviewer was tightened
after game 0 to distinguish late opening withdrawal from follow-up return, so
the return-order count is exploratory. That extra death was an opening
scout at frame 4,610, before follow-up return control activated; identical
seeds did not keep the opening trajectory fixed. The return lease remains a
tested control fix, not an established win-rate gain. The natural filter in
that DLL still selected a zero-mineral marker on Destination, so the source
has since been tightened to require remaining minerals and its regression
test covers a stale nonzero patch count. This correction has no live result
yet.

The next bounded PvZ intervention addressed the whole anti-Hydra path. The opt-in
`PROTODD_PVZ_EARLY_SPLASH` build raises Core after the opening mobile/static
screen, then demands Robotics Facility, Support Bay and one Reaver as soon as
two Hydras or a Den are known, unless Spire/Mutalisk evidence or a main breach
calls for a different response. Native strategy and spending tests pass.
The default DLL has this option off. The same-input eight-game screen in
`build/pvz-early-splash-20260927` was healthy, but both arms lost all four
games. Only one candidate game saw the eligible Hydra signal early enough;
its army collapsed before Robotics could start. A Reaver completed in another
game at frame 13,694, then died 45 frames later with no Scarabs fired as the
base was overrun. The functional and decisive-win gates both failed. Do not
promote this option or repeat another reactive splash timing variant without
a materially earlier defensive mechanism and broader opponent evidence.

### PvP fog detection screen (in progress, 2026-09-27)

A fresh four-game PvP/PvZ baseline with the current default DLL was structurally
healthy and lost 0/4. In its second BananaBrain game, a first Dark Templar
appeared at frame 9,707; the Observatory did not finish until frame 10,370.
Protodd still had 13 combat units but no Probes by frame 10,800. The known
enemy two-Gateway army did not satisfy the older quiet-tech-gap rule, and the
six-Dragoon checkpoint suppressed the lower-priority Observatory goal.

The opt-in `PROTODD_PVP_FOG_DETECTION` experiment reserves one Observer after
Robotics is complete when a two-Gateway enemy main has stale tech information,
our home has a completed Cannon and a six-unit mobile screen, and no hard main
breach is occurring. Its native plan/spending test passes. The default option
remains off. Frozen DLL SHA-256: reference
`52C5D39DD5D7E125BE4C6F778FB045BD1620BCB7339929DCE700784991500012`,
candidate `98590F6DF4B1C0851EC17E4BB4E537087F97E7A94D53EFB2619747D9F43AD009`.
The same-input BananaBrain screen was prepared for four games per arm across
Benzene and Destination, alternating sides, under
`build/pvp-fog-screen-20260927`. The reference attempt stopped after one
healthy reported loss and must be rerun; the candidate has not started.
The functional gate is Observatory/Observer ahead of the first DT in eligible
games without a new opening collapse. Promotion requires a real win gain with
healthy paired reports; earlier detection alone is insufficient.

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
