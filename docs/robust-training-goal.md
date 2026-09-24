# Whole-game training and verification

Active objective: test the games themselves and train all aspects of play to
produce a robust bot. Protoss comes first, followed by Terran and Zerg. This goal
is not complete when a replay predictor finishes fitting or a smoke game passes.
Local compute is authorized; cloud spending still requires a cost proposal.

The [strength-first plan](strength-first-plan.md), revised on 24 September after
the user's strategy review request, defines the active architecture direction and
work queue. Use replay learning to initialize coherent decisions, then establish
improvement through functional execution and matched games. Learned scopes expand
across the whole game; macro-only learning is still a baseline. The
[whole-game model log](whole-game-model.md) preserves the packet-model experiments.
The current reference campaign is `build/strength-first-20260924/baseline-01`.

## Coverage required before claiming completion

| Domain | Training/control work required | Verification required |
|---|---|---|
| Economy | Mining, worker/gas allocation, expansion, supply and production use | Income, idle-worker/producer time, supply blocks and losses under harassment |
| Macro/technology | Event timing, intent choice, persistent command execution, tech transitions and upgrades | Accepted-command feedback, resource contention, prerequisite/placement failures; non-wait recall and full games |
| Scouting and belief | Exploration, coverage, enemy-memory uncertainty, detection and information value | Fog/cloak/brief-sighting fixtures, unseen opponents and maps, scouting ablations |
| Army strategy | Composition, reinforcement, attack/retreat, defending multiple bases | Diverse rush/timing/economy opponents; paired game outcomes and failed-fight diagnostics |
| Micro and tactics | Movement, targeting, kiting, focus fire, spell use, transport and harassment | Unit/scenario fixtures, game engagements and each unit/ability's supported behavior |
| Long-term decisions | Temporal observations, opponent adaptation and delayed outcome learning | Causal sequence tests and independently adjudicated training episodes |
| Robust execution | CPU inference, time limits, pathfinding, recovery and legal observations | Exact compiled package, complete frame timings, crashes, stalls and tournament-host tests |
| Transfer | All matchups; Protoss first, then own Terran and Zerg | Race-specific models/controllers; maps, opponents and seeds excluded from training |

The current v2 replay data contains only macro intents, so it cannot supply all
these targets. Expand legal observation/action recording and the training models
for the missing domains, and use separately marked interactive training games.
Do not relabel macro imitation as full-game training. Retain a sealed final test
pool and never train from evaluation traces. Wins require healthy consistent
reports from both players; one bot's END message cannot establish that an opponent
did not crash. Hardware/runtime failures remain operational failures.

## Evidence and current work — 22 September 2026

- A0 macro reference completed 20 epochs; CPU export verified. Non-wait macro
  recall is only 4.94%; action timing remains an open issue.
- Rebuilt the current Win32 Protoss/Terran/Zerg DLLs. Frozen Protoss baseline:
  `build/robust-training-20260922/baseline-Protodd.dll`, SHA-256
  `0ec21a0176d092d6c1b001498d392073b9b941d72f0e1802ad7a38dfef79106e`.
- Actually played Protoss versus UABTerran on Benzene, requested/observed seed
  220922. Single-sided diagnostic result: loss at frame 10,914, with no caught or
  logging errors. Maximum own army three versus 23 observed enemy army units;
  first core 4,944, first Dragoon 5,688, first range 7,560, base breach 7,152.
  This exposes early production/defense weakness; it is not paired reward evidence.
  Manifest/log: `build/direct-logs/robust-20260922-baseline-001.{json,log}`.
- Added `training.arena` to prepare immutable training/development/final-test
  campaigns without relying on the missing 18 September workspace. It verifies
  runnable JARs, runtime paths, map archives, opponent packages and both host sides,
  preserves required opponent read data, and fingerprints all initial artifacts.
- Development-01 failed to enter games with UDP mode. Development-02 entered a
  real game using Local PC, but insanitybot reported STARCRAFT_CRASH at frame
  28,800; the other client failed collecting a missing crash-log directory. Both
  campaigns are stopped, preserved and excluded from training/reward evidence.
