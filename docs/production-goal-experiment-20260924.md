# Population goals after the production pilot

## Completed local cycle: no candidate advanced

The previous command-count controller lost its matched pilot, 0/2 versus
reference 1/2. This cycle tested whether longer-lived population goals address
the observed worker-production gaps. Both variants remain offline research.
The joint model failed development. A narrower worker-only model passed a
separate train holdout, but failed reserved validation. No native export, arena
campaign, full-corpus fit or tournament promotion followed these results.

## Targets, scope and provenance

Inputs remain the 614 legal current-state and strictly past accepted-command
features. Targets are the peak completed Probe, Zealot and Dragoon populations
over the next 1,200 frames, with complete 24-frame observation coverage.
Missing cadence or a censored endpoint excludes that window; future observations
never enter the current feature row. Active incomplete units and waiting queue
entries are distinct in both extraction and live encoding.

The first variant additionally learns the first accepted command among types
with a population deficit. Same-frame ties are excluded. Its priority metric
is a raw offline classification diagnostic, not qualified live arbitration.

Training uses 36 games omitted from the earlier whole-game full fit, with nine
separate historical train-development games. Source snapshots, replay aliases,
tensor checksums, actual gradient exposure and criteria are recorded. These
cohorts are not globally fresh. No gameplay evaluation traces became labels;
final-test payloads remain sealed.

## Joint population/priority model

Two hidden layers of width 192 learn normalized population regression and a
priority classifier. The 512-row capacity check passed after 1,600 updates:
Probe MAE 0.043, army MAE zero, priority accuracy 100%. This verifies learnability
of the small sample only.

The bounded fit consumed all 9,073 training rows, with 3,200 updates and
1,638,400 row presentations. On 2,366 development rows:

| Measurement | Joint neural model | Linear population reference |
|---|---:|---:|
| Normalized population MAE | 0.0611 | 0.0553 |
| Probe MAE | 0.889 | 0.708 |
| Late worker-growth recall | 71.4% | 90.2% |
| Zealot MAE | 0.178 | 0.219 |
| Dragoon MAE | 0.111 | 0.090 |

Priority accuracy was 81.5%, below the 85.5% majority reference; macro recall
was higher, but the fixed accuracy gate also had to pass. The model failed
population-error, priority and late-worker gates. It was not selected.

## Second variant: worker-only learned linear goals

The stronger linear population reference motivated a narrower candidate.
Only Probe goals are proposed; army production would retain the reference
controller. Its coefficients are fit on the same 36 training games using ridge
regularization 0.01. No neural threshold or validation-dependent weight change
is used.

The persistent execution contract takes the maximum unexpired absolute goal.
Each proposal keeps its original 1,200-frame expiry; lower later forecasts
cannot erase it. Measured complete, incomplete, queued and pending work must
be counted once before issuing more. Tests cover repeated predictions,
expiry and replacing lost workers. This is a Python contract model; the new
worker policy has **not** been qualified in the native runtime.

Before reading a separate nine-game train holdout, the weights and criteria
were frozen: MAE <=1.25 workers, improvement over persistence/time-race
baselines, overall precision/recall >=90%, late precision/recall >=85%,
and growth recall >=70% in every game. The holdout has three metadata-selected
games per matchup, disjoint from fitting/development and the earlier full fit.

| Check | Rows | Probe MAE | Growth precision / recall | Late precision / recall | Result |
|---|---:|---:|---:|---:|---|
| Separate nine-game train holdout | 2,436 | 0.694 | 92.3% / 99.5% | 85.1% / 98.5% | Pass |
| Reserved 24-game validation | 6,496 | 0.834 | 95.8% / 94.4% | 94.5% / 85.2% | **Fail** |

The exact same model and gates were used for validation, with no optimizer
updates. Two games failed the per-game recall floor: one PvZ game at 67.8%
and one PvP game at 62.3%. Across all eight PvP validation games, late worker
growth recall was only 65.1%; PvT was 97.7% and PvZ 91.6%. Aggregate error
would have hidden this weakness. The gate remains failed.

The validation cohort was previously used by historical macro/production
experiments. It is disjoint from this fit, but not an untouched final test.

## Diagnosis and next dependency

The failing PvP trajectory had 18–20 committed workers at frames 4,800–5,976,
with future targets of 20–23; the model predicted only 16–19. These worker
counts lie within the training range, so this is not explained by a simple
out-of-range population count. This observation alone does not identify the
causal feature or predict a gameplay result.

The two bounded variants exhausted this population-head cycle. Do not keep
changing thresholds or fitting another similar head against these same
validation games. The next work should establish outcome-labelled **training**
scenarios for worker spending and recovery: qualify persistent worker ownership,
compare bounded allocation interventions against the same reference on matched
training seeds, and retain actual wins/losses and army/economy costs. Those
episodes are interventions, not assumed expert demonstrations. Use separate
evaluation seeds and retain the per-game and per-phase checks. A new candidate
must pass bounded learning/development and frozen confirmation before export,
live qualification, paired strength evaluation or the 72-game gate.

## Artifacts and verification

Under `artifacts/replay-learning`:

- `production-goal-capacity-20260924`
- `production-goal-development-20260924`
- `production-worker-goal-check-20260924`
- `production-worker-goal-confirmation-20260924`, including `failure-analysis.json`

Frozen sources are in `build/production-goal-source-20260924`,
`build/production-worker-goal-source-20260924`, and
`build/production-worker-confirm-source-20260924`. The verified cycle status is
`build/strength-first-20260924/production-goal-status.json`, regenerated with
`python -m training.production_goal_review`. The full training plan remains
incomplete. This cycle's fits and checks have all exited; no new arena is queued.
