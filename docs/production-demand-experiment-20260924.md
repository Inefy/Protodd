# Concurrent production demand experiment

**Live continuation:** causal inputs, native unit-production feedback and full
callback checks now pass. Two bounded local control games passed the fixed
production screen. The completed seed-matched pilot failed advancement:
candidate 0/2 wins versus reference 1/2. The queue has stopped; see
[production-live-integration-20260924.md](production-live-integration-20260924.md).
The remaining-work statement at the end of this file records the earlier offline
phase, before live integration. No tournament promotion has occurred.

## Hypothesis and scope

Continue the strength-first plan with independent bounded production quantities,
instead of another exclusive next-action classifier. The old macro extractor
keeps the first accepted action per 24-frame window; its verified full action
logs preserve additional accepted actions. The new target uses all of them.

Predict accepted starts within the next 240 frames for Probe, Pylon, Gateway,
Assimilator, Cybernetics Core, Zealot, Dragoon and Nexus. Counts are bounded at
four for decoding, with overflow retained in evaluation. A future prerequisite
may become available, so current command masks cannot turn that future demand
into a negative target. Execution must still check actual current legality.
Accepted starts are not completion labels or proof of a good strategy.

Inputs are current legal macro features plus each scope action's strictly past
240-frame accepted count and age of its last accepted event. Same-frame events
belong to targets, never history. No future or enemy command enters features.
The action logs are bound through the tensor release's source manifest, sources
hash, extraction receipt and action-file hash. Every tensor positive must match
an accepted action. Replay aliases are checked across train/development cohorts.

## Fixed local budget and criteria (before fitting)

Use the already pinned 300 train / nine train-development game cohort from
`macro-commitment-early-cohort-20260924.json`. It is development material, not a
fresh validation set. Use observation frames below 7,200 and censor incomplete
240-frame target horizons. All final-test payloads remain sealed.

First: a deterministic 512-row capacity sample with positive coverage of each
action, 1,000 updates, batch 512. Require at least 95% exact component counts and
85% macro F1 on fitted examples before the larger bounded comparison.

Then, if capacity passes: one 3,200-update comparison, batch 512, game-balanced
sampling. The local CUDA model has two 192-wide layers, training-only feature
normalization and monotonic ordinal count outputs. Decode at the fixed 0.5
probability threshold. No threshold selection on development.

Reference predictions are zero demand, the training median count and repetition
of the preceding 240 frames. Require macro F1 at least 45% and five percentage
points above the strongest reference, count MAE at least 5% below the best
reference, and these separate event precision/recall floors:

| Type | Precision | Recall |
|---|---:|---:|
| Probe | 70% | 60% |
| Pylon | 50% | 40% |
| Gateway | 45% | 30% |
| Zealot and Dragoon, each | 40% | 30% |

Each required type needs at least ten positive development rows. Report all
eight types, per-game results, concurrency, uncapped count errors and exact
sample exposure. A pass permits confirmation and integration work only.

## Persistent execution contract

In parallel with the fit, implement an offline reference commitment executor.
An immutable proposal ID binds a type, bounded quantity and expiry. Repeating
that ID must not add demand. Each dispatched item has a ticket and one legal
producer/builder. It holds a single resource reservation until explicit spend
confirmation or rejection. Acceptance, spending and completion are separate
events; acceptance alone must not count as completion.

Duplicate feedback is idempotent. Occupied producers, unavailable prerequisites,
failed placement, actor death, cancellation, expiry and fallback must release or
retain ownership deliberately. Failed ambiguous dispatches are never retried
without a resolved outcome. The adapter supplies current legal candidates and
observed feedback; the policy cannot bypass legality or invent success.

This first executor is a research reference with deterministic scenarios. It
does not issue BWAPI commands. Native integration, observation-to-feedback
binding, parity, full callback timing and matched games remain required before
any live control. Reject the family if the bounded comparison fails; do not
automatically rescale or start more next-action fits.

## Development result and frozen confirmation

The capacity job passed: 512 fitted rows, 512,000 presentations, 98.61% exact
component counts and macro F1 96.41%. The larger run used all 87,933 selected
rows, 3,200 updates and 1,638,400 presentations. On 2,592 rows from nine separate
development games, macro F1 was 59.80% and count MAE 0.08936, versus 0.12428 for
the best simple reference. All predeclared development checks passed.