- Rebuilt the local client to tolerate missing crash logs and restrict cleanup
  to its exact runtime executable, avoiding termination of its peer's game.
  Prepared `build/robust-training-20260922/development-03`: 12 development games,
  BananaBrain / McRaveZ / insanitybot, Benzene / Destination, both host sides.
  `processes.json` records the actual server/client handles. Inspect those handles
  and logs rather than treating the manifest as proof of continued execution.
- Development-03 completed its first paired game: NORMAL reports from both
  players, loss to BananaBrain at own frame 20,896 / opponent frame 20,927. Own
  trace: maximum army 23, probes 38, two Nexuses; first enemy contact 10,800,
  first base breach 17,136, first Nexus loss 20,112; no caught/logging errors.
  These timings prioritize scouting and army control alongside macro work.
  One game does not establish a win rate or diagnose every cause of defeat.
- Development-03 stopped after its first game because its finished StarCraft
  processes denied forced termination and locked BWAPI.dll. Its game 0 telemetry
  was recovered with hashes in `development-03/game-0-archive-provenance.json`.
  `local-client-v5` now tracks exact launched-process ownership and waits for
  normal exit when termination is denied; `local-server-v2` archives each game's
  telemetry separately. Development-04, -05 and -06 verified two-game paired
  continuation and independent write archives without crashing.
- Development-04 Protodd lost both sides against McRaveZ on Destination at frames
  11,968 and 17,610. Development-05 with the opt-in whole-game live trace lost
  both at 14,789 and 11,968. Development-06, after observation normalization,
  lost both at 11,720 and 11,689. All had paired NORMAL reports and archived
  Protodd traces. These are baseline gameplay results; the learned model did not
  control the bot. Evaluation results remain excluded from reward import.
- Added evaluation-purpose rejection to both offline reward importers. Opening
  traces also require explicit validated-train mode. Campaign/trace mismatches
  must not turn development data into training episodes.
- Whole-game v3.1 native extraction passed a nine-replay training-split pilot:
  46,198 observations, 39,597 commands, 11,188 candidate action labels and 671
  reference playback checkpoints. The normalized v3.2 three-matchup pilot passed
  privacy and command-transition fixtures with source/binary hashes archived.
  Both replay and BWAPI adapters now use the same v3.2 observation serializer,
  with own technology state and static terrain.
- The exact-game development-06 parity audit aligned 480 frames per host side.
  Vision, own technology, upgrades, gas and walkability matched throughout;
  minerals and supply used matched 474/480 frames on each side. Entity signatures
  agreed for 53,604/55,639 and 50,667/52,796 live entity rows. Shield values
  and orders show remaining frame-phase differences; terrain height was excluded
  from comparison because BWAPI provides only build-tile height. Reports are in
  `development-06-whole-game-parity/game{0,1}-parity-fields.json`.
- Implemented a recurrent whole-game GPU model with legal entity, spatial and
  own-tech inputs, actor/target pointers, action and argument heads, and masked
  losses. The initial width-512 teacher fit on 192 training games completed 512
  updates but managed only 3/72 held-out action kinds. The 4,096-step
  action-weighted continuation reached 9/72 on the independent three-game
  audit. Its actor top-1 was 22/72, and its attack, attack-move, build, train and
  right-click top-1 were each zero of six. A supported kind/target-pair
  projection still reached only 9/72 full pairs, so legality projection alone
  cannot rescue this teacher. The 160-games-per-matchup width-512 streaming GPU
  continuation is running separately from the pinned replay extraction.
- The tournament path is GPU-trained **whole-game** teacher, measured against
  disjoint replays and paired games, then compiled CPU inference in the AIIDE
  Win32 bot. The tournament host has no GPU. A width-128 student smoke was
  exported, embedded, and checked against PyTorch; control-mode paired games
  stayed healthy but accepted zero learned commands, so that student has no
  gameplay-strength claim. Actor-balanced and kind/target-compatible action
  objectives are now isolated as an experiment, with gradient and minibatch
  tests. A three-training-game GPU smoke passed, including a disjoint validation
  report. A guarded local launcher now waits for the source-pinned baseline fit
  to finish successfully, then trains the structured objective from the same
  initialization and exact 160x3 replay cohort. Its status and logs live under
  `build/robust-training-20260922/whole-game-structured-160.*`; compare both
  objectives before adopting either one.
