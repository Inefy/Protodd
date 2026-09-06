# Follow-up strength audit — September 2026

This pass starts from commit `3a74e70` and the previous v5 audit candidate.
The changes address observed economy, defense, placement, and information
failures. They do not establish that Astra can win AIIDE. BananaBrain remains
the demanding opponent used to expose weaknesses; UAlbertaBot is an
integration and rush-response benchmark.

## Corrected failure modes

| Area | Observed failure | Change |
| --- | --- | --- |
| Mining | Returning workers target the Nexus, so their mineral/refinery assignments were repeatedly lost. | Retain patch assignments through cargo returns, count returning workers in patch loads, and preserve refinery slots during the return cycle. Release obsolete assignments on death, depletion, or transfer. |
| Resource balance | One-base assembly accumulated hundreds of gas while minerals delayed defenders. | Extend the existing mineral-recovery gas policy to one-base Hold posture, preserving the gas buffer for the next production cycles. |
| Worker recovery | After a raid, the last available Probe could still be assigned to gas, leaving no mineral income to rebuild workers. | Keep at least six unleased workers available for minerals before filling gas slots. |
| Supply timing | The strategic forecast added queued units to BWAPI supply that already included them. | Count current used supply once and reserve headroom for the next production cycle. |
| PvP opening | Seven-Probe Forge/Cannon spending delayed the Core and ranged army. | Start Gateway production at nine supply, grow workers, and fund gas/Core access. Add static support in response to scouted two-Gateway melee production; prioritize Dragoons and range once the transition is established. |
| Expansion decisions | Saturation recovery could override an explicit one-base defense and spend 400 minerals before the attack arrived. | Apply the matchup's base limit after style, recovery, and safety modifiers, then trim expansion goals before infrastructure planning. |
| Defensive pursuit | Zealots followed ranged enemies far beyond Cannon support. | Bound defensive pursuit and return stragglers directly to the screen. Include the defended economy in both the permitted area and last-stand trigger so rear Cannons cannot prevent fighting a mineral-line breach. |
| Retreat and reinforcement | A short waypoint from the squad centroid could lie behind a unit or inside buildings. | Route an uncontested retreat and a return to defense to their actual destinations; preserve local threat avoidance during contact. |
| Shield Battery | Priority-100 retreat always beat priority-92 recharge for critically wounded units. | Critical recharge receives priority 101; attack-frame protection and higher-priority Storm escape still apply. Reject unpowered or disabled Batteries. |
| Placement | Adjacent buildings formed movement pockets; first-legal Cannon placement left workers uncovered. | Reserve a full build tile between structures, including pending builds. Score defensive sites for mineral coverage and proximity, within legal power, path, and mining-lane constraints. |
| Tactical targets | Workers, ordinary structures, and unfinished proxies were absent from the tactical target set. | Add nearby visible legal targets separately from the combat-simulation input. Units can explicitly attack workers, construction, and surviving production. |
| Cloaked breakout | General retreat and pursuit rules kept healthy Dark Templar behind the Cannons against a Marine contain. | Permit a covert advance while cloaked, healthy, not under attack, and outside observed detection influence. Detection or incoming attacks restore normal retreat rules. |
| Reserve counterattack | The global Defend posture pinned an independently strong reserve force to its rally despite a favorable local estimate. | Permit a main-army reserve group to counterattack only with a decisive estimate and the full minimum number of fighters able to hit the observed enemy. Assigned base defenders stay allocated; static defenses and support casters cannot satisfy the fighter count. |
| Ammunition | Macro reservations could prevent a Reaver from buying even its first Scarab. | Fund a small usable payload before surplus ammunition; avoid stacking ammunition queues. |
| Enemy inference | Reprocessing the same remembered unit increased confidence without new evidence. | Normalize current evidence once per update; observation age and strategic hysteresis provide persistence. Repeated callbacks cannot manufacture certainty. |
| Construction timing | Enemy remaining build time is unavailable; combining changing progress with first-seen time fabricated early starts. | Track a conservative construction-start upper bound from legal observations. Preserve it across updates, but not across a unit-type morph. |
| Scouting | Reserving an Observer for combat and then skipping another left two-Observer builds without a scout. | Scout with completed Observers that have not already been leased as escorts. |

The placement rule is a clearance heuristic, not a proof of connectivity around
cliffs, ramps, mineral walls, or large late-game bases. Cannon coverage also
depends on where Pylons provide power. These remain important live-test cases.
Likewise, the combat evaluator still approximates collision, projectile travel,
high ground, spell effects, and splash geometry.
Covert advances infer risk from legal observations; they do not establish what
the opponent can see. A hidden detector or a new scan can invalidate that
inference. The core tests cover both observed detection and incoming attacks.

## Experiments

All rows below used Python and requested seed `1788550258`. A fixed seed does
not guarantee a fixed opponent opening or complete process determinism.
Opponent learning/randomness and starting arrangements must be checked before
treating two games as a controlled pair. These are diagnostic samples, not a
win-rate estimate. Mixed binaries must not be pooled to rate the final one.

