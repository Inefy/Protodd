# Production model: live integration and bounded evaluation

Follow-up: the [population-goal training cycle](production-goal-experiment-20260924.md)
has completed. Its joint model failed development and its worker-only model
failed reserved validation. Neither replaced the controller evaluated below.

## Scope and controls

The confirmed 614-input production-demand model now has a live adapter. It
samples legal macro observations every 24 frames, freezes strictly past command
history at that sample, and predicts seven frames later. Matching producer
transitions establish history events; API success, observed spending and product
completion are logged separately. History is never backdated when confirmation
arrives after a sample. The observed build confirmation delay is up to eight
frames in the first shadow scenarios; replay/live timing is not identical.

Default builds support shadow inference only. Experimental command ownership
requires both `PROTODD_PRODUCTION_LOCAL_EVALUATION=ON` and the explicit
`local-train-units` mode in a separate local arena campaign. The arena preparer
requires a passing, hash-bound shadow/feedback receipt for the initial
7,200-frame screen. Full-game local comparisons additionally require a passing
control-screen receipt. Tournament configuration has not been changed.

The first control experiment is limited to **Probe, Zealot and Dragoon training**.
Buildings, technology, gas allocation, scouting and combat remain with the
existing controller. Forecast quantities form one persistent 240-frame quota;
the intervening 24-frame predictions cannot multiply that quota. Only real
available producer slots, legal current commands and reserved resources can be
used. Each quota item permits at most two failed attempts. Quotas expire, and
runtime load, invalid input or unresolved acceptance triggers fallback.

Native bookkeeping distinguishes accepted, spent, started, completed, rejected,
failed and uncertain work. A newly observed product releases its resource
reservation and producer lease, while its completion remains independently
tracked. Reusing a producer still requires the existing tested
`trainingSlotAvailable` rule and BWAPI legality. A prior product cannot be bound
to a second ticket.

## Experiments and fixes

All campaigns are under `build/strength-first-20260924`; all use frozen DLLs,
weights, opponent files and launch settings. Source snapshots are in
`build/production-live-source[-NN]-20260924`. Full input tensors, callback
durations and lifecycle logs are archived per game by the local manager.

1. `production-shadow-01` exposed a coordinate mismatch. Build commands contain
   the top-left tile; producer orders target the footprint center. The adapter
   stopped at its first Pylon. The obsolete campaign was ended and its partial
   evidence preserved under `aborted-diagnostic`; it is not a gameplay result.
2. `production-shadow-02` completed two host-swapped 7,200-frame scenarios.
   All 600 decoded outputs matched Python and all history features matched
   independently reconstructed causal history exactly. Full callback p95 was
   0.279/0.299ms; maxima 12.527/14.686ms, with no tournament timeout violations.
3. `production-feedback-03` passed input/timing checks but failed the feedback
   scope: 17/16 reservations were missed. Producers were held until completion,
   while the baseline queues their next unit within the latency window. Builder
   reassignment also left unresolved commitments. Assimilators create a morph
   lifecycle event, which the independent lifecycle checker now handles.
4. `production-feedback-04` isolates training actions and releases a producer
   after its product is bound. Its remaining misses identified the distinction
   between an empty queue and a queue slot available within network latency.
5. `production-feedback-05` uses the same tested queue-slot rule as the live
   baseline. Its first game tracked 23 reservations/spends and 21 completions,
   with two products still in progress at the cutoff and zero misses/errors.
   Both games passed: 48 reservations/spends, 44 completions and four products
   still in progress at cutoff, with zero reservation misses or binding errors.
6. `production-control-06` exercised actual local training control in two
   bounded games. Every fixed screen passed. Mean Probe count at frame 3,600
   was 15 versus reference 15; army starts through frame 7,200 averaged 4.0
   versus 4.5. This passes the 80% non-regression screen but does **not** show
   improved strength. Source snapshot `production-live-source-06-20260924`
   preserves the screen definition as it stood before launch.

No building-control gate has passed. Builder cancellation, reassignment and
spending reconciliation must be separately qualified before expanding that scope.
No new fit or final-test dataset access was needed for this integration.

## Predeclared bounded control screen