- A second audit samples commands at their actual frequency from disjoint
  complete validation games. At group 9/20 of the baseline streaming fit, only
  2/288 natural commands had the correct top-1 kind; 207 were right-clicks and
  none was predicted as right-click. The first 3,455 extracted replay receipts
  contain 2.82 million right-click candidates of 3.93 million total, while the
  baseline gives every observed kind/category roughly equal action updates.
  The exact frozen 480-game training cohort is 69.4% right-click, 8.6% train,
  8.5% attack-move, 2.8% build and only 0.19% cancel-queue by candidate count.
  A prior computed strictly from the frozen training cohort made 207/288
  correct by predicting right-click on **every** example, so that number does
  not demonstrate learned play. Target-mode, actor and rare-command quality
  remain open. The natural audit must accompany category-balanced validation.
  `training.whole_game_audit_compare` refuses comparisons across different
  held-out frames and reports non-majority recall beside overall accuracy.
- A third, source-hashed GPU ablation mixes replay-frequency action sampling
  with rare-kind coverage while keeping the structured objective, initialization
  and cohort fixed. Its local GPU smoke passed with the production two-worker
  collector. A guarded launcher waits for the structured run to finish cleanly,
  then starts this mixed run; status and logs are under
  `build/robust-training-20260922/whole-game-mixed-160.*`.
- The compiled execution path was proven separately using an explicitly
  diagnostic width-128 policy that always proposes map-centre moves. Its
  exported weights matched the Win32 CPU evaluator within 3.6e-7 and matched
  the bytes embedded in the control DLL exactly. The paired
  `development-13-control-execution-probe` campaign ended normally on both
  host sides, with 2,158/2,158 and 2,145/2,145 learned commands accepted,
  zero timeout frames, no model errors, and maximum complete callbacks of
  31.89 and 21.06 ms. The controller audit reports operational health but
  `strength_validated: false`; this synthetic policy lost both games and is
  strictly a command-path test, never a candidate tournament weight package.
- The width-512 baseline streaming fit completed 5,120 GPU updates on its
  frozen 160-games-per-matchup cohort. Its independent category-balanced audit
  improved to 14/72 action kinds and 30/72 actors, but still got 0/6 build,
  0/6 train, 0/6 attack-move and 0/6 right-click kinds. On the natural-frequency
  288-command audit it got only 5 top-1 kinds, including 0/207 right-clicks.
  This teacher is not a candidate for distillation or tournament control.
  The structured-objective full fit started automatically after baseline
  completion; the mixed-sampling fit remains queued behind it.
- Argument and location auditing exposes a separate failure: in the same 72
  balanced actions, the teacher chose the correct target entity on 6/13,
  a position within 64 pixels on 1/26, and the correct unit type on 1/12.
  Even the nearest of all 12 Gaussian position components was within 64 pixels
  on only 1/26, so changing the component-selection rule cannot fix it.
  A spatial 16x16 map head with actor/kind context now has gradient and decoder
  tests. With oracle actor and kind, a fixed 512-step probe over eight train
  games per matchup fit 167/196 training positions within 64 pixels but only
  1/26 disjoint validation positions. Its validation median error fell from
  2,567 to 869 pixels, but this remains far from useful control. The next
  position experiment needs more distinct games and must be rechecked on the
  full validation split after extraction; it is not yet a CPU runtime feature.
- A source-pinned, resumable GPU spatial stream now trains a 16x16 map head on
  the same frozen 160-games-per-matchup cohort. It keeps an encoded replay
  buffer across groups and adds player-visible own, enemy, remembered-enemy,
  and neutral geometry to the vision/walkability grid. A real three-game GPU
  smoke completed; a two-group pause/resume smoke completed; the final
  seven-channel path got 1/26 positions within 64 pixels on the tiny disjoint
  benchmark after only four updates. With the compiled decoder's owned-actor
  filter and legal kind/target-pair projection, the same smoke scored 13/26
  correct actors and 1/26 correct kinds on those position commands, with 0/26
  both correct. It records location quality with both oracle and predicted context
  to prevent a misleading standalone win. This is a diagnostic, not a
  controller. The guarded launcher waits for the mixed action fit to
  finish before starting the 480-game spatial run; status and logs live under
  `build/robust-training-20260922/whole-game-spatial-160.*`. Promotion requires
  held-out location gains with predicted actor/kind, a compiled decoder, and
  paired-game strength.