| Candidate | Opponent | Outcome | Finding |
| --- | --- | --- | --- |
| Prior v5 | UABProtoss | Win at 24,368 | Previous working integration baseline. |
| Prior v5 | BananaBrain | Loss at 15,254 | Late ranged transition, weak economy, and excessive defensive pursuit. |
| v6 | BananaBrain | Loss at 14,262 | Earlier Core/Dragoon and more workers did not resolve the breakthrough. |
| v7 | BananaBrain | Loss at 11,007 | Mobile opening was too weak against the observed melee flood. |
| v8 | UABProtoss | Loss at 22,322 | Mining/routing fixes alone did not compensate for the opening's poor defense. |
| v9 | UABProtoss | Win at 23,624 | Scouted support and inference fixes restored a win; economy survived an army loss and rebuilt. |
| v10 | BananaBrain | Loss at 13,952 | Persistent production evidence and recovery fixes still left placement weaknesses. |
| v11 | BananaBrain | Loss at 11,689 | Building clearance let Dragoons move, but pursuit limits excluded part of the economy and saturation overrode the expansion hold. |
| v12 | BananaBrain | Loss at 14,293 | Economy and pursuit corrections did not stop the worker losses. The surviving Probe remained on gas; v13 adds a mineral-worker floor and an economy-centered last-stand trigger. |

The v9 win was 744 frames earlier than v5 in these two samples. This is a
timing observation, not evidence of a general win-rate improvement. At frame
9,360 of v11, only two Probes remained while several defenders still had full
health. That trace exposed the pursuit-area and expansion conflicts corrected
in v12.

Final-candidate match results and exact hashes are recorded in
[the validation record](validation.md). Local manifests, pre-cleanup logs,
frozen DLLs, and source patches remain under `build/direct-logs/pass2-*` and
`build/strength-pass2/`. The v7 trace was recovered from an archived runtime
log; its manifest records that provenance.

## Validation and next experiments

The strict verifier covers warnings-as-errors core compilation, x86 adapter
compilation, core scenarios, two log-analyzer tests, six direct-report tests,
seven ladder tests, privacy checks, and whitespace. Release CTest covers the
core, official BWAPI catalog, log analyzer, and direct report.

New regression scenarios exercise cargo-return assignments, depletion and
transfer, stable evidence confidence, enemy construction/morph timing, pursuit
and mineral-line defense, critical recharge, legal economic targets, building
clearance, resource balance, and expansion limits.

State logs now include Core/range readiness, gas worker observations, gathered
resources, and unit ammunition. `tools/direct_report.py` reports first observed
completed Core/Dragoon/range, peak worker count, and mining diagnostics. States
are sampled every 360 frames, so those are observation milestones rather than
exact completion frames. `gasTarget` is the strategic request before the worker
manager's mineral-recovery adjustment; a temporary mismatch is not necessarily
an assignment bug.
The counterattack field records the exact frame when its decision guard first
enabled an advance; it does not prove that every unit immediately moved. Build
diagnostics also remain available for incomplete games without turning their
shutdown records into competitive outcomes.

The next strength gate is repeated, balanced testing of the same frozen DLL
against independent opponents and additional maps. Prioritize melee floods,
ranged pressure, cloak deadlines, and whether both early mining income and
surviving army value improve. Preserve the prior package for comparison. Native
Windows 10 verification and a broad tournament benchmark are still outstanding.

The v13 BananaBrain test was another loss, at frame 12,371. This opening
included worker harassment and an enemy ranged force that outgrew Astra's
army. The scout's limited production observations did not establish a reliable
picture of that transition. The next opening experiments should measure Probe
production gaps and Core/first-Dragoon timing, plus a second safe tech check
before Observers arrive. The v13 UABProtoss win at frame 20,958 does not resolve
this stronger-opponent failure. Small raids also still interrupt the global
attack posture; local defense allocation should be evaluated before changing
that behavior.

The v13 Terran run exposed an additional containment regression: its army and
Dark Templar remained behind the defensive pursuit boundary. The harness
stopped it as incomplete after 900 seconds, with a last sampled frame of
52,200. The v14 covert-advance correction then completed a Terran win at frame
27,747, with a 25.522-ms peak callback and no runtime overruns or caught errors.
The exact final-candidate match matrix is maintained in the validation record.

The v14 Protoss test also completed a win at frame 22,725. Its Reaver had three
Scarabs at frame 15,480 while macro was saving for expansion, providing a live
check of ammunition upkeep. The prior v5 Protoss sample ended at frame 24,368;
the 1,643-frame difference is a single-game timing observation.

The v14 Zerg run then exposed the reserve-army stall: it reached maximum
supply on one base but remained unfinished at the 900-second test limit
(last sampled frame 57,960). V15's reserve-counterattack guard activated at
frame 17,445 in its Zerg test, pressure at home cleared, and expansion resumed.
The frozen v15 binary completed a win at frame 29,018, with a 10.577-ms peak
callback and no recorded runtime overruns or caught errors. This is evidence
that the observed stall was corrected, not that the Zerg matchup is solved.