The selected early training period has 10,977 accepted scope events in the full
logs, versus 10,348 first-command tensor labels. Future windows contain multiple
positive types on 22,023 of 87,933 training rows. These are exposure/coverage
counts, not proof that missing labels alone caused the previous failures.

The completed fit hit a Windows sharing violation when publishing the next
status after its final checkpoint. Evaluation-only recovery verified the frozen
source, checkpoint step/exposure, cached tensor hashes and reproduced references.
It performed zero additional optimizer updates. `recovery.json` retains evidence.
Atomic JSON publication now retries a brief Windows replacement denial.

Confirmation uses the already reserved 24 validation games, eight per matchup,
and their exact reserved perspectives. Freeze weights and reuse the same gates,
decoding, frame range and reference definitions before opening those payloads.
No retraining or threshold adjustment. All 24 were exposed to the historical
macro model; they are disjoint from this fit but **not globally untouched**.
Keep final-test payloads sealed and report all eight output types/per-matchup
results. The 9.17% development recall for Assimilator is a material limitation;
the gate pass does not authorize control of gas production or any other scope.

Artifacts: `production-demand-capacity-20260924` and
`production-demand-development-20260924` under `artifacts/replay-learning`, with
frozen source and launch logs in the correspondingly named `build` directories.

## Confirmation and Win32 result

The fixed checkpoint passed the unchanged confirmation gate on 24 validation
games (7,139 early-game rows): macro F1 **56.88%**, count MAE **0.10001** and
90.64% exact component counts. Window targets overlap within games; these are
24 games, not 7,139 independent trials. No optimizer updates or threshold
changes followed development. Assimilator recall remains only **7.35%**;
retain that limitation when defining any eventual controlled scope.

| Required type | Confirmation precision | Confirmation recall |
|---|---:|---:|
| Probe | 76.70% | 81.21% |
| Pylon | 72.45% | 54.30% |
| Gateway | 55.50% | 58.89% |
| Zealot | 49.64% | 39.04% |
| Dragoon | 70.61% | 49.55% |

The standalone native export passed in a 32-bit Windows process on every one of
those 7,139 feature rows. All decoded quantities match Python CPU and CUDA;
maximum native/CPU logit error is 0.00003052 (predeclared tolerance 0.0002).
Model inference alone measured p50 61.9 microseconds, p95 70.8 microseconds and
maximum 718.2 microseconds. Feature extraction, online history reconstruction,
BWAPI callbacks, ownership, dispatch and logging are outside this benchmark.
It therefore **does not pass the complete callback gate**.

Weights: SHA-256
`3b2cc069b10d5e6a3f9377c33a40b4a9ec1c55d2a1f0c90d12d1901dcd084b20`.
The native binary, exact tested executable, input/output tensors and C++ source
snapshot are preserved in `artifacts/replay-learning/production-demand-native-20260924`.
It has no BWAPI execution interface and no tournament mode was changed.

The offline commitment reference now passes scenarios for repeated proposals,
single resource reservation, occupied/illegal actors, placement rejection,
bounded retry, accepted versus spent versus completed feedback, actor death,
uncertain dispatch, cancellation, expiry and fallback. Build start can release
the builder after observed spending without falsely claiming completion.
These tests use explicit synthetic feedback; a real observation adapter and a
native persistent executor are still required.

Final verification: **21 focused tests passed**, including target causality,
concurrent counts, monotonic count decoding, contract scenarios and the Windows
atomic-write retry. `python -m training.production_training_review` verifies the
frozen Python/C++ sources, cached tensors, checkpoint exposure, confirmation
binding and native artifact hashes. Its consolidated output is
`build/strength-first-20260924/production-training-status.json`.

**Next work:** implement and verify the causal live accepted-command history
adapter, then the native commitment executor and actual spend/completion
feedback. Measure complete callback timing in shadow/local scenarios before
paired controlled games and the 72-game strength gate. Neither offline
confirmation nor fast scalar inference demonstrates improved playing strength.
No training job remains active; no model has live or tournament control.
