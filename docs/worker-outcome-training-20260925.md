# Worker spending training games — 25 September 2026

## Why this experiment exists

The production-demand model lost its two-game local pilot, and a subsequent
longer-horizon worker-goal model failed fixed validation. One PvP validation
trajectory underpredicted future worker growth from frames 3,600 to 7,200.
Another fit against the same replay labels would not establish that extra
worker spending improves play. This experiment collects adjudicated **training**
outcomes from controlled worker-allocation interventions.

## Frozen pilot

`build/worker-outcome-20260925/pilot-plan.json` was written before any game.
Three conditions use the same local Win32 DLL, UABTerran opponent, both host
sides, Benzene and Destination maps, and actual BWAPI seeds 202609250–202609253.
Each condition runs four full games; the queue is sequential and never launches
duplicate fits or tournaments.

| Condition | Worker treatment from frames 2,400–7,199 |
|---|---|
| baseline | Existing planner |
| plus-one | Up to one additional desired worker; worker goal priority at least 100 |
| plus-two | Up to two additional desired workers; worker goal priority at least 108 |

The treatment caps its added worker target at 24 on one base or 32 on
multiple bases; it preserves any higher target already set by the planner.
It leaves defend/recover plans, prioritized reinforcements, mobile detection
emergencies and deliberate worker targets below 16 alone. It only adjusts the
existing planner's worker goal. Macro reservations, producer queues, building,
army, scouting and combat continue through the normal executor. It is compiled
only in the opt-in local evaluation build and enabled by the frozen training
mode file. All three conditions freeze policy learning during the comparison.

The per-game review requires two consistent normal Tournament Manager reports,
observed enemy activity, exact seed/map/host matches, the same DLL and opponent,
and the recorded treatment mode and exposure. Wins are the outcome labels.
The prespecified screen for **more training episodes** requires strictly more
wins than baseline, mean army at frame 7,200 at least 80% of baseline, mean
workers at frame 6,000 at least baseline plus one, and exposure in all four
games. Four pairs cannot establish a general win rate. No result in this
training pilot authorizes promotion or model control.

## Original pilot setup

Frozen campaigns are `build/worker-outcome-20260925/baseline`,
`build/worker-outcome-20260925/plus-one` and
`build/worker-outcome-20260925/plus-two`. The owned queue wrote
`build/worker-outcome-20260925/pilot-status.json`. A fully healthy completion
would have run `training.worker_outcome_review` to write `pilot-report.json`
with the outcome table, replay log hashes and sampled army/economy measurements.
Source and build receipts are under `build/worker-outcome-source-20260925` and
`build/worker-training-client-20260925`.

The training-game outcomes will guide the next learning objective and scenario
budget. No ranker is fit from this small pilot alone. A future learner needs
broader healthy training episodes and disjoint fixed confirmation before native
export, live control, paired strength evaluation or the 72-game gate.

`training.worker_outcome_dataset` extracts the frame-2,160 state strictly
before treatment begins. It pins the reviewed report, campaign manifests and
game logs by hash, and pairs the baseline and treatment by opponent, map,
seed and host. Only examples whose measured pre-treatment states exactly
match are eligible for a bounded fit. The fit gate requires at least 24 distinct
contexts, 48 eligible comparisons, and at least three positive and three
negative win differences. All samples remain training only.

### First pilot stopped at baseline health gate

The two Benzene baseline games produced normal Protodd win reports but
opponent `STARCRAFT_CRASH` reports. A UABTerran crash log shows a
`std::length_error` thrown inside BWAPI. Both games are excluded, not training
wins. The two Destination baseline games ended normally, both Protodd losses.
The prespecified 4/4 health gate stopped the queue before either treatment.
`build/worker-outcome-20260925/pilot-status.json` records `failed`; its
prepared treatment campaigns were never launched. Only the three recorded
Java launcher processes were stopped after their identities and start times
were checked. No model fit or promotion occurred.

### Destination-only recovery pilot

The next pilot was frozen before launch under
`build/worker-outcome-destination-v2-20260925`. It uses the same local DLL and
opponent, Destination only, four rounds with both host sides, and a rebuilt
local client with fresh seeds 202609260–202609263. Baseline, plus-one and
plus-two each have separate verified training manifests and frozen learning.
`pilot-plan.json` records the reason for changing maps. The earlier prepared
Destination campaigns under `build/worker-outcome-destination-20260925` use
old seeds and remain unlaunched; they are not evidence.

The owned sequential queue is running and writes
`build/worker-outcome-destination-v2-20260925/pilot-status.json`. It will stop
at a failed health gate and will not run duplicate fits. The fixed treatment
screen and no-promotion restriction above still apply. The outcome dataset
builder will run only after a qualified report exists.
