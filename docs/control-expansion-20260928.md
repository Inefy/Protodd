# Broader control improvement cycle — 28 September 2026

## What the completed games show

The prior small PvT tactic screens did not establish a win-rate improvement.
`tools/control_outcome_audit.py` now joins full-game economic and army losses
to sampled main-army routes and accepted orders. It is diagnostic: the routes
co-occur with losses and do not by themselves prove causation.

In the four-game frozen `pvt-controlled-mech-screen-20260927/reference`, all
games lost normally. Destination game 1 provides a high-impact control case.
Between frames 27,840 and 28,560, Protodd fell from 34 to five Probes while
its army held at 39 units. The main army had 31 sampled `cover-expansion`
routes in that interval and six detection-blocked samples. Its natural Nexus
had already fallen; the strategy selected that empty former natural as a new
expansion target while the surviving main economy was attacked. This is a
reason to test command routing and defensive allocation together, rather than
raise another isolated unit-production priority.

The four-game `current-strength-pvp-pvz-20260927` reference lost all games.
Substantial PvP/PvZ Probe or Nexus losses occurred with few or no main-army
samples in several games. Those require opening survival and early defense
work, not a late-game expansion guard. Earlier accepted-order traces suggest
route execution and terrain remain a separate control problem; the stricter
continuous 720-frame travel audit found no qualified stall in this frozen
campaign.

## Current isolated control experiment

`PROTODD_BASE_THREAT_EXPANSION_GUARD` is off by default. When an army would
cover an expansion, the guarded build keeps its normal rally/defense mission
if a surviving base has one attacker inside 320 pixels or at least two
recently seen ground-capable attackers within 800 pixels. The event is logged
as `base-threat-expansion-guard`. The guard does not cancel the economic
building plan; it changes only the main-army cover mission. A native test
checks nearby attackers, stale sightings and the breach case.

The guarded DLL completed four normal Steamhammer Vultures development games:
one win and three losses. The original run reported games 0–2; its game 3 was
interrupted before a result pair. `candidate-remainder` re-ran only game ID 3
in a separate verified package, preserving side, map and seed 202609903.
All four games have both normal reports and archived Protodd logs.

The sole win logged **zero** guard activations, so it cannot be credited to
this rule. Game 2 also had zero activations and lost. Games 1 and 3 lost with
52 and 15 sampled activations, respectively. In game 1 the natural fell at
frame 21,240 and Probes later reached zero. The rule is **rejected for default
control**. A fresh current-default four-game reference was prepared with
identical non-DLL inputs but was not launched because the candidate failed its
mechanism check. The older September 27 reference diagnoses failures; it
cannot isolate this intervention because its DLL predates other changes.

The same second-game trace showed a later 720-frame window with 22 Probe
losses and only one worker with an accepted `worker-evacuate` action. That
motivated a **separate opt-in** `PROTODD_ABANDONED_BASE_WORKER_EVACUATION`
experiment. When an unowned resource site still contains a worker and has a
recent ground attacker, while an owned base is farther than 640 pixels and
unthreatened, the worker receives a high-priority safe-step evacuation toward
the owned mineral line. A native test covers live threat, stale sighting,
valid escape and default-off behavior. The two-side Destination reference and
candidate differed only in Protodd's DLL and both completed with normal
reports. The reference lost at frames 35,652 and 38,535; the candidate lost
at frames 23,965 and 29,514. The new worker path issued evacuation orders
near the threatened natural in candidate game 0 before that Nexus fell at
frame 8,703. Later large Probe-loss windows still had no evacuation orders.
The candidate is **rejected for default control**. The small paired screen and
different early trajectories cannot measure a precise causal effect, but
they provide no strength gain or worker-survival basis for promotion.

The next control test should address the earlier command handoff: a main army
with a favorable fight estimate can be held by the detection gate while a
visible enemy breaks an owned base. A bounded base-breach assist must take
ownership of that defensive objective, select only visible legal targets,
then return to detector-aware travel when the breach ends. Measure whether
the issued orders reach attackers and preserve workers, not just whether the
gate changed state.

## Emergency defense consolidation screen

