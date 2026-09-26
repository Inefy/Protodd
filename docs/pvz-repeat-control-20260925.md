# PvZ paired-repeat measurement control — 25 September 2026

The failed Core-priority pilot used identical seeds, maps and host sides,
but its two conditions already differed at frame 6,000. The rule first
activated at frame 7,200. That pre-treatment drift prevents a clean causal
interpretation of its later Core and survival deltas. The prior mobile-screen
pilot also showed pre-treatment worker or army differences in two pairs.

This control repeats the **same frozen reference DLL** from the Core-priority
pilot against McRaveZ with the same seeds 202609310–202609313, Benzene and
Destination, and both host sides. Every prepared arena input hash matches
the original reference campaign, including the DLL and seeded client JAR.
`build/pvz-repeat-control-20260925/repeat-plan.json` pins the inputs and
review code. The owned supervisor runs only the new four-game repeat and
writes `repeat-status.json` and `repeat-report.json`.

The review checks normal reports, enemy activity, exact pair identity, and
the first difference among sampled pre-7,200 states. It also reports
frame-6,000 army/Probe differences, first defender timing, early losses,
Core timing, duration and win differences. This is measurement calibration;
no gameplay change, strength claim or promotion follows from it.

## Result

The owned repeat completed all four games. The plan, manifests, DLL, seeded
client, review code and supervisor hashes matched their receipts. All eight
original/repeat games were healthy and paired with exact seed/map/host and
identical prepared input hashes. The repeat was **not deterministic**.

| Pair | First sampled difference | First unit/structure difference | Differing states before 7,200 | Probe delta at 6,000 | Core completion delta | Game duration delta |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Benzene, host | 360 | 3,120 | 41/60 | 0 | no comparable completion | -2,976 |
| Destination, host | 2,280 | 3,360 | 32/60 | -1 | -1,505 | -3,255 |
| Benzene, away | 3,960 | 4,440 | 21/60 | 0 | +199 | +21,297 |
| Destination, away | 1,440 | 5,640 | 26/60 | 0 | -88 | -4,743 |

The first sampled difference in each pair was just eight minerals, but
production or worker counts diverged later, still before the Core-priority
rule would have activated in the separate treatment pilot. First defender
completion differed by +38, +8, -3 and -34 frames. Frame-6,000 army and
pre-6,000 own-loss counts matched in all four same-DLL pairs, while later
Core and game-duration variation was substantial. Both runs lost all four.

Matched seeds and identical DLLs therefore do not provide an exact
counterfactual on this runner. Future gameplay comparisons need a wider
repeated-seed sample and a baseline-variance estimate for each metric; a
four-pair timing or duration delta by itself cannot establish an improvement.
The immediate technical diagnosis should inspect the worker-mining and build
command timeline around the first eight-mineral drift, and log pending
builder position/order at release to explain occasional long construction
delays. No source or tournament artifact was changed by this control.
