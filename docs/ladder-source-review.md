# Ladder source review — September 6, 2026

See the subsequent [containment investigation](containment-review.md) for the
deeper BananaBrain diagnosis and additional implementation and match results.

This pass inspected the combat, production, worker, or information subsystems
of every opponent in `ladder/ladder.local.json` (the same eleven opponents in
`ladder/ladder5.local.json`). It is a focused subsystem review, not a claim to
have audited every line of every dependency or imported every opponent feature.
UAlbertaBot's shared implementation was also reviewed for the three race
variants used by the direct-match harness.

## Source coverage

The AIIDE 2025 sources are from the locally extracted organizer packages under
`build/tournament-bot-downloads/extracted/`, or the identical imported source
under `ladder/bots/`. BananaBrain's source is under its imported `src/Source`.
Iron's missing source was obtained from the organizer's
[AIIDE 2016 package](https://davechurchill.ca/starcraft/aiide/results/2016/bots/Iron.zip)
and extracted under `build/ladder-source-audit/Iron/`. No opponent installation,
binary, training data, or source file was changed.
The downloaded Iron DLL exactly matches the installed ladder DLL by SHA-256:
`B5AA58AD22B54C538C9D3366B4389707F28096405019AFBFD173AD636E6716BB`.
Hashes of the 21 inspected source/license documents are saved in
`build/ladder-source-audit/source-manifest.json`.

| Opponent | Source inspected | Useful lesson and disposition |
| --- | --- | --- |
| BananaBrain | `Source/Micro.cpp`, `Source/License.txt` | Attack timing, target persistence, mine awareness, and Reaver ammunition matter more than raw army counts. Keep existing attack-frame protection; make our simulator consume finite Scarabs. Its tournament-restricted implementation is not copied. |
| Stardust | `src/Bullets.cpp`, `src/Workers/MiningOptimization/Readme.md`, license | Distinguish delayed damage from damage already applied; observe actual mining transitions. Implement original imminent-projectile accounting. Its learned mining optimizer and tournament-restricted code are not imported. |
| PurpleWave | `Micro/Coordination/DamageTracker.scala`, `Micro/Targeting/FiltersSituational/TargetFilterVisibleInRange.scala` | Coordinate shots already committed and prefer available firing opportunities. Implement our own projectile reservations and a local firing envelope. |
| McRaveZ | `Micro/Combat/Simulation.cpp`, `Strategy/Spy/SpyGeneral.cpp` | Debounce engagement changes and retain timing evidence once per observation. Our engagement memory now survives reinforcements and moving targets; existing first-seen/morph history remains in use. |
| Microwave | `Microwave/Source/CombatSimulation.cpp` | Local combat estimates must distinguish air and ground capability. Remove fictitious weapon power against incompatible targets and unreachable static support. |
| Steamhammer | `Steamhammer/Source/ProductionManager.cpp` | A production queue must recover from impossible work and preserve valid builder ownership. Exclude disabled producers and repair our construction lifecycle. Existing prerequisite repair and resource reservations already cover much of this pattern. |
| Iron | `Iron/behavior/kiting.cpp`, `LICENSE.txt` | Evaluate nearby threats and reachable movement, with persistent unit behavior. Retain existing influence-based movement and navigation; preserve engagement decisions independently from route invalidation. Terran-specific behavior is not transplanted into Protoss. |
| insanitybot | `Source/WorkerManager.cpp`, `Source/Squad.cpp` | Separate worker defense/repair roles and gate kiting by combat state. Existing worker defense and attack-frame protection cover the transferable pattern; SCV repair does not apply to Probes. |
| C0mputer | `C0mputer/unitmicro.cpp` | Gate orders by latency and recognize disconnected terrain before assigning transport work. Existing command arbitration, navigation, and transport missions cover the common structure; its generic taxi behavior is not a tested Reaver-drop implementation. |
| InfestedArtosis | `unit/squad/MutaliskCombatSimulator.java`, `unit/squad/SquadManager.java` | Specialized air squads should evaluate their own threats. Split Corsairs from Dark Templar, then split both by local connectivity. Do not copy its particular Mutalisk damage thresholds into Corsair decisions. |
| VOID | `macro/ProductionManager.java` | Explicit queued/in-progress production state and resource availability prevent conflicting spending. Preserve our existing ledger/producer occupancy design and close its disabled-producer gap. |
| UAlbertaBot variants | `UAlbertaBot/Source/CombatSimulation.cpp` | Bounded local simulation and explicit supported-unit handling provide reproducible regression cases. Keep the existing portable simulator and add the new scenarios below. |

These are original changes in Protodd's data model. No source text or substantial
implementation from an opponent is incorporated. In particular, BananaBrain
and Stardust have additional tournament restrictions in their licenses.
Unknown licenses in other packages are not assumed to permit code reuse.

## Implemented behavior

- Visible, moving Dragoon and Photon Cannon projectiles that should arrive
  within 24 frames reserve their target's remaining durability. Reservations
  are rebuilt each observation, deduplicated by bullet ID, and apply shields,
  armor, and damage type one hit at a time. Unsupported effects, hidden
  targets, dead/unknown sources, and impact animations without movement are
  excluded. Observed HP is never overwritten with a prediction.
- Volley allocations retain fractional damage and account for earlier
  commitments when shields deplete. Ranged units with an available shot avoid
  chasing an expensive target well outside their firing envelope.
- Reavers consume observed Scarabs in simulation. Interceptors remain reusable.
  Empty Reavers were already prevented from attacking; the defect fixed here
  was unlimited repeated attacks from a single Scarab.
- Ground-only enemies contribute no weapon power against an air-only squad.
  Static weapons outside range cannot claim support against stationary or
  outranging opponents. Harmless opposition no longer forces a retreat.
- Corsairs and Dark Templar form distinct, locally connected harassment squads.
  Engagement memory uses role and the oldest surviving unit ID, while routing
  retains the full membership/objective signature. Losing that anchor unit
  still resets memory; this is not a complete persistent squad tracker.
- Disabled, loaded, or hallucinated producers cannot reserve training or
  research resources that usable buildings need.
- All pending construction uses exact top-left tile coordinates, including
  pre-positioned Nexuses. A neighboring existing structure cannot satisfy the
  lease. Travelling builders retain ownership while moving; stalled orders
  are cancelled before reassignment, with the existing bounded retry policy.

## Validation

`testLadderSourceImprovements` adds outcome-oriented scenarios for projectile
overkill, duplicate IDs, shield depletion, fog/source loss, local shots,
air/ground threat separation, siege support, Scarab consumption, harassment
connectivity, engagement continuity, and disabled producer reservations.

The strict verifier passed, including the strict core/adapter compilation,
17 Python tests, ladder privacy audit, and whitespace checks. All four
Release/Win32 CTest targets passed. The BWAPI catalog executable also checks
that real Pylon, Gateway, and Nexus commands expose top-left pixel coordinates
and round-trip their build tiles, the contract used by the construction fix.
The initial working-tree patch and baseline/candidate DLL snapshots are kept
under ignored `build/ladder-source-audit/`.

| Binary | SHA-256 |
| --- | --- |
| Baseline | `4A7A0E6B01F3523CF0627133F98394B84610B69F9B41E386C14559ADFB2FF0D7` |
| Candidate | `010F1F70373967164A5500A8B3A92B0FA04B523D59A5005DCED22F180ABEC0E0` |

The candidate is built at `build/protodd-tournament/Release/Protodd.dll` and
frozen as `build/ladder-source-audit/candidate.dll`.
The ignored default `ladder/ladder.local.json` now stages that Protodd build
instead of the older `AstraBot.dll`; its previous settings are backed up at
`build/ladder-source-audit/ladder.local.before.json`. This configures subsequent
local experiments and does not submit or promote a tournament release.

| BananaBrain on Benzene, seed 42 | Baseline | Candidate |
| --- | --- | --- |
| Terminal result | Loss at 8,527 | Loss at 13,208 |
| Maximum Probes / army / Nexuses | 18 / 2 / 1 | 24 / 12 / 2 |
| First completed Core | 4,920 | 4,608 |
| First completed Dragoon | Never | 5,544 |
| First completed expansion | Never | 8,448 |
| First enemy contact | 4,632 | 9,120 |
| Peak callback | 3.465 ms | 8.944 ms |
| Callbacks over 55 ms | 0 | 0 |

Both games completed naturally; their manifests and pre-cleanup traces are
`build/direct-logs/source-audit-{baseline,candidate}-banana-42.{json,log}`.
The candidate exercised ranged production, technology, and expansion without
runtime overruns. Enemy contact timing differed substantially despite equal
game seeds and opponent package hashes: the opponent's runtime choices and
learning state are not fully controlled by this harness. These two losses do
not establish a strength improvement or an effect size. The mixed-binary report
is `build/ladder-source-audit/banana-comparison.json`.

The legacy UABZerg admission attempt timed out after 300 seconds without a
Protodd start log (`source-audit-candidate-uabzerg-42.json`). It is recorded as
incomplete with no result, not a loss. That imported package includes a proxy
launcher alongside its DLL. The launched proxy was cleaned up after the test;
its admission issue remains separate from gameplay validation.

The 150-second McRaveZ smoke run reached the frame-8,640 telemetry sample with
25 Probes, two Nexuses, and no logged errors or callbacks over 55 ms. The game
was still running at the time limit, so its manifest records an incomplete
attempt, not a competitive result (`source-audit-candidate-mcravez-42`).

The 150-second insanitybot/Terran smoke run likewise reached frame 8,640 with
23 Probes, two Nexuses, five completed Dragoons, and no logged errors or
callbacks over 55 ms. Its manifest also records incomplete
(`source-audit-candidate-insanity-42`). A loss line emitted during cleanup is
excluded from its pre-cleanup trace and must not be treated as a game result.
The startup player alias persisted from the preceding client profile, so use
the manifest's opponent DLL/configuration hashes to identify this attempt.

The combined report is `build/ladder-source-audit/match-report.json`: two
completed BananaBrain losses across different binaries, two running-game
smokes stopped at their time limits, and one failed UABZerg admission. All
processes launched for these tests were cleaned up. No full ladder batch or
statistically meaningful strength comparison has completed in this pass.

## Limits of this pass

This is not evidence of being the strongest bot or of a statistically established
win-rate improvement. Full mining optimization, collision-aware simulation,
terrain-specific walls, projectile miss prediction, coordinated Reaver drops,
and a tested opening portfolio remain substantial separate systems. Existing
strategy adaptations are preserved; adding every opponent's build orders would
not make them coherent or competitive without matchup testing.

Promote a candidate only after the balanced opponent/map matrix described in
the ladder guide. Preserve incomplete-game accounting, CPU limits, exact
opponent versions, and held-out maps.