`PROTODD_BASE_DEFENSE_CONSOLIDATION` is off by default. With six or more
nearby threats, at least two visible ground attackers and eight available
mobile fighters, base defense claims at least 75% of available mobile
fighters before the main army forms. A native test checks a major attack and
a lone raider. The two-side Destination screen against Steamhammer Vultures
used identical schedules and frozen files except for Protodd's DLL. Both
arms had complete normal reports and archived logs. The reference lost at
frames 35,807 and 23,872; the candidate lost at 37,326 and 45,386. The
candidate activated the rule 89 and 78 logged times. These are throttled
event records, not all decision ticks.

On side 0, the candidate's natural survived from the reference's frame
22,560 loss until about frame 28,200. In sampled large threats with at least
12 combined units in base-defense and main squads, the base-defense share
rose from 43.5% to 74.0%. The candidate nevertheless lost its mobile army
before its worker line fell. On side 1, two Nexuses and 42 Probes were still
active at frame 20,640, after the reference had recorded two Nexus-count
drops by frame 16,320.
The candidate later lost 35 Probes in one 720-frame window while its main
army routed toward an attack objective. This candidate demonstrates a longer
survival mechanism but **no win-rate gain** in the paired screen. It remains
off by default. Squad-unit shares include static defenders and reflect the
sampled states; they do not measure mobile-unit travel or damage by themselves.

The side-1 collapse exposed a separate economy-control bug: after both owned
mineral sites reported zero remaining resources, numerous Probes repeatedly
accepted gather orders for one patch near an enemy-held base. The BWAPI bridge
fell back to the nearest mineral anywhere on the map whenever the owned-base
allocator had no local patch. Default control now excludes depleted owned
bases from mining assignments, recalls stranded workers toward a surviving
owned base, and removes the unrestricted bridge fallback. A native regression
checks transfer to a live owned site and recall when all owned sites are
depleted. This is an execution correction, not evidence of a win-rate gain.
The first default-control DLL completed two normal losses at frames 43,619
and 24,492, versus 35,807 and 23,872 in the earlier default screen. It
issued thousands of recall moves late in game 0, so the current source also
keeps an active recall move when the next safe step is nearby. That refinement
builds for Win32 but was not part of the two-game default screen. A one-game
diagnostic under `build/consolidated-depleted-minerals-screen-20260928`
tested it with emergency defense enabled against the prior side-1 trace. The
candidate lost normally at frame 40,736 versus the prior 45,386. It removed
426 accepted late worker-gather orders to an enemy-held mineral patch, but
still lost its mobile army and then its workers. The worker-routing correction
is a legality and safety fix; it is not a measured win gain.

An optional `PROTODD_SAFE_REMOTE_MINING` rule then leased up to eight workers
to a neutral planned expansion after owned minerals depleted, only when no
ground threat was observed at that site. The focused same-seed game logged
29 activations and lost normally at frame 39,217, earlier than the prior
40,736. Its gathered minerals increased only 176 between frames 28,800 and
31,200, then stopped while the army collapsed. The rule remains off. This
episode shows that late emergency remote mining cannot compensate for an army
that never secures a new income site.

## PvT contain and attack conversion

The Destination side-1 trace shows a 16-unit main group at frame 24,000 with
a combat estimate of 8.13 against six nearby enemies, yet its route was
`assemble-at-rally` and it was marked `Wait for mobile detection`. The
matchup strategy requested Pressure, but the frozen policy had no learned
weights and changed that request to Hold. At frame 28,800 the army did move
forward, but its attack target was the distant Terran main at `2112x3824`
while remembered Command Centers existed at `288x1808`, `2784x3120`, and
`2784x2288`. In that game, the army declined from 34 at frame 31,200 to
19 by frame 33,600; Terran's bases continued to multiply. Unit counts and
local estimates are observations, not proof that an earlier attack wins.
The sampled audit found 212 empty-policy Pressure-to-Hold overrides, 39
main-army frames with at least 16 units and a favorable local fight routed
to rally, and 56 favorable frames blocked on mobile detection. These counts
are correlated decision samples, not independent missed wins.

`PROTODD_PVT_CONTAIN_BREAK` is an opt-in test of a narrower decision. It
requires a connected ground force of at least 16, an Observer within 512
pixels, two owned bases, no current worker attack or combat unit inside a
320-pixel base perimeter, and a 1.6 combat-value lead over recently seen
opposition near the force. It chooses a known Terran resource depot by
distance from that force plus nearby defense cost, including unfinished
Command Centers that can still be denied. When the empty frozen
policy requests Hold over matchup Pressure, the rule preserves the Pressure
posture only while these conditions hold. Local combat evaluation and mobile
detection still govern issued orders. The rule is off by default and requires
same-seed live evidence before promotion.
It also requires a known outer depot; the lone remembered Terran main is not
a breakout destination. The strategic director keeps authority over a
current emergency before the field group receives a breakout mission.

