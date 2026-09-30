# Combat and economy improvements - 30 September 2026

The final candidate won both matched UAlbertaBot Protoss retests that the
previous default lost. These are development diagnostics, not evidence that
Protodd beats every bot. All live games used Protodd as Protoss.

## Changes

- A ready first Gateway funds its first mobile screen before optional opening
  infrastructure. Paid queues count toward the screen; busy or unpowered
  Gateways do not reserve duplicate cycles. Urgent detection and the PvZ static
  opening retain their existing precedence.
- PvP scouting now recognizes a remembered, unfinished second Gateway without
  requiring an experimental flag. Observed Core, Robotics, Dragoon or Reaver
  evidence prevents this construction-only melee classification.
- Against the scouted two-Gateway melee opening, fund the first escort, then
  start Forge and Cannon construction while it trains. Gas transition waits
  for a completed Cannon instead of consuming the intercept's bank.
- Scouted one-base double-Barracks production triggers earlier PvT static
  defense funding, with temporary worker and gas restraint. Factory evidence
  prevents this production-only bio classification.
- A surviving army can request replacement mining before owned minerals run
  out. A high-priority Nexus protects the full 400-mineral fund against routine
  production. Worker/main breaches cancel it; decisive and supply-cap closeouts
  do not recall their armies for this growth rule.
- Frozen deployment with no loaded policy weights no longer lets the untrained
  policy fallback replace strategic attack/pressure with Hold. Explicit training
  mode and loaded policy snapshots remain supported.

## Validation

Final Win32 Release build: 47/47 CTest suites passed. Portable Release build:
46/46 passed, for 93 passing suite executions across the two configurations.
The new income/screen suite covers queue budgets, detection precedence,
construction scouting, rush funding, mining recovery and breach/closeout guards.

Final DLL SHA-256:
`7649D842F7CBB18479AE98325557FB1E7AB92438D9344DB800DFF0E363B9E262`.
It matches the frozen final campaign copy. The source base was `f296418`.

## Matched Final Retest

Same local opponent packages and settings as the [previous diagnostic](uab-diagnostic-20260929.md):
Destination, 28,800-frame cap, frozen modes, empty initial learned reads,
manager bot-file learning off, DLL opponents rather than proxy launchers.
The final subset retained original game IDs, host sides and actual logged seeds.
Destination map hash: `4e24f217d2fe4dbfa6799bc57f74d8dc939d425b`.

| Game | Seed | Protodd Host | Previous Default | Final Candidate | Final Frame |
| --- | --- | --- | --- | --- | --- |
| 2 | 202609902 | Yes | Clean loss at 8,558 | Clean win | 26,693 |
| 5 | 202609905 | No | Clean loss at 9,488 | Clean win | 24,120 |

Both reports in each final pair were NORMAL, agreed on the winner, and ended
below the cap. Enemy visible army peaks were 13 and 18, with positive opponent
scores; these were active opponents, not empty matches. Both own summaries had
zero caught errors and zero logging errors. The reviewed empty-policy
pressure-to-Hold sample count was zero in both.

In game 2, Forge construction moved from frame 5,199 in the preceding candidate
to 3,414 in the final candidate. Its first Cannon completed at 5,024; the
preceding candidate never completed one and lost at 8,806. This trace directly
exposed the unfinished-second-Gateway classification fix.

## Earlier Iterations

These used different DLLs. Their results must not be pooled into a final-build
win rate. Each six-game campaign reused IDs 0-5 and seeds 202609900-202609905.

| Iteration | Terran | Zerg | Protoss | Accepted Total |
| --- | --- | --- | --- | --- |
| First screen/mining fund | 1 loss, 1 cap | 2 wins | 2 losses | 2 wins, 3 losses, 1 cap |
| Rush anchors/policy fix | 2 opponent failures excluded | 2 wins | 1 win, 1 loss | 3 wins, 1 loss, 2 excluded |
| Final production-memory fix | Not rerun | Not rerun | 2 wins | 2 wins |

The middle iteration's Terran games ended with opponent STARCRAFT_CRASH and
GAME_STATE_NOT_UPDATED_60S respectively. Neither is an accepted win, regardless
of Protodd's own report. No fault cause was established. The first iteration's
28,802-frame Terran result is capped, not a decisive loss.

## Remaining Risks

- Final game 5 lost expansion Nexuses around frames 10,080 and 12,840 before
  recovering and winning. Expansion timing and escort allocation still need
  work; the final wins do not mean the defense is lossless.
- Two seeds on one map are not a strength benchmark. Final-build Terran/Zerg,
  other maps, stronger opponents and own Terran/Zerg remain untested here.
- The pre-depletion rule has focused native coverage, but these short games do
  not establish robust long-game economic recovery. Its aggregate mineral
  threshold and broad mobile-army screen merit longer stress tests.
- Frozen-empty policy no longer supplies live decisions. Training-mode behavior
  still needs a separate live campaign before learned deployment is promoted.

All reports remain development-only: `training_ready=false` and
`strength_validated=false`.

## Local Artifacts

- Reference: `build/uab-diagnostic-20260929/bounded/`.
- First iteration: `build/uab-income-screen-20260930/candidate/`.
- Middle iteration: `build/uab-combat-economy-20260930/candidate/`, DLL SHA-256
  `B5F92F765C66AAC38C450189E856C6D3AC0248D43399B720C4152B9467C77021`.
- Final: `build/uab-production-memory-20260930/candidate/`.
- Each run has a verified manifest, paired raw reports and archived own logs.
  Candidate roots also preserve their DLL, source diff and new native test.
- Review tool: `build/review_income_screen.py`; it wraps the unchanged historical
  parser and labels frame-limit results as caps. Run it with the local model
  virtual environment and a campaign directory to regenerate its report.
- `build/combat-economy-review-20260930.json` pins the local evidence, scripts and
  this document. Build artifacts are ignored, not shipped as source files.

Completed campaign runners were stopped using their recorded PID and start
time after both clients reported and StarCraft exited.
