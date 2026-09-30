# UAlbertaBot diagnostic - 29 September 2026

The committed default bot was tested without gameplay changes. This is a small
development diagnostic, not a promotion gate or an estimate of tournament strength.

## Frozen Inputs

- Source commit: `f296418` (`Refine Observer safety and correct defense allocation`).
- Protodd DLL SHA-256:
  `06CB9BB992227073D74B8F39DAA2F8ED995392B1C1135D7E73F9EFB404B87EF9`.
- Local UABTerran, UABZerg and UABProtoss DLL packages, registered as DLL opponents,
  not their optional proxy launchers. All three packages contain the same universal
  UAlbertaBot DLL, SHA-256
  `A99DF20B634298D746114673596A1CE4D6A152D148CD086F8FA67BC803598183`.
  The manager sets the requested race. The installed configuration selects
  `Terran_MarineRush`, `Zerg_ZerglingRush` and `Protoss_ZealotRush` respectively.
- Frozen opening/policy modes, learned macro and production-demand control off,
  empty initial opponent read directories, and manager bot-file learning disabled.
  The policy has no loaded weights; its fallback can still affect posture.
- Campaign component hashes, actual seeds, maps and starting Nexus coordinates
  are preserved under `build/uab-diagnostic-20260929/`.

The initial `baseline` schedule requested twelve games across Benzene and
Destination. Its first Terran game reached the full 86,400-frame limit. To keep
the diagnostic bounded, its following Benzene Zerg game was intentionally
interrupted; its pre-cleanup trace is archived in
`baseline/interrupted-game-1-011747/`. Neither that partial game nor the unplayed
schedule entries has an outcome label. An earlier snapshot is also retained.

A separate, verified `bounded` campaign then completed six Destination games,
one per race on each host side, with a 28,800-frame limit. All six ended before
that limit. Actual seeds are 202609900-202609905. This is not an A/B comparison:
the bot DLL never changed, and host reversal also uses a different seed.
Host-side balance does not imply spawn balance. Five Destination games started
Protodd at (2112,3824); only game 2 started it at (1056,272).

## Results

| Campaign/Game | Opponent | Protodd Host | Protodd Final Frame | Classification | Peak Army / Probes |
| --- | --- | --- | --- | --- | --- |
| baseline/0, Benzene | UABTerran | Yes | 86,402 | In-game cap, no decisive label | 37 / 30 |
| bounded/0, Destination | UABTerran | Yes | 20,276 | Excluded: opponent StarCraft crash | 30 / 44 |
| bounded/1, Destination | UABZerg | Yes | 23,066 | Excluded: opponent StarCraft crash | 42 / 44 |
| bounded/2, Destination | UABProtoss | Yes | 8,558 | Clean loss | 2 / 17 |
| bounded/3, Destination | UABTerran | No | 12,092 | Clean loss | 4 / 17 |
| bounded/4, Destination | UABZerg | No | 22,694 | Clean win | 32 / 44 |
| bounded/5, Destination | UABProtoss | No | 9,488 | Clean loss | 1 / 18 |

The four healthy, consistent pairs yield **one win and three losses**. Two
apparent wins are excluded because the other report records `STARCRAFT_CRASH`.
Those opponents were active before exit: their scores were 23,008 and 16,228,
with substantial armies visible in Protodd's trace. That activity does not make
their terminal result healthy. Both crashes occurred with UAB away; this is a
runner/opponent diagnostic clue, not an established cause. No crash log was
archived that establishes the underlying fault.

All four accepted pairs have two normal reports, opposite win flags, matching
maps and final-frame differences of 31 frames. Their opponents visibly fielded
armies and scored 10,801-14,533. Neither side crossed a configured runtime limit.
Protodd had one frame above 55 ms in game 5, below the 320-frame allowance, and
its six completed traces report zero caught or logging errors. This is playing
strength evidence for these starts, not an integration-only set of idle opponents.

The generated analyses are `baseline/diagnostic-report.json` and
`bounded/diagnostic-report.json`. They include result pairs, trace hashes,
construction lifecycle milestones, sampled state, control decisions and loss
windows. `training.arena verify` passes for both frozen campaigns. All owned
StarCraft games and Java runners were stopped after archiving; no learning
snapshot or gameplay code was changed.

## Improvement Priorities

