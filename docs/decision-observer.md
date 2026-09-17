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
| `SQUAD` | Persistent squad identity and members, nearby support count, objective, proposed and selected fight, ratio and required ratio, simulation result, detection gate, reason |
| `MACRO` | Actual reconciliation action, resource reservation, execution status and reason, accepted flag |
| `ORDER` / `SCOUT` | Issued tactical/scouting intent and BWAPI acceptance |
| `HEALTH` | Bank, income counters, army/economy health, cumulative idle worker/Gateway unit-frames, supply-tight frames, expansion status, command pipeline totals |
| `WORKERS` | Actual gas/mineral assignment counts, worker leases and requested gas policy |
| `ENTITY` / `SNAPSHOT` | Full one-second unit snapshots, including empty snapshots; own native order, queues, build/train/research/upgrade timers, energy, ammunition, transport, power and combat state |
| `BELIEF` | All enemy-plan weights rather than only the winning hypothesis; sampled every 10 game seconds |
| `LOSS` | Observed destruction, side, unit, position and BWAPI purchase price; `costBatchSize` converts paired Zerg eggs to individual loss cost, including older logs; last observed HP/shields, accepted action and frame, strategy, and nearby visible enemies |
| `PHASE` | Calls, total and peak execution time by subsystem, including diagnostics and overlay |
| `ACTION` | Actual commands from every adapter path: worker jobs, building, training, research, upgrades, scouts, combat, transports, spells and ammunition. Includes source, target, native order, resources and exact BWAPI rejection, or an explicit preflight block reason |
| `ACTION_TOTAL` | Exact cumulative counts by source, stage and outcome; repeated sampled action rows must not be counted as total attempts |
| `DAMAGE` | HP/shield decreases between consecutive visible observations, with position, cooldown, attack/Storm state and last accepted action |
| `LIFECYCLE` | Legal discover/show/create/complete/morph/ownership-change callbacks, including construction completion |
| `INCIDENT` | Sustained observed conditions, with unit ID, onset, duration, heartbeat, resolution and state evidence |
| `ERROR` | Exception message, subsystem and cumulative caught-error count, including repeats suppressed from detailed output |

Squad records additionally expose `travelGoal` and `travelReason` (the mission
destination and its owner), `objective` (which may be a short navigation waypoint),
`retreat`, and `defenseCenter`. Compare these with actual `ENTITY` positions and
accepted `ACTION` records; an Attack posture alone does not prove army movement.
The viewer's Movement destination column shows the destination and whether it
comes from attacking, assembling, joining the forward army, or expansion cover.

Unit snapshots also record `groundRange`, `groundDamage`, `airRange`,
`airDamage`, `armor`, `shieldArmor`, and `topSpeed`. Enemy values include the
upgrade information BWAPI exposes for visible completed units and retain that
observation in fog. Enemy resources, queues, and unobserved technology are not queried.

Combat commands confirmed as still active in the engine count as redundant.
They retain their priority in unit ownership without issuing another attack,
move, or hold command. `attackWindup` separately identifies a just-issued,
in-range own attack protected through latency plus ten frames; `attackFrame`
still means the native BWAPI flag. Routine retargeting waits for that short
windup, while Storm escape, critical-health escape and mission extraction can
interrupt it. Stopped out-of-range attackers remain eligible for path retries.

Visible Psionic Storm bullets from either player enter the hazard map. Escape
uses exact spell positions with a clearance margin, terrain checks and local
crowding; pursuit avoids stepping straight back into an active Storm. The fight
simulation includes discounted enemy-only radial splash for Scarabs, Archons and
Corsairs. It still uses fixed positions and does not model full pathing, projectile
collisions, future spell casts or every weapon's splash geometry.

`ORDER` rows are emitted when a unit's semantic intent changes and as a
five-second heartbeat; small coordinate changes do not create a new row.
`PERF` rows retain the worst frame in each one-second window, while
`PERF_SUMMARY` remains the authoritative count of all slow frames.

Health and entities are sampled every 24 game frames. Durations integrate
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

Live detail is bounded (including 1,800 entity snapshots); the report marks retention
limits. End results come only from the runner's completed pre-cleanup manifest.
Cleanup-generated `END` lines and timeouts never become verified losses or wins.

