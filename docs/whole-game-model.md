# Whole-game learned controller

## Active direction — 24 September 2026

The [worker spending training pilots](worker-outcome-training-20260925.md)
stopped at baseline health because UABTerran crashed on apparent Protodd wins;
they yielded no qualified treatment outcome labels. The active
[PvT Core bridge experiment](pvt-core-bridge-20260925.md) tests a targeted
production change in matched Steamhammer games. It has no learned command
authority or tournament control.

Latest bounded cycle: [population goals](production-goal-experiment-20260924.md).
The joint goal/priority model failed development. A worker-only learned linear
policy passed a disjoint nine-game train check but failed frozen 24-game
validation, including 65.1% late worker-growth recall in PvP. All jobs finished;
no new model was exported or given control. Next, establish outcome-labelled
training scenarios for worker spending and recovery before another fit.

Earlier integration evidence follows.

Latest continuation: [concurrent production-demand experiment](production-demand-experiment-20260924.md)
passed bounded development, frozen 24-game confirmation and standalone Win32
model parity. Confirmation macro F1 is 56.88%; decoded counts match Python on
all 7,139 rows. Live causal history, persistent training feedback, complete
callback timing and bounded Probe/Zealot/Dragoon control now pass. The subsequent
seed-matched full-game pilot failed advancement: candidate 0/2 wins versus
reference 1/2. Its supervisor and arena processes have stopped, with no 72-game
campaign or tournament promotion. See the
[live integration report](production-live-integration-20260924.md) for evidence
and the next bounded training direction. The earlier whole-game command model
and exclusive macro classifiers remain rejected. The reserved validation games
were previously seen by the historical macro model, so are not globally fresh.

Current execution evidence:
[local-training-results-20260924.md](local-training-results-20260924.md).
The new group-command capacity test passed at 66/80 training signatures and 37/37
near-position hits, but remained poor on other games. The source-pinned bounded
development fit completed 4,800 updates and failed: 4/781 signatures versus
reference 10/781, and 4/449 near-position hits versus 5/449. Both exclusive
economy next-intent probes also failed. All these local runs have finished;
none of those earlier candidates passed to confirmation or live control. The full training plan
remains incomplete at the learning/execution stage. The repaired baseline finished 12 normal games,
all losses, and its loss review selects economy/army readiness before contact.
All 1,380 release validation games were previously evaluated by the macro model;
the newly reserved command cohort must not be described as globally untouched.

The [strength-first plan](strength-first-plan.md) now governs the work queue,
following the user's request to reassess and adjust the path to the strongest
bot. Full-game learning remains the goal; the existing six-slot packet model is
an offline research baseline. Prioritize gameplay failure diagnosis, coherent
learned scopes, actor-set execution and consistent sequence training over further
small frozen-backbone head probes. No new full-corpus fit or model promotion is
authorized by a completed extraction or a lower replay loss alone.

The v32d release and 7,200-game fit are complete. The latest fixed position
arbiter improves the historical 24-game regression audit to 241 signatures and
395 position hits, but combat and the existing gates still fail. The new
source-pinned strategy audit documents sampled training exposure and the gap
between group replay commands and one-actor runtime decoding. A frozen 12-game
deterministic reference campaign was launched at
`build/strength-first-20260924/baseline-01`; no experimental model controls it.

Latest update: that campaign has now reported all 12 games (eight normal PvP/PvZ
losses; four excluded PvT games that never entered detected game state).
Following the user's request for more reliance on training and explicit choice
of local compute, a complete-network capacity diagnostic completed in
`artifacts/replay-learning/whole-game-local-capacity-20260924` from frozen source
under `build/local-capacity-20260924-r2`. It checks 48 fixed train windows and
48 disjoint-game development windows before broader sequence training. It failed
its predeclared gate: training signatures rose from 0/80 to 29/80, but training
position hits were only 1/37 and development signatures fell from 2/98 to 0/98.
All 48 train windows were consumed in 600 updates; 69 model tensors changed.
The result prioritizes conditioning/position/representation fixes; no new
full-corpus fit, cloud job or controller promotion is implied. See the active
plan for its predeclared budget and diagnostic gates.

The architecture proposals and dated experiment log below are historical
evidence. They do not override the active plan or constitute current job status.

## Original tournament direction: GPU fit, compiled inference

The GPU-trained whole-game policy is the intended primary Protoss tournament
controller. Training may use a large teacher and as much replay/self-play
compute as is justified by held-out strength. The submitted bot will carry
compiled weights and run inference locally within the AIIDE match process;
its online command path still needs measured callback latency, legal-action
checks and paired-game wins. Existing hand-written modules remain comparison
and guarded fallback behavior until the learned controller passes those gates.

The current one-command-per-24-frame action head has a measurable ceiling.
On the separate 24-game PvP/PvT/PvZ validation release, 11,597 completed
cadence windows contain 28,906 confirmed replay commands. Of the action
windows, 7,982 contain multiple commands, 3,153 multiple kinds, and 1,741
multiple domains. An oracle choosing the best single kind/mode pair could
cover at most 24,072 commands; an oracle choosing a single full command packet
could cover only 13,156. These are generous replay-coverage upper bounds, not
model accuracy. Six ordered packet runs cover 28,751 commands, although
6,689 windows reuse an actor and therefore need time-ordered dispatch rather
than simultaneous issuance. At the causal decision frame, 28,904 commands
have their actors available and 28,849 have both actors and labeled target
arguments available. The machine-readable report is
`artifacts/replay-learning/whole-game-window-capacity-validation8-v32c-causal-20260923.json`.

For the first decoder, retain the actual chronological command events rather
than merging equal packets. Six individual slots cover 28,715/28,906
validation commands and 38,896/39,217 commands on the separate 24-game
training cohort. Their timing and actor reuse remain explicit; longer windows
are flagged as truncated targets and require separate evaluation. The training
cohort report is
`artifacts/replay-learning/whole-game-window-capacity-train8-v32c-causal-20260923.json`.

The next GPU policy should encode the causal game state once, then decode up to
six ordered commands with a STOP option. Each slot predicts whether to act,
actor set, kind, supported target mode, target or position, required argument,
and delay within the following 24 frames. A lightweight recurrent slot state
conditions later slots on earlier generated slots. Training uses teacher
forcing on ordered replay commands, then scheduled sampling and live games to
test compounding error. The native executor must recheck visibility, ownership,
resources and BWAPI legality at dispatch time and arbitrate repeated actors.
The ordered targets are now exposed by `training.whole_game_cadence_sequences`:
future commands stay out of the encoder/context, censored final windows are
excluded, and actor/target availability at the decision frame is explicit.
It also emits delay targets and an explicit STOP target; windows longer than
six commands report overflow instead of teaching a false STOP.

