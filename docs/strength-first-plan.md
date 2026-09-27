# Strength-first development plan — 24 September 2026

## Current checkpoint — 27 September 2026

The latest default source includes the clearer in-game status overlay, combat
and scouting correctness fixes, and several bounded strategy changes. The
experimental PvZ and PvP tactics are build options that remain **off** in the
default bot. The overlay improves diagnosis; it is not evidence of stronger
play. At this checkpoint all 42 development test
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

Current order of work (updated after the 27 September PvZ screens):

1. Stabilize the PvP opening against early Zealot and Dragoon pressure.
   The fresh four-game default reference lost 0/4 without a logged Dark
   Templar. Measure the first mobile army, first Reaver, powered Cannon
   coverage, and Probe survival before spending on optional detector count.
   A fog-only Observer rule missed its target timing in an earlier four-game
   candidate; keep it off and avoid another copy of that screen until a
   genuine early cloak exposure is available.
2. Address the PvT natural-base collapse and stalled counterpressure against
   Steamhammer mech. The larger mobile screen lost all four controlled games;
   a closer third base built in one paired game but also did not win. Test the
   frozen policy's repeated late `defend` decisions as a separate controlled
   factor, then inspect mine coverage, engagement estimates and Reaver firing.
   Compare frozen packages on both maps and starting sides.
3. Attack the earliest repeatable loss mechanism: opening mobile defense,
   protected tech/expansion transitions, army cohesion and detector coverage.
   Choose one bounded change at a time and compare frozen binaries in healthy
   matched games on both starting sides.
4. Promote a trained tactic only after its action contract, legal runtime
   execution and matched live outcome are verified. The existing offline
   target/command metrics and failed screens do not establish a win gain.

The target is a stronger full-game bot. No current result establishes a
major win-rate gain or tournament readiness.

The repository now carries the complete 59,391-file cwal.gg source replay
snapshot as six Git LFS archives under `replays/cwal-source/`, alongside the
frozen split manifest. Restore defaults to the 11,183 approved training
games; the source archives also preserve validation, test, and unassigned
games and must not be treated as additional training examples without a new
split review. See `replays/README.md` for the restoration command and hashes.

### PvP default reference on both maps — 27 September

`build/pvp-fog-screen-640-20260927/reference` completed four healthy
BananaBrain games with both map and host sides, all normal losses at frames
13,673, 13,022, 22,105 and 8,589. All had observed opponent activity and no
runtime errors. No enemy Dark Templar was logged. In game 0, a large Dragoon
force arrived while Protodd's first Reaver completed at frame 10,164; the
second completed at 11,354, during the first major attack.
In game 1, enemy Zealots reached the main before the first Reaver completed
at 10,610; all Probes were gone by frame 10,800. In game 3, a two-Gateway
Zealot flood reached the main before a Cybernetics Core or Cannon completed;
the first Cannon under construction was destroyed, and all Probes were gone
by frame 7,200. Game 2 ran longer but also lost.
The opt-in fog candidate from the earlier 320-slow-frame package also lost
0/4 and did not trigger before its observed Dark Templar. A new 640-frame
candidate package is prepared but unplayed: this reference has no cloak
exposure to test its intended benefit. No matched strength claim is made for
that candidate, and the option stays off. The next bounded experiment should
protect the first mobile screen and first Reaver timing, with a healthy
four-game reference and safeguards for workers, early losses and detection.

The first bounded PvP response is `PROTODD_PVP_SCOUTED_TWO_GATE_ANCHOR`.
Game 3 showed both enemy Gateways by frame 2,068, but Protodd's Forge began
at 3,981 and its first Cannon began at 4,788. The Cannon was destroyed at
5,528, about ten frames short of its build timer. The option requests one
Forge as soon as both enemy Gateways are known, our second Gateway and first
Zealot have started, at least 12 Probes exist, and no enemy ranged tech or
hard breach is visible. The existing first-Cannon rule then owns placement
and spending. Its native plan test and Win32 build passed; first DLL SHA-256:
`553069308CEF1101A3B99456F9FABFC008E7813DD17EAF73CB433D0592494720`.
The frozen first four-game candidate at
`build/pvp-early-two-gate-screen-640-20260927/candidate` differs from the
healthy PvP reference only in the DLL. All four games had healthy normal
losses (0/4 versus reference 0/4), and the new early goal never activated.
In candidate game 3 the scout remembered two enemy Gateways at frame 3,600,
but the generic quiet-opening branch remained active because it counted only
completed Gateways for pressure. Protodd built the Core and delayed its
second Gateway until frame 5,696; the Forge began at 5,338, later than the
reference. This is a failed functional pilot, not evidence for an early
Forge improvement.