The first side-1 diagnostic lost normally at frame 23,128 with zero breakout
activations. Its opening had already diverged from the earlier comparison:
at frame 19,200 it had one Nexus and 24 Probes, versus two Nexuses and 44
Probes in the prior trace. It therefore supplies no evidence about the
breakout itself. A revised diagnostic includes unfinished outer Command
Centers, which the original target filter incorrectly excluded.

That second diagnostic reached the late game. It lost normally at frame
37,791 after 70 throttled breakout events and 73 sampled strategy decisions
with the rule active. The attack target changed to a nearby known Command
Center at `288x944`. A 20-unit main group routed there at frame 26,400,
but the target's health stayed at 1,500 through frame 36,000. Terran grew
from three known Command Centers at frame 24,000 to five at frame 33,600.
At frame 22,560 the base-defense allocator held 18 units against six nearby
enemies while the main group had five; by frame 22,920 it held 20 units and
the main had six. The rule changed the intent but did not give the connected
army ownership of the attack. Its 84 favorable but detection-blocked main
samples were also higher than the earlier diagnostic's 56. This variant
establishes a target-routing effect, not a win or economic damage gain.

The next opt-in candidate sets `breakContainment` only while the same
detector-covered opportunity is active. Squad formation releases the field
force from an outer base-defense claim only when at least 60% of mobile power
is local and exceeds the nearby Terran threat by 1.6 times; any enemy inside
the 320-pixel economy perimeter vetoes the opportunity. A native test checks
weak and strong outer contains. Its first side-1 game lost normally at frame
24,213 with zero activations after the natural fell early. It does not test
the release mechanism. The side-0 diagnostic also lost normally at frame
33,916 with zero activations. It briefly had 23 mobile units after losing
its natural, but the rule requires two owned bases. At frame 19,560, a
21-unit main group with a favorable local estimate was still routed to
`cover-expansion` while the remaining Nexus was under nearby Terran pressure;
the base-defense squad had six units and an unfavorable estimate. This is a
separate command-ownership problem. Four one-game contain-break diagnostics
have no win gain: three never exposed the rule, and the exposed target-only
variant never damaged the chosen Command Center. The guarded release remains
off by default and needs a matched exposure before any promotion. After these
screens, the source also requires a known outer depot and lets the strategic
director resolve emergencies before committing `breakContainment`; that
safety refinement has native coverage but no live outcome yet.

The next control experiment should treat the visible contain and the last
mineral line as one operation: keep detection with the connected force, clear
the near tank/mine line while locally favored, then commit to an outer economy
only after a safe route exists. If the last Nexus is threatened, attacking a
distant depot cannot outrank its defense. Measure actual damage to the
contain, Command Centers, own army, and Probes across both starting sides.

## Expanded improvement loop

1. **Opening survival:** At the first threat, measure worker escape, defense
   force location, first Cannon/Observer readiness and whether the local army
   actually fires. Develop PvP/PvZ opening fixes from those episodes.
2. **Economy under attack:** Track Nexus and Probe losses while 12+ fighters
   remain, including the chosen army objective, orders accepted, and distance
   to each surviving base. Test defense allocation and objective ownership as
   one coherent response when a base is breached.
3. **Army execution:** Detect accepted assault orders that make little travel
   progress, repeated detector waits, retreat loops and volleys interrupted
   before firing. Judge control by movement and damage in the game.
4. **Replay learning:** Widen beyond the small target ranker to strategic
   movement, engagement and expansion decisions. Align legal replay
   candidates and runtime command ownership before fitting. Evaluate on
   disjoint training-development games, then paired live games; offline
   imitation accuracy alone is insufficient.
5. **Promotion:** Preserve the known default until a candidate improves
   healthy matched development games across sides and maps, then broaden
   opponents. Report win rate alongside economy survival and command
   consequences so a favorable short screen cannot hide a new failure.
6. **Resource exhaustion and conversion:** Track remaining minerals at every
   owned site, worker target site ownership, mobile army survival and damage
   after the first defended attack. A defended but mined-out economy must
   choose a safe expansion or decisive attack before its income disappears.