`training.whole_game_multislot_model` now implements the shared-encoder,
six-slot decoder. Later slots receive only earlier command tokens, including
actor, kind, mode, target, position, unit type and relative dispatch time.
`whole_game_multislot_batch` uses the existing masked action evidence and
balanced actor loss; `whole_game_multislot_collect` streams bounded command
windows. `whole_game_multislot_fit` pins cohort and source hashes and saves
model, optimizer and RNG state per group. A one-group CPU smoke completed on
three training and three disjoint validation games, with finite losses and a
checkpoint. This verifies training plumbing only; no GPU multi-slot fit or
gameplay strength result exists yet.
The first slot now copies the verified baseline event, actor, kind, mode,
target, position and argument heads exactly. Its extra conditioning begins at
zero, so the sequence model begins from a trained one-command policy instead
of discarding that fit. A focused parity test checks these logits.
Its two-group paused/resumed CPU fit and uninterrupted fit produced exactly
equal values for all 103 teacher tensors and identical disjoint-validation
rows. This verifies deterministic group restart, not policy quality.
The free-running `whole_game_multislot_audit` now measures command count,
kind, actor, target, position, unit type and dispatch timing on causal cadence
observations. It completed a three-game CPU plumbing run; the essentially
untrained smoke checkpoint produced only 38 commands against 4,634 replay
commands and zero complete command signatures. This result is expected for
the smoke model and confirms that promotion must rely on the later full GPU
fit's free-running 24-game audit, not training loss alone.

The separate spatial head stream completed 2,560 GPU updates on 480 training
games. On its small three-game held-out probe, its oracle-actor/kind position
prediction placed 0/26 targets within 64 pixels; predicted actor/kind context
also scored 0/26. It is not a reason to promote or scale that spatial head
without broader causal position evaluation.

Before scaling, compare the currently running baseline, structured, mixed,
actor-conditioned and focal teachers on the same 24 held-out games. Select the
objective by non-majority command, actor, argument and complete-signature
accuracy, not aggregate right-click accuracy. Fit the multi-command teacher on
the versioned, repaired full replay release; retain separate map/player/game
validation and sealed final test. Distill the winning teacher to a shared
encoder plus compact slot decoder, prove compiled numerical parity and worst
case callback time, then run paired games against the frozen tournament bot
and opponent pool. Do not promote a model whose legal action rate or win rate
regresses, even if offline imitation loss improves.

## Implementation evidence: replay and live pilots

`tools/replay_native/extract_whole_game.cpp` and `whole_game.hpp` implement a
separate native v3.2 pilot with causal local entity IDs, own-unit state, detected
enemy observations, last-seen memory, neutral entities and tile visibility.
Memory updates every frame; observations are emitted every 24 frames and before
each issued command. `whole_game_actions.hpp` decodes issued commands into
actor, action and legal target fields. Immediate own-unit state changes supply
candidate action labels; unchanged/rejected commands and unavailable targets
are masked. Raw packets remain diagnostics, never policy inputs. Private enemy
energy, cooldowns and orders are absent. Native tests perturb hidden state and
cover brief sightings, target privacy, builds, queued movement, spells and
transport commands. Eventual command completion remains unverified.

`include/protodd/WholeGameObservation.hpp` is the shared serialization contract.
The opt-in `src/bwapi/WholeGameRuntime.cpp` writes live cadence traces without
issuing commands. Static walkability now includes StarCraft's playable border;
non-caster energy, non-shielded units and mineral-field variants are normalized.
Replay terrain height remains diagnostic because BWAPI exposes build-tile height,
not the same walk-tile values.

`training.whole_game_pilot` selects qualified **training-only** perspectives from
the immutable release, checks replay hashes, runs native privacy tests, validates
command/observation references, and compares instrumentation checkpoints against
the existing pinned simulation binary. The nine-replay v3.1 pilot produced
46,198 observations, 39,597 command records, 11,188 candidate labels and 671
matching playback checkpoints. The normalized v3.2 three-matchup pilot passed
the same checks at `artifacts/replay-learning/whole-game-normalized-v32-fixed-20260922`.

`training.whole_game_parity` compared both sides of an actual paired McRaveZ
campaign to their classic game replays. Each game had 480 aligned cadence frames.
Vision, technology, upgrades, gas and static walkability matched every compared
frame/tile. Minerals and supply used matched on 474/480 frames per game. Exact
entity signatures matched 53,604/55,639 and 50,667/52,796 live rows respectively.
Shield regeneration sometimes differs by one point, and orders sometimes differ
for several frames. These simulator/live phase differences remain unresolved.
Reports are in `build/robust-training-20260922/development-06-whole-game-parity`.

`training.whole_game_model` has entity, spatial and recurrent encoders and masked
heads for events, actions, actors, targets, positions and arguments. A 24-step
RTX 5070 Ti gradient probe on the training pilot exercised 24 action targets and
96 complete cadence windows, including 54 positive and 42 no-action timing
targets, with 1.25 million parameters. Its report is
`artifacts/replay-learning/whole-game-gpu-timing-probe-v32-20260922/report.json`.
`training.whole_game_sequences` censors incomplete final windows and updates
recurrent memory only on cadence, so human UI command frequency does not become
an implicit clock. This is a pipeline probe, not a trained playing model or
strength result.

The native executable and its selftest are targets `replay_extract_v3` and
`replay_extract_v3_selftest` under `tools/replay_native/CMakeLists.txt`; build in a
fresh directory with the same pinned OpenBW checkout. Example pilot invocation:

```powershell
./build/model-venv/Scripts/python.exe -m training.whole_game_pilot `
  --release artifacts/replay-learning/protoss-data-release-20260922 `
  --root artifacts/cwal-dataset `
  --extractor build/replay-whole-game-v31/Release/replay_extract_v3.exe `
  --selftest build/replay-whole-game-v31/Release/replay_extract_v3_selftest.exe `
  --decoder build/replay-native/replay_decode.exe `
  --reference build/replay-native/Release/replay_checkpoints.exe `
  --mpq build/match-runtime-a --assets build/replay-modern-assets `
  --output artifacts/replay-learning/whole-game-pilot-v3-next
```

This is an experimental pilot, not a trainable whole-game data release. Replay
commands lack eventual completion labels; rare abilities and scouting/army
strategy need richer supervision; live parity is not exact. A later compiled
runtime added a legal command adapter, but promotion still needs a strong fit.

## Frozen whole-game shard build

`training.whole_game_release` now extends the verified v3.2 extraction to the
frozen qualified Protoss train and validation perspectives. It selects one
perspective per game and inherits the existing game/map/player split. It never
opens the sealed final-test replay files. Each replay is hash-checked, decoded,
validated for causal observations and partial command labels, and compared
against the pinned native playback checkpoints. A native privacy/action selftest
is run once for the pinned binaries. Each game is compressed to separate
observation, command, label, terrain, checkpoint and summary artifacts with an
atomic hash receipt. A restarted run verifies receipts before skipping games.

The bounded six-game, 6,000-frame benchmark across train and validation and
PvT/PvZ/PvP passed 156 checkpoint comparisons and yielded 2,667 candidate
labels in 9.75 MB of compressed shards. The full eligible scope is 11,183
training and 1,380 validation games, or 192,132,765 valid-prefix frames. The
benchmark is only a throughput/format check; its labels are not a strength
result. `release.json` deliberately retains `training_ready: false`, since
command completion, full observation parity, CPU execution and gameplay
validation are still open.