The revised option treats two scouted enemy Gateways, including one still
warping, as opening-pressure evidence before frame 7,200. That enters the
existing mobile two-Gateway response before considering its Forge anchor.
The native regression test covers an incomplete second Gateway; the Win32
DLL builds and has SHA-256
`5A07E75AEB371F17DAAE4624970AABAAAC4B96A87E2C3724CFEDB1B6A540247A`.
The revised four-game package at
`build/pvp-early-two-gate-screen-640-20260927/candidate-v2` verified and
matched the reference's maps, sides, seeds, and all non-DLL inputs. Both
players reported normal ends, enemy activity was observed, and no runtime
errors were logged. It lost all four games at frames 15,719, 16,339, 21,299,
and 20,307, versus four reference losses. The new branch never activated:
none of the revised games had two enemy Gateways in the scout's known
composition before frame 7,200, while reference games 1 and 3 did. Opponent
opening variation makes this an unexposed live test of the revised condition,
not evidence that it holds or fails against an actual early two-Gateway rush.
The option remains off. The native test establishes the planned response for
that observation, but a promotion needs live exposure and a win gain without
extra opening collapses. Next PvP work should address the mobile opening
screen that fails across both one- and two-Gateway openings.

### Current-default PvT baseline against Steamhammer — 27 September

`build/current-strength-steamhammer-640-20260927` pinned the default DLL
(`52C5D39DD5D7E125BE4C6F778FB045BD1620BCB7339929DCE700784991500012`)
for four games on Benzene and Destination, both starting sides. The package
verified before and after play. Both players reported consistent normal ends
without crashes or timeouts; the Protodd telemetry recorded enemy activity in
every game. All four were losses, ending at frames 25,019, 33,265, 27,964
and 26,445. This is a development sample against one opponent, not a ladder
win-rate estimate.

The two mech-heavy games exposed a repeatable natural-base defense problem.
In Destination game 1, Protodd had 44 Probes, two Nexuses and 27 army units
at frame 24,000, yet Terran's growing Factory force pushed through its
natural; by frame 28,800 the state held no Probes and one Nexus while 28
army units remained. A natural defense squad was present and its simulation
still predicted a favorable local fight shortly before the collapse. In
Benzene game 2, 50 Probes, two Nexuses and 31 army units at frame 19,200 fell
to 44 Probes, one Nexus and eight army units by frame 24,000. The opponent
had observed Siege Tanks, Vultures and Spider Mines. These traces suggest
both engagement evaluation and defense execution merit inspection; they do
not isolate a single cause. Game 0 against a bio-heavy opening also lost an
expansion and most of its army late. Game 1's main mineral field was depleted
and the natural nearly depleted by frame 26,400, while the requested third
Nexus had not completed. Review third-base protection alongside tactical
defense, without assuming that an earlier expansion alone would survive.

The first PvT follow-up tested a larger mobile screen against observed mines
or Tanks and recalled a scouting Observer while home squads needed mine
detection. The frozen broad candidate
`build/pvt-natural-defense-screen-640-20260927/candidate-v2` was structurally
healthy and differed from the prior Steamhammer reference only in Protodd's
DLL. It won 1/4 versus 0/4, but Steamhammer chose different openings despite
matched maps and game seeds: two candidate losses faced an early two-Barracks
Marine rush absent from their paired references. The sole win cannot be
attributed to the defense rule. A follow-up binary aligned mine recall with
the squad's eight-second mine memory.

To expose that follow-up to a consistent threat, an isolated Steamhammer
template was configured to choose its `Vultures` opening, with evaluator
selection disabled. Its four-game reference at
`build/pvt-controlled-mech-screen-20260927/reference` and candidate at
`build/pvt-controlled-mech-screen-20260927/candidate` matched every non-DLL
input, map, side and observed game seed. Both campaigns were healthy, with
opponent activity, normal adjudication and no bot errors. Both lost 0/4.
Reference ends were frames 23,190, 40,209, 34,846 and 24,771; candidate ends
were 28,863, 36,179, 28,243 and 36,055. The candidate sometimes delayed a
loss but still lost the mineral lines while combat units remained. Its source
changes were removed from the default bot; these experiments do not establish
a strength gain.

The next bounded issue was expansion placement. The generic site selector
required gas for every Nexus, although its comment intended that rule only
for the natural. In the mixed mech game Protodd chose a third at 320×240,
ground route 3,079 from the main, while a mineral base at 416×1264 had route
1,909. The distant third split the defense. A revised selector keeps the gas
natural, then lets a substantially closer mineral base beat a far gas flank
while retaining a modest gas preference. Native regression and Win32 build
passed. The controlled four-game reference and revised-site candidate matched
all non-DLL inputs, map, side and game seed; both campaigns ended normally with
opponent activity and no bot errors. Both lost 0/4. Reference ends were frames
23,190, 40,209, 34,846 and 24,771; candidate ends were 47,370, 32,614,
25,639 and 25,484. In paired game 0 the third Nexus moved from the far gas
base to the closer mineral pocket and the bot survived much longer. In games
1–3 the candidate did not complete a third Nexus before defeat, so those games
do not test the site's defensive value. Keep this bounded placement correction
as a route-selection fix, without claiming a win-rate gain.

