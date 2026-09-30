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

## Observer survival and attack conversion — 29 September

The user's turret/Wraith/Science Vessel observation is supported by the
logs. In the earlier long side-1 contain loss, 18 Observers died; 16 last
received `scout-travel`. The new matched Destination reference lost 6 and 13
Observers across the two sides, including 5 and 12 whose last action was
`scout-travel`. The old scout selector sampled only eight route points over a
whole map, merely discounted air risk, and could send an Observer to the
exact center of a known Terran base. The bridge kept issuing that destination
every few frames. It had no Observer escape controller.

A direct Observer rendezvous with the main army is isolated behind
`PROTODD_DIRECT_DETECTOR_RENDEZVOUS`, off by default. Its paired run used
the same seeds, 202609900 and 202609901, and lost both games at frames
24,058 and 22,415, compared with reference losses at 23,035 and 27,871.
There is no win gain and the earlier 26,400-frame favorable detection window
was not reached. Do not promote the option from this screen.

The first Observer-survival build refused known air-threat/detection routes
at 64-pixel samples, withdrew damaged or endangered Observers each combat
cadence, and kept a brief escape lease. Its matched games still lost, but
Observer deaths fell from 6 to 3 and from 13 to zero. The games lasted to
frames 31,312 and 45,076; the latter peaked at 50 mobile army units versus
20 in its reference. These are a control/survival improvement, not a win-rate
improvement. The live trace also showed escort orders fighting escape orders,
and all three remaining Observer deaths occurred while evading Wraith or
Science Vessel pressure near map edges.

The stricter combined follow-up staged Terran scouting at a perimeter,
cancelled an in-flight unsafe mission, changed map-edge escape, and excluded
endangered Observers from escort assignment. Its paired campaign
`build/pvt-detector-rendezvous-screen-20260928/observer-safety-v3` still
lost both games at frames 32,025 and 28,336, with one Observer death each.
The side-1 economy and army collapsed much earlier than in the first safety
build. That bundle was rejected; source again uses the first safety behavior.
Its individual changes need isolation before any retry.

Keeping the Observers alive exposed the next conversion failure more clearly:
in the survival-only side-1 loss, a 31-unit main army at frame 28,080 had a
very favorable local estimate but remained at its rally under a Hold
posture. The audit counted 277 favorable main-army rally samples and 49
favorable detection-blocked samples, all sampled decisions rather than
independent missed wins. The next paired experiment uses the exact first
safety DLL and disables only its empty frozen policy through the package
setting. Judge it by normal wins, Probe/Nexus survival, attack routes, and
damage to Terran bases, not just the selected posture.

That policy-off experiment lost both matched games at frames 38,659 and
44,456. It removed the empty-policy Hold and sent a 36-unit main army into
combat on side 1, but did not convert the attack to a win. On side 0, the
main force traveled toward a distant target while tanks destroyed both
Nexuses. Observer losses were 2 and 8, compared with 3 and 0 for safety
alone; all ten last had `observer-evade` orders. Several died at the map
boundary, including three at y=16 near the end of side 1. Disabling the
policy globally is not promotable. Isolate boundary escape and keep the
near tank line plus Nexus defense in the same attack decision.

An isolated boundary candidate that forced an inboard step toward home was
stopped before a valid game result: on the recorded side-1 loss, Wraiths were
between several cornered Observers and home, so that step could move into
their weapons. The candidate was reverted. The next escape change must
consider enemy direction and friendly anti-air cover, and must be tested
against the preserved safety build on both starting sides.

## Local contain clearance screen — 29 September

Two opt-in PvT variants tried targeting tanks and mines near an owned base
before a distant Command Center. The first released too early: at frame
18,264 it saw 26 total fighters, but 22 were allocated to base defense and
only four formed the main army. Later an 18-unit main force pushed into the
mine and tank line while combat reported missing mobile detection; it fell
to two units by frame 23,520. The two games lost normally at frames 31,064
and 44,332, with no win gain against the saved Observer-safety build.

The second variant required 30 connected fighters, a detector inside the
combat coverage radius, and a quieter perimeter. It still inherited the
helper's outer-depot fallback and fired 29 times on side 0, often targeting
a distant Command Center. It lost normally at frames 39,000 and 31,343;
side 1 lost the natural by frame 17,380 before the stricter gate activated.
The source reverted both variants and the breakout flag stays off. This
family is closed until the bot can retain a defensible natural and measure
the field squad separately from fighters allocated to base defense.

At frame 15,000 in the second variant's side-1 loss, the natural had 13
defenders against 16 local enemies including several tanks, while three
fighters remained in the main squad. The bot held 591 minerals and 479 gas,
but a blocking third-Nexus goal was reserving minerals even as the siege
approached from roughly 800–900 pixels away. The exposed-economy check only
considered visible attackers within 640 pixels of an owned base. The next
experiment should cancel that distant expansion bank when a substantial
observed mech group approaches the natural, then measure reinforcements,
natural survival and normal wins.

