# Containment investigation — September 6, 2026

See the subsequent [opening tempo experiment](opening-tempo-review.md) for a
production-order correction and an identified limitation of these test
conditions: opponent runtime learning was not reset by the old runner.

The failure is a chain of economic, tactical, and placement decisions. Merely
lowering the attack threshold does not fix it. This investigation follows the
[eleven-opponent source review](ladder-source-review.md), with additional reads
of BananaBrain's `Strategy.cpp` and `ProtossStrategy.cpp`, Stardust's PvP
strategy selection and `DefendMyMain.cpp`, and McRave's combat state machine.
These are architectural references; the implementations below are original.

## Failures reproduced in full games

The frozen starting DLL (`010F1F70...ABEC0E0`) lost to BananaBrain on Destination
at frame 12,681. It peaked at 22 Probes, nine army units, and one Nexus. A
second candidate exposed a different failure after successfully leaving home:
its local estimate discarded enemies the moment retreat took them out of
vision. In its trace, the main squad's ratio changed from 0.65 against eleven
enemies at frame 10,320 to 7.61 against three at 10,680. At 11,040 it was again
retreating, now with a 0.34 ratio. Disappearance from vision was being treated
as a victory without evidence of enemy losses.

The first intermediate candidate also lost exposed Gateways while defenders
were held near rear Cannons. Expensive buildings, available fighting units,
and the static screen were protecting different locations.

## Implemented changes

| Area | Change | Why it matters |
| --- | --- | --- |
| Construction deadlines | Wait to reserve the next tech step until an existing prerequisite is within ten seconds of completion; handle recursive prerequisites too. | Idle Gateways can spend income during a long Robotics build, then the next tech step receives its reservation before completion. |
| Opening timing | In the quiet PvP opening, request Robotics at 20 displayed supply, a second Gateway at 22 with Robotics committed, and the third at 29. Schedule the Support Bay during the detection chain. | The first Reaver should reach the first substantial Dragoon fight, rather than start its tech chain after the Observer has finished. The separate observed-rush opening remains active. |
| Splash transition | Four mobile units plus a completed Cannon, or four completed Dragoons, can protect the first Robotics/Support Bay/Reaver investment ahead of repeated Gateway cycles. | A one-base defense must gain a stronger unit type; indefinitely replacing basic units cannot close an expanding opponent's production advantage. |
| Transport ownership | The first two PvP Reavers remain army units. Emergencies recall transported Reavers. Delay the optional Shuttle until two Reavers are complete. | The first expensive splash unit is available for the battle it was built to solve. |
| Reaver cohesion | An uncontested vanguard rendezvous with nearby trailing Reaver support. Active combat decisions and distant new production do not trigger this wait. | Fast Gateway units should not march away from the splash support they need. |
| Economic transition | After the opening, compare recent threats at every owned economy with compatible nearby mobile support and the combat simulation; allow covered growth and restore worker targets for committed Nexuses. | A historical rush label or a small perimeter group no longer cancels every expansion reservation. Actual breaches and losing local estimates still interrupt growth. |
| Expansion cover | Give the army and builder a common selected expansion site, then retain cover while the new Nexus builds. | The first field army must not abandon a new economy during its vulnerable construction window. |
| Army allocation | A stabilized breakout army stays together. An ordinary home guard can only take units above the declared attack minimum. | The strategy's attack size becomes attainable; the same army is not divided into a permanent defense group and an undersized attack group. |
| Attack objectives | Score known enemy depots by distance and observed local protection. | Exposed expansions can be selected before the defended main. This depends on scouting; it is not knowledge of hidden bases. |
| Combat memory | Keep recent mobile enemy observations in local estimates for eight seconds, with a bounded possible approach margin. Stationary defenses retain their remembered location. | Retreating out of vision no longer instantly erases the opposing army. Hidden units remain invalid tactical targets. |
| Retreat volleys | Healthy ranged units may fire a ready shot at a visible, detected target already inside weapon range while falling back. Cooldown, critical health, empty Scarabs, and the existing attack-frame guard preserve movement and completed attacks. | Retreat no longer concedes every firing opportunity. Out-of-range attack orders cannot reverse the retreat into a chase. |
| Production placement | Search and rank compact legal powered Gateway sites around the economy, keeping passage and mining-lane checks. | A first-legal-tile search must not repeatedly put replacement production outside the defensive screen. |
| Production exits | Keep a full tile of clearance beside producers, including when later non-producing tech is placed next to them. | A valid Probe path and a legal building footprint do not prove that a newly produced Reaver can exit. |
| Static placement | When an army approaches, score coverage toward that observed approach alongside mineral coverage. | Cannons should support the fighting screen rather than pull it behind the Nexus. |
| Gas balance | Apply the excess-gas mineral-recovery rule to a one-base pressure army and the economic transition too. | A posture change no longer sends workers back onto gas while minerals remain the bottleneck. |
| Scouting | Quiet home guards do not claim an Observer; growing PvP economies request a spare. | Detectors can discover expansions while the field army retains its escort. |
| Diagnostics | Log each squad's role, size, target, ratio, decision, and detection gate, plus the economic and containment flags. | A strategic label alone cannot explain whether units actually moved or fought. |