A second six-game benchmark at complete valid prefixes passed 426 checkpoints,
yielded 8,361 candidate labels and occupied 104.24 MB for 100,189 frames.
Its mix includes abilities and transports but is dominated by unit-control
right-clicks. This makes domain and rare-action sampling a requirement; overall
action accuracy would disguise weak scouting, spell, transport or combat play.
Scaling its bytes/frame to the selected corpus gives an uncertain ~200 GB
compressed estimate. The builder refuses to start a new replay below 80 GB of
free space. The first full local run exposed an intermittent Windows directory
commit denial after 13 games and was stopped. The builder now retries the
rename and can copy artifacts with the receipt written last. A six-game rerun
passed. A second full run found that a transiently observed entity could be
assigned an internal ID but disappear before any sampled observation published
that ID; a later own order leaked the unpublished reference. The shared replay
and live serializers now mask those order/cargo IDs. The exact failing replay
passes causal validation and 103 reference playback checkpoints. Another
six-game end-to-end run passed. The corrected full job was launched on
2026-09-22 with four workers at
`artifacts/replay-learning/whole-game-release-v32c-20260922`;
`progress.json` in that output records completed games, bytes and failures.

`training.whole_game_coverage` aggregates the split/matchup/action/evidence
distribution from receipts without using validation as training. The
`training.whole_game_shards` reader verifies each shard, refuses final-test
access, and supplies causal observation/action/timing streams from gzip. These
are preparation interfaces; they do not certify that the current experimental
model has mastered every domain.

`training.whole_game_fit` runs a causal short-history teacher fit from those
shards. It samples action kind and matchup buckets across complete games and
uses held-out validation games separately. A 12-update RTX 5070 Ti smoke run
on six full-prefix benchmark games covered all three matchups, event timing and
rare ability/transport examples, with 27 MB peak allocated and a 167k-parameter
small-width model. It is a plumbing test, not evidence of stronger play: the
sample is tiny, the model has no strategic/scouting/value head yet, and no
learned command reaches the tournament DLL.

`training.whole_game_capacity` checks whether the bounded entity encoder can
represent own actors and target pointers. In the three training games of the
full-prefix benchmark, 16,069 observations had at most 210 own and 433 total
entities; none exceeded the current 512-entity cap, and all 697 target-entity
labels were representable. The full release still needs this audit before
choosing final capacity or a truncation policy.

`training.whole_game_future` derives auxiliary targets only from later legal
observations, with incomplete windows censored. Targets describe exploration,
newly known enemies retained in memory, own damage, technology, economy, and
confirmed intervening command domains. In three full-prefix training games,
902/2,380 complete 24-frame windows gained explored tiles, 320 retained a new
enemy ID, and 534 showed damage to persistent own units. At 240 frames, the
corresponding counts were 1,631/2,353, 1,027/2,353 and 922/2,353. These are
observable prediction targets, not hidden game truth or labels of good play.
The 1,440-frame unit-control/production domain flags saturated, so the current
model trains optional forecast heads at 24 and 240 frames only.

`training.whole_game_outcomes` now audits observable post-command progress without
turning silence into a failure label. It checks for a matching new building near
the requested tile, a requested unit entering the issuing actor's queue,
research/upgrade state advancing, or movement toward the commanded location.
A later accepted order on the same actor supersedes the attribution; an
incomplete horizon is censored. On three full-prefix train games, the 240-frame
audit found positive evidence for 50 builds, 493 trains, one research, 17
upgrades and 282 attack-move commands. These are simulated legal-observation
diagnostics, not certified game-engine completion or causally proven effects.

The `training.whole_game_fit` teacher now alternates action, event-timing and
forecast updates. A 12-step, six-game RTX 5070 Ti smoke run completed four
updates per task, with a 168k-parameter small-width model and 26.6 MB GPU peak.
Its held-out metrics are saved by matchup and task. This validates plumbing,
not that forecasting improves wins; the `--no-forecast` option permits an
action/timing ablation when more complete train and validation shards exist.

The trainer now pads legal entity sets for BF16 GPU minibatches, preserving
masked attention and pointer losses. Synthetic event, action and forecast
minibatches match single-example FP32 losses; a six-game BF16 smoke fit completed
two examples per update. A separate-release option checks train/validation game
IDs and replay hashes before using a frozen held-out benchmark while the full
release extracts. The interim 24-games-per-matchup GPU fit is running locally.
`training.whole_game_export` packages all model tensors in deterministic float32
order with SHA-256 integrity; a smoke checkpoint round-tripped exactly. This
package is not yet a tournament runtime: C++ forward parity, latency, action
arbiter and playing-strength validation remain outstanding.

The full local extraction can be resumed with the same command (omit both
`--per-matchup` and `--max-frames` for complete valid prefixes):

```powershell
python -m training.whole_game_release `
  --release artifacts/replay-learning/protoss-data-release-20260922 `
  --root artifacts/cwal-dataset `
  --extractor build/replay-whole-game-v31/Release/replay_extract_v3.exe `
  --selftest build/replay-whole-game-v31/Release/replay_extract_v3_selftest.exe `
  --decoder build/replay-native/replay_decode.exe `
  --reference build/replay-native/Release/replay_checkpoints.exe `
  --mpq build/match-runtime-a --assets build/replay-modern-assets `
  --output artifacts/replay-learning/whole-game-release-v32c-20260922 --workers 4
```

The next learning step must consume only train shards for optimization and use
validation shards solely for tuning and reporting. Measure label coverage by
domain and matchup before reweighting, add explicit long-term strategy and
belief targets, then train and compare recurrent offline teachers. The current
model's action/timing heads are a prototype, not complete scouting, tactical
planning, or unit-control competence. Completion feedback and interactive play
remain essential before replacing scripted decision ownership.

## What exists

The A0 model completed 20 epochs. It sees 598 aggregate features and predicts 46
macro intents. Non-wait top-1 validation accuracy is 4.94%. ModelRuntime only
observes and logs; its weights do not control commands. More epochs or wider
layers cannot add missing observations, action types or execution.

Preserve A0 as an offline reference and preserve the existing frozen dataset.
Do not present its training completion as completion of a playing agent.

## Architecture

Use a hierarchical policy with a shared causal memory and specialized heads.
All decisions must derive from information available to the controlled player.

Inputs:

- Global economy, supply, technology, production queues and recent command results.
- Own unit entities with position, health, energy, cooldowns, orders and capabilities.
- Visible enemy entities and explicitly aged last-seen memories. Never refresh
  unseen enemy positions or health using replay truth.
- Terrain, walkability, bases, explored/visible regions, routes and observed threats.
- Previous actions, elapsed time and recurrent state. Reset memory between games;
  training sequences must not cross games or perspectives.

Pool variable-length entity sets and encode spatial regions, then fuse those
representations with global features in recurrent memory. Start with a bounded
entity encoder and GRU; compare larger attention-based teachers only after useful
targets and CPU execution exist. Entity limits need deterministic selection,
overflow indicators and preserved urgent threats, not silent arbitrary truncation.