### 1. Finish The Opening Defense Before Contact

Both clean PvP losses failed before any useful army formed. In game 2 the first
Gateway completed at frame 2,972, but the first Zealot did not begin until 3,619
and completed at 4,220. The scouted two-Gateway response was already active at
frame 3,600, with 244 minerals; the queue reserved the second Gateway at priority
115 before the mobile response at 114. The first Cannon began at 4,977, after the
first sampled base breach at 4,968, and no Cannon completed. Neither a Core nor
a Dragoon completed. The worker count fell from 17 to zero between sampled
frames 5,400 and 6,840.

Game 5 used an early ranged path instead, but still peaked at only one army unit.
The Core began at 3,432 before the first Zealot began at 3,647. The first Dragoon
completed at 5,162, after the sampled breach at 4,872. Its first Cannon did not
begin until 6,528 and never completed. Thus neither mobile-first nor ranged-first
planning met the survival deadline on these starts.

The clean Terran loss likewise breached at sampled frame 5,640. Its first
Dragoon and first completed Cannon arrived at 6,674 and 6,691, and the army peaked
at four. Funding extra production, prerequisites, batteries and an emergency
Cannon in competing passes did not produce a timely defensive screen.

Next test a narrow opening reservation: protect the first actual combat-unit
cycle before another optional structure, and schedule any necessary static
anchor using its full construction lead time. Regressions should exercise
`MacroPlanner::reconcile` with the real simultaneous goals and bank, not just
assert that a Zealot/Cannon goal exists. Require completed defenders before the
observed breach, fewer early Probe losses, and healthy paired outcomes. Preserve
the successful Zerg static opening as a guard against collateral regressions.

### 2. Prevent The Mined-Out Recovery Deadlock

The long Benzene Terran game completed its natural at frame 12,856 and lost it
at 16,151. Later it had 37 army units at sampled frame 27,120, then lost nine by
27,720 and seventeen more by 28,440. Those windows include expansion-cover,
focus-fire and retreat orders; the trace does not establish that one command
source caused the losses. There was no mobile-detection block in this game.

More fundamentally, mined minerals remained at **13,750 from sampled frame
25,680 to the cap**, while 25 Probes survived. The remaining main had zero
minerals. The late plan requested one base and lacked an expansion target;
Gateways waited for defensive reinforcements that could no longer be funded.
The PvT expansion-ready rule depends on surviving Dragoons and its base cap can
conflict with generic depleted-base recovery after the army is lost.

The interrupted Benzene Zerg trace shows another version of the same problem:
at frame 32,520 it had 68 logged army units, 22 Probes, one mined-out Nexus and
only 44 minerals. It requested three bases and named a natural, but no additional
Nexus ever began. The operation remained "Assemble beside expansion; fund
construction" while priority-112 reinforcement reservations continued.

Test a protected, feasible expansion budget *before* depletion, with reservation
policy accounting for actual defensive strength and the existing base cap. A
successful test needs a completed, income-producing Nexus, not another army move
or accepted builder command. Do not simply re-enable the earlier rejected remote
mining and builder-routing variants. Once the bank is below 400 and every owned
patch is empty, a Nexus priority increase alone cannot generate income.

### 3. Convert Surplus And Field Strength Into Useful Actions

The clean Zerg win established its natural at frame 14,686, reached 44 Probes and
32 army units, and won at 22,694. It still accumulated **5,744 minerals** and
logged 481 high-bank samples out of 947. These are samples, not independent
incidents or a measured number of lost seconds. Inspect the producer queues,
gas/prerequisite reservations and supply deadlines before adding more buildings.

The frozen-empty policy also demoted aggressive strategic postures to Hold in
the longer traces. The clean Zerg win contains 44 sampled Harass-to-Hold and ten
Attack-to-Hold proposals. The interrupted Zerg trace at frame 32,496 explicitly
shows `strategyPosture=Harass`, `policyAction=3`, `policyWeights=0` and Hold.
Current nearby enemies can justify defense, so these counts do not prove that
every hold was wrong. A targeted policy-off comparison can isolate this effect;
it must preserve real breach safety and use the exact same opponent/seed inputs.

First address the reproducible PvP/PvT opening failure. Then evaluate economic
funding and policy/army control separately, retaining negative results. This set
does not validate any new implementation or demonstrate dominance over UAB.