The mech-bank release screen did cancel planned expansion saving when a
recent tank/vulture/goliath group approached an owned base. Both games still
lost normally, at frames 39,744 and 30,196. On side 0 the bot held two
Nexuses, 44 Probes and 33 fighters at frame 25,200, but lost a Nexus at
29,856. On side 1 the natural fell before the proposed third-base bank
decision mattered. The change was reverted; the result does not establish
that the bank release improved a matched win outcome.

A separate pending-natural defense candidate treated an unfinished Nexus at
a neutral map marker as a defended base, and consolidated at least three
quarters of available mobile fighters there when three attackers breached
it. Its native scenario passed. In two normal Steamhammer games it still lost
at frames 42,131 and 53,105. Side 0 retained two Nexuses until frame 35,202,
substantially later than the saved Observer-safety run's first loss at
21,409, but side 1 ultimately lost its natural at frame 44,776 and ended
with one Nexus. The side-1 opponent path did not repeat the earlier
frame-9,240 unfinished-natural breach, so the live run did not isolate the
new behavior. The code was reverted, and the frozen campaign remains at
`build/pvt-detector-rendezvous-screen-20260928/pending-natural-defense`.

## Observer threat-field screen — 29 September

The pending-natural side-1 trace still lost five Observers by frame 45,490.
The last orders were `observer-evade`, including deaths at x=16 on the map
edge; nearby threats included Wraiths, Science Vessels and Missile Turrets.
The new isolated Observer candidate cancels an in-flight scouting order as
soon as a newly revealed turret blocks its route, refuses escort assignment
when the current Observer or escort anchor is exposed, begins withdrawal
earlier around Wraiths, and scores escape steps against all nearby air and
detection threats plus the map boundary. Native tests cover route
cancellation, combined Vessel/Wraith pressure, boundary retreat, and unsafe
escort anchors. Its two-side campaign is
`build/pvt-detector-rendezvous-screen-20260928/observer-threat-field`.
That first isolated campaign lost both normal games at frames 39,620 and
27,561. Observer deaths were 3 and 1, versus 3 and 0 in the saved safety
reference. The side-1 regression rules out promotion. Its game-0 trace
contained 2,172 accepted `observer-evade` moves; some frames issued both an
escort move and an evade move to the same Observer. On side 1 an escape
lease ended far from home, then `scout-travel` sent the same Observer back
across enemy territory before it died at the southern edge.

The revised candidate keeps an escape waypoint while it remains safer than
the alternatives, avoids repeating an active move command, excludes a leased
evader from escort control, and keeps a withdrawal lease until it reaches a
friendly Nexus. Tests cover the waypoint and homebound lease. The frozen
revised campaign is
`build/pvt-detector-rendezvous-screen-20260928/observer-threat-field-v5`.

That screen finished with two normal losses at frames 43,774 and 59,119.
Longer survival did not fix the two-base starvation. In side 1 the bot had
45 Probes, 23 units in its formed MainArmy, and 2,533 banked minerals at
frame 24,000; only 3,596 minerals remained across its two owned bases.
The planned third at (2816,240) never started despite repeated Nexus
orders. By frame 30,000 both owned mineral fields were empty and it still
had only two Nexuses. At frame 48,960, 31 MainArmy units with a local
ratio of 27.77 were ordered back to rally. Their nearby Wraith opponents
were visible, detected, and uncloaked, yet the squad required an Observer
because `detectionThreat` classified every Wraith as an invisible unit.
The Nexus build-lease diagnostics show a repeated route failure: from
frames 15,619 through 36,145, each of 20 Probe leases expired after
45 seconds while its builder remained roughly 500–1,000 pixels short of
the (2752,192) footprint. BWAPI still reported the footprint buildable
and a path available. Reissuing the command without clearing that corridor
did not produce a Nexus.

The next candidate removes that unconditional Wraith/Ghost classification.
Actual cloak, burrow, lack of detection, mines, Lurkers, and Dark Templar
still gate advancement. It also commits a formed PvT field army of at
least 20 units to a nearby neutral third when exactly two Nexuses remain,
owned mineral fields have at most 5,000 minerals, and the strategic posture
is Hold or Pressure. Base-defense squads retain their own assignments;
the field mission uses the local third's assembly point rather than a
distant known Command Center. Native tests cover uncloaked/cloaked Wraiths,
the exhausted economy trigger, the detached-small-force rejection,
healthy mineral lines, and enemy-owned sites. The paired live screen is
`build/pvt-detector-rendezvous-screen-20260928/economic-window-v1`.
Promotion requires actual third-base construction and improved normal
wins; local tests and an army move alone do not establish either.
The pair lost normally at frames 41,108 and 25,701, with no third Nexus.
On side 0 the 24-unit field squad did move toward the third at frame 23,400,
but the natural approach faced a larger mech wave 600 frames later and
the force was reallocated to defense. Side 1 lost its natural at frame
16,222, before the economic-window condition could act. This candidate
is not promoted. The differing early opponent pressure also limits what
the two-game outcome can attribute to this change.