| Head | Decisions | Initial evidence and training |
|---|---|---|
| Economy | Worker production, mineral/gas allocation, transfers, expansion timing | Own commands and trajectories; income, idle time and supply-block diagnostics |
| Production and technology | What, when, producer, placement region, upgrades and transitions | Accepted replay events, prerequisites and subsequent command outcomes |
| Scouting and belief | Scout assignment, destination, revisit timing, inferred enemy threats | Legal sighting history and scout orders; calibrated belief targets with separate supervision |
| Army strategy | Composition priorities, squad assignment, attack, defend, regroup, retreat and reinforcement destinations | Orders and trajectories; explicitly mark inferred squad/intent labels as weak supervision |
| Combat | Actor/selection, move location, target, attack timing, kite or disengage | Unit commands, cooldowns and local engagements; interactive combat scenarios |
| Abilities and transport | Spell, target/position, load/unload, drop timing and extraction | Relevant ability/transport events; specialized scenarios for rare actions |
| Value and risk | Expected outcome, local survival and fight risk | Completed, healthy training games; avoid treating every losing-game action as wrong |

Factor actions into head, action type, actor(s), target entity or spatial location,
and timing. Use separate conditional masks instead of expanding the existing
64-bit macro mask to represent every possible command. Include explicit continue
or hold behavior and persistent goals so decisions do not repeatedly restart work.

Strategic heads should run more slowly than urgent combat decisions. Determine
cadences from measured latency and missed-event tests. Maintain legal observations
every frame even when inference is skipped. CPU execution is a first-class model
selection constraint; GPU training alone does not establish deployability.

## Replay extraction v3 is the first implementation dependency

1. Define a versioned entity/spatial/action schema shared by extraction and live
   observation. Record replay identity, perspective, frame, sampling interval,
   schema/extractor hashes and frozen split assignment.
2. Preserve all relevant commands and actor/target identities within each interval,
   not only the first macro event. Map identities to causal perspective-local IDs.
   Record issued commands separately from accepted commands and observed effects;
   ambiguous acceptance must be marked unknown rather than invented.
3. Separate policy inputs from labels and diagnostics. Future outcomes and hidden
   enemy truth may supervise a belief/value target, but cannot enter the policy
   encoder, normalization statistics or recurrent state. A target is not an input.
4. Store observation snapshots plus intervening events, with denser combat windows.
   Preserve timing and sampling probabilities so rare-action oversampling does not
   silently change the intended training objective.
5. Run a small training-split pilot across PvT/PvZ/PvP. Compare replay observations
   against original-game/live observations, including brief sightings, cloak,
   spells, transport contents and simultaneous commands. Quantify missing targets
   before committing to corpus-wide extraction.
6. Publish immutable v3 shards only after parity and leakage checks pass. Keep the
   current split boundaries; do not reopen the sealed final test for development.

Existing v2 data remains usable for macro pretraining and temporal experiments.
It cannot be relabeled into missing spatial or combat supervision.

## Execution must be trained and tested with the policy

Route heads through one arbiter with explicit unit ownership, resource reservations,
priority, expiry and cancellation. Emergency survival may preempt a strategic order,
but must report the preemption. Enforce final BWAPI legality, placement and pathing
through existing controllers. Feed accepted, rejected, deferred and failed action
results back into memory. Define bounded retries and safe fallbacks.

Reuse Workers, MacroPlanner, Scouting, Squads, Combat, Technology and Transport
where useful. Replace their decision ownership explicitly as learned heads graduate;
do not let learned and scripted controllers issue competing orders to the same unit.

## Training sequence and gates

### High-MMR replay curriculum

The frozen v3.2 release now has an external, split-checked quality sidecar built
from the original replay audit (`training/whole_game_quality.py`). It joins each
release perspective by replay path and SHA-256, then reads only the MMR claim
attached to that exact Protoss player. A high opponent rating or an unverified
`proId`/`proName` tag cannot raise the training player's tier. These are catalog
MMR claims, not authenticated professional identities. The v2 sidecar hashes
the acting player's source identity for diversity sampling; a zero Aurora ID
is treated as missing, not as a shared identity.

For the current release, train games at MMR ≥2300 are 411 PvT, 578 PvZ and 648
PvP; at ≥2500 they shrink to 63, 76 and 28. Use ≥2300 as the first high-rating
tier. The offline teacher's optional `--quality-index` targets half its selected
training games per matchup from that tier when enough completed shards exist.
Within each tier it cycles through distinct player groups before reusing one,
and records the distinct groups selected. It also keeps separate high/base
reservoirs for each action category before merging the requested ratio, so
rare-action sampling does not erase the high-MMR curriculum. The report records
the actual example mix. This limits overfitting to accounts with many replay
files.

The sampler now separates the per-replay offer cap (default two per category)
from the global training reservoir (default 64 per category). The previous
shared cap of two reduced a large replay collection to a few hundred training
examples. These bounded defaults are still a development fit, not full-corpus
epoch training; that requires scalable feature caches and game-strength tests.

Its validation selection remains the same seeded, unweighted selection. The fit
report pins the sidecar hash, records selected tier counts, and breaks held-out
metrics down by rating tier. Rating metadata stays outside model inputs. Hold the final test
sealed. A category seen in only one rating tier can draw only from that tier.

Run paired quality-weighted and unweighted fits with identical architecture,
seed, game budget and steps. Compare balanced action, event and forecast metrics
on natural and high-MMR held-out slices, then compare actual bot strength after
the CPU runtime exists. Do not infer strength from source MMR or imitation loss
alone. Inspect date/map/player diversity and repeated openings so a narrow
high-MMR cohort does not teach the bot one brittle style.

1. **Data pilot:** v3 schema, extractor/live encoder parity, leakage tests and target
   coverage report. Deliver one reproducible batch containing each supported head;
   absent/unknown labels must be masked, never silently assigned a negative class.
2. **Offline prototype:** shared recurrent encoder, conditional heads and masked
   losses. Use causal burn-in, sequence padding masks and event-timing supervision.
   Balance games/matchups and combat scenarios. Report per-head loss, action recall,
   timing error and rare-action metrics rather than aggregate wait-heavy accuracy.
3. **Runtime prototype:** versioned C++ export, sequence-level numerical parity,
   arbiter and shadow comparison. Test resets, invalid targets, deaths, cancellations,
   resource contention and total frame latency, not inference latency alone.
4. **Scenario curriculum:** economy under harassment, scouting/cloak, defense,
   kiting/focus fire, spells, drops and multibase operations. Introduce head control
   separately, then jointly; single-head gains can hide coordination regressions.
5. **Interactive improvement:** imitation-initialized policies play training-only
   opponents and historical snapshots. Use outcome learning with audited auxiliary
   rewards and explicit anti-stalling checks. Compare against imitation-only and
   scripted baselines. Replay imitation cannot by itself resolve distribution shift.
6. **Strength validation:** frozen paired development campaigns across opponents,
   maps and seeds. Separate legitimate losses from crashes/timeouts. Review activity,
   report uncertainty and matchup regressions, and retain a rollback candidate.
7. **Deployment:** choose or distill a CPU policy using actual full-frame timing and
   measured game strength. Freeze the complete compiled package before final-test
   evaluation. Only then extend race-specific actions and scenarios to Terran/Zerg.

