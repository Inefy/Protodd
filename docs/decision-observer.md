# Decision diagnosis and harassment

This pass makes the bot's actual decisions inspectable during a game and in an
archived report. It also adds economic harassment, changes PvP investment order,
and addresses feedback loops found in the test traces. These are implemented
heuristics with regression coverage; the controlled BananaBrain tests have not
established an improvement in winning strength.

## Watching a test

`scripts/direct-match.ps1` now starts a read-only observer on an available local
port and prints `LIVE_OBSERVER`. Open that URL while the game runs. The runner
stops its observer after archiving the game; `-NoObserver` disables that helper.
Every test produces `<label>.decisions.html` alongside its pre-cleanup log and
manifest, including incomplete tests. The HTML works offline without packages
or external assets.

To reopen a report as a local interactive observer:

```powershell
python tools/decision_report.py build/direct-logs/decision-v3-nzcore-benzene-43.log --serve 8765
```

To generate an archived report or compare the original match diagnostics:

```powershell
python tools/decision_report.py build/direct-logs/decision-v3-nzcore-benzene-43.log
python tools/direct_report.py build/direct-logs/decision-v2-nzcore-benzene-43.json build/direct-logs/decision-v3-nzcore-benzene-43.json
```

The timeline slider and event timestamps select historical strategy, squads,
economy, production and enemy hypotheses together. The map shows observed unit
coordinates; click a unit for HP, shields, cooldown, target and last issued order.
Use the timeline filter for Probe harassment, fight decisions, losses or commands.
The in-game compact/detailed overlay also includes the current mission, health
metrics, scouting action and squad explanations.

## What the trace measures

| Record | Meaning |
| --- | --- |
| `STRATEGY` | Proposed and selected posture, plan, enemy hypothesis, expansion status |
| `SQUAD` | Persistent squad identity and members, objective, proposed and selected fight, ratio and required ratio, simulation result, detection gate, reason |
| `MACRO` | Actual reconciliation action, resource reservation, execution status and reason, accepted flag |
| `ORDER` / `SCOUT` | Issued tactical/scouting intent and BWAPI acceptance |
| `HEALTH` | Bank, income counters, army/economy health, cumulative idle worker/Gateway unit-frames, supply-tight frames, expansion status, command pipeline totals |
| `WORKERS` | Actual gas/mineral assignment counts, worker leases and requested gas policy |
| `ENTITY` | Own and observed/remembered enemy positions, durability, cooldown, order target, visibility and last-seen frame |
| `BELIEF` | All enemy-plan weights rather than only the winning hypothesis |
| `LOSS` | Observed destruction, side, unit, position and resource cost |
| `PHASE` | Calls, total and peak execution time by subsystem, including diagnostics and overlay |

Health is sampled every 24 game frames and entities every 120. Durations integrate
elapsed frames rather than the number of callbacks. Macro logging uses the ledger
and actions from the real reconciliation; it no longer calls the stateful planner
a second time merely to describe its decision.

The report links accepted fights to observed losses of those particular squad
members in the following ten seconds. It also counts fight transitions and
retreat-to-engage reversals within three seconds. It separates proposed,
superseded, redundant, deferred, attempted and accepted combat commands, and
measures recorded macro waiting intervals without filling long gaps in the trace.

An accepted command is not proof that a shot landed or a structure finished.
Worker harassment orders do not measure lost enemy income. Enemy casualties are
observed callbacks, not omniscient totals. Fight windows are correlation, not causal
attribution. Hypothesis weights and combat ratios are not win probabilities.
Entities contain last-known positions in fog; unseen enemy economy is unavailable.
Frame-time profiles are cumulative per call, not whole-frame wall time.

Live detail is bounded (including 720 entity snapshots); the report marks retention
limits. End results come only from the runner's completed pre-cleanup manifest.
Cleanup-generated `END` lines and timeouts never become verified losses or wins.

## Harassment behavior

The opening Probe keeps its scout lease after locating the enemy. It selects
visible workers near an observed enemy depot, attacks, and opens distance when a
worker targets it, several workers surround it, its shields are low, or its weapon
is reloading. A short escape interval prevents instantaneous reversal. Safe attack
frames finish before another order interrupts them. It resumes attacks after the
chase stops. An observed completed fighter, static ground weapon, damaged hull or
the opening time limit latches withdrawal. The return uses terrain waypoints and
local threat avoidance, then releases the Probe to mining at home. Builders cannot
be selected as scouts. Retreat is a policy, not a guarantee of survival.