The longer game exposed a separate late-game bottleneck. At frame 28,800 the
candidate had three Nexuses and 66 Probes, and at frame 38,400 it still had
four Nexuses and 54 Probes, but the opponent had grown from three to eight
Command Centers and from seven to fourteen Factories. The bot's posture was
`Hold` for much of that interval. `PolicyTrace.log` shows that 24 of 36
decisions from frames 24,000–38,400 were forced `defend` (`mask=8`) because
the embedded frozen policy saw at least two nearby enemies; the runtime then
overrode the strategy's pressure posture with `Hold`. This is a concrete
counterpressure hypothesis, not proof that attacking would win. Compare a
frozen-policy-off arm against the same binary and opponent before changing
the default policy guard.

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

A train-only opening slice in `build/human-hydra-zvp-20260927.json` selects
the same 256 frozen ZvP games and then the 11 with at least ten visible
Hydralisks at frame 9,600. Those surviving perspectives had a median of 33
Probes, five Zealots and two completed Cannons at frame 8,400; by frame
10,800 the median was five Cannons. Separately, in the replay-opening
candidate's early Zergling loss on Destination, Protodd had 37 Probes, five
Zealots and two Cannons at frame 8,400, then faced 21 visible Zerglings
near frame 9,600 and lost its natural. These are different attack types;
the small selected Hydra sample is descriptive and cannot establish a
counter to that Zergling loss. A defensive experiment should measure
powered and completed natural Cannons before contact, preserve the first two
Cannon start times, and avoid the previously observed second-Cannon
placement delay. Repeating a simple third-Cannon count goal would not
address that failure.

The new opt-in powered-Cannon screen waits for two completed Cannons, a
completed Core, two completed Nexuses, at least 26 Probes and four Zealots
before requesting a third Cannon. It requests a fourth only after the third
finishes, within frames 7,200–15,840 and outside a hard main-base breach.
The build placer requires a powered Pylon near the chosen Nexus and spreads
Cannons toward the least-defended powered base. Native tests confirm that an
unfinished second Cannon or unfinished Core vetoes the extra goal, and the
first extra Cannon receives a funded reservation; the Win32
candidate compiled. The four-game frozen campaign at
`build/pvz-powered-cannon-screen-640-20260927/candidate` matches all 415
non-DLL replay-opening inputs after normalizing arena ports. Its isolated
four-game screen started on the shared local clients after the original
mineral-fallback campaign completed. Review completed Cannon timing, natural survival,
early economy and valid paired wins before promotion.
In the three valid replay-opening reference games, the Core finished at
frames 11,112, 7,137 and 10,417. Only the early Zergling loss (game 1) has
the Core, both Cannons, four Zealots and enough Probes in place near frame
7,200 for this staged rule to add a Cannon before frame 9,600. The other
seeds test later defense and cost, not the early Zergling timing hypothesis.
In that Zergling loss, the first two Cannon coordinates were both near the
starting Nexus (2,112x3,824); the natural Nexus at 992x3,472 had a completed
Pylon but no Cannon. The second Cannon began before natural construction
finished. By the new rule's eligibility window the natural is powered, so
the placement code should choose its undefended Nexus for the third Cannon.
The live screen must verify that actual position; total Cannon count alone
cannot establish natural protection.
Game 0 of the powered-Cannon screen was a valid loss at frame 12,154. Its
first two Cannons started at frames 2,886 and 4,845, earlier than the
replay-opening reference's 2,909 and 5,520, but its Core never completed;
the new rule never became eligible. A 33-unit Zerg force was visible at
frame 9,600 and the natural fell. This early divergence is not evidence that
the extra Cannon rule harmed or helped the game.
Game 1 shows the intended build mechanism: the first two Cannon starts were
2,775 and 5,676 versus the reference's 2,786 and 5,770. The third started
at 7,385, completed at 8,206 by the powered natural (1,088x3,584), and
the fourth completed at 9,251 by the main. The reference had no Cannon
within 416 pixels of the natural by frame 9,600. The opponent's observed
composition had already diverged by frame 4,800, before this rule acted:
the reference later sent a large Zergling wave while this trial showed a
Hydralisk Den and no comparable attack at frame 9,600. Any survival or
economy difference cannot be credited to the Cannons from this pairing.
Game 1 nevertheless lost at frame 24,988. It had 71 Probes, four Nexuses,
48 army units and full 400 supply at frame 19,200. At frame 20,400 a large
Hydra wave left 30 army units and 324/400 supply, while the bot held 4,271
minerals, only 32 gas and ten idle Gateways with no macro action. By frame
21,600 all Gateways were busy with emergency production, but the delayed
cycle could not stop the later collapse. This exposes a specific combination
to test: keep the successful natural Cannon timing, then apply the existing
mineral-surplus fallback to fill the newly open supply during a gas-starved
Hydra battle. The combined behavior has not been screened and cannot be
inferred from either isolated loss.
The opt-in combined DLL compiles with both rules. Its prepared four-game
campaign is `build/pvz-cannon-mineral-combo-screen-640-20260927/candidate`,
DLL SHA-256
`7FEB5FF9B005BEE5E6A6D40CDE2ED0CB3ECAB48AB203F53A2947BC73141E2B8B`.
All 415 non-DLL inputs match the replay-opening and Cannon screens after
normalizing ports. Its isolated screen began after the Cannon run completed.
In game 0 the fallback issued 16 accepted extra Zealot training commands
starting at frame 17,281, but the game still lost at frame 22,043. At frame
16,800 Protodd had 64 Probes on four Nexuses and only seven combat units;
Storm had not finished. By the time the fallback filled some Gateways, the
enemy attack was already removing bases. A future experiment should test
whether a temporary two-base army floor before further expansion is worth
more than another economic base, while preserving the early Core and Cannons.
Earlier worker-cap and eight-Zealot targets delayed key tech; any new gate
must activate only at this later, demonstrated vulnerability window.
Combined game 1 lost at frame 16,587 before the mineral rule's start; its
third and fourth Cannons completed near the natural, but a 31-Hydralisk
opponent history preceded the economic collapse. Game 2 lost at frame
13,332 before the third Cannon finished or the mineral window opened.
These two losses did not test the added spending behavior.
Game 3 later lost at frame 22,012 without fallback activation. The combined
campaign finished with four valid losses. Against the four valid isolated
Cannon games, all frozen inputs and actual matches aligned, so the strict
review was fully paired. The Cannon placement rule remained functional,
but the spending rule fired only in game 0 (16 accepted extra Zealot orders),
the high-bank idle reduction gate failed, and wins stayed 0/4 versus 0/4.
See `build/pvz-cannon-mineral-combo-screen-640-20260927/mineral-review.json`
and `cannon-review.json`. Keep this combination off.