Choose epoch limits separately for each stage using held-out metrics and game
performance. There is no meaningful universal epoch count for professional play.
Do not spend on a large teacher or league run before pilot throughput, memory,
sample requirements and expected cost have been measured and approved.

## Definition of progress

Track each domain as data available, trained offline, executable, scenario-tested,
and game-validated. None of those states implies the next. Report the scope of
learned control in every experiment. A robust whole-game controller requires all
the domains above; professional-level strength remains an empirical target, not a
claim justified by architecture size or by using human replay data.

## Compiled GPU-policy deployment path (22 September 2026)

The GPU-trained checkpoint exports through `training.whole_game_export` to a
versioned float32 `weights.bin`. The Win32 `src/cpu/WholeGameCpu` evaluator and
`src/cpu/WholeGameEncoder` reproduce PyTorch outputs and live/replay features on
sampled legal replay observations. A width-128 checkpoint matched all tested
heads within 3.5e-7; width 256 matched within 4.17e-7. The Win32 CPU forward
range on the sampled game was 11–18 ms at width 128 and 13–39 ms at width 256,
before BWAPI observation, command processing and other bot work. A width-512
GPU teacher therefore needs measured distillation or a faster evaluator before
it can be selected for the 42 ms callback target.

`PROTODD_WHOLE_GAME_WEIGHTS` embeds an exported weight package as resource 101
in `Protodd.dll`. For a build, configure the existing Win32 CMake project with
`-DPROTODD_WHOLE_GAME_WEIGHTS=<absolute path to weights.bin>`. The
`whole_game_resource_probe` opens that DLL as resource data, checks the bytes
against the source file and requires exact equality of embedded and file-loaded
inference. This avoids a dependency on tournament `bwapi-data/read` contents.
The smoke checkpoint currently used to test this path has `tournament_ready:
false`; it is not a competitive policy. The DLL currently evaluates it in
shadow mode and never issues its suggested commands. A first paired run found
that a normal defeat with zero surviving own units caused an unnecessary
encoder error; the runtime now skips inference in that terminal state. The
repeat paired campaign at `build/robust-training-20260922/development-08-embedded-terminal-fix`
passed both sides against McRaveZ with no crash, timeout frame or model error.
Its worst complete callbacks were 30.84 and 29.70 ms on this PC, and the
largest model forward was 19.71 ms. This establishes deployment health for a
small shadow model, not playing strength; the scripted bot controlled both
games and lost both. Final promotion requires an arbiter with legal
actor/target/position handling, explicit ownership of commands over the
existing bot, paired opponent games, and total callback latency under the
tournament limit.

`training.whole_game_distill` now provides a GPU student-fit path. It preserves
the teacher's event, action-argument, actor/target, position and forecast heads,
uses the same causal history and legal observation masks, and combines softened
teacher targets with the replay's observed labels. The exported student uses
the same deterministic package format as a directly trained model. Its unit
test verifies entity padding and gradient flow. A three-step end-to-end student
smoke exported to the Win32 evaluator with a maximum PyTorch difference of
3.6e-7; a meaningful student strength comparison remains outstanding.

The first width-512 teacher fit finished on 192 training games with 512 updates,
4.32 million parameters and a 0.80 GB peak CUDA allocation. Its three-game
held-out sample showed only 3/72 correct action kinds in the training report.
An independent action audit, using a different bounded selection from the
same held-out games, found 5/72 and showed predictions concentrated on five
kinds. This teacher is not ready for control or distillation. The larger
student run was stopped, while the three-step distillation/export/parity smoke
remains a successful pipeline check. A 4,096-step teacher continuation is now
running from the same checkpoint, with action examples scheduled for half of
updates and a bounded encoded-observation cache. Wider held-out validation
and game strength are still necessary before any controller promotion.

The Win32 decoder now turns event, action kind, argument, actor, visible target
and position heads into a typed intent. A BWAPI adapter constructs candidate
commands only for live owned actors and currently visible targets, checks map
bounds and Protoss action types, then calls `canIssueCommand`. This path remains
read-only in the live bot; the CSV records how many commands would pass the
legality check. It provides a measurable bridge from replay action predictions
to executable orders without handing control to the initial smoke checkpoint.

The first paired intent-shadow campaign (`development-09-intent-shadow`) had
normal game reports, no crashes and no model errors, but only 253 of 1,127
decoded intents had a compatible action/target shape. The model often selected
`attack_move` with target mode `none`. One of the two games also had a 56.98 ms
callback; its peak was in whole-game observation on a frame shared with strategy
and diagnostics. The shadow health gate therefore failed. Inference has been
staggered to frame 7 of the 24-frame window for the next measurement. No
strength claim or controller promotion follows from these shadow runs.

The paired legality-shadow campaign (`development-10-legal-intent-shadow`)
finished with two normal reports per game, no model errors, no timeout frames,
and peak callbacks of 23.02 and 30.56 ms. It measured 0 BWAPI-legal proposals
from 1,201 decoded smoke-student intents; only 255 had a compatible action and
target shape. This verifies that the adapter rejects bad actions while the
existing bot plays. It is not evidence that the learned policy can control a
game. The separately frozen staggered-inference build is being tested next.

That staggered build (`development-11-staggered-intent-shadow`) passed both
host sides with normal reports, no model errors or timeout frames, and peak
complete callbacks of 25.23 and 27.03 ms while extraction and GPU training
were also active on this PC. It inferred first on frame 7 as intended. This
supports the runtime schedule; the smoke student still produced zero legal
candidate commands, so no learned control was enabled.

An explicit CMake option, `PROTODD_WHOLE_GAME_CONTROL`, now compiles the
learned-command path only when embedded weights are configured. The runtime
uses normal event and actor thresholds in that build, sends BWAPI-legal
commands through the bridge, and leases each accepted actor for 24 frames so
scripted commands cannot overwrite it immediately. The ordinary build keeps
the shadow thresholds and never issues a learned order. Both build variants
compile. The control variant was played only with the deliberately untrained
pipeline smoke in `development-12-control-smoke`: both host sides ended
normally, with no timeout or model error and peak callbacks of 28.33 and
33.18 ms. It attempted zero learned commands, so the controller audit correctly
failed its operational gate. Passing live execution and game-strength gates
with a trained student remains necessary before making this variant a
tournament artifact.

The next `development-13-control-execution-probe` used a deliberately
synthetic policy that proposes owned-unit moves toward the map centre on every
model tick. It is marked diagnostic-only in its weight provenance. In paired
games against McRaveZ, both sides ended normally with no timeout frames or
model errors. The controller issued and BWAPI accepted 2,158 and 2,145 learned
commands respectively; maximum complete callbacks were 31.89 and 21.06 ms.
This proves the compiled action and actor-lease path can execute. The policy
lost both games as expected and provides no playing-strength evidence.

The compiled decoder now chooses the best supported kind/target-mode pair
jointly from the two logits heads. Its Protoss command mask is checked against
the Python training schema, and the Win32 intent probe covers an independently
incompatible top kind/mode combination. This guarantees that the decoded pair
has an adapter path, while BWAPI still decides whether the actor and arguments
make the command legal. Projection did not improve the weak group's held-out
top-1 action accuracy; trained action quality remains the gate.

