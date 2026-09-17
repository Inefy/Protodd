# Logging-driven strength pass — September 12, 2026 UTC

This pass uses the version 3 diagnostics already present in the working tree.
The original logging and strategy changes were preserved. Test games use frozen
DLLs, fresh learning on both sides, and pre-cleanup results from the direct runner.

## Reproduced faults and changes

| Fault observed in the traces | Correction |
| --- | --- |
| A busy Probe caused legal construction tiles to be blacklisted; pending builds repeatedly issued `Build` during the same construction order. | Select commandable builders, preserve an active build through latency/construction, and blacklist only `Unbuildable_Location` rejections. Stalled leases still recover. |
| The highest priority and largest count of overlapping structure goals were combined. A completed two-Gateway checkpoint promoted optional additional Gateways above technology. | Select the strongest unmet checkpoint with its own count and blocking policy; retain that pairing in demand memory. |
| Supply forecasting ignored a Gateway about to finish and treated queue commitments as continuous consumption. | Include finishing producers, their remaining training time, explicit unit demand, and discrete production cycles over the Pylon construction/travel horizon. Existing queues remain included in used supply exactly once. |
| Gas workers ran toward an Assimilator at a lost natural instead of the surviving main. | Require a nearby owned base, reject observed weapon coverage and unsafe transfers, and issue gas orders to the exact assigned refinery. |
| Several gas-starved Gateway goals each protected another unit's mineral cost, immobilizing the bank. | Protect one future gas-dependent unit per producer type; leave surplus available to executable defense and infrastructure. |
| The opening Probe saw the Core at frame 3,048 but remained until the first Dragoon arrived. | Withdraw when the Core is observed or on the first hull damage, retaining the existing latched escape and return-to-mining behavior. |
| Probe entity rows sometimes reported a queued Pylon because BWAPI reuses mobile-unit queue storage. | Read production queues only for buildings and ammunition producers. Ordinary mobile units report an empty queue and zero training timer. |
| The direct runner defaulted to a different DLL directory from the build/package scripts and ladder template. | Default all of them to `build/tournament/Release/Protodd.dll`; experiments still use an explicit frozen `-BotDll`. |
| Defense and main squads evaluated a shared enemy group without nearby allied squad members or supporting Cannons. | Include nearby, reachable combat support and in-range static defense in the estimate. Orders remain assigned to the original squad; support cannot justify an independent counterattack or raid. `SQUAD.supportUnits` explains the additional power in the viewer. |
| The ranged mirror spent 400 minerals on a natural before its first splash defender, moving a small Dragoon screen away from home. | Before twelve minutes, finish a Reaver before the contested natural unless eight Dragoons, an Observer, and recent enemy observations establish a decisive mobile lead. |
| Early Terran bio arrived while the bot had only two mobile defenders. | Fund an opening Zealot before optional spending and prioritize the first defensive anchor when early bio is observed approaching through our side of the map. |
| A crowded four-base economy repeatedly ran nested BWAPI placement searches and more than 8,000 fallback tile checks per building. | Test preferred tiles directly and spread the broad search into windows of 256 expensive checks. The compact scored defense/production search still runs each pass, and fallback windows resume until all candidates have been inspected. |
| A completed splash tech chain still lost each new gas deposit to four Gateway reinforcement requests. | Give the first Reaver a distinct priority above repeated Gateway orders. Preserve the first urgent Observer, then build splash before additional detector backups. |
| During a visible Terran bio attack, uncertainty alone reserved the remaining gas for a 200-gas Robotics Facility. | Suspend that speculative detection requirement during current ground pressure. Observed cloak, mines, factories, and Starports retain their separate warning paths. |
| A scouted two-Gateway melee rush arrived while an early Forge still had no completed Cannon. | Fund the first Cannon before extending the Zealot queue once the Forge and a bodyguard are committed. |
| At frames 11,352–11,640 in v4's BananaBrain game, many healthy Dragoons followed distant targets while the front few took concentrated fire. | Prefer targets already inside actual weapon range, including adjacent blockers for melee; charge out-of-range targets for time to contact. A wounded target no longer attracts unbounded overkill priority. |
| A favorable whole-squad estimate could send the first ranged unit into several enemies while the rest of the army was still approaching. | Compare immediate enemy coverage with allies currently able to support that contact. Exposed front units fire a ready volley, then regroup toward the nearby army. This includes armed air units and supporting Cannons. |
| Shield-depleted combat units continued to absorb focus fire until critically wounded. | During reload, rotate behind a healthier nearby ally that can cover the threat. The rule applies to melee, ranged ground, and armed air units; attack animations and available ranged volleys remain protected. |
| A defensive pursuit leash could pull a ranged unit away even with an available shot. | Take the in-range volley before returning; resume the leash on reload. |
| Routine target changes could replace an attack before command latency elapsed. | Protect an accepted attack through latency plus two frames from equal/lower-priority movement and retargeting. Emergency escapes retain priority. |
| v4 issued 9,674 accepted `defense-hold` commands against BananaBrain. | Keep an unchanged hold stance for up to 120 frames; any intervening attack or move immediately permits a new stance. |
| Short retreat/kite steps ignored terrain connectivity and friendly congestion. | Check the entire ground segment and score nearby allies' planned positions when choosing combat steps. |
| In v5, a third-base request imposed a distant defensive leash on an army already fighting elsewhere. Many losses at frames 13,800–14,500 had `defense-return` as their last order. | Adopt an expansion leash immediately during uncontested travel or once the army reaches it; retain the current fight/retreat policy while contact continues elsewhere. |
| A conservative navigation cell could reject all retreat steps even though the unit was observed standing on its passable edge. | Admit the observed origin cell, then validate every subsequent terrain cell. |
| `LOSS` used BWAPI's whole-egg purchase price for each destroyed Zergling or Scourge. | Record the production batch size and normalize reports to individual units, including older traces and fractional Scourge prices. |
| In v5's full-supply Protoss game, an uncontested 33–51-unit army repeatedly received a destination behind itself to rendezvous with a trailing Reaver. | Replace whole-army rendezvous with individual orders for at most two nearby bodyguards per squad. New/detached/transport-owned Reavers cannot recall the army. |
| Spare-fighter raids could recruit units from a full-army attack, then send them home on extraction. | A committed Attack posture starts no new spare-fighter raids; healthy active raiders join the main attack. Existing emergency withdrawals remain latched. |
| In v7, two Dark Templar killed the workers while 21 combat units remained alive. The first Observatory/Observer kept losing gas to splash and Gateway cycles before the cloak alarm. | Once Robotics and six completed Dragoons are committed, protect the first Observer and its prerequisite above those repeated cycles. A required detector or ten-minute timing also enables the checkpoint. Backup detectors still yield to the first Reaver. |
| In v8, a detached High Templar's squad repeatedly requested `join-vanguard`, but tactical control anchored it to its own center and held it near home. | High Templar, Dark Archons, and Arbiters use the squad's full destination during clear travel. Rear-screen positioning resumes on contact; existing retreat, Storm, and defensive restrictions retain precedence. |