All decisions use legal current observations or bounded observation history.
The local simulation and detection gate remain in control of individual
engagements; a map-control plan does not force a losing engagement.

## Other possibilities assessed

| Possibility | Assessment |
| --- | --- |
| Attack unconditionally or remove all safety checks | Does not solve production losses or unfavorable fights; actual breach and cloak protection remain necessary. |
| Expand unconditionally while contained | Risks donating 400 minerals and a builder. The implemented transition requires a stabilized economy, favorable local simulation, and a shared expansion objective. Route-aware site selection and a map-specific natural defense remain further work. |
| Copy every opponent opening | The openings have incompatible economy, race, micro, and terrain assumptions. A tested, coherent portfolio is needed rather than simultaneous quotas. |
| More Cannons everywhere | Often worsens mineral starvation and delays ranged/splash tech. Placement and a bounded mobile-supported transition matter more than raw Cannon counts. |
| Early Reaver raids | Useful when there is surplus splash and a sound drop controller; harmful when they remove the only Reaver from a losing defense. |
| Storm, speed Zealots, and upgrades | Existing spell/micro support provides a foundation. Late-game timing and composition need full-game tuning after the early/midgame economy survives reliably. |
| Mining optimization | Stardust and BananaBrain demonstrate substantial specialized work here. Per-patch travel-time learning, collision-aware mineral walking, and measured income comparisons remain high-value projects, beyond the current balanced assignment. |
| Terrain-aware walls and formations | Proper natural/choke identification, collision-aware combat, and coordinated retreat corridors remain major improvements. Geometric distances do not substitute for a complete terrain model. |
| Opening learning | Seeded game starts do not control all opponent runtime choices. Use multiple starts and held-out maps, retain opponent artifact hashes, and separate learning conditions before attributing win-rate changes. |

The next competitive work should prioritize three measurable questions:

1. Can the opening field enough ranged units **and** splash at first contact?
   Compare first-Dragoon/range/Reaver timing and army losses against the exact
   opponent opening. Earlier Robotics is an experimental spending choice, not
   an independently established improvement.
2. Can the army retain favorable terrain while exchanging volleys? Record
   actual shots, collision stalls, retreat routes, and surviving unit value.
   A favorable geometric simulation does not establish a traversable firing
   position or a successful Scarab path.
3. Can an expansion survive the next reinforcement wave? Measure Nexus
   completion, worker transfers, income, and army value after the opponent's
   response. Selecting a site or issuing an expansion order is insufficient.

Only then does a broad fixed-binary ladder matrix establish whether the new
economy can convert into wins. Keep baseline and candidate results separate,
vary map/start conditions, and include every opponent race. No current result
supports a tournament-winning claim.

## Validation and artifacts

New portable scenarios cover future tech deadlines, protecting splash
investment, exposed economic targets, genuine breaches, reachable attack-size
thresholds, bounded fog memory, Reaver ownership/recall, and firing/escaping
through a ranged retreat cycle. Release CTest and
the strict C++/BWAPI/Python verifier are run against the resulting source.