The 4,096-step action-weighted teacher continuation finished on 192 training
games and 8,558 sampled examples. Its bounded held-out action-kind score rose
from 3/72 to 12/72. The independent audit, using a different selection from
the same three held-out games, rose from 5/72 to 9/72; actor top-1 improved
from 11/72 to 22/72. It still missed all six held-out `attack`, `attack_move`,
`build`, `right_click` and `train` examples in that independent audit. This is
far below a useful controller. Distillation of this checkpoint is deferred;
the next fit must stream a far larger and more varied slice of the high-MMR
corpus without retaining all replay samples in RAM.

`training.whole_game_stream_fit` now freezes a player-diverse cohort, decodes
only one small group of games at a time, and writes an atomic model/optimizer/RNG
checkpoint after each group. It can use one to three matchup collection workers
and validates any reused cohort against the training split and existing shard
receipts. Exact restart was tested by pausing after the first group: with
deterministic CUDA algorithms, the resumed and uninterrupted two-group fits
had identical model tensors and held-out loss. A four-step real-replay smoke
also completed validation and export-compatible checkpoint writing. This
solves the previous all-samples-in-RAM limit, though collection throughput and
useful held-out action accuracy still need improvement before scaling to all
available high-MMR games.

The first real streaming continuation freezes 160 games in each Protoss
matchup, in 20 groups of eight games per matchup. Its first group retained
1,714 stratified samples and completed 256 GPU updates in 185 seconds with
about 748 MB peak CUDA allocation and bounded host memory. All 20 groups and
5,120 GPU updates completed. Independent category-balanced action top-1 rose
to 14/72, but build, train, attack-move and right-click remained 0/6 each.
The natural-frequency audit found only 5/288 top-1 action kinds and no correct
right-click among 207 examples. This is not a tournament-ready teacher.
`training.whole_game_stream_fit_structured` now tests balanced actor and
kind/target-compatible losses on the same cohort and initialization; the
subsequent `training.whole_game_stream_fit_mixed` adds an equal blend of
empirical and rare-category action updates. Both are isolated, source-hashed
GPU runs. They must beat a right-click-only prior and improve non-majority
actions, targets and actors before any student is promoted.

A deeper argument audit found 6/13 correct target entities, 1/26 selected
positions within 64 pixels, and 1/12 correct unit-type arguments. Choosing the
nearest of all 12 Gaussian position components still reached only 1/26, so
the missing spatial information is a model problem. An experimental 16x16
map-cell head conditioned on the observed actor and command kind now trains
separately from the frozen teacher. On 24 training games its 512-step probe
fit 167/196 training positions within 64 pixels but only 1/26 held-out
positions; median held-out error improved from 2,567 to 869 pixels. This is an
oracle-context architecture probe, not a deployable policy. More distinct
replays and disjoint maps are required before a C++ port is justified.

For any opt-in control campaign, the DLL writes `WholeGame-controller.txt` and
`WholeGame-inference.csv`; the shadow audit rejects these games.
`training.whole_game_controller_audit` requires paired normal reports, a clean
frame budget, and at least one accepted learned command in each game before it
calls the controller operational. That report explicitly leaves playing
strength unvalidated; promotion still needs diverse opponents and a frozen
baseline comparison.

The GPU six-slot teacher now has a version-2 export package with a frozen
six-slot run receipt, source/checkpoint hashes, and exact tensor validation.
The Win32 evaluator autoregressively chooses an owned actor, supported action,
target, arguments and a dispatch frame for each active slot. It retains the
causal game memory from one backbone pass. A 24-frame scheduler replaces stale
plans at each cadence, issues due commands only after current BWAPI legality
checks, and defers same-frame actor collisions. In the explicit control build,
the six-slot learned policy is the sole order source while its model is healthy;
version-1 diagnostic weights leave the established controller in command. The
scripted controller also resumes if model initialization or inference fails.

Compiled/PyTorch parity passed for six active slots on two legal replay
observations using a synthetic width-512 package, with a maximum absolute
head error of 0.0000182. Reusing entity key projections and skipping unused
single-command heads cut the standalone compiled forward time on those early
replay observations from 46–52 ms to 26.6–27.4 ms. These are early-game CPU
probe measurements, not a complete BWAPI callback or a real trained teacher.

Later replay profiling exposed a 77 ms forward pass at frame 9,000 with 124
entities, above the 55 ms per-frame threshold. Reusing the backbone's entity
embeddings and vectorizing dense/terrain dot products with SSE2 reduced a
20-run Win32 standalone probe on that same fixture to 20.9 ms median and
28.0 ms 95th percentile in the saved benchmark while GPU training was active. Numerical parity still
passes in 32-bit execution at a late-game frame and across PvP, PvT and PvZ
mid-game frames. These timings exclude BWAPI observation, legality and command execution,
so the complete callback still needs a paired-game measurement with the trained
weights.

The live adapter now refreshes its entity snapshot only at model cadence or
when a scheduled command becomes due. It still resolves each due command
against current owned units, visible targets and BWAPI legality. This avoids
scanning own, enemy and neutral unit sets on idle frames. The Win32 control and
shadow builds compile, and the native schedule probe covers due-frame checks.

The six-slot tournament control build requires a
`protodd-whole-game-promotion-v1` receipt. CMake reruns
`training.whole_game_multislot_promotion verify` and rejects missing, altered
or incomplete evidence. The receipt generator checks that the export belongs
to the frozen checkpoint and disjoint replay releases; that a causal audit
covers at least eight games per matchup with nontrivial command quality; that
the same Win32 x86 executable agrees with the checkpoint at early, mid and
late game frames; and that a late-game 100+ entity benchmark fits the frame
budget. It also checks that the evaluation DLL embeds those exact weights and
that its paired live games have no timeouts, model errors or callback overruns.
The live trace check rejects the repeated early mass moves that sent Probes
toward the map centre. Promotion then requires at least 72 games on the same
maps, opponents and host sides as the established bot, opponents of all three
races, an overall win improvement, and no severe race-specific regression.
The tournament manager does not expose identical game seeds across campaigns,
so this comparison controls schedule and binaries but cannot prove seed pairing.

`PROTODD_WHOLE_GAME_EVALUATION_BUILD=ON` builds
`ProtoddEvaluation.dll` for local paired games before a receipt exists. Its
post-build record binds that DLL's SHA-256 to the source fingerprint; promotion
rejects changed compiled source or a different evaluation DLL. The causal
audit separately pins its own code and every model/encoder file pinned by the
GPU fit. The
normal tournament build leaves that option OFF and requires the complete
receipt. Exported packages default to `tournament_ready: false`; no checkpoint
is promoted by export alone. The version-1 synthetic probe still compiles for
diagnostics but cannot take primary control at runtime.
The free-running validation audit also records early predicted Probe position
orders and counts repeated four-Probe orders to one 64-pixel target cell across three
decision frames. Promotion requires this count to be zero, so the observed
map-centre failure can be caught from held-out replays before any live game.
The source-pinned width-512 GPU fit and its 24-game causal audit completed on
the disjoint validation release. The checkpoint was exported and passed Win32
early, mid and late replay parity. A 138-entity late-game fixture with six
active slots took 18.23 ms median and 19.14 ms at the 95th percentile in the
standalone Win32 probe. The complete BWAPI callback and paired strength still
need evaluation before tournament promotion.

