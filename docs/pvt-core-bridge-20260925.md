# PvT mobile screen during Core warp-in — 25 September 2026

The quickest reproducible gameplay bottleneck is an idle Gateway while the
Cybernetics Core is already paid for and warping. In all four earlier healthy
UABTerran baseline losses, five or six sampled 120-frame windows had one
mobile army unit, at least 100 minerals, free supply and an idle Gateway while
the Core was incomplete. Two recent Destination traces showed four and six
such windows. In one loss, the Gateway was idle from roughly frame 4,440 to
4,824; the first large bio wave hit near frame 5,870. Its Core had been
ordered at frame 3,901, so a second Zealot during warp-in would not delay
buying the Core. That is a concrete opportunity, not proof the extra unit wins.

`MacroPlanner` now requests one second Zealot only in PvT, frames 3,600–5,999,
while exactly one completed Zealot is the mobile screen and a Core is already
under construction. It requires a free powered Gateway, 100 minerals and
available supply. It does not run during a reinforcement or detection emergency
or duplicate an existing Gateway train order. The normal reservation and BWAPI
command path still owns execution. The candidate has no model control.

The frozen reference DLL is SHA-256
`329671b57423af71b0aac1da3b26318ea52f4746a38464878eca59650ad03ae7`;
the candidate DLL is
`35ff8fd865cc03f2bae90f0c79aa206b1bd23e35dd72d99d2287a7b41d44217a`.
`build/bridge-zealot-20260925/build-receipt.json` pins the changed source
and native core test binary. The Win32 build and core tests pass.

The first UABTerran worker-outcome pilot and a Destination-only retry both
failed their baseline health gate. UABTerran reported `STARCRAFT_CRASH` in
every apparent Protodd win; the opponent's BWAPI stack includes
`std::length_error`. Those wins are excluded and neither intervention ran.
This failure does not establish an effect of the worker treatment.

For a new opponent, a Steamhammer Terran development pilot has frozen
reference and candidate campaigns on Destination, both host sides, actual
seeds 202609270 and 202609271. The same client, map, opponent and read modes
are used. The predeclared screen in
`build/bridge-zealot-20260925/pilot-plan.json` requires two normal, paired
games per condition, observed bridge execution, at least one more mean mobile
unit at frame 6,000, mean workers no worse than two below reference, the first
Dragoon no later than 240 frames after reference, and no fewer wins. Passing
would only justify more diverse paired games. It would not establish a strength
gain or authorize promotion.

The owned supervisor `scripts/continue-bridge-zealot.ps1` completed the
two-pair pilot and wrote `build/bridge-zealot-20260925/pilot-report.json`.
All four games ended normally with observed enemy activity, identical inputs
apart from the candidate DLL, and exact seed/map/host matches. Both versions
lost 0/2 to Steamhammer.

| Metric | Reference | Candidate |
|---|---:|---:|
| Mean mobile army at frame 6,000 | 2 | 3 |
| Mean Probes at frame 6,000 | 20 | 22 |
| First Dragoon frames | 4,747 / 4,789 | 4,744 / 4,717 |
| Final frames | 26,848 / 10,046 | 35,342 / 30,320 |

The candidate issued the intended bridge order in both games. The fixed
functional screen passed, so more development games are justified. Longer
survival and a better early army have not yet produced a win against this
opponent. Two pairs are too small for a strength claim.

A follow-up four-pair comparison completed under
`build/bridge-zealot-generalization-20260925`. It used the same source-pinned
DLLs and Steamhammer Terran, Benzene and Destination, both host sides, and fresh
seeds 202609280–202609283. The report and plan hashes match the on-disk
receipts; each condition produced four normal games, no exclusions, matching
seed/map/host pairs, and no runtime errors. The candidate issued the intended
bridge order in all four games. Neither version won a game.

| Metric | Reference | Candidate |
|---|---:|---:|
| Wins | 0 / 4 | 0 / 4 |
| Mean mobile army at frame 6,000 | 2 | 2.75 |
| Mean Probes at frame 6,000 | 22 | 21 |
| First Dragoon, frames by paired game | 4,894 / 4,750 / 4,687 / 4,861 | 4,726 / 4,741 / 4,735 / 4,726 |
| Final frames by paired game | 28,801 / 39,465 / 21,051 / 30,289 | 28,987 / 15,006 / 36,799 / 34,846 |

By map, Benzene averaged 2 versus 3 mobile units and 22 versus 22 Probes at
frame 6,000, with final frames 24,926 versus 32,893 for reference and
candidate. Destination averaged 2 versus 2.5 mobile units and 22 versus 20
Probes, with final frames 34,877 versus 24,926. Both conditions lost both
games on each map. These survival differences are descriptive, not paired win
improvements.

The predeclared additional-game screen failed: it required a gain of at least
one mean mobile unit at frame 6,000, and the observed gain was 0.75. On the
host-side Destination pair, the candidate had 18 versus 22 Probes and two
versus two mobile units at frame 6,000, then lost at frame 15,006 versus the
reference's 39,465. Both eventually lost, and the paired games do not isolate
which downstream action caused that large difference. The bridge rule was
removed from the working source; the candidate DLL, logs and report remain
archived for diagnosis. There is no case for further strength games of this
candidate or for promotion.

The next higher-value production issue is the repeated pre-contact PvZ worker
stall in the frozen baseline. All four games held at eight Probes from frame
1,200 to at least 3,240 without early own-unit deaths. A first causal trace
found the direct planner reason: `StrategyEngine::planPvZ` caps
`desiredWorkers` at eight until two Zealots or a Cannon complete. The snapshots
show an idle Nexus, 8/8 Probes, and 248–264 minerals at frame 2,400, so the
planner has no unmet Probe goal. The Forge/Cannon prerequisite and reservation
path also affects spending, but it is not needed to explain why Probe training
stops in that interval. Raising the cap needs a source-pinned development
comparison that checks worker growth, first-defender timing, early survival and
wins; the current build has not yet been shown to repeat the same stall.

The late PvT losses show a separate challenge: Steamhammer fielded Vultures,
mines, Siege Tanks and large bio forces and eventually broke our bases. The
early Destination collapse had the Forge still warping when Marines approached;
the planned Cannon was waiting on its prerequisite, and the Nexus was idle with
over 190 minerals during part of the attack. The strategic worker target was
also lowered to 14 while 18 Probes remained, so the idle Nexus cannot be
attributed solely to resource reservation. New defense and late-combat changes
need separate, matched evidence after the production commitment trace.