## Diagnosing the next game (trace version 3)

The observer's **Investigate these problems** list links to timestamps for BWAPI
rejections, exceptions, costly engagements, and sustained idle-worker,
idle-production-with-bank, unpowered-building, empty-ammunition, stalled-movement,
supply-block, mineral-bank and stalled-expansion incidents. Each item says what
was observed and which state or decision to inspect next. Clicking a map unit
filters the timeline to that unit; the unit detail shows native orders and queue
timers. Use the action-failure, damage, lifecycle and incident filters to narrow it.

The incident thresholds are three seconds for idle workers and empty ammunition,
two seconds for supply blocks and unpowered buildings, five seconds for idle
production with at least 150 minerals or a bank of at least 800 minerals, and six
seconds without 24 pixels of displacement for move/attack-move orders whose
destination remains over 96 pixels away. Expansion alerts use five seconds of
the coordinator's stalled-progress measurement. These conditions can be
intentional: they are investigation candidates, not automatic diagnoses.

Repeated incidents update cumulative durations without double-counting heartbeats.
Missing observations break consecutive-duration tracking. The report retains
whole-game action totals, incident totals and damage totals after its detailed
timeline reaches retention limits. The raw log remains the source for older unit
detail. Incident durations in unfinished games stop at the last incident record;
the parser does not invent activity up to EOF. Coverage shows telemetry gaps,
malformed records, logging errors and whether an END was recorded. A recorded
END alone still does not verify the match result.

Damage is an observed durability decrease, not a projectile-hit counter: it omits
the final lethal hit, same-frame damage/healing that cancels out, and damage while
an enemy is hidden. Shield regeneration and friendly spells can complicate its
interpretation. A unit's last accepted action and nearby enemies give context;
they do not prove who killed it or which decision caused its death. Enemy queues
and hidden enemy orders are never queried. Action details use a five-second
heartbeat and also change when source, target, command, outcome, payload or
64-pixel destination cell changes; exact outcome totals count every issued call.

Diagnostics run by default in the rebuilt DLL. Generate the same HTML and JSON
summary with `python tools/decision_report.py <game.log>`. Direct-match archives
and `tools/direct_report.py` automatically include the expanded diagnostics.

### Version 3 smoke validation

The September 12 UTC local test `diagnostics-v3-smoke` used BananaBrain
`PvP_nzcore`, Benzene and seed 43. It stopped deliberately at frame 7,200 and is
an incomplete test, excluded from win rates. The archived pre-cleanup log has
301 health/map snapshots, 11,279 entity rows, 595 sampled action records, 12
damage records and one loss callback. There were no malformed records, telemetry
gaps, caught exceptions or diagnostic callback errors. The 4,057,934-byte trace
reported a mean of 0.642 ms per diagnostics call (2.217 ms peak) and 0.00569 ms
per damage-observation call. These measurements cover this opening, not a
late-game stress test or a controlled before/after performance comparison.

The trace identified two supply-block episodes totaling 21 sampled seconds,
two idle-production-with-bank episodes totaling 44 unit-seconds, repeated
`Unit_Busy` responses during building, and Probe #0 dying at frame 4,996 after
an accepted withdrawal at frame 4,983. Its last observed state was 2 HP and 2
shields at frame 4,995; a visible Dragoon was nearby. These are starting points
for follow-up changes, not proven causes of a match loss.

The smoke DLL hash is recorded in `build/direct-logs/diagnostics-v3-smoke.json`.
After the smoke, the report parser was hardened against context fields replacing
unit IDs, and the DLL's diagnostic reset/type-field naming was cleaned up. The
final build was saved at `build/protodd-tournament/Release/Protodd.dll`.
The runner now defaults to `build/tournament/Release/Protodd.dll`, matching
the tournament build, package script and ladder template. Pass `-BotDll` for
a frozen experimental build. Strict verification, all five Release CTest
suites, and 17 report regressions passed. Browser checks covered the live problem
list, timestamp selection, unit filtering, and an 800-pixel viewport. No complete
match or late-game validation was performed in this pass.

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
  The subsequent [logging strength pass](logging-strength.md) adds a splash
  checkpoint before a contested ranged-mirror natural.
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