A further isolated tactic is a proactive first Reaver after the *third*
Cannon completes. The rejected reactive early-splash rule
waited for Hydra evidence and produced a first Reaver at frame 13,694 in a
collapse game; it died before firing. This option reserves Robotics at
priority 104, Support Bay at 103 and the first Reaver at 104 after the
completed third Cannon/Core/two-base safety checkpoints, while raising gas
workers to at least six. It deliberately comes before the fourth Cannon
and optional Stargate/Citadel spending, but cannot delay the first three
Cannons. Native goal and funding tests plus the Win32 build pass. The frozen
four-game campaign at `build/pvz-proactive-reaver-screen-640-20260927/candidate`
has DLL SHA-256
`C3C9A3EC8F42A907EE1A17EAEC88CF82329CD4B7C5DDD199799A987F084DB995`
and the same 415 non-DLL inputs as the isolated Cannon screen. Its arena
screen began after the combined mineral campaign completed. Measure first
Reaver completion, loaded Scarabs,
natural survival and valid wins before promotion.
Its game 0 lost at frame 15,006 against a Mutalisk branch. At frame 11,160
nine Mutalisks were visible while Protodd had 16 Zealots, two Cannons, an
unfinished Stargate and no active gas workers despite two completed
Assimilators. The third-Cannon/Reaver checkpoint never became eligible;
this game does not test proactive splash. A separate anti-air question is
whether the existing five-Cannon goal at priority 90 should become a funded
emergency goal as soon as Mutalisks are observed, ahead of gas-starved
Dragoons and routine Zealot spending. That change needs its own screen.
Game 1 reached the new tech checkpoint: Robotics completed at frame 9,742,
Support Bay at 10,276, and the first Reaver at 12,128, 1,728 frames before
the isolated Cannon reference's first Reaver. But the third Cannon in this
run was placed by the main; the first natural Cannon completed at 10,031
versus 8,206 in the reference. The opponent's opening had already diverged,
and the natural-defense delay exceeds the predeclared 300-frame safeguard.
The revised review records natural position and completion timing so a
Reaver speedup cannot conceal that cost. Completing a third Cannon alone
does not guarantee natural coverage; the build placer selected the main.
Game 1 still lost at frame 27,995. The first Reaver produced a Scarab by
frame 12,264 and received its first accepted attack at 14,064, then
survived until frame 23,340; Storm was cast repeatedly. This differs from
the earlier Reaver that died without ammunition, so further gains require
surviving the larger midgame ground army rather than merely completing the
first splash unit.
Game 2 also built a Reaver, but it finished at frame 15,966 versus the
isolated Cannon reference's 13,914. Hydralisks were seen at 14,512; the
first Scarab ammunition appeared at 16,104 and the first accepted Reaver
attack at 16,872. The first natural Cannon completed at 13,855 versus
12,121 in the reference, and there was one additional early loss. Game 3
lost at frame 12,464 without a Reaver or a completed natural Cannon.
All four candidate games were valid losses. The strict paired review at
`build/pvz-proactive-reaver-screen-640-20260927/review.json` matched all
four games and found 0/4 wins in both arms. No candidate Reaver met the
predeclared frame-12,000 timing, and both the first-Cannon and natural-Cannon
safeguards failed. The functional and win gates failed. Keep proactive
Reaver off. The next experiment should address a concrete spending or
defense failure without assuming faster Reaver tech is the answer.