The supply forecast remains a heuristic rather than an income/placement simulation.
Refinery threat checks use legal visible observations and eight seconds of enemy
memory. They can temporarily reduce gas collection under pressure. The scouting
change trades harassment time for an earlier escape; it does not guarantee survival.

## Controlled development games

BananaBrain uses its installed AIIDE 2025 package with the explicit `PvP_nzcore`
opening, Benzene, requested/observed seed 43, and reset runtime learning. Each
game has a 43,200-frame limit. Only a natural result is counted as a win or loss.
The same seed/opening does not guarantee identical movement or opponent decisions.

| Build | Natural result | Peak army | First main breach | Supply-block incident duration |
| --- | --- | ---: | ---: | ---: |
| Starting logging build | Loss at 15,223 | 9 | 11,640 | 20 seconds |
| v1: construction, priorities, supply, hull-damage retreat | Loss at 20,090 | 16 | 16,248 | 14 seconds |
| v2: also gas recovery, reservations, scout warning, queue diagnostics | Loss at 13,828 | 10 | 11,832 | 10 seconds |

The first two supply blocks fell from 20 to 11 seconds. v1 incurred another
three-second block later in the game. Total recorded BWAPI rejections fell from
179 to 30, but v1 accumulated more idle-production time and a 110-second large-bank
incident while surviving longer. These games justify the targeted fixes, not a
general win-rate claim. v1 still lost its natural and then its army.