Two spare Zealots/Dragoons can raid a recently observed worker cluster. A mission
requires at least ten available main-army ground units and enough remaining units
to meet the strategic attack-size requirement. Home defenders are allocated first.
The pair stays together as a mission rather than changing members every update.
No new pair launches during a base threat, urgent reinforcement/detection demand
or recovery. Defenders, casualties, health loss, a cleared target or a sixty-second
mission limit trigger withdrawal; surviving members return home before a thirty-
second cooldown. Units already assigned to an emergency defense have priority.

Dark Templar select exposed worker lines. Corsairs select attackable air logistics
such as Overlords and transports. Candidate economic targets must be visible or
seen within twenty seconds. Observed defenders and detection near the target, plus
weapon coverage along the direct route, can veto the opportunity. A veto means
waiting at home rather than automatically sending the raider at the main army's
objective. This conservative route test can miss alternate safe routes.

Spare Reavers use the same economic-target selection for Shuttle drops, with
separate air-route and ground-drop safety checks. The target remains attached to
the mission; a nearby fighter along the route no longer triggers a premature drop.
Newly observed defense or loss of a safe target sends the Shuttle home. The first
army Reavers remain reserved for the fighting force, and emergency defense can
recall loaded army support. Existing extraction, damage and exposure limits remain.

## Macro, micro and decision changes

- Quiet PvP funds four Dragoons before optional Robotics, adds Gateway capacity
  independently, and does not pause workers merely to unlock that capacity. Six
  completed Dragoons allow optional Observer scouting and expansion; the Observer
  itself is no longer that opening's expansion prerequisite. Observed cloak still
  funds urgent mobile detection.
- Ranged perimeter containment cancels new Forge/Cannon goals, including old macro
  reservations, and funds mobile units/range. Separate emergency melee, actual
  economy-breach and air-defense responses remain. The reinforcement budget cannot
  resurrect an explicitly cancelled Cannon demand.
- Combat history follows overlapping membership through leader losses and role
  changes. Ordinary reversals require elapsed evidence, not three rapid callbacks.
  Engagement holds for at least 48 frames; recovery needs a 72-frame interval,
  sustained evidence and an advantage margin. Overwhelming danger can still end an
  engagement immediately. Brief loss of local enemy contact does not reset retreat.
- The old economy-breach rule could override a heavily losing simulation and force
  an outward attack. The replacement preserves normal fight thresholds and permits
  only ready ranged volleys or close melee interception while falling back to the
  screen. It does not make protecting a Nexus a license to chase.
- Reloading Dragoons can spread away from visible splash threats without cancelling
  ready volleys, crossing a protected boundary or crowding a teammate's chosen
  position. BWAPI validates spacing destinations. The simulator still does not
  model complete splash, terrain firing arcs or dynamic collision.
- Gas collection has separate pause/resume thresholds when gas accumulates but
  minerals are scarce. Blocking technology costs adjust the threshold; posture
  changes alone cannot repeatedly reshuffle gas workers.
- A stalled expansion releases its builder and savings after eight seconds of
  failed progress, then defers the same site for twelve seconds. Confirmed Nexus
  construction takes precedence. The army assembles beside the site and safely
  clears the footprint instead of occupying the Nexus center. Builder travel,
  blockage and command latency have separate trace reasons.

## Validation and evidence

The Release/Win32 build and all five CTest suites cover the portable core, BWAPI
catalog, decision report, direct-match report and log analyzer. The strict verifier
also checks adapter translation units, privacy rules and whitespace. Regression
scenarios cover harassment selection/escape/recall, worker leases, cloak exceptions,
cancelled macro reservations, sustained combat evidence, footprint clearance,
gas-policy hysteresis, splash spacing and report validity.

Controlled tests use BananaBrain `PvP_nzcore`, Benzene, seed 43, reset runtime
learning and archived DLL hashes. These builds contain multiple changes and are
not feature-isolation experiments:

| Build | Result | Peak Probes | Peak army | Peak Nexuses |
| --- | --- | ---: | ---: | ---: |
| Earlier terrain pass | Loss, frame 21,392 | 22 | 16 | 1 |
| Decision v1 | Loss, frame 19,966 | 41 | 15 | 2 |
| Decision v2 | Loss, frame 20,307 | 22 | 18 | 1 |
| Decision v3 | Loss, frame 15,564 | 27 | 10 | 2 |

V3 verified accepted Probe attacks, chase evasion, re-engagement and withdrawal.
The Probe died on the return at frame 5,146. The first Observatory was ordered at
8,791, after the ranged screen had assembled. The trace exposed an unwanted Forge
order during a cloak-driven plan change and the forced losing economy-defense
override. Those findings motivated the final containment, screen-interception and
terrain-return fixes. V3 is diagnostic evidence, not a successful strength result.

See the corresponding files in `build/direct-logs` for exact manifests, hashes,
replays, reports and pre-cleanup traces. Those local game artifacts are intentionally
excluded from source packages.
