# Combat forecast audit (opt-in)

This is observational telemetry only. It does not feed combat decisions,
opponent history, policy weights, or PvT strategy selection. It is disabled
unless `bwapi-data/read/Combat-forecast-audit.txt` begins with `on` (case
sensitive). The sample tree intentionally does not include that opt-in file.

At each combat update with visible local enemies, the audit starts or updates a
record keyed by a monotonically allocated match-local encounter ID and the
existing squad engagement key. The first available simulation forecast and
its frame, decisions, and observed roster are frozen; later simulation values
do not replace it. Rows include the latest observed outcome roster and its
frame, match seed, map hash, opponent race (0=unknown, 1=Protoss, 2=Terran,
3=Zerg, 4=Random), opponent-name hash, and numeric PvT arm (0=standard,
1=safe two-Gateway, 2=economic one-Gateway).

The audit does not capture the simulator's exact included unit IDs. A matching
forecast/outcome observed roster therefore cannot establish that the simulator
used that same roster; every row says
`simulationRosterCoverage=unknown`. The scorer only calls a matching exact-
horizon row `observationally_aligned_unverified_simulation`. It rejects any
claim that simulation roster coverage is complete. This label is not a claim
of calibrated forecasts or a causal effect.

The match ID is a 64-bit FNV-style hash of seed, map hash, opponent-name hash,
and match-start clock ticks. Repeated seeds therefore remain distinct when
the start ticks differ. As with any finite hash, collisions remain possible;
the separate identity fields are retained in each row to help detect them.

The audit tracks at most 32 encounter records and 64 units per record. A full
record buffer increments `droppedStartAttempts`, which counts start attempts
that could not be stored, not unique encounters. The fixed arrays bound memory
and total rows per match.

Forecast power/survival and observed HP+shields are incompatible quantities in
the current combat model. Every row therefore declares
`calibrationStatus=uncomparable_simulation_power_vs_hp_shields`; the scorer
suppresses all calibration accuracy metrics, including MAE and Brier score.
It counts every record close/censor reason, including early, friendly, and
enemy elimination, and calls out that the eligible horizon subset can be
survivorship-biased: encounters that resolve through elimination before the
fixed 336-frame (14-second) horizon are excluded from aligned diagnostics.

Retreat, lost contact, roster change (including reinforcement or
squad-composition changes), incomplete health, late horizon, and match end
are also censored. A unit disappearing from sight is never counted as
destroyed. A row and `COMBAT_FORECAST_SUMMARY` are written in `onEnd`; summary
counters include `created`, `flushed`, `censored`, `incomplete`, `duplicates`,
`droppedStartAttempts`, and `writeErrors`. As with any in-memory telemetry, a
process crash before `onEnd` loses the pending buffer.

Use `tools/score_combat_forecasts.py <Protodd.log>` for offline alignment and
flush diagnostics. It deduplicates by game ID, opponent, seed, map hash, and
encounter ID; reports only unverified observational alignment counts and
censor-reason counts; and always returns `calibration_metrics=None`. No
historical forecast/outcome rows were found in the archived logs, and no
missing outcomes were synthesized.

Focused offline fixture tests: `python -m unittest tests.test_combat_forecast_score`.
The tests use synthetic fixtures only to validate parsing, pair eligibility,
censoring, deduplication, and flush accounting. C++ lifecycle regression
scenarios are in `tests/test_main.cpp`; they were not run for this telemetry-
only correction.