v2 reduced issued rejections further to eight, preserved the opening scout, and
restored main-base gas assignments after the natural was lost. It nevertheless
lost sooner than both earlier builds. Correctness metrics alone do not establish
playing strength.

The v2 race sweep used the installed UAlbertaBot package, fixed seed 43, Benzene,
fresh learning, and the same frame limit:

| Opponent | Natural result | Peak army | Peak bases |
| --- | --- | ---: | ---: |
| UAB Zerg | Win at 31,374 | 51 | 4 |
| UAB Terran | Loss at 10,852 | 3 | 1 |
| UAB Protoss | Loss at 9,085 | 2 | 1 |

All six development traces above have zero caught exceptions, logging errors,
malformed diagnostic records, and health sampling gaps. The Zerg win exposed
1,340 frames at least 42 ms and 905 at least 55 ms, with a 264.088 ms peak; macro placement
dominated the phase timings. The other five games had no frame-budget overruns.

v3 adds combat support, contested-mirror expansion gating, early bio defense,
and bounded placement. It lost to BananaBrain at 14,851 (peak army 18, first
main breach 12,672), UAB Terran at 18,168, and UAB Protoss at 12,061. Its Terran screen preserved the
worker line through the first push but failed to transition out of a gas-starved
contain. It beat UAB Zerg at 24,616, 21.5% fewer game frames than v2's 31,374.

| Zerg trace measurement | v2 | v3 |
| --- | ---: | ---: |
| Whole-frame peak | 264.088 ms | 12.938 ms |
| Frames at least 42 ms | 1,340 | 0 |
| Frames at least 55 ms | 905 | 0 |
| Macro phase peak | 263.961 ms | 10.172 ms |
| Macro phase mean | 14.505 ms | 0.093 ms |

These are observed whole-game measurements; the games differed in duration,
base count, and battlefield state. The search budget provides a direct mechanism
for the speedup, but this is not an isolated microbenchmark. v3's four traces
have no caught errors, logging errors, malformed records, or health gaps.

v4 adds the first-Reaver and first-Cannon checkpoints, pressure-aware speculative
detection, and preservation/revalidation of fallback tiles across placement windows.

| v4 opponent | Natural result | Peak army | Whole-frame peak |
| --- | --- | ---: | ---: |
| BananaBrain | Loss at 20,152 | 19 | 16.350 ms |
| UAB Protoss | Loss at 9,860 | 4 | 18.371 ms |
| UAB Terran | Loss at 23,841 | 16 | 17.788 ms |
| UAB Zerg | Win at 31,188 | 47 | 17.630 ms |

All four v4 games have zero frames over 42 ms, caught errors, logging errors,
malformed records, or health sampling gaps.

## Army control follow-up

The v4 BananaBrain loss included 39 friendly Dragoon losses versus eight observed
enemy Dragoon losses, plus 18 friendly Zealots and three Reavers versus 26
observed enemy Zealots. These are legally observed loss events, not full enemy
production or attacker-specific kill attribution. The trace supports the user's
assessment that unit value, across the army, needs attention beyond build order.

v5 adds the seven army-control corrections above and an `attackFrame` field to
entity snapshots. It has passed all five Release suites and the strict verifier.
`build/logging-strength/unit-value.json` records
per-kind observed combat losses, costs, damage received, under-fire snapshot
counts, and exact accepted action counters. Cooldown-zero snapshots are not
counts of missed shots, and unknown or unseen enemy losses are not inferred.

