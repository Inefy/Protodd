# Engine and gameplay validation of the deep audit

## Scope

This follows `deep-performance-audit-20260926.md`. It validates the twelve
correctness fixes against the archived pre-audit source, with the older Reaver
prerequisite and available-composition development changes present in both arms.
Those inherited changes are not credited to this audit. Whole-game and learned
production control remain disabled; no tournament package is changed.

Evidence lives under `build/audit-validation-20260926`. The source snapshots,
DLLs, UMS maps, scenario receipts, complete callback traces and live manifests
are preserved separately. Hourly follow-up remains paused.

## A real engine test caught an incomplete fix

The original producer correction checked `canUpgrade()` before selecting the
lowest-ID producer. In the UMS fixture, BWAPI 4.4 returned **true** for an
unpowered Forge. The subsequent command failed, leaving a powered Forge idle.
The initial corrected selector therefore still failed this engine scenario.

`TechnologyProducer.hpp` now checks `isPowered()` explicitly. The native fake
producer deliberately allows `canUpgrade`-equivalent legality to return true
without power, so the regression no longer assumes the property it is meant to
verify. The same engine scenario then accepted the order on the powered Forge
and observed that Forge upgrading after command latency. This refines audit
issue B08; it is not counted as an additional independent bug.

## Controlled UMS scenarios

The test-only `AuditScenario.dll` links the actual core and BWAPI adapter. Maps
are generated from the bundled BWAPI test terrain. Neither map knowledge nor
test commands enter the playing DLL. Preconditions check legal engine state
before evaluating the correction.

| Scenario | Required behavior |
| --- | --- |
| Storm with allies outside the caster's squad | Withhold Storm over the allied Carrier/workers |
| Clear enemy cluster | Issue Storm, receive command acceptance, observe energy use |
| Two Forges, lowest ID unpowered | Upgrade using the powered Forge and observe native progress |
| Level-two ground weapons without Archives | Fund/build Archives first and observe actual construction |
| Melee target and hidden-health Dark Templar | Ignore the nearby invincible target; retain threat from the undetected DT |

The pre-audit DLL casts over allies, fails producer fallback, reserves an
unusable upgrade instead of building Archives, and lets the invincible enemy
mask the legal melee target. The clear-Storm positive control succeeds in both.

Setup attempts are retained but do not count as passing evidence. The original
terrain template enabled random player assignment, shared vision and alliances;
the final maps use separate fixed forces. BWAPI also withholds the cloak flag
for the undetected Dark Templar: the health precondition therefore checks its
visible type, lack of detection and zero/unavailable HP, without requiring a
private flag. A launcher's early window close could race `onEnd`; final cleanup
allows the test module to finish and flush before closing an owned process.

## Complete callback under battle load

`AuditLoad.dll` dynamically loads the exact playing `Protodd.dll`, forwards its
callbacks, and times the complete `onFrame()` call from outside the bot. It
adds no gameplay commands. The map starts with **200 supply**, 125 own entities
including structures and 96 initially visible enemy entities; the observed
enemy count rises above 100. Each run records 1,202 complete callbacks.

| Build | p99 | Maximum | At least 42 ms | At least 55 ms |
| --- | ---: | ---: | ---: | ---: |
| Pre-audit reference | 12.29 ms | 18.44 ms | 0 | 0 |
| Fixed candidate | 14.19 ms | 21.95 ms | 0 | 0 |

These are runtime checks for the observed battle and hardware conditions, not
win labels or an exhaustive upper bound. The playing module also gains an
opt-in `CallbackAudit-mode.txt` marker to record its callback through the final
accounting/logging work in normal matches. Both comparison arms include this
same instrumentation.

## Development comparison and decision rules

The prepared comparison has 12 independent opponent/map/host/seed groups:
McRaveZ, Terran Steamhammer and BananaBrain, on Benzene and Destination, both
host sides. Reference A and reference B use the exact same DLL; the candidate
uses the fixes. All other prepared inputs match. The seed rule is
`202609800 + game ID` in every arm. There are 36 games total, capped at 30,000
frames for this development screen. Capped games carry no win label.

Before launch, `comparison-plan.json` freezes the source, binaries, maps,
scenario logs, client, manifests, reviewer, supervisor and these gates:

- All controlled candidate fixtures pass with the expected counterfactual failures.
- Normal, healthy paired reports, active opponents and exact seed/map/host/input matching.
- Complete callback coverage, p99 below 42 ms and every measured callback below 55 ms.
- Across the overall pool and each opponent, no fewer wins than either reference;
  mean army at 8400 at least the weaker reference minus 0.5; workers at least the
  weaker reference minus one; early losses no more than the worse reference plus
  0.5; first defender no later than the worse reference plus 120 frames.
- To advance: at least two more wins than the better reference, with decisive
  outcomes and all preceding checks passing.

Early losses without a frame-8400 observation count as zero army/workers for
that measurement. A descriptive seed-cluster bootstrap keeps both identical-DLL
reference repeats together and reports their variation. Small timing/duration
differences are not treated as causal strength evidence.

A passing development screen only enables a separately frozen, disjoint
72-game minimum strength gate. Failure or inconclusive strength improvement
does not authorize tournament promotion. Final test and campaign results will
be recorded here once review completes.
