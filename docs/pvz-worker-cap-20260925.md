# PvZ early worker cap development screen — 25 September 2026

Four frozen baseline PvZ losses held at eight Probes from frame 1,200 through
at least frame 3,240 without early own-unit deaths. Their archived logs show
an idle Nexus and 248–264 minerals at frame 2,400 while `desiredWorkers=8`.
`StrategyEngine::planPvZ` deliberately imposes that cap until two completed
Zealots or a completed Cannon exist. The planner therefore has no Probe goal;
changing only resource reservation cannot remove this pause.

One bounded candidate raises the cap from eight to ten **after the first
Pylon completes**. Before that Pylon, the eight-worker cap still funds its
construction. The Forge, Cannon, Gateway, Zealot, placement and command paths
are unchanged. The candidate was built from the same current source snapshot
as the reference, with only `src/core/Strategy.cpp` changed between DLLs.
Native core tests cover both sides of the Pylon boundary and pass.

`build/pvz-worker-cap-20260925/pilot-plan.json` froze the screen before any
game, and `build-receipt.json` records the source manifests, DLLs, review code
and client hashes. The reference DLL is
`5a4eaeaeb367d464720780be5a336472834069990d1f6a1eee782d4cccffdfd2`;
the candidate is
`d4af5c06022fe20ba4291640c1c3cd07308894c9db2fcf6be1107771e5f03e12`.
The two manifests differ only in `src/core/Strategy.cpp`.

The owned supervisor `scripts/continue-pvz-worker-cap.ps1` completed four
reference games against McRaveZ, verified their normal paired health, then
completed four candidate games. Both conditions used Benzene and Destination,
both host sides, and BWAPI seeds 202609290–202609293. The report, plan, source
manifests, review script, client bundle and both DLL hashes match their frozen
receipts. All eight games were normal with observed enemy activity and no
exclusions or logged runtime errors; seed, map and host match within every
pair. `pilot-report.json` records the exact per-game metrics.

The predeclared functional screen requires at least one more Probe at frame
3,000 in three of four pairs and one more mean Probe at frame 6,000. Mean first
completed Zealot-or-Cannon timing may be no more than 240 frames later; there
must be no added own-unit losses before frame 6,000 or fewer wins. Failure
stops expansion of this candidate. Passing would justify more diverse paired
games, not a strength claim or promotion. No model or tournament control is
enabled.

## Result and disposition

| Measure | Reference | Candidate |
|---|---:|---:|
| Wins | 0 / 4 | 0 / 4 |
| Probes at frame 3,000, each game | 8 / 8 / 8 / 8 | 10 / 10 / 10 / 10 |
| Mean Probes at frame 6,000 | 15.25 | 16.5 |
| Mobile army at frame 6,000, each game | 4 / 4 / 4 / 4 | 4 / 4 / 4 / 4 |
| Mean first completed Zealot-or-Cannon frame | 3,350 | 3,471 |
| Own-unit losses before frame 6,000 | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| Final frames, paired order | 12,154 / 11,689 / 14,510 / 14,913 | 12,557 / 11,689 / 16,215 / 12,247 |

The fixed functional screen passed: each pair gained two Probes by frame 3,000;
mean frame-6,000 gain was 1.25; mean first-defender delay was 121 frames;
there were no added early losses and no fewer wins. Benzene survival averaged
1,054 frames longer for the candidate; Destination averaged 1,333 frames
shorter. Those differences and the four losses per condition provide no
evidence of greater playing strength. The candidate DLL and source copy remain
development artifacts. Working `Strategy.cpp` was restored byte-for-byte to
the reference version, and the focused native core tests pass after restoration.
No promotion or broader cap-only campaign is justified from these outcomes.

At frame 8,400 the bot generally had only five mobile units while committing
to expansion and technology. The subsequent losses came against waves of
roughly 35–60 Zerglings, 18–26 Hydralisks, or 8–12 Mutalisks, depending on
the game. The near-term strength bottleneck is converting the early economy
into timely, matchup-appropriate mobile defense. Any new spending or defense
candidate should be assessed on first useful army, preservation of the natural,
survival and wins against matched opponents. More varied paired evidence would
also be required before making a strength claim for the archived worker-cap
candidate.