The full replay release was restarted as `whole-game-release-v32d-20260923`
after one valid replay repeated an owned unit ID in a selected-unit list.
Identical actor evidence is now canonicalized before causal analysis and label
generation; conflicting duplicate evidence still fails. The formerly failing
replay passed 121 reference checkpoints and produced 2,281 candidate labels.
The new release reused 8,344 hash-verified shards, committed that repaired
game, and resumed the remaining extraction. Its first new progress receipt was
8,354/12,563 games with zero failures. The conditional teacher's bounded
held-out action-kind result was 7/72, below the prior streaming baseline's
11/72; it is not a promotion candidate. The focal and six-slot fits remain
separately gated.
`start-multislot-offline-gates-after-fit.ps1` now waits for that fit and audit,
then exports the actual checkpoint, runs Win32 early/mid/late replay parity,
and produces a reproducible 20-run late-game timing report. The benchmark
pins the executable, weights, input, output and active slot count. It does not
substitute for the complete BWAPI callback measurement or paired games.

A paired diagnostic control campaign prepared without the local client/server
bundle failed before either game entered a StarCraft game state. Its frame -1
reports are launch failures, not model failures. The replacement campaign
`development-15-scheduled-control-owned` uses the verified owned-process
client/server bundle and the frozen synthetic move-toward-centre weights; its
first game reported normally but visibly sent probes to the map centre and had
13 frames over the 55 ms limit. The second game ended abnormally after the
visible diagnostic was stopped. That synthetic controller is now barred from
primary control by the version-2 guard; this campaign is not a strength result.

`start-full-corpus-after-release.ps1` completed its fresh-start smoke fit,
balanced 2,400-games-per-matchup fit, and 24-game disjoint causal audit. The
v32d release receipt covers all 12,563 selected games with no extraction
failures, and the launcher status is complete. The 76,800-step teacher reached
2.933 mean held-out validation loss, compared with 4.372 for the earlier
160-games-per-matchup teacher. Its continuous-memory causal audit nevertheless
matched only 34 complete command signatures out of 28,715 slots, 74 of 7,965
non-right-click actions, and 26 of 20,829 position targets within 64 pixels.
It missed all audited attack and attack-move actions. This fails the offline
promotion floor, so this checkpoint has no tournament control receipt. A
diagnostic suggests a mismatch between the eight-observation training history
and memory carried across an entire game during inference. A separate 24-game
probe resetting memory every eight decisions raised complete signatures to
72/28,715, non-right-click matches to 200/7,965, and position targets within
64 pixels to 48/20,829. It still matched no attack or attack-move actions and
remains below the promotion floor. This diagnostic cannot substitute for a
source-pinned audit of a matching compiled runtime. No live control or strength
campaign was run for this checkpoint.

A bounded first-slot diagnostic on one held-out game per matchup isolated a
second exposure gap. The decoder predicts an actor first, then predicts the
action kind conditioned on that actor. Training conditions the kind head on the
replay-confirmed actor, while causal decoding uses the actor head's choice.
With the replay actor supplied for diagnosis, the kind head selected
`attack_move` in 58/152 first-slot examples; free decoding selected it in
0/152 and selected the correct actor in 8/152. The corresponding
`attack` result was 0/17 even with the replay actor. These results make actor
ranking and actor/kind decoding the next bounded experiment. The diagnostic
uses future replay labels and is never promotion evidence.

A proposed inference-only joint actor/kind score was checked on three train
games omitted from the 2,400-per-matchup fit. Across 1,761 first-command
examples, the existing actor-first decoder matched 254 actor/kind pairs;
joint scores using actor log probability weights of 0.5, 1, 2 and 4 matched
195, 221, 242 and 252 pairs respectively. All variants still matched zero
`attack` and `attack_move` kinds in this cohort. This shortcut did not improve
the decoder, so it was not applied to the runtime. Further work should test an
actor-ranking training objective and improve STOP, combat-kind and position
prediction before another full-corpus fit.

A bounded actor-ranking fine-tune froze the full-corpus backbone and all
other heads, then trained only the first-slot actor key/query on five train
games per matchup omitted from the original fit. On one separate omitted train
game per matchup, first-command actor matches rose from 380/1,761 to
583/1,761. Its 24-game periodic-reset causal probe used the same validation
frames as the prior reset probe: first-actor matches rose from 1,019 to 1,473,
complete signatures from 72 to 93, and positions within 64 pixels from 48
to 143. Attack and attack-move kind matches were still 0/331 and 1/2,040;
non-right-click and complete-signature rates remained below promotion floors.
This is an experimental checkpoint, not a replacement for the source-pinned
full fit or a live-control candidate.

Scaling that actor-only objective from five to 30 omitted train games per
matchup improved the three-game development actor score from 583 to 627 out
of 1,761, but the 24-game causal audit showed little net benefit: 95 complete
signatures versus 93 for the smaller actor fit, 227 non-right-click matches
versus 240, and 127 position targets within 64 pixels versus 143. The smaller
actor checkpoint remains the better balanced diagnostic. Two bounded
kind-head exposure fits were rejected: uniform rare-kind sampling damaged
common right-click accuracy, while a mixed loss preserved common accuracy
but still recognized no combat actions. A separate kind-first prototype
improved development actor/kind pair counts largely by predicting right-click
and also failed to recognize combat. None is a causal or promotion candidate.

Position errors are an independent bottleneck. In the smaller actor fit's
24-game audit, even slots with both the correct kind and actor had only
95/2,074 target positions within 64 pixels and a 493-pixel median error.
On three omitted train development games, with the replay actor and kind
supplied solely for diagnosis, the existing highest-weight mixture decoder
placed 93/1,172 first-slot targets within 64 pixels. Selecting the component
with the highest density peak placed 81, and using the mixture mean placed 76.
The existing decoder was therefore retained. Future training must improve
position evidence and action/actor conditioning together; changing mixture
selection alone did not solve the problem.

An omitted-train geometry check found that 3,579/11,087 position labels lay
within 64 pixels of a visible encoded entity and 7,405/11,087 within 128
pixels. This justified a bounded visible-entity pointer probe with the replay
actor and kind supplied for diagnosis. On the separate three-game development
cohort, 360/1,172 first-slot targets had a visible entity within 64 pixels.
The original mixture placed 93/1,172 targets within 64 pixels. The first
pointer fit learned to select no pointers; a balanced fit selected pointers
but its best gated score was 69/1,172, still worse than the mixture.
Neither pointer head was saved as a candidate. A future position head needs
spatial map-location evidence and a reliable fallback for targets away from
visible entities, then a causal audit with predicted actors and kinds.