An opt-in `PROTODD_PVZ_ARMY_FLOOR` screen now tests the two-base spending
bottleneck separately from Reaver tech and the earlier mineral fallback.
After frame 9,600 and only with two completed Nexuses, two Cannons, Core,
two Gateways and at least 26 Probes, it compares completed ground defenders
with one per three Probes (bounded to 12–22). While below that floor, it
defers another Nexus, caps routine Probe growth at 44, and reserves up to
four Zealots when at least 600 minerals are banked. Known Mutalisks or a
strong air threat veto this ground-only branch. Its native test and Win32
build pass. The DLL SHA-256 is
`E0751566D304D85B66A38BFF262141EF1297953048468135706F66436C49F5EE`.
The four-game frozen campaign at
`build/pvz-army-floor-screen-640-20260927/candidate` has the same 415
non-DLL inputs as the isolated powered-Cannon screen after normalizing
ports. The screen will require an observed floor trigger and funded Zealot,
first two Cannon starts and natural Cannon completion within 300 frames of
reference where observed, no added early losses, and a valid paired win
gain before more games. The rule is off by default.
The screen finished with four valid losses at frames 13,115, 12,743,
12,836 and 26,414. The strict review at
`build/pvz-army-floor-screen-640-20260927/review.json` paired all four
games against isolated powered Cannons and found 0/4 wins in both arms.
The floor appeared in games 0, 1 and 3, but only game 3 had the minerals
to fund its new action: 11 accepted extra Zealot training orders. It had
20 army units and 44 Probes on two bases at frame 12,000, then 58 army
units at frame 19,200. A Reaver and High Templar completed later, and four
Storm casts were accepted. Zerg still destroyed every base by frame
26,414. The reference for that map ended at 11,999, but its opponent
opening diverged; the longer survival cannot be attributed to this rule.
Game 2 never reached the army-floor checkpoint, and its second Cannon
started at frame 7,867 versus 5,492 in the reference. That pre-intervention
divergence fails the strict Cannon-start safeguard without showing that
the floor caused the delay. Natural Cannon completion met the timing
safeguard where both games observed one; no added early losses occurred.
The functional and win gates failed, so this option stays off. The larger
army in game 3 suggests the next loss review should inspect force position,
engagement timing and composition against the late Zerg mass before another
economy quota experiment.
That loss reached 41 Zealots, 12 Dragoons, two High Templar and one Reaver
at frame 19,200, with 1,459 minerals, 195 gas and 58 combat units in the
state report. The opponent history contained 71 Hydralisks and five Lurkers;
27 enemy army units were currently visible. By frame 21,600 Protodd had
19 combat units and only one Nexus, after losing the Reaver and Templar.
The next bounded diagnosis should check where those splash units stood,
which engagements depleted the army, and whether reinforcement and supply
timing could have protected the bases. The first attack at frame 11,976
shows that the failure was not simply a missing attack order.

The same loss exposed a detector allocation bottleneck. A Lurker was
observed near the natural at frame 14,109. At frame 16,416 two Observers
were complete, but the escort planner kept one for scouting while a
24-unit main army and a second squad had detection-blocked advances.
The newly completed Observer received a scout-travel order away from home.
At frame 19,200 three Observers existed, yet a lone Reaver main-army squad
was still detection-blocked while two escorts covered other squads. The
Reaver did attack later, so this is a lost movement and allocation window,
not proof that it never fought.

An opt-in `PROTODD_PVZ_DETECTOR_SURGE` tactic releases the scouting reserve
when a recently seen Lurker is within 900 pixels of an owned base and
ground squads need detection. It gives larger detection-blocked squads
priority over small guards, then returns to the normal scouting reserve
when the threat moves away. The native allocation test and Win32 build
pass. The DLL SHA-256 is
`5D258D5499468603CE86CC2F2BD223F77B4CC3722C73E5D89EB76446D97291F5`.
The frozen four-game package at
`build/pvz-detector-surge-screen-640-20260927/candidate` matches all 415
non-DLL inputs of the army-floor run after port normalization. Compare
escort movement, detection-blocked main-army time, Reaver attacks, early
defense and valid wins; an observed improvement in detector allocation
alone does not promote this combination without a win gain.

