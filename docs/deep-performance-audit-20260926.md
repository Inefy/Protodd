# Deep performance and playing-strength audit — 26 September 2026 UTC

## Result and scope

Twelve concrete issues were corrected in the working source. They affect
production legality, resource reservations, movement, target selection, threat
estimation, spell safety, worker allocation and command identity. These are
development fixes, not a demonstrated win-rate improvement or a promoted bot.

The audit traced the portable decision loop and its BWAPI callers: observation
and visibility, catalog/prerequisites, strategy-to-macro reservations, worker
and scout assignment, navigation, influence, combat simulation and tactical
control, command arbitration, transport/expansion coordination, model loading,
and runtime accounting. Particular attention went to disagreements between
planning, engine legality, and the information available to each subsystem.
This is broad coverage, not proof that every bug has been found.

All evidence is under `build/deep-audit-20260926`. The directory contains
pre-edit sources, focused failing cases, final test/build logs, a source receipt,
the isolated development DLL and a read-only review of eight reference games.
Existing unrelated changes and prior experiment artifacts were preserved.

## Confirmed issues and corrections

| ID | Priority | Failure and impact | Correction and coverage |
| --- | --- | --- | --- |
| B01 | High | `NavigationGrid::lineWalkable` accepted diagonal movement through blocked corners, although A* rejected the same move. Its direct-waypoint shortcut could recommend an unreachable route. | Apply the same orthogonal clearance rule as A*. Tests cover one/two blocked corners, an orthogonal detour and open travel. |
| B02 | High | Melee targeting's preliminary close-target test included invincible/loaded enemies and targets already covered by lethal damage. Such a target then failed the main legality check while suppressing all distant legal targets. | Both stages now agree on target legality, remaining durability and weapon separation. Regression cases cover Stasis, cargo, incoming projectiles and squad allocations; a real nearby opponent still wins. |
| B03 | High | The combat evaluator handled BWAPI's unavailable enemy-health sentinel, but the influence map multiplied an undetected enemy's weapon threat by zero. Cloaked attackers could look harmless to pathing and economic routing. | Estimate conservative full vitality locally for unknown enemy health. Do not rewrite observations or detection. Known zero HP stays zero. |
| B04 | Medium | Loaded cargo projected influence-map weapon threat. Harassment routing also treated loaded/hallucinated units and unpowered Cannons as armed obstacles. | Filter unavailable threats consistently in those paths. A real powered Cannon continues to block the route. |
| B05 | High | Storm penalized casualties only in the caster's squad. The live caller supplies other combat units and all own units separately, so another squad or a Probe line could be ignored. Fake or loaded enemy clusters also contributed target value. | Deduplicate the union of squad, support and own-unit snapshots before friendly-fire scoring. Ignore loaded/hallucinated targets. Tests cover other squads, the live caller's own-unit input, 24 workers, loaded allies and overlapping input spans. |
| B06 | High | Higher-level upgrades lacked their engine-required technology buildings. The planner could reserve resources for an upgrade BWAPI would reject, without funding the missing prerequisite. | Model the additional Archives/Fleet Beacon/Core requirements. Blocking goals unlock the chain; optional goals wait. Early construction leaves resources free; a near-complete prerequisite can reserve, but cannot issue, the upgrade. Compare all supported upgrade levels and research costs/producers against the bundled BWAPI library. |
| B07 | Medium | An unfinished second research building contributed its whole remaining construction time even when a completed building was already available. Upgrades could stop simply because another Forge was being built. | Wait only when no completed required building exists, using the earliest completion. Regression includes one complete Forge plus a newly started second Forge. |
| B08 | High | BWAPI technology execution selected the lowest-ID building before testing legality. An unpowered building, or one busy with the other kind of operation, could hide a second commandable producer. | Select from producers that pass `canResearch`/`canUpgrade` first. A shared selector is tested for unpowered/busy first producers, ID stability, and dead/unfinished buildings; compile both engine command paths into the Win32 DLL. |
| B09 | Medium | Loaded, disabled and hallucinated Probes counted as economic workers and could consume gas/mineral assignments they could not perform. | Exclude them from worker assignment. Test that only the available worker receives an assignment. |
| B10 | Medium | Opening-scout selection could choose a loaded or disabled Probe, including retaining one from the previous scouting cycle. | Apply availability checks before selection/retention. An unloaded Probe becomes eligible again. |
| B11 | Low | Command deduplication compared actor/order/target but omitted the technology. Distinct spells on the same target could be mistaken for a repeated command. | Include technology in command identity; identical spells retain latency suppression. The current maintenance spell path is separate, so this is not claimed as a present win-rate bottleneck. |
| B12 | Medium | The empty-ammunition fallback returned power before checking target compatibility. An empty Reaver gained apparent anti-air threat while an armed Reaver correctly contributed none against an all-air squad. | Require a compatible potential target before applying residual ammunition power. Preserve the existing potential threat against ground units. |