Frozen binaries and generated reports are under `build/containment-audit/`.
Authoritative match records and pre-cleanup logs are under
`build/direct-logs/containment-*.{json,log}`. Intermediate versions must not be
pooled into one win-rate claim. Timeout/cleanup results are not competitive
results. Match hashes identify each frozen implementation independently of
the working DLL.

### Intermediate games

| Frozen version | Map | Natural result | Peak Probes / army / Nexuses |
| --- | --- | --- | --- |
| Starting baseline | Destination | Loss, frame 12,681 | 22 / 9 / 1 |
| v1: scheduling and transition | Destination | Loss, frame 13,735 | 22 / 7 / 1 |
| v2: splash ownership and initial placement fixes | Destination | Loss, frame 18,075 | 37 / 15 / 2 |
| v3: compact production and combat memory | Benzene | Loss, frame 13,890 | 22 / 12 / 1 |
| v4: simulated economic gate and expansion cover | Destination | Loss, frame 14,851 | 22 / 17 / 1 |
| v5: earlier splash timing | Destination | Loss, frame 13,828 | 22 / 8 / 1 |
| v6: producer clearance and Reaver cohesion | Destination | Loss, frame 13,828 | 22 / 8 / 1 |
| v6: same frozen DLL, seed 43 | Benzene | Loss, frame 14,758 | 22 / 12 / 1 |

v4 on Destination also lost (frame 14,851), so numeric/local safety changes
alone were insufficient. v5 moved the first splash timing earlier but lost at
13,828. Its first Reaver stayed at `(2203, 4045)` from at least frame 8,880
through 10,080 with ammunition and attack targets, consistent with a blocked
production exit. The code allowed zero clearance beside Robotics. v6 applies
producer clearance symmetrically and adds nearby Reaver travel cohesion.
On Destination, v6's first Reaver reached `(1955, 3817)` by frame 8,880,
alongside its defending Dragoons. This confirms an exit on that layout; it
does not prove every terrain/building configuration is safe. The game still
lost at frame 13,828 with eight army units and one Nexus at peak. The combat
controller then revealed another issue: retreat suppressed even ready,
in-range ranged shots. v7 adds bounded retreat volleys.

The v2 game demonstrated a successful departure from the main and worker
growth on two Nexuses, then lost the army and expansion. The v3 game reached
a larger field army but approved growth while the combat estimate was losing.
Those failures motivated the simulation gate and explicit expansion cover in
v4. None of these intermediate results is evidence of tournament readiness.
Opponent openings varied even with the same requested game seed; this is an
observational development sequence, not a controlled estimate of effect size.

### Final candidate v7

Frozen DLL: `build/containment-audit/candidate-v7.dll`.
SHA-256:
`1142E3A7142D5E981EE623CD3D2DE88452CFF87ED50B77D5CA977B89464E84A0`.
At the end of this pass, the Release DLL at
`build/protodd-tournament/Release/Protodd.dll` contained the same implementation;
subsequent experiments may replace that working artifact. All four Release
CTest targets passed. The strict verifier
passes the portable C++ tests, BWAPI compilation, 17 Python tests, and privacy
audit; `git diff --check` is clean.

| Opponent | Map / requested seed | Natural result | Peak Probes / army / Nexuses |
| --- | --- | --- | --- |
| BananaBrain | Benzene / 43 | Loss, frame 15,192 | 22 / 11 / 1 |
| BananaBrain | Destination / 42 | Loss, frame 13,766 | 22 / 10 / 1 |

In the Benzene game, the first Reaver moved with the field army and its
ammunition/cooldown trace records firing. The expansion transition activated,
but the incoming force interrupted it before a second Nexus started. The
final build therefore still failed to solve sustained containment in this
game. Its 14.813 ms peak callback and zero recorded budget overruns are runtime
evidence only, not evidence of stronger play.

Destination also lost its mobile army before expansion. The final binary is
**0 wins, 2 losses against BananaBrain in these completed games**. Neither game
completed or started a second Nexus. Both are natural game results, not timeout
cleanup records. These observations do not demonstrate an overall strength
gain, and this build is not validated for tournament submission. The earlier
two-base v2 game must not be attributed to v7. Frozen baselines remain available
for further comparisons; the complete generated report is
`build/containment-audit/report.json`.