The next isolated revision guides a Nexus Probe toward a terrain waypoint
after six seconds without getting materially closer to its reserved site.
It retains the same build tile and retries the Nexus when the Probe reaches
the footprint. Ground-threat checks prevent the guide from routing a
builder through a currently dangerous short segment. The core and Win32
builds pass; the two-side screen is
`build/pvt-detector-rendezvous-screen-20260928/economic-window-builder-route-v2`.
Its first side lost at frame 27,747 with two Nexuses at peak and 38
accepted route-guide moves. The guide changed a leased Probe's order from
PlaceBuilding to Move, but successive fresh A* requests sent it between
positions near x1,200–1,300 rather than toward the x192 third-base tile.
That is direct evidence of waypoint oscillation. A further route candidate
keeps the full path in the pending build lease and advances its cursor only
after the Probe reaches each waypoint; it replans only if progress toward
that waypoint stalls. The frozen v2 finished with two normal losses at
frames 27,747 and 25,515 and no
third Nexus. Its first side accepted 38 guide moves but never moved the
builder through the choke. On side 1 both Nexuses were lost under early
mech pressure before the late economic fix could matter. The cached-path
revision is frozen in
`build/pvt-detector-rendezvous-screen-20260928/economic-window-cached-route-v3`
for a two-side screen; route completion and actual Nexus start are required
before making any strength claim.
Its first side lost normally at frame 35,590 with no third Nexus. The
cached guide issued 57 accepted moves. Several guided Probe leases ended
as `order-lost` because lease ownership recognized the original Build
order but not a guided Move; this is fixed in the next build. That same
trace still shows repeated leases ending hundreds of pixels short of the
third. The next candidate also records repeated failed Nexus footprints
and, after two failures with two completed bases, switches to another
neutral mineral site outside recently observed ground threats. A test
ensures replanning does not cancel a Probe already leased to the new site.
The cached-route pair finished with two normal losses at frames 35,590
and 39,589 and no third Nexus. The alternate-site plus lease-ownership
revision is frozen as
`build/pvt-detector-rendezvous-screen-20260928/alternate-third-site-v4`
for another two-side screen. Its decisive checks are a logged change of
expansion target, a Probe reaching that alternate footprint, construction
beginning before mineral exhaustion, and normal game outcomes.
The v4 pair lost normally at frames 25,980 and 27,592; neither built a
third. Side 0 did trigger alternate-site selection after the original
route failed twice, but Terran siege destroyed its natural at frame
16,915. The site switch had no demonstrated economic or win benefit.
The economic-window push and builder-routing/alternate-site changes were
removed from the default code after these negative screens. Their frozen
DLLs and logs remain under the campaign directory for analysis.

The next isolated combat candidate retains the Observer-control work and
the visible-Wraith detection fix. A formed PvT MainArmy of at least 20
units may clear a currently visible, targetable Terran combat unit within
960 pixels of the squad and 1,100 pixels of an owned base when its local
combat estimate accepts the fight at ratio at least 1.8. It never borrows
base-defense units and still waits for detection if a real cloaked or mine
threat blocks the advance. This is narrower than the failed distant-base
or third-site pushes. The Win32 build and native regression pass; the
paired screen is `build/pvt-detector-rendezvous-screen-20260928/favorable-front-v1`.
It finished with two normal losses at frames 30,227 and 27,282. Neither
game formed a 20-unit MainArmy at a favorable visible front before losing
its natural, so the rule did not activate. The outcome cannot establish
whether it would resolve the saved frame-48,960 stall. A second bounded
version allows a 16-unit actual field squad and a Defend posture after a
natural falls, but still requires ratio at least 1.8, a detected visible
target near an owned base, and at least eight field members that can hit
that target. Native tests cover those gates. Its paired screen is
`build/pvt-detector-rendezvous-screen-20260928/favorable-front-v2`.

That v2 screen completed with two normal losses at frames 31,653 and 42,441.
It exercised the favorable-front rule on side 1 but established no win gain.
The existing source was preserved for a fresh reference before changing the
defense allocator. The 29 September correction addresses unavailable enemy
health and out-of-range static-defense credit; its tests and frozen comparison
are recorded in `docs/defense-allocation-20260929.md`.
The fresh allocation screen finished 0/2 for both arms. The candidate lost at
frames 30,165 and 22,756, versus 27,282 and 43,929 for its exact pre-fix source
reference. All four results were normal, healthy and paired on actual seeds and
starting locations; no third Nexus formed. The static-credit variant remains
available behind an off-by-default evaluation switch. Only the unknown-health
consistency correction remains in default allocation, without a live win claim.