Before launching controlled scenarios, require the completed two-game shadow
and training-feedback receipt. Use the same Benzene map, UABTerran opponent and
both host sides for two bounded control games. These are diagnostic comparisons;
their random seeds are not matched to the reference and they cannot establish a
causal win-rate improvement.

The bounded screen requires all input/model parity, lifecycle binding and
callback checks, no fallback/error, and no severe production regression:
mean Probe count at frame 3,600 must be at least 90% of the two-game reference;
mean accepted Zealot plus Dragoon starts through frame 7,200 must be at least
80% of the reference. Thresholds are fixed before controlled games. A failure
stops advancement to full-game strength evaluation and is retained as evidence.
A pass only supports broader scenarios and seed-matched games, followed by the
72-game strength gate. It does not authorize tournament promotion.

## Full-game pilot completed: advancement failed

The subsequent `production-paired-reference-07` and
`production-paired-candidate-07` campaigns use the **same DLL**, model, opponent,
map, host-side schedule and local client. They differ in shadow versus local
train-unit control. Both are two-game, full-length development campaigns with
the ordinary 86,400-frame upper bound. Learned control ends at frame 7,200;
already-owned products continue to be observed until completion or failure.

The new reproducibly built client writes BWAPI's supported `seed_override` as
`20260924 + game_id` for each host. Actual engine seeds and map hashes must match
across conditions; this is verified from `MATCH` telemetry. The first reference
game reported seed 20260924 and won normally at approximately frame 25,300.

The predeclared pilot gate in `production-paired-07-plan.json` requires two
healthy normal outcomes per condition, exact seed/map/host matches, runtime
health and **strictly more candidate wins** before investing in the diverse
72-game gate. Two pairs cannot establish a statistically reliable win rate.

`scripts/continue-production-paired.ps1` owns sequential dispatch. It waits for
both reference games and clean runtime exit, verifies health, stops only its
recorded Java launchers, starts the already-frozen candidate, and writes the
paired review. Status is
`build/strength-first-20260924/production-paired-07.status.json`.
The supervisor completed and exited. All associated arena processes have stopped.
It created no app automation and performed no tournament promotion.

| Seed | Reference | Learned candidate |
|---|---|---|
| 20260924 | Win, frame 25,298 | Loss, frame 11,937 |
| 20260925 | Loss, frame 9,333 | Loss, frame 10,325 |

The candidate won **0/2**, versus reference **1/2**. All four games had normal
outcomes and passed the runtime checks. Actual seeds, map hashes, host sides,
DLL, weights and opponent matched. All 600 candidate and 600 reference early
input/output rows passed parity. Candidate callback p95 was 0.235/0.288ms,
with maxima 13.011/14.323ms. All 47 candidate and 42 reference production
tickets completed, with no outstanding reservations or binding errors.

`production-paired-07-report.json` records `advance_to_72=false`: the fixed
win-improvement gate failed. No 72-game campaign was launched. Two pairs are
insufficient to establish a population win rate, but do reject advancement
under this pilot's predeclared rule.

### Loss diagnosis and next training direction

In seed 20260924, the candidate had 19 Probes at frames 4,800 and 6,000;
the reference grew from 19 to 22. The candidate issued one further Probe
command at frame 5,287, but nine of its ten quota observations from frame
4,800 through 6,960 predicted zero Probes. At frame 7,200 the candidate had
19 Probes and two army units, versus reference 22 and three. Its base was
breached around frame 7,104. Technology timings were similar, so this is not
evidence of a simple technology-delay explanation.

This suggests a closed-loop production weakness in predicting short-window
accepted command counts. It does not prove the sole cause of either loss.
The coarse frame-3,600 worker and aggregate army-start screen missed this
later readiness gap. Per-game telemetry and hashes are preserved in
`production-paired-reference-07-loss-review.json` and
`production-paired-candidate-07-loss-review.json`.

The next bounded experiment should learn sustained worker/composition goals
and spending priorities, including recovery from production gaps, using
omitted train replays and explicitly designated training scenarios. These
evaluation traces remain diagnostic evidence, not expert training labels.
Require production/readiness measurements across the whole controlled window
and improved paired play before another full-corpus fit or strength campaign.
Building ownership and cancellation remain a separate unqualified scope.
