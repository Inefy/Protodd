# Strength audit — September 4, 2026

This is the initial audit record. The [follow-up audit](strength-pass2.md)
documents subsequent implementation changes and live experiments; consult
[validation](validation.md) for the current candidate.

Astra has a useful architecture and reproducible local testing, but there is
not yet evidence that it can contend for first place at AIIDE. The immediate
work in this audit fixes concrete execution and evaluation errors. The next
stage must measure strategy quality against several independent opponents,
with equal emphasis on maps, starting positions, and failure rates.

The working tree already contained substantial unfinished changes when this
audit began. Those changes were preserved. The starting diff and DLL were
saved under `build/strength-audit/`; the changes described below are additions
to that starting state, rather than a claim to have authored all pending work.

## Competition target

AIIDE 2026 registration closes September 14 and submission closes October 14.
Opponent names will be randomized aliases, and the ten-map pool is withheld
before competition. BWAPI 4.4.0 remains supported. Train adaptations on the
alias encountered during this tournament, and prioritize general map handling.
The organizer requires full source and a native Windows 10 check.
[Official AIIDE schedule and rules](https://davechurchill.ca/starcraft/aiide/).

Registration and submission have not been performed by this audit.

## Changes implemented

| Area | Fault found | Correction and practical effect |
| --- | --- | --- |
| Macro | Explicit production goals could reserve a Dragoon and a Zealot for one idle Gateway in the same tick. | Track producer commitments throughout reconciliation; preserve resources for other usable production. |
| Macro | Explicit goals did not share a supply budget, though composition filling did. | Apply supply checks to explicit training goals as well. A smaller legal unit can still use remaining supply. |
| Macro | An Arbiter had no producer mapping. | Map Arbiters to Stargates, including active queues and composition filling. |
| Macro | Unpowered production and a second upgrade on a busy building could tie up resource reservations. | Count usable producers and reserve research slots alongside training slots. |
| Execution | A rejected mandatory building placement stopped all lower-priority commands, including ones already funded from surplus. | Keep the failed action's ledger allocation while executing other reserved actions. |
| Build decisions | Infrastructure goals were generated before opening-style and emergency adjustments. | Generate routine infrastructure from the final intent; propagate worker and expansion limits. |
| Responses | PvZ emergency defense could request gas-dependent counters while keeping every Probe off gas. | Restore gas for sustained defense and force gas access for air/cloak responses. |
| Responses | PvP could remain in the no-Core emergency branch despite establishing a defensive army. | Permit the technology transition after three Cannons and four Zealots. |
| PvP recovery | A live test held at five or six workers and never funded its Battery while replacing Zealots. | After two completed Cannons, protect a ten-Probe recovery target and fund the first Battery before continued Zealot replacement. |
| Micro | Own range, movement-speed, and armor upgrades were absent from snapshots. | Use the player's upgraded BWAPI values, so targeting, kiting, and simulation benefit from research. |
| Micro | Multi-hit information omitted Zealot hits and could double-count a weapon damage factor. | Combine unit hit counts with weapon factors and store upgraded damage per hit. |
| Micro | Center-to-center range understated effective range against large units and buildings. | Use observed collision bounds for weapon distance. |
| Micro | Reaver range was copied from the Scarab weapon's 128-pixel metadata. | Use the Reaver's 256-pixel launch range and 60-frame firing cycle. |
| Combat estimate | Explosive/concussive reduction preceded armor, and shields were treated as HP. | Subtract HP armor before size reduction; resolve shield armor and shield depletion hit by hit. |
| Combat estimate | Disabled/unpowered units and unreachable stationary weapons could contribute fictitious attacks. | Exclude unavailable combatants, respect invincibility/detection, and limit stationary firing assumptions. |
| Spells | Storm candidates and friendly-fire estimates excluded air units. | Consider air and ground units, avoid already-stormed enemies, and include friendly air casualties. |
| Survival | Active Storm did not trigger evacuation. | Give Storm escape priority over ordinary attacks and attack-animation preservation. |
| Command arbitration | Deduplicating a high-priority order allowed a lower-priority order to take over the same unit. | The winning order retains ownership even when no new command needs issuing. |
| Detection | Photon Cannons were absent from the detection field; the field used a square footprint. | Add powered Cannons and circular detector coverage. |
| Army decisions | Only the single most threatened base received a defense squad. | Form separate detachments for simultaneous threats, with unique unit assignments. |
| Army decisions | Static defenses counted toward the minimum number of mobile defenders. | Track the mobile count separately. |
| Army decisions | Distant defenses at an attack objective polluted a squad's local fight estimate. | Include them once the squad actually approaches. |
| Army movement | Uncontested travel could pull advancing units back to the centroid and funnel the whole force through short centroid waypoints. | Send units toward the actual objective using BWAPI's movement routing when no local enemies are present; retain local combat and retreat routing. The v4 PvP test won on the earlier stalled run's seed. |
| Decision making | A numerical `FastRush` winner in an almost uniform belief distribution kept PvP/PvT at home until minute 16. | Require confidence or a current threat for that extended hold; uncertain stale labels alone no longer delay a ready army. |
| Learning | Every opponent wrote the same cumulative history filename. | Use separate, deterministic, path-safe filenames encoding the actual opponent alias. |
| Evaluation | Closing a timed-out local game generated an apparent loss. | Record completion before cleanup, keep manifests and raw traces, and exclude incomplete games from win rate. |
| Diagnostics | Army movement could not be reconstructed from the periodic trace. | Log attack/rally targets, army positions, and remembered structures; report outcome intervals separately for each binary, opponent, and map. |

BWAPI contracts were checked against the installed official 4.4.0 source and
an executable linked to its library. That executable compares the costs,
supply, and build times of 28 producible Protoss units/buildings with Astra's
catalog. Range/cooldown semantics are documented in the
[BWAPI unit reference](https://bwapi.github.io/class_b_w_a_p_i_1_1_unit_interface.html).
The Reaver correction also follows Blizzard's
[Reaver unit specification](https://classic.battle.net/scc/protoss/units/reaver.shtml).

## Remaining weaknesses, in priority order

### 1. Establish a credible opponent benchmark

UAlbertaBot's three races are useful integration and rush-defense tests, but
they are one opponent implementation. They do not cover modern positional
play, adaptive openings, or varied late-game compositions. The local ladder
also has Iron, and research checkouts exist for McRave, PurpleWave, Stardust,
and Steamhammer. Research checkouts are not tested runnable opponents.
The audit subsequently imported the official 2025 BananaBrain package and
completed one game with the final candidate: a loss at frame 15,254. That
establishes a useful tougher baseline, not a sufficiently broad benchmark.

Prioritize BananaBrain, Stardust, and PurpleWave as demanding Protoss
benchmarks, then add independent Zerg and Terran opponents. My tally of the
organizer's 18,746 unique game records for AIIDE 2025 gives roughly 88%, 84%,
and 83% wins for those three, versus 37% for UAlbertaBot. These are historical
field-wide results, not predictions of Astra's head-to-head results. They
explain why beating UAlbertaBot alone is an insufficient release target.
[Official 2025 detailed results](https://davechurchill.ca/starcraft/aiide/results/2025/results/detailed_results.txt)
and [submitted opponent packages](https://davechurchill.ca/starcraft/aiide/results/2025/bots/)
provide versioned benchmark inputs.

Import versioned, runnable opponent packages with their required BWAPI
versions. Test rush, economic, mech, air, cloak, and spell-heavy opponents.
Use balanced starts and the same binaries/map set for baseline and candidate;
record seeds rather than assuming unrelated local games are paired. Keep a
held-out map group to expose terrain overfitting. Start with short diagnostic
batches, then at least 100 games per important matchup for release decisions.
Report each opponent/map separately, plus confidence intervals and incomplete
games. Never classify a cleanup loss as a competitive loss.

### 2. Make the opening portfolio economically competitive

The current PvT/PvP opening commits to a Forge and multiple Cannons very early
and temporarily caps Probe production at seven. This buys rush safety at a
large economic cost. The four learned styles mainly modify one underlying
plan; they are not four thoroughly tested build orders.

The next experiment should compare a Gateway/Core economic opening with the
fortified opening, using scout evidence to trigger additional static defense.
Evaluate first-combat-unit timing, worker count, first expansion, range timing,
and surviving army value together. PvZ should compare a terrain-aware Forge
expansion with a two-Gateway opening. Do not promote an opening because it wins
one scripted rush on one spawn.

The final candidate's BananaBrain test makes the cost concrete: at frame
10,800 it still had one base and 12 Probes, with eight completed Zealots and
its first Dragoon under production against visible enemy Dragoons. It lost
at frame 15,254 without a runtime overrun. Prioritize a protected Core/gas
transition and sustained income against ranged pressure, then verify that
the change still holds early melee rushes.

### 3. Improve information and transition confidence

Enemy-plan inference repeatedly reuses observations and can become certain
without fresh evidence. Construction timing combines a persistent first-seen
frame with changing build progress; a future timing model should retain the
original start-time estimate. Worker scouting currently ends when an enemy
depot or combat unit is found, leaving a gap before air scouting is available.

Track evidence age explicitly, retain alternative plausible tech paths, and
test whether an observer/scout can return safely for a second tech check.
Treat production buildings as capacity evidence, not proof of the units being
made. Compare detection completion with plausible cloak arrival, including
uncertainty in travel time and hidden production.

### 4. Improve positional combat and spell execution

The bounded simulator is still an approximation: no collision-aware movement,
high-ground miss model, projectile-flight accounting, splash geometry, healing,
or full spell simulation. Friendly cloaking is not the same as knowing whether
the enemy can detect it; BWAPI reports our own units as detected to us. Do not
treat that flag as an enemy-vision oracle.

Add small replay-derived scenarios for chokepoints, tank lines, minefields,
Mutalisk focus fire, and Dragoon pathing. Track in-flight damage and attack
windup before changing target allocation further. Preserve engagement memory
across minor membership/objective changes. Split incompatible harassment units
into appropriate groups rather than evaluating Dark Templar and Corsairs as
one force.

The BananaBrain loss exposed a concrete defensive pursuit problem. At frame
11,880, while the plan still said `Defend`, several Zealots had followed
enemies to approximately `(830, 1900)`, far from the Cannons around
`(448, 2880)`. Add a defensive pursuit boundary and preserve Cannon/Battery
support instead of letting local target selection drag defenders across the
map. Validate it against ranged kiting, siege pressure, and a real base breach;
a blanket refusal to leave home would introduce another failure.

The live module passes combat units and static defenses into tactical target
selection. Ordinary workers and production buildings therefore depend mostly
on BWAPI's attack-move behavior. Separate the hostile target list from the
combat-simulation list, so harassment can deliberately target workers and
cleanup can prioritize production without counting harmless buildings as
army strength.

Storm selection now covers air, but placement remains centered on observed
units and does not predict movement. Stasis/Recall/merge orders need their own
cast-completion protection. Reaver drops need tested unload tiles, ammunition
readiness, and terrain-aware pickup behavior rather than a fixed exposure timer.

### 5. Improve economy and terrain robustness

Worker assignment already handles patch balancing, gas retention, and militia.
It still uses heuristic saturation and distance scores, which can repeatedly
transfer workers or send them toward an incomplete/depleted expansion. Measure
gathered resources and transfer travel time, not just worker count.

Expansion and placement need testing on islands, narrow ramps, mineral walls,
unusual depot geometry, and routes blocked by buildings. The navigation grid
does not model all dynamic occupancy; safest-step movement is not itself a
terrain pathfinder. Add explicit wall/choke geometry and validate that a
retreat waypoint is reachable by every ground component. An unplaceable
expansion must have a bounded retry/fallback policy.

### 6. Make maintenance and late game resource-aware

The strategy still uses substantial clock-based thresholds and aspirational
unit goals. Composition weights do not completely price gas income, build
time, producer utilization, and the number of supported spellcasters. Archons
depend on a spent-Templar merge heuristic rather than a direct composition
controller. Higher-level upgrade prerequisites need explicit planning, rather
than relying on adapter rejection.

Measure idle producer time, sustained bank, gas starvation, and the percentage
of army value committed to useful counters. Spend on technology when its
payoff can arrive before the relevant threat. Test cleanup of lifted Terran
buildings and island bases, not only destruction of the starting depot.

## Validation and release discipline

### Suggested schedule to October 14

| Window | Work | Evidence required to advance |
| --- | --- | --- |
| September 4–10 | Freeze a reproducible baseline; make independent opponents runnable; reproduce PvP recovery and delayed contact. | Complete match manifests, exact binaries, meaningful outcome accounting, and replay/log evidence of each failure. |
| September 11–21 | Compare economic openings, rush contingencies, repeat scouting, and tech-arrival estimates. | Better economy/timings without a material rush-defense regression across balanced starts. Register by September 14. |
| September 22–October 3 | Address the most frequent combat, terrain, expansion, and cleanup failures from the benchmark. | Small reproducible scenarios plus improvements against held-out maps and opponents. |
| October 4–10 | Run the frozen candidate and baseline through the broader match matrix. | At least 100 games per important matchup as a starting point; report uncertainty, crashes, incomplete games, and CPU tails. |
| October 11–14 | Native Windows 10 verification, source/package review, and submission buffer. | Exact tested DLL hash, clean data directories, reproducible build, complete source, and confirmed organizer acceptance. |

This is a proposed work schedule, not a scheduled background job. One hundred
games can still leave a wide confidence interval; close results need more
games. Do not tune on held-out results and continue calling them held out.

`tests/test_main.cpp` adds 29 focused competition regression assertions in
addition to the existing suite. `tests/bwapi_catalog.cpp` checks the official
library contract. `tools/direct_report.py` adds four outcome-accounting tests.
The strict verifier covers core compilation, adapter compilation, both report
tools, ladder tests, the privacy audit, and whitespace. Release builds run the
core, catalog, and report CTest targets.

Use the direct-match manifest as the outcome authority:

```powershell
./scripts/direct-match.ps1 -OpponentRace Protoss -Map 'maps/aiide/(4)Python.scx' `
    -Label unique-candidate-label -TimeoutSeconds 900
python tools/direct_report.py build/direct-logs/unique-candidate-label.json
```

The default direct match clears Astra learning files for an independent
opening test; `-PreserveLearning` deliberately tests adaptation. The regular
tournament module always supports cumulative per-alias learning.

See `docs/validation.md` for exact tested hashes and outcomes. A small smoke
matrix establishes execution and specific behaviors; it does not establish
tournament superiority. Before October 14, run a frozen candidate against the
full versioned opponent/map matrix and verify it on native Windows 10. Keep
the previous accepted binary available for rollback.