Twenty-three assertions fail when the new regression executable is linked
against the archived original core sources (`original-core-regressions.log`).
The later Reaver-domain case adds one separately recorded failing assertion
(`ammunition-before.log`). The BWAPI producer-selection issue has a source
control-flow diagnosis and selector tests; the counterfactual core executable
does not emulate a live engine producer. Keep that distinction explicit.

The earlier Reaver Support Bay prerequisite fix and available-tech composition
experiment predate this audit and are documented separately. They are not
counted among these twelve issues.

## Validation

- Native debug build and the complete registered CTest suite: **38/38**.
- Win32 Release build, including `Protodd.dll`: successful.
- Win32 core, deep-audit, BWAPI catalog, Reaver-prerequisite and composition
  suites: **5/5**. Catalog validation checks 28 unit prices/supply/build times,
  trainable-unit prerequisite sets, 14 supported upgrades at every level and
  three research technologies against BWAPI.
- Broader Python discovery with pytest: **263 passed, 2 skipped, 50 subtests
  passed**. The two skips require the compiled `model_tool`; CTest separately
  supplies it and passes those schema/parity tests. The command-line policy
  pipeline test is excluded from pytest collection and passes through CTest
  with its required trainer argument.
- The first generic unittest-discovery attempt was unsuitable for that
  command-line test and found pytest absent in the local model environment.
  Its failed log is retained. Pytest was installed into that existing local
  environment; the corrected broad test run passed. These were test-harness
  setup problems, not repaired bot defects.

Reproduce the final checks:

```powershell
cmake --build build/dev --parallel 2
ctest --test-dir build/dev --output-on-failure
build/model-venv/Scripts/python.exe -m pytest tests --ignore=tests/test_policy_pipeline.py -q
cmake --build build/test-review-20260926-win32 --config Release --target protodd_bwapi_catalog_tests protodd_deep_audit_tests protodd_tests protodd_bwapi --parallel 2
ctest --test-dir build/test-review-20260926-win32 -C Release -R 'protodd_(bwapi_catalog|deep_audit|core|reaver_prerequisite|available_composition)_tests' --output-on-failure
```

## Runtime evidence and decisions

`review_runtime.py` reads and hashes eight completed games from the two
reference arms of `pvz-available-composition-20260926`. Their recorded peak
was **16.404 ms**, with **zero samples at or above 42 ms**, zero at 55 ms,
and zero caught errors. These runs do not support spending this pass on a
speculative performance rewrite.

The recorded measurement excludes the final performance-accounting/logging
tail; it is not the complete BWAPI callback gate. These are baseline DLL
games, mostly early losses, not measurements of this patch set or max-supply
late-game fights. Full callback timing remains required for a release candidate.

## Ruled-out suspicions and remaining work

- The planner already reserves producer slots for ongoing technologies.
  A busy Forge alone does not trap a second upgrade's budget; the existing
  regression and a new control both pass. B07 and B08 are distinct failures.
- Bundled BWAPI exposes visible/detected Reaver/Carrier payload counts;
  assuming those fields are always private would be incorrect. Undetected
  payload-state uncertainty still deserves a dedicated policy study; no
  fabricated ammunition observations were added.
- Enemy-memory recency, local combat simulation, route risk sampling and
  strategic production caps contain approximations. No broad heuristic change
  was made solely because a coefficient looked suspicious.
- Previously demonstrated build-order-to-construction delays remain a
  high-value integration problem. This pass does not reinstate retired lease,
  builder-handoff or extra-unit patches. Their native/live evidence still governs.
- Validate each high-impact correction in a controlled engine scenario, then
  use repeated independent seeds and a same-DLL reference arm for diverse live
  comparisons. First prioritize Storm safety and production legality; neither
  a single game nor four noisy pairs establishes a strength improvement.
- Preserve source/DLL receipts for every live treatment. Do not attribute the
  concurrently running composition experiment to these audit fixes: its
  frozen source snapshots and DLLs contain none of them.

That comparison completed during the audit. All twelve games were healthy
losses, and the composition candidate failed the frozen army-growth threshold
(5.75 versus 5.25/5.0 at frame 8400). All 260 pinned hashes verified without
mismatch. Its result and continued unvalidated development status are recorded
in [available-composition-20260926.md](available-composition-20260926.md).

Whole-game learned control remains gated. No tournament package was changed,
and hourly automation remains paused. A successful build and native tests do
not replace engine parity, full callback timing, paired live evaluation and the
72-game strength gate.