The completed detector screen has four matched, normal games with identical
non-DLL inputs; both versions lost 0/4. The candidate made escort orders after
seeing Lurkers in games 0, 1 and 3, but only game 3 exposed Lurkers in both
arms. In that game, detection-blocked main-army unit samples fell from 33% to
31% over frames 14,000–21,000. Cannon start timing and early army losses met
the safeguard. Games 0 and 1 cannot isolate a detector effect because the
reference did not observe a Lurker in the review window. The functional gate
passed, but the win gate failed, so the rule stays opt-in and no larger PvZ
screen follows. The frozen report is
`build/pvz-detector-surge-screen-640-20260927/review.json`. The game-3
candidate survived to frame 30,754, but this screen does not establish the
detector rule as its cause. Redirect effort to matchup baselines and the
earliest recurring losses instead of more closely related PvZ variants.

The isolated Cannon screen's game 2 also lost (frame 25,391). Its third and
fourth Cannons completed near the natural at frames 12,121 and 13,326.
By frame 20,880, Zerg had shown 95 Hydralisks across the game and Protodd
had 22 army units, 5,555 minerals, ten idle Gateways and a supply deficit
after a Pylon and Nexus were destroyed. The combination of resource spending
and base survival now matters as much as static-defense count. Game 3 lost
at frame 11,999 despite a natural Cannon completing at 8,094 and a fourth
at the main completing at 9,424. All four
candidate games were valid losses. The strict review at
`build/pvz-powered-cannon-screen-640-20260927/review.json` matched games
0–2 to the three valid replay-opening reference games. The reference's
excluded fourth game prevents a full paired gate; both arms won zero
matched games. The staged placement mechanism fired in games 1–3, but the
functional and win gates failed overall. Keep the isolated rule off.

### PvP fog detection screen (2026-09-27)

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
healthy reported loss. The candidate completed four structurally valid games
and lost all four. Its new Observer goal appeared in games 1 and 3, where
no enemy Dark Templar was logged; an enemy Dark Templar appeared in game 0
before the rule became eligible. The trigger missed the target timing and the
candidate is not promoted. The four-game reference rerun is prepared but has
not started.
The functional gate is Observatory/Observer ahead of the first DT in eligible
games without a new opening collapse. Promotion requires a real win gain with
healthy paired reports; earlier detection alone is insufficient.

### Replay-derived PvZ economy opening (in progress, 2026-09-27)

In a stable SHA sample of 256 qualified frozen train-split ZvP games, the
Protoss median was 17 Probes with a second Nexus underway by frame 4,800;
at frame 7,200 it was 26 Probes on two Nexuses. Recent default losses against
McRaveZ had 10-11 Probes and one Nexus at frame 4,800, and 16-17 Probes and
one Nexus at frame 7,200. The benchmark is descriptive and does not use
validation or test games (`training/human_opening_benchmark.py`,
`build/human-opening-zvp-20260927.json`).