- A second GPU architecture experiment conditions command kind, target mode,
  unit type, target entity, and Gaussian location on a soft selection of owned
  actors. Its added heads start at zero residual on the verified baseline,
  while the backbone remains trainable. A real three-game fit/validation smoke
  completed and unit tests confirm initial output parity, owned-actor masking,
  and gradients to the new heads. A source-pinned 160-games-per-matchup fit with
  the same mixed schedule and structured objective is queued after the spatial
  GPU slot. The disjoint action auditor can load both model families and compare
  their predictions on identical examples. This is an offline teacher
  candidate; it has not been distilled, compiled, or shown to win games.
- A source-verified diagnostic snapshot of the structured run at group 14/20
  was audited on the exact held-out baseline examples. It tied the completed
  baseline at 14/72 balanced kinds but fell from 30/72 to 23/72 actors. On the
  288-command natural audit it rose only from 5 to 8 correct kinds while
  non-right-click recall fell from 5 to 2; 207/288 true commands were
  right-clicks, and it found 6 of them. Position within 64 pixels stayed 0/209.
  This is interim evidence, not a reason to interrupt the source-pinned fit;
  final checkpoints and the mixed/conditional ablations decide what to scale.
- The structured objective finished all 20 groups and 5,120 GPU updates. On
  identical held-out examples, it scored 13/72 balanced kinds and 29/72 actors
  versus the completed baseline's 14/72 and 30/72. Both scored 5/288 natural
  command kinds; structured reached 102/288 actors versus baseline 105/288.
  Position and unit-type arguments remain poor. This objective alone is not a
  scaling candidate; the automatically started mixed-sampling and queued
  actor-conditioned fits must be assessed separately.
- A causal full-trajectory event audit found 1,418 action windows in 1,791
  disjoint natural cadence windows (79.2%), while the baseline teacher averaged
  only 57.0% action probability. At a 0.5 gate it recalled 55.5% of action
  windows; its Brier score was 0.260 versus 0.165 for the validation-rate
  constant predictor. The frozen 480-game training cohort independently has
  266,231/339,500 positive windows (78.4%), implying a +1.290 train-only logit
  prior correction from the event-balanced training mix. That correction cut
  held-out Brier to 0.193 and raised 0.5-gate recall to 91.2%, but precision
  fell to 78.5%; matchup-specific train priors reached Brier 0.191. These are
  timing diagnostics, not a live strength result. The queued conditional fit
  now samples event classes at each train group's natural frequency while
  retaining equal matchup exposure. The modified conditional trainer completed
  a real three-game GPU smoke with source hashes and held-out validation; this
  verifies execution, not strength. Deployment still needs joint command
  legality and paired-game evidence.
- A separate cadence-action audit scores the first confirmed command after
  each observation the compiled policy could actually consume. In the same
  three held-out games, the baseline got only 34/1,418 first kinds (65/1,418
  if any later command in that window counts), 25/1,418 supported kind/mode
  pairs and 422/1,418 actors; it predicted zero right-clicks despite 815
  right-click first commands. The structured fit reached 47/1,418 first kinds
  and 402/1,418 actors, still far below the 815/1,418 majority-kind prior.
  This is the deployment-aligned gate for future models, not the easier
  before-command audit alone. The queued conditional fit now replaces its
  command-frame action buckets with bounded train-only cadence examples labeled
  by the first confirmed command in the following 24 frames. A real 1x3 GPU
  smoke with this exact collector completed held-out validation; future labels
  were never inputs, and its source hashes are pinned for the full fit.
  The cadence comparison now requires the same held-out game, frame, command
  and actor-known target for every candidate. On identical 1,418 action
  windows, the structured fit scored 47 first kinds, 22 supported kind/mode
  pairs and 402 actors versus baseline 34, 25 and 422; the right-click
  majority prior gets 815 kinds. This prevents a small top-1 increase from
  being mistaken for deployable improvement. A local gate watcher now waits
  for the queued conditional fit, audits mixed and conditional checkpoints on
  the same cadence windows, and writes a four-fit comparison against baseline
  and structured. It runs on CPU after the sequential GPU fits, so it does
  not contend for the current training slot.