v5 lost its BananaBrain game at frame 19,718. Its observed losses were 42
Dragoons, 11 Zealots, and two Reavers versus 20 enemy Dragoons and seven enemy
Zealots. The observed enemy-to-own combat loss cost ratio rose from 0.420 to
0.464, but this is one game per build and different unit mixes; it is not a
general strength estimate. Accepted `defense-hold` commands fell from 9,674 to
536. There were 98 accepted wounded rotations and 28 frontline regroup orders,
with a 16.157 ms peak and no frame-budget overruns.

v5 then beat UAB Protoss at frame 38,659 and UAB Terran at frame 29,576. The
previous v4 results were losses at 9,860 and 23,841 respectively. The remaining
v5 Zerg test was skipped after the Terran game was safely archived so testing
could prioritize the user's observed movement fault.

The Protoss win was inefficient: from frames 30,600–33,480 its large uncontested
army repeatedly switched between the enemy base and trailing support. For
example, at frame 31,080 a 33-unit army centered at 507x2872 was ordered toward
422x3175, then at 31,200 back toward 3808x464. At frame 33,000 a 50-unit army
centered at 895x2011 received 421x2985. Individual Dragoon 443 followed these
reversals, then was recruited into a raid and extracted home. These are actual
orders and positions, independent of the strategic Attack label.

v6 passed checks for the expansion handoff, conservative terrain origin, and
paired-egg loss accounting. Its queued games were replaced before starting.
v7 includes those fixes plus bounded escorts and attack/raid ownership. New
`SQUAD.travelGoal`, `travelReason`, `retreat`, and `defenseCenter` fields distinguish
the mission destination from a short navigation waypoint.

| v7 opponent | Natural result |
| --- | --- |
| UAB Protoss | Loss at 17,238 |
| BananaBrain | Loss at 14,200 |
| UAB Zerg | Loss at 11,844 |

These games ended before a large late-game attack. Their absence of backward
attack goals does not validate departure or sustained forward movement. The
BananaBrain trace showed a different failure from the earlier ranged fights:
two undetected Dark Templar entered the mineral line while the mobile army
waited for detection. At frame 11,040, 21 combat units remained but only one
Probe survived. v8 adds the first-detector checkpoint and passes both its
missing-Observatory and ready-Observatory resource-allocation regressions.
Its Terran game won at 36,768; BananaBrain won at 20,493. The first Observatory
completed at 9,312 and Observer at 9,936 in the BananaBrain game. That game had
no observed Dark Templar, so it validates the earlier detection timing, not
defense against the previous game's DT attack. It still lost the larger ranged
fight and its economy. The Terran trace has a 16.270 ms whole-frame peak and zero
frames at least 42 ms.

The v8 Terran run validates actual late army departure: at frame 35,832 a
56-unit army centered at 3495x836 was attacking toward 3808x464. From the
closeout transition at 32,040 through the win, none of 197 sampled frames with
at least eight older combat units had a quarter of them within 768 pixels of
the main Nexus. v5 Terran had 16 such samples out of 273. Units completed less
than 720 frames earlier are excluded from this older-unit measurement. This
does not measure time in transit around every expansion or establish a win rate.

The raw travel audit reports six apparent backward-goal samples in v8 Terran;
all occur during cleanup at the enemy base (frames 36,384–36,720). Inspection
shows the mission switching between 3808x464 and a remaining structure at
3808x144, ahead of the next five-second `STATE` sample. They are not returns
home. Recorded late-attack `raid-extract` unit snapshots fell from 990 in v5
Terran to eight in v8; this includes the tail of already-latched withdrawals
and comes from games of different duration.

Earlier in that same v8 win, High Templar 595 remained within the main's
768-pixel radius from completion at 23,832 through 28,416. It appeared in 182
uncontested squad samples during that period. For example, its one-unit squad
was centered at 408x3368 with a `join-vanguard` goal at 592x1948, yet the caster
held its own rear screen. This is distinct from the Reaver rendezvous fault.
v9 corrects clear travel for all three support-caster types and passes isolated
caster regressions while retaining the existing combat-screen and Storm tests.
Its Terran follow-up lost at 15,998, with a peak army of six and no Templar
production. Therefore the new caster path has regression coverage but no live
game coverage in this pass. Its whole-frame peak was 14.432 ms with no frames
at least 42 ms. The earlier v8 win demonstrates the large-army travel fixes;
it must not be presented as a v9 win.