The opt-in `PROTODD_PVZ_REPLAY_OPENING` build tests continuous Probe production,
one initial Gateway, and an earlier fortified natural once a Cannon and
Gateway have started. Observed early ground pressure returns to the defensive
plan. The native strategy and spending checks pass. Frozen DLL SHA-256:
reference `52C5D39DD5D7E125BE4C6F778FB045BD1620BCB7339929DCE700784991500012`,
candidate `F3D9883BAE8F45D96291B9DD9A861BB4F032762CB1864693A6CAF82EE10928F7`.
Four games per arm against McRaveZ are prepared on Benzene and Destination
with both host sides in `build/pvz-replay-opening-screen-20260927`; the
candidate is running. The mechanism gate is a second Nexus underway and at
least three extra Probes by frame 4,800 in most matched games, with no new
early collapse. A larger win-rate campaign requires healthy paired reports
and an actual pilot win gain; the default build stays off pending broader proof.
In the first two candidate games, Protodd had 19 Probes at frame 4,800 but
did not start its natural until about frame 7,400. A Zergling last seen at the
enemy base switched the opening back to the default two-Gateway plan even
though no enemy army was near the main. A second frozen candidate keeps the
economy opening through distant Zergling sightings and still cancels it for
immediate ground pressure. Its native regression test and Win32 build pass;
DLL SHA-256 is
`60061B73C16EF650E725AD346CD1D237563EC48F62B6255BB109B415E6F1035A`.
An initial four-game screen was prepared in
`build/pvz-replay-opening-v2-screen-20260927` but not launched. The replacement
paired screen below uses the same mechanism and outcome gates; the first two
worker counts alone are not strength proof.
The first candidate's game 2 displayed `won=true`, but McRaveZ exceeded the
frozen 55 ms slow-frame allowance (321 of 320 frames), so the arena verifier
excludes it as a runtime forfeit. Its early Nexus and 19 Probes at frame 4,800
are usable mechanism observations, not win-rate evidence. For the refined
candidate and reference, a fresh paired campaign under
`build/pvz-replay-opening-v2-screen-640-20260927` pins the same 640-frame
slow-frame allowance in both arms; the old reports remain unchanged. The
arena preparer and its regression test now pin that development-only setting.
The first candidate completed with three valid losses and one slow-frame
forfeit excluded by the arena. The refined candidate completed with three
valid losses and one frame-limit draw excluded by the arena. Its early Nexus
started at frames 4,612, 4,705 and 4,665 in the three valid games, with
19 Probes at frame 4,800 in each. This repaired the intended timing but did
not show a win gain. Its frozen reference completed with four valid losses.
For the three valid matched games, the candidate had 7, 8 and 7 more Probes
at frame 4,800, and its natural began 2,508, 1,759 and 2,262 frames sooner.
It took one extra early loss in the first match and won none of the three.
The excluded fourth candidate game prevents the full paired gate from
passing. Neither opening option is promoted.
The extended train-split benchmark has 177 surviving samples at frame 12,000:
median 45 Probes, four Gateways, a completed Templar Archives and two
Corsairs. In the first candidate loss at that frame, Protodd had 46 Probes,
five Gateways and two Corsairs but no Archives; at frame 13,200 its Archives
was still unfinished as a large Hydra wave reached home. This makes earlier
Storm access a separate follow-up hypothesis after the opening comparison.
Across those 177 games, 114 had a completed Archives, 50 had a completed High
Templar, 16 had researched Storm, and only three had a completed Robotics
Support Bay. Protodd was building a Support Bay and reserving extra Reavers
at frame 12,000 instead. The next tech experiment should prioritize the
Archives path before optional Reaver harassment and measure first Templar,
Storm research, army survival and wins; it must not assume Storm was universal
in the replay sample.
Later benchmark frames condition on games that lasted that long, so these
medians are descriptive rather than a causal win recipe.

An opt-in `PROTODD_PVZ_ARCHIVES_FIRST` tactic now tests that separate tech
hypothesis on top of the replay economy opening. Once two bases, 26 Probes,
a Core, Cannon and four mobile defenders are established, it reserves
Citadel, Archives, then the first High Templar and Storm research ahead of
optional Reaver drop production. Immediate ground pressure still vetoes the
tech window. Native tests and the Win32 build pass; frozen combined DLL
SHA-256 is `B906F5E4019B2A4CEDFD60EB5ED7225073AD68D1C09A268D8FCD53C55BFE4758`.
The four-game 640-frame-allowance `candidate-v2` screen in
`build/pvz-archives-first-screen-640-20260927` completed with four valid
losses. The earlier `candidate` package was never run; it was replaced so
Storm research takes precedence over the first Templar cycle. Against the
three valid replay-opening matches, Archives finished 3,243 and 1,079 frames
earlier in games 0 and 2 and also completed in game 1 where the reference
never reached it. Two High Templar finished by frame 13,002, but no Reaver
was built in those three games; one extra early loss occurred in game 2.
Both arms won zero games, and the replay-opening reference's excluded fourth
game prevents the full paired gate from passing. The declared functional and
win gates failed. Archives-first stays off.

The refined economy candidate's fourth game exposed a distinct late-game
failure before ending at the frame limit; the arena excluded that result.
At frame 53,640 it had about 50 combat units,
12 Probes, two completed Nexuses and no minerals remaining at either owned
base, while several neutral mineral bases remained available. The log showed
routine Pylon/army savings ahead of Probe recovery and a new Nexus. At frame
24,000 it already had only 12 Probes across two bases, with roughly 9,100
minerals still present at those bases. The recovery rule currently activates
below 12 workers for two bases, and its priority is lower than late combat
goals; the expansion rule reacts after a base has nearly mined out. An opt-in
late-economy candidate now protects a modest worker floor while safe, then
reserves a replacement base before the owned patches are empty. Its native
spending test and Win32 build pass. Its frozen four-game screen under
`build/pvz-late-recovery-screen-640-20260927/candidate` completed with four
valid losses. The worker-floor branch fired in game 2 and Probes rose from
15 at frame 17,280 to 21 at frame 19,200, but a later Hydra attack killed
the economy. The pre-depletion Nexus branch fired in games 0 and 3. Game 0
lasted to frame 37,946 rather than the replay-opening reference's 23,035,
but both were losses and the openings had already diverged before the new
rule activated. The original long-game seed lost at frame 23,066 in this
run, so its late mined-out scenario was not reproduced. No win gain was
shown; the late-economy option stays off.
Measure funded Probes, new Nexus timing, remaining owned minerals, army
survival, and valid wins in paired games. The long game is mechanism evidence,
not a valid win.
The same game also shows a late movement failure: a ground unit remained at
1584x3644 from at least frame 67,920 to 71,520 while `Attack_Move` toward
2080x656 was repeatedly accepted and its native order still said moving.
Several nearby army units likewise held their positions. A Corsair alternated
between scout travel and raid extraction commands every few dozen frames.
Later incident logs show several combat units motionless for more than 26,000
frames with `AttackMove` still accepted. An opt-in stalled-army routing build
switches to a persistent terrain waypoint when at least a quarter of a main
army has stopped for 240 frames. Its Win32 DLL and frozen four-game screen
are prepared under `build/pvz-stalled-routing-screen-640-20260927`;
live movement, frame cost and outcomes remain untested. Actual movement and
valid wins are required before promotion.