A bounded 64-by-64 spatial map-cell head was then trained on the same 15
omitted train games, using the frozen full-fit state plus terrain, vision and
visible-entity occupancy. Replay actor and kind were supplied only for this
position diagnostic. On the disjoint three-game omitted-train development set,
the best map-cell checkpoint placed 45/1,172 first-slot position targets
within 64 pixels (step 900), compared with 93/1,172 for the existing mixture.
At step 1,200 it placed 44/1,172; its median error was 250 pixels, compared
with 228 pixels for the mixture. The head was rejected and no candidate
weights were saved. Evidence is in
`artifacts/replay-learning/whole-game-multislot-spatial-target-probe-20260924/report.json`;
the source file is `training/whole_game_multislot_spatial_target_probe.py`.
This does not warrant another full-corpus fit. The remaining research target
is a position architecture that improves upon the mixture and an actor/kind
conditioning scheme that recovers combat commands without collapsing to
right-click, followed by a causal validation audit and the live gates.

A separate mixture-anchored spatial refinement probe began at the existing
mixture location and learned a map-cell correction on the same omitted-train
cohort. The untrained 64-by-64 grid snap placed 104/1,172 development targets
within 64 pixels, compared with 93/1,172 for the continuous mixture. Its
trained checkpoints placed at most 79/1,172 within 64 pixels. Training did
reduce median error from 228 pixels for the mixture to about 203 pixels at
step 900, but nearby-target accuracy fell to 63/1,172 at that step. No trained
checkpoint met both metrics, so no refinement head was saved or deployed.
This diagnostic also supplied replay actor and kind, and cannot satisfy the
causal command gate. Evidence is in
`artifacts/replay-learning/whole-game-multislot-spatial-refinement-probe-20260924/report.json`.

A bounded joint actor/combat classifier was then trained over the smaller
actor-ranked checkpoint's frozen causal features. It scored every owned actor
for attack, attack-move, or fallback and applied a margin to override the
source decoder. On 1,760 first-slot development examples, the source decoder
had 776 kind matches, 377 actor/kind pair matches and no combat-kind matches.
At step 300 and margin 2, the joint head recognized 38 combat kinds and 16
combat actor/kind pairs, but made 204 false combat overrides; kind matches
fell to 749 and pair matches to 370. At margin 3 it made only five false
combat overrides but recognized no combat. Later checkpoints improved neither
tradeoff enough: step 1,200 and margin 4 recognized three combat kinds and one
combat pair while making 34 false combat overrides. No checkpoint met the
predeclared development guardrails, so no candidate head was saved. The report
is `artifacts/replay-learning/whole-game-multislot-combat-pair-probe-20260924/report.json`.
The combat features have some signal, but the balanced training objective and
margin do not separate true combat from other commands reliably. Future work
should check label/context alignment and harder noncombat negatives before a
new fit. This probe did not justify a validation audit or another full-corpus fit.

A read-only omitted-train context check found no obvious temporal label shift:
first-command attack and attack-move both had a median five-frame delay after
the cadence observation on the three development games, compared with four
frames for right-click. Attack has strong local enemy evidence: 28/31
development attacks had a visible enemy within 256 pixels of the replay actor,
with a 98-pixel median nearest-enemy distance. Attack-move is different:
26/93 had a visible enemy within 256 pixels, and its median target was 563
pixels from the actor. Right-click had a nearby enemy in 347/1,140 cases, so
distance alone cannot distinguish these commands. The corresponding train
cohort showed the same broad pattern. Evidence is in
`artifacts/replay-learning/whole-game-multislot-combat-context-probe-20260924.json`.
The next bounded combat architecture should use explicit actor-relative enemy
geometry and hard right-click negatives, while learning attack and attack-move
as distinct decisions.

A geometry-aware joint actor/combat head added each owned actor's nearest
visible-enemy distance and direction, local enemy counts, location, cooldowns,
and global visible-enemy count. It trained on the same 15 omitted train games
with extra right-click negatives near enemies. At development step 300 and
margin 2, it recognized 6/93 attack-move kinds and 3 complete actor/kind pairs,
with 28 false combat overrides. Total kind and pair matches were 775 and 377,
versus 776 and 377 for the actor-ranked source decoder. This narrowly met the
probe's nonregression guardrail and saved a diagnostic head, but recognized
0/31 direct attacks and yielded no net pair gain. It is not a causal candidate.
Evidence is in
`artifacts/replay-learning/whole-game-multislot-combat-geometry-probe-20260924/report.json`.

Equal sampling of attack and attack-move in a second geometry fit recovered
direct attacks. At step 900 and margin 4, it matched five attacks with 14 false
combat overrides, but no attack-moves; total kind and pair matches fell to 768
and 371. At step 1,800 and margin 3, it matched 12 attacks and four
attack-moves, but made 155 false combat overrides and fell to 690 kind and 338
pair matches. No threshold at any checked checkpoint recognized both combat
kinds while satisfying the false-combat and overall command guardrails. No
balanced head was saved. Evidence is in
`artifacts/replay-learning/whole-game-multislot-combat-geometry-balanced-20260924/report.json`.
These results support distinct attack and attack-move decision paths, but the
current three-class actor head is not reliable enough. Additional evidence
would need to improve both combat recall and specificity on a separate
development cohort before another causal validation audit.

A legal cadence-observation feature offers a stronger position signal: each
owned actor's current `order_position`. On three omitted-train development
games, a fixed rule used that position when it was within 256 pixels of the
frozen mixture prediction and otherwise retained the mixture. With replay
actor and kind supplied only to isolate position quality, first-slot targets
within 64 pixels increased from 93/1,172 to 240/1,172, and median error fell
from 228 to 188 pixels. The rule was then frozen and checked on the disjoint
24-game validation cohort: 723/7,922 mixture targets versus 1,694/7,922 under
the rule, with median error falling from 230 to 192 pixels. The order position
was selected for 3,603 targets; it reduced error in 2,710 and increased it in
893. The cadence row precedes the supervised command, and the same order
coordinates are present in the BWAPI runtime observation and CPU encoder.
This is a position-only diagnostic using replay actors and kinds, not causal
command or promotion evidence. Reports are
`artifacts/replay-learning/whole-game-position-order-probe-20260924.json` and
`artifacts/replay-learning/whole-game-position-order-validation8-20260924.json`.

The rule was also applied after free-running six-slot decoding on the same
disjoint 24 validation games, using each *predicted* actor's cadence-observed
order position and no replay labels at inference. The paired baseline exactly
reproduced the smaller actor-ranked periodic-reset audit. Complete command
signatures increased from 93 to 241/28,715 and positions within 64 pixels
from 143 to 395/20,829. The signature counts improved in PvP 43→102, PvT
20→62, and PvZ 30→77. Position counts improved in PvP 78→192, PvT 24→99,
and PvZ 41→104. The arbiter selected the current order target in 5,180
decoded position slots. This is a substantial causal offline improvement, but
the fixed promotion floors still require at least 288 complete signatures,
417 position targets within 64 pixels, and 399 non-right-click kind matches;
the candidate has 241, 395, and 240 respectively. Direct attack kind matches
remain 0/331 and attack-move 1/2,040. The current order rule is therefore an
offline candidate for further work, not a promotion or live-control model.
The source-pinned paired report is
`artifacts/replay-learning/whole-game-position-order-causal-validation8-20260924.json`.