- The GPU-to-tournament distiller now loads either the baseline or
  actor-conditioned teacher, while always producing the compiled runtime's
  narrower baseline student. An opt-in cadence action source chooses the same
  bounded train cohort and supervises the first confirmed command after each
  deployable observation. A three-game real-shard collection yielded 1,410
  action windows and 61 bounded examples; a real cadence sample produced a
  finite conditional-teacher/student distillation loss and a student gradient
  on CPU. This checks the transfer path, not the student's playing strength.
  Its balanced validation remains command-frame based, so a separate cadence
  audit and paired games are still required before promotion.
- A new group-streamed distiller transfers either baseline or conditional GPU
  teachers to a narrower compiled-runtime student without retaining the full
  corpus in host RAM. It uses cadence-first-command action labels, mixed
  natural/rare action exposure, natural event frequency, per-group optimizer
  checkpoints and source/teacher hashes. A real 1x3-game, four-update RTX
  5070 Ti smoke completed training and held-out validation; its width-64
  student exported to the existing binary format and matched the compiled CPU
  evaluator on two replay observations (maximum absolute error 2.61e-7).
  A two-group pause/resume run and an uninterrupted run then produced exactly
  equal values for all 61 student tensors and identical held-out validation
  loss. This verifies the handoff and restart machinery only. Student
  cadence-action and paired-game strength gates remain before production fit.
  Distillation now reuses its already computed student forward for the
  supervised loss. Action, event and forecast losses and parameter gradients
  matched the original duplicate-forward implementation in focused tests; a
  fresh four-update GPU smoke completed held-out validation with this path.
- The 160x3 mixed natural/rare action fit finished 5,120 GPU updates. On the
  exact independent held-out before-command examples, it scored 6/72 balanced
  kinds (baseline 14/72) and 195/288 natural kinds, but zero correct
  non-right-click natural kinds (majority right-click alone gets 207/288).
  At deployable cadence it scored 760/1,418 first kinds, including only two
  non-right-click kinds, versus the 815/1,418 majority prior. Supported
  kind/mode pairs rose to 537/1,418 and actors to 454/1,418, but those gains
  do not make it a whole-game controller. Do not scale this objective alone;
  assess the queued actor-conditioned cadence fit before selecting a teacher.
  A diagnostic subtraction of half the frozen training-cohort log kind prior
  recovered only one non-right-click natural kind and reduced natural top-1
  from 195/288 to 48/288. The collapse is therefore not rescued by a simple
  train-prior correction; no validation-tuned bias was promoted.
- A separate actor-conditioned cadence fit now replaces the categorical kind
  cross-entropy with gamma-2 focal loss while keeping the same structured
  actor loss, natural event schedule, mixed action exposure, frozen 160x3
  replay cohort and initialization. Its real 1x3-game, four-update GPU smoke
  completed held-out validation; the full fit is queued after the current
  conditional run so the GPU jobs remain sequential. This is an ablation,
  not a presumed improvement. A watcher will score baseline, structured,
  mixed, conditional and focal on identical windows from the expanded
  24-game validation cohort before any teacher is selected.
- The deployable-cadence audit now also checks visible entity targets, map
  positions, unit types, queued flags and other known command arguments after
  the model has made its causal prediction. On the original three held-out
  games, the baseline retained its exact 34/1,418 kind, 25/1,418 supported
  pair and 422/1,418 actor scores; newly measured position accuracy was only
  10/978 within 64 pixels, visible target-entity accuracy 24/258, and full
  known-command signatures 7/1,416. The expanded comparison includes these
  metrics only when every candidate has them, preventing old reports from
  being mistaken for zero scores.
- The earlier teacher comparisons use only one disjoint validation game per
  matchup. A separate source-pinned v3.2c full-prefix release is extracting
  eight frozen train and eight validation games per matchup with one worker,
  followed by an exact-MMR quality index. Its 24 additional validation games
  will support wider matchup, map and player checks before choosing a scale-up
  teacher. The sealed test split remains untouched.