There were 23 completed development games across the frozen builds: six wins
and 17 losses, with no interrupted/timeout results counted. All 23 traces have
zero caught errors, logging errors, malformed records, or health gaps. The
v5 Terran win had three frames at least 42 ms and one at least 55 ms (90.746 ms
peak); its later v8 game had none. These are whole-game observations under
different battlefield states, not isolated performance or win-rate comparisons.

The confirmed army-destination, raid-ownership, and isolated-caster faults are
corrected. Reliable early-rush defense and beating BananaBrain remain unresolved:
all eight BananaBrain development games were losses. One map/seed and individual
games per candidate cannot establish general playing strength or ensure every win.

## Reproduction and artifacts

Frozen binaries are in `build/logging-strength/`. The starting SHA-256 is
`6378C7439A0551B8916C5F62BDE395CE7765B0D7D85218F158A2007D57FF5CFD`;
v1 is `A9C6242CB106628118C242AC7F0B8CA56DF6DCEC9BD36A6B776B491A2CB50A6A`;
v2 is `755A55D024BEDB84DB41A39B6DD9D0F1998C10B9B129D9BC349C8C5B8F7D7161`;
v3 is `43D7A8B73717EC27904F0DF48E79CF16DC0BC2F157D5515A134F810C58452A05`;
v4 is `623D39B28A17C8B373042768582DB8FB10C4269CCDC5D5DBC27960BD7A33FA1B`;
v5 is `240E555C58352694783127AB5AE897DA5BDACF9A34D6312294A670C718E9A83C`;
v6 is `057043D4C4635F622200296E260C03807FE6F9F4B24F4BB81F88EF8FE0FEAA77`;
v7 is `C9BDCB79C2469A211F8DE9D517CA47A6F68544B4AF9CF8A6678FE47BF27674EF`;
v8 is `49E1C91E4FA3CD0FEDDCEF25218BB417DF2CAF45F84B721129D6D04C3889F45D`;
v9 is `B1541F6E62CC733DCC97B68427FECA42F455F2B7D21984C9D1C6B8FF143A5EB9`.

```powershell
./scripts/direct-match.ps1 -OpponentName BananaBrain -OpponentRace Protoss `
  -OpponentOpening PvP_nzcore -Seed 43 -Label unique-test-label `
  -BotDll build/logging-strength/candidate-v9.dll -FrameLimit 43200
```

Authoritative manifests, archived traces, and offline decision reports are under
`build/direct-logs/logging-strength-*`. Opponent/map/configuration hashes and
the observed seed are in each manifest. Generated comparison data are under
`build/logging-strength/`; local game/opponent assets remain Git-ignored.
`metrics.json`, `unit-value.json`, `travel-audit.json`, and `caster-audit.json`
preserve the measurements and their scope. The final default DLL is v9 at
`build/tournament/Release/Protodd.dll`; its frozen binary, complete source ZIP,
and file-hash manifest are retained as `candidate-v9*`.

Regression scenarios cover structure priority tiers, supply during Gateway
completion, pending Pylon credit, the first hull damage and Core warning, orphaned
and threatened refineries, exact refinery assignment, gas-starved reservation
pressure, nearby support, expansion release, early bio priorities, and complete
coverage of bounded placement scans. Army-control regressions cover in-range
target choice, pursuit cost, melee blockers, splash overkill, loaded targets,
melee/ground/air relief, attack animations, unsupported frontline regrouping,
ready defensive volleys, cliffs, latency, emergency overrides, bounded Reaver
escorts, attack/raid ownership, expansion handoff, first detection, and detached
support-caster travel. The viewer exposes movement reasons and destinations;
its updated JavaScript also passes Node's syntax check.
All five Release CTest suites and the strict verifier pass. The verifier
includes C++ warning-as-error compilation, BWAPI adapter compilation, 38 Python
regressions, privacy checks, and whitespace checks.