The replay-opening candidate's game 0 exposed a separate midgame spending
bottleneck. At frame 14,400 it held 1,624 minerals and seven of eight
Gateways were idle; at frame 16,800 it held 3,982 minerals and seven of
eight completed Gateways were idle. The composition filler refuses to exceed
its Zealot share while gas-heavy units wait, so the large mineral bank did
not become an army before the Hydra wave. An opt-in
`PROTODD_PVZ_MINERAL_FALLBACK` candidate spends at most four extra Zealots
from idle Gateways per planning pass when at least 800 minerals are banked,
gas is scarce and 400 free minerals can remain after each purchase. It
reserves higher-priority goals first. Native reservation tests and its Win32
build pass; the frozen four-game screen is prepared under
`build/pvz-mineral-fallback-screen-640-20260927/candidate`. Measure Zealot
production, idle Gateway time, bank size, early losses and valid wins before
considering promotion. This is a later mineral-surplus intervention, distinct
from the rejected early eight-Zealot target that delayed Core construction.
Its first game reached the frame-86,402 limit and is excluded. Between frames
12,000 and 18,000 it had 1,136 idle-Gateway samples but no sample with both
at least two idle Gateways and 800 minerals; the fallback could not fire.
The same seed's replay-opening reference had a large midgame bank, but this
run diverged before the rule's eligibility window: it had only one completed
Cannon at frame 7,200 rather than two. The result tests exposure, not the
effect of fallback spending. Game 1 was a valid loss at frame 19,160. It had
75 midgame samples with at least two idle Gateways and 800 minerals, mainly
before the original frame-17,280 activation; by that threshold it had lost
most of its economy and never invoked the fallback. A second isolated variant
opens at frame 12,000 only after two completed Nexuses, Core and Cannons and
26 Probes. This preserves the first defense and tech checkpoints while
allowing the observed frame-12,480 to 13,920 bank to fund Zealots. Native
reservation tests and Win32 build pass. A first package at
`build/pvz-early-mineral-fallback-screen-640-20260927/candidate` was prepared
but never run. It still vetoed fallback with 130 free gas while six Gateways
were idle. The revised `candidate-v2` permits a small mineral-surplus Zealot
cycle regardless of a modest gas bank, after higher-priority reservations.
Its DLL SHA-256 is
`CB2E2D9BE309276DF0234EACF869E8CF696CDE974CEA41EF7BAF2E8979B740E8`;
all 415 non-DLL inputs match after port normalization. The original
window's game 2 also lost (frame 22,911) without invoking the fallback; it
had only six high-bank idle samples. Game 3 later lost at frame 12,030.
The detailed budget trace found zero original-window ready samples and zero
fallback mentions in the first three games. Game 1 had four state samples
meeting the revised early-window budget, supply and completed-defense gates.
This is a plausible exposure opportunity, not evidence of an improved result.
The original-window campaign finished with three valid losses and one
frame-limit exclusion (game 0). The strict
review in `build/pvz-mineral-fallback-screen-640-20260927/review.json`
matched only games 1 and 2 because game 0 was excluded here and game 3 was
excluded in the replay-opening reference. Neither matched game was won,
the fallback never appeared in any of the four candidate logs, and both
the functional and paired gates failed. Keep the original-window option off.
The revised early-window `candidate-v2` subsequently completed four valid
losses at frames 12,619, 11,689, 23,748 and 16,060. Its only exposed game
(game 2) issued 24 accepted extra Zealot orders beginning at frame 12,247,
yet had 15 army units at frame 18,000 versus 26 in the replay-opening
reference, and one additional early loss. Games 0 and 1 collapsed before
the new spending window; game 3 never issued the fallback. The strict
review at `build/pvz-early-mineral-fallback-screen-640-20260927/review-v2.json`
matched the three valid replay-opening reference games. The reference's
excluded fourth game prevents a full paired gate; both arms won 0/3
matched games. Its functional and win gates failed. Keep the early-window
option off and close this mineral-fallback family after the two predeclared
activation windows.

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
