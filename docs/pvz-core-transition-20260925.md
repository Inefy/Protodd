# PvZ Core transition development screen — 25 September 2026

The prior ten-Probe cap and high-priority extra-Zealot rules did not improve
paired wins against McRaveZ and were retired. In the four reference games from
the mobile-screen pilot, frame 8,400 had two or three Gateways, none busy, five
or six Zealots, and no completed Cybernetics Core. Two bases were already
started in two games. One game had a 400-mineral Nexus reservation while only
114 unreserved minerals remained; another reserved 250 for a Pylon and Cannon.
The Core costs 200 and the written PvZ goal has priority 82, below the
expansion's 95. Its later completion leaves no ranged unit when the Zerg
attack grows. The log's goal summary prints only the top 12 goals, so a Core
goal omitted from that summary is not evidence that the rule failed to ask
for it. The combination of early economic commitments and late ranged access
is the concrete hypothesis for this pilot. Placement failures remain a
separate possible bottleneck.

The archived reference traces also limit the hypothesis: in pair 1 the first
Core command was issued at frame 7,708, but the building did not appear until
10,303 and completed at 11,274. The command-to-placement delay is 2,595
frames, so raising priority may be insufficient even if it advances the
initial request. Across the four reference losses, pre-12,000
`build-no-location-placement-search` events numbered 10, 33, 19 and 35,
mostly for optional Cannons, with four Core placement deferrals in pair 3.
The live screen must therefore measure **completed** Core timing and survival,
not just whether an earlier command was issued.

In that earlier pair, the accepted Core build at frame 7,708 was followed by
`builder-release` Stop commands at frames 7,861 and 7,929, then new Core
orders; construction began only at 10,303. `BwapiBridge.cpp` releases a
pending non-Nexus builder after 48 frames without position progress and
blacklists that footprint for 480 frames. This is a plausible explanation
for the repeated attempts, but the trace does not establish why the Probe
stopped moving. In the new pilot, Core issue-to-creation delays were only
64–244 frames in reference and 17–178 in candidate, so the large lag is
intermittent. Inspecting builder position, target and order at release is a
better next diagnostic than globally raising Core priority again.

The single candidate raises the first Core build priority to 96 only from
minute five onward, once one Cannon, four Zealots, and two Gateways are
completed, no Core exists, and there is no direct ground breach. First
defender priorities remain 97–100. It changes only `src/core/Strategy.cpp`;
the archived reference DLL was built from the same source manifest without
this rule. The native scenario verifies entry and exit under missing static
safety or a direct breach. It passed before the campaign was prepared.

`build/pvz-core-transition-20260925/pilot-plan.json` freezes four paired
development games per condition versus McRaveZ on Benzene and Destination,
both host sides, seeds 202609310–202609313. The reference and candidate arena
inputs match except for the DLL. The source manifests, DLLs, client bundle,
review script and supervisor are archived beside the plan. This is a
development screen, not strength evidence or a promotion request.

The frozen gate requires healthy exact pairs and visible intervention in at
least two candidate games, with none in reference; all Core completions that
occurred in reference must still occur in candidate; at least two comparable
Core timings must improve by 480 frames and none regress over 240; first
Zealot-or-Cannon completion may delay at most 120 frames; mean frame-8,400
army and Probe deltas must be at least -1 and -2; there may be no extra own
losses before frame 6,000 or game duration regression over 2,400 frames; and
the candidate must win at least one more of the four games. Passing would
justify diverse paired games only. Failed whole-game models remain gated and
no tournament control is enabled.

## Paired result

The owned supervisor completed both four-game campaigns. The plan, source,
DLL, client, review, supervisor and arena-manifest hashes matched their
receipts. All eight games had normal runtime health and identical paired
inputs apart from the DLL: seeds 202609310–202609313, Benzene/Destination,
both host sides. The conditional Core priority appeared 6, 1, 6 and 5 times
in candidate snapshots and never in reference.

The candidate **failed the frozen screen**. Completed Core timings advanced
by 2,225, 2,216, 1,395 and 747 frames, and first defender timings stayed
within the 120-frame allowance. Frame-8,400 army differences were 0, +1, -1
and 0; Probe differences were +2, -4, 0 and 0. But it added one own-unit loss
before frame 6,000 in each of pairs 0 and 1, and its game durations changed
by -3,224, -434, -2,511 and -4,650 frames. Both versions lost **0/4**.
The extra early losses, three large duration regressions, and absent win gain
each independently fail the predeclared gate. This screen provides no
evidence that earlier Core access improved play.

The pairs also reveal a measurement limit. The conditional rule first appeared
at frame 7,200 in every candidate game, yet frame-6,000 army counts already
differed by -1, +1, 0 and 0 and Probe counts by +1, -3, -1 and 0. The two
extra pre-6,000 losses happened before the intervention. Thus identical
seeds, maps and host sides did not give identical pre-treatment trajectories;
those losses cannot be attributed to the Core rule, and the size of its
apparent Core-timing benefit is not a clean counterfactual estimate. The gate
still fails as written. Before interpreting another close paired effect,
measure same-DLL, same-seed repeat variance or add a deterministic
pre-intervention state check.
The [same-DLL repeat control](pvz-repeat-control-20260925.md) subsequently
confirmed pre-treatment divergence in all four pairs, so the pilot's
Core-timing and duration deltas must be judged against run-to-run variance.

The source change and its native scenario assertions were removed. Working
`src/core/Strategy.cpp` again hashes byte-for-byte to the archived reference;
the focused native core tests pass after restoration. The DLLs, reports and
logs remain archived under `build/pvz-core-transition-20260925` for diagnosis.
The next investigation should follow actual builder orders and placement
retries from accepted command to building creation, and quantify whether
optional Cannon/expansion reservations are delaying both tech and defenders.
No expanded campaign or promotion is justified by this pilot.
