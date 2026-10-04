# PvT strategy portfolio

The adaptive portfolio is disabled unless both the binary is configured with
`-DPROTODD_PVT_STRATEGY_PORTFOLIO=ON` and
`bwapi-data/read/PvT-portfolio-mode.txt` contains `adaptive`. When enabled, it
selects once at match startup and leaves that PvT strategy fixed for the match.
Provide `bwapi-data/read/PvT-match-id.txt` before each match with a unique
validator-assigned ID using letters, digits, `.`, `_`, `:`, or `-` (up to 128
characters). Reusing an ID with different result data is an error. The ID,
build version, selected arm, opponent, race, and map appear in selection and
result log records. The game seed is diagnostic only and may repeat. A missing
or malformed match ID disables adaptive selection. The existing
`PvT-strategy.txt` override has precedence. Only Terran opponents are eligible.
The default build therefore retains existing strategy selection behavior.

Frozen evaluation (`Protodd-learning-mode.txt` beginning with `frozen`) skips
adaptive selection and does not write portfolio history. The portfolio does
not treat BWAPI's `onEnd(bool winner)` as a validated result: that callback
cannot distinguish a played-out match from an opponent crash. The module logs
raw outcomes as `externally_validated=0`.

## Validating match results

Supply a trusted results ledger produced by the independent
evaluation/tournament validator. This tool checks pairing and consistency but
does not authenticate the ledger's origin; only pass it validator-produced
results. It must contain one row per match with these columns:

```csv
match_id,validator_id,opponent,opponent_race,map,strategy_version,strategy,outcome,completed,crashed,externally_validated
```

The offline validator pairs rows with `PVT_STRATEGY_SELECTION` and
`PVT_STRATEGY_MATCH_RESULT` in `Protodd.log`. It adds an outcome only when the
IDs and identity fields match, the logged arm and outcome match the trusted
ledger, the game completed, it did not crash, and the external validator marked
it validated. Missing pairs and mismatches are excluded.

Run the bounded exporter after each evaluation batch, retaining both output
files across batches. Point `--history` at the runtime's read-side portfolio
snapshot so the validator state and its derived CSV stay separate from the
bot's local write-side copy:

```powershell
python tools/pvt_portfolio_validate.py `
  --log bwapi-data/write/Protodd.log `
  --trusted-results validated-results.csv `
  --state bwapi-data/write/PvT-portfolio-validator-state.json `
  --history bwapi-data/read/PvT-strategy-portfolio.csv
```

Do not start the next evaluation harness until the exporter exits successfully.
If export fails, stop the harness and rerun the exporter; its state-first,
recoverable two-file commit repairs the cumulative CSV before it processes the
next batch.

The state file remembers processed IDs and complete result fingerprints,
including completion/crash/validation flags. Conflicts for an ID are rejected
before eligibility filtering, including a later row that marks a credited
match crashed or unvalidated. Rerunning a
campaign batch is idempotent, and distinct IDs count as distinct matches even
when their game seeds repeat. Reusing an ID with conflicting identity/outcome
data fails closed. The exporter writes the state and cumulative v1 CSV consumed
by the module. State contains a SHA-256 checksum over its canonical complete
payload, including processed IDs and result fingerprints, plus the derived CSV
checksum. This detects accidental state corruption; it does not authenticate
the state or the trusted-ledger source. Inconsistent or checksum-invalid state
is rejected before replay or history repair. If state and CSV atomic
replacements are interrupted, the next exporter run verifies state then
repairs the CSV before processing another batch. Legacy v2 state with
unchecksummed processed IDs fails closed; restore a trusted state backup or
rebuild state from a complete trusted results ledger. If dedup state is missing
while a nonzero history exists, the exporter stops rather than recounting old
matches. It bounds input bytes, line lengths before full-line allocation, row
counts, and counters. The module applies matching byte, line, and row limits
when loading history and atomically replaces its local snapshot.

## Cumulative history format

The runtime snapshot preserves the existing v1 format. The version column
isolates evidence for this strategy portfolio build:

```csv
# PROTODD_PVT_PORTFOLIO 1
# version,race,opponent,map,strategy,wins,losses
r3-siege-v3-pvt-portfolio-v1,terran,ExampleBot,Python,standard,12,8
r3-siege-v3-pvt-portfolio-v1,terran,ExampleBot,Python,safe-2gateway-range-observer,4,3
r3-siege-v3-pvt-portfolio-v1,terran,ExampleBot,Python,economic-1gateway-observer,8,2
```

A malformed or old-version snapshot is rejected transactionally. Same-opponent,
same-race, same-version outcomes from other maps may contribute up to four
virtual prior games per arm without counting as local-map games. Seeded
exploration visits legal untried arms in a deterministic order before using a
smoothed UCB score. An empty legality mask falls back to `standard`.
