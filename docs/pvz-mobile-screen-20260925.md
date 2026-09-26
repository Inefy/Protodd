# PvZ mobile-screen development screen — 25 September 2026

The bounded ten-Probe cap experiment improved early workers but did not improve
wins or frame-6,000 mobile army. In its four paired McRaveZ losses, Protodd
usually had only five mobile units at frame 8,400 while committing to a natural
and technology. Later waves of Zerglings, Hydralisks or Mutalisks overran those
bases. `planPvZ` asks for only five Zealots at that point, and composition
filling can refuse more Zealots while its other weighted unit types still lack
prerequisites. The archived logs show idle Gateways and available mineral bank
in some of these windows. This is a concrete spending and army-readiness test,
not a claim that more Zealots alone win the matchup.

One source-pinned candidate asks for up to eight Zealots at blocking priority
102 between game minutes five and eight when two Gateways are completed, the
Cybernetics Core has not completed, and observed air pressure is low. It keeps
the old opening rule outside that interval. The goal uses the normal resource
ledger and command path. The native scenario test checks activation and exit
when the Core completes. `build/pvz-mobile-screen-20260925` contains the frozen
plan, source manifests, DLLs, review script, client bundle and build receipt;
the reference and candidate source manifests differ only in
`src/core/Strategy.cpp`.

The reference DLL SHA-256 is
`5a4eaeaeb367d464720780be5a336472834069990d1f6a1eee782d4cccffdfd2`;
the candidate is
`0ab74b9be854e2e251726111d5acad1785efd8838757a39c880447309544a786`.
The owned supervisor `scripts/continue-pvz-mobile-screen.ps1` completed four
reference and four candidate games versus McRaveZ on Benzene and Destination,
both host sides, with paired seeds 202609300–202609303. Its frozen plan and
review results are in `pilot-plan.json` and `pilot-report.json`. The plan,
source, DLL, client and review hashes were verified against their receipts.
All eight games had normal runtime health, enemy activity and no review
exclusions; all pairs used identical inputs except the candidate source.

The predeclared screen requires normal paired results; at least two additional
mean mobile units at frame 8,400; no more than three fewer mean Probes then;
candidate Core completion in every game where reference completed one, with
mean delay at most 720 frames; no added own-unit losses before frame 6,000;
and at least one additional win across four pairs before a larger campaign.
Failure retires this candidate. Passing would justify diverse paired games,
not a strength claim or tournament promotion. The failed whole-game model
remains gated, and no model or tournament control is enabled.

## Result

The candidate failed the frozen screen. It issued the intended extra-Zealot
goal in all four games (3, 2, 3 and 3 logged interventions; zero in the
reference games), but gained only 2, 0, 2 and 1 mobile units at frame 8,400:
**+1.25 mean**, below the required +2. Its Probe differences were 0, +1, -4
and -2 (**-1.25 mean**), and it added no pre-6,000 own-unit losses. Both
versions won **0/4**.

Core completion did not satisfy the gate. The candidate completed it five
frames later in pair 1 and 1,081 frames later in pair 2; in pair 3 the
reference completed it at frame 10,816 and the candidate never did. Pair 2
also ended at frame 13,797 for the candidate versus 32,893 for the reference.
The extra early army did not translate into survival or wins and competed with
the needed technology in some games. The source change and its native scenario
assertions were removed; `src/core/Strategy.cpp` again hashes identically to
the archived `Strategy.reference.cpp`. The archived candidate DLL and all game
artifacts remain for diagnosis. The focused native core test passed after
restoration.

The next investigation should trace production demand, mineral reservations,
and build placement in the actual losses before another unit-count candidate.
At frame 8,400, all four reference games had two or three Gateways and none
were busy, but only five or six Zealots and no Dragoons. In reference pair 1,
the ledger had 114 unreserved minerals and two idle Gateways; a Nexus reserve
of 400 minerals had priority 95, above the PvZ Core goal's 82. The Core goal
does not appear in the logged `goals` summary because that diagnostic prints
only the first 12 goals; the source rule requests it after the supply
threshold. The composition filler
uses the total planned army count and a 0.35 Zealot weight even when Dragoon,
Templar and Corsair prerequisites are incomplete; at six Zealots its quota
rejects another affordable Zealot. In pair 3, a Pylon and Cannon reserved 250
minerals, leaving 118 free, while both Gateways were idle. The candidate in
that pair still had 532 minerals and 484 gas at frame 8,400 with an idle
producer and unfinished Core; its log repeatedly reported Cannon
`build-no-location-placement-search`. These are concrete diagnostic leads,
not causal proof. A future source-pinned paired screen should target the
reservation/composition interaction while protecting Core timing, defender
timing, workers, and survival.