- The ongoing full v3.2c extraction found one PvZ replay that the current label
  materializer rejects. A local re-extraction matched all 121 reference
  playback checkpoints, so this is not a playback divergence. The replay has
  65 late-game commands whose selected-unit list repeats one owned unit and
  whose repeated actor-effect records are identical. Strict canonicalization
  of only identical duplicate evidence allowed all 7,351 commands to validate
  and yielded 2,281 candidate labels in a diagnostic copy; conflicting
  duplicates still fail. The original release is source-pinned and continues
  extracting without edits. A new versioned label/release path must incorporate
  this repair, reuse verified clean shards with explicit provenance, and pass
  full release verification before the corpus is declared complete. The
  versioned migration helper now verifies every source receipt and compressed
  artifact, requires unchanged replay/native/assets inputs, hard-links only
  complete shards, and issues destination-identity receipts with source
  provenance. Its focused corruption, compatibility, and resumability tests
  pass. It will run only after the active extractor finishes and the repaired
  label source is pinned in a new release identity.
- The completed 24-game disjoint validation release exposed a structural limit
  in the current single-command head. Across 11,597 positive 24-frame cadence
  windows there are 28,906 commands, including 7,982 multi-command windows and
  3,153 multi-kind windows. An oracle one-packet policy covers at most 13,156
  commands; six chronological individual command slots cover 28,715, but
  6,689 windows repeat an actor, so slots require paced, legal dispatch. The new
  `whole_game_window_capacity` audit and causal `whole_game_cadence_sequences`
  reader establish the data contract for a six-slot GPU teacher. The GPU
  policy remains the intended tournament controller after distillation to
  compiled CPU inference and paired-game promotion gates.
- The six-slot teacher and bounded sampler are implemented with actor-first
  command decoding, STOP, supported modes, target/position/argument heads and
  within-window dispatch delay. One 1x3-game CPU fit completed train and
  disjoint validation; a two-group paused/resumed fit matched an uninterrupted
  run exactly across all 103 model tensors and held-out rows. A source-pinned
  launcher now waits for the focal GPU fit, runs a width-512 CUDA smoke, and
  then fits the same frozen 160x3 cohort with eight validation games per
  matchup. This is an experimental teacher and is not tournament-promoted.
- The spatial-only head completed 2,560 GPU updates but scored 0/26 positions
  within 64 pixels on its tiny oracle-actor/kind held-out set; this does not
  justify adopting that head yet. The position component remains a major
  strength risk for the six-slot teacher and needs larger causal validation.
- The intended width-512 GPU teacher now has a versioned six-slot compiled
  export/inference path and a 24-frame command scheduler. On two real early-game
  replay observations a synthetic six-slot package matched PyTorch within
  0.0000182 in the checked heads. Shared entity projections reduced standalone
  compiled forward time to 26.6–27.4 ms for six active slots. This makes the
  trained checkpoint deployable in principle, while later-game callback time,
  legal command coverage and match strength remain unverified.
- A later 124-entity replay exposed a 77 ms compiled forward pass. Sharing the
  backbone entity embedding and using SSE2 for dense dot products brought a
  20-run Win32 standalone measurement on that fixture to 20.9 ms median and
  28.0 ms 95th percentile in a saved benchmark. Late-game and three-matchup mid-game parity pass
  within 0.000001 for the synthetic width-512 package in 32-bit execution. A six-slot control
  build now rejects weights without an exact-hash promotion receipt. The
  complete live callback budget and trained-policy strength remain open gates.
- The live adapter skips whole-game entity scanning on frames without a model
  observation or scheduled command while preserving current-state legality
  checks at dispatch. Both Win32 control and shadow DLLs compile, and the
  schedule probe passes; paired live callback timing remains unmeasured for a
  trained six-slot policy.

Inspect the campaign:

```powershell
./build/model-venv/Scripts/python.exe -m training.arena inspect build/robust-training-20260922/development-06-whole-game-parity
```

Next: complete the actor-conditioned and focal objectives, then run the queued
width-512 six-slot GPU fit and 24-game causal audit. Export that frozen teacher
only if the audit improves non-majority battle, economy and production
decisions and full command signatures. Verify compiled parity on disjoint
replays, measure late-game full callback latency, then run paired matches
against the frozen baseline and diverse opponents. Scale the best objective
to the remaining high-MMR corpus only after those gates. Development and
final-test campaigns remain excluded from training.
