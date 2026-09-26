# Available-tech composition development experiment

## Why this is next

The fresh Pylon approach comparison reproduced delayed construction but all
three arms lost 0/4 and had essentially the same army at frame 8400. Two
candidate games never completed a Core. That mechanic is not established as a
strength improvement, and its integration remains outside working source.

The current macro planner has a separate reproducible failure. Its PvZ mix is
35% Zealot, 12% Dragoon, 25% High Templar, 18% Corsair and 10% Archon. Five
Zealots already exceed `0.35 * (5 + 1)`, so composition reinforcement stops even
with two idle Gateways, supply and free minerals while the other technologies
do not exist. Reference-a game 2 at frame 8160 in the fresh Pylon comparison
has exactly this condition: five Zealots, no Core and 112 unreserved minerals.
Explicit opening quotas are already satisfied. This is a production accounting
failure, independently reproducible without StarCraft randomness.

## Bounded correction

Normalize composition shares across directly trainable types whose direct
prerequisites exist. Count paid-for, unfinished prerequisites as committed tech;
retain their shares through construction. Do not renormalize on affordability,
power, availability of an idle producer, or gas shortage. Composition remains
after all explicit worker, tech, defense and expansion reservations, at priority
58. It cannot overbook queues or supply. Strategy priorities and quotas are
unchanged. This differs from the retired priority-102 eight-Zealot patch.

`tests/test_available_composition.cpp` reproduces the archived shortage and
covers queues, unusable Gateways, supply, almost-finished Core, gas shortage,
Dragoon catch-up, Core reservation and worker/tech/surplus spending. The original
planner fails five expectations; the correction passes the scenario suite and
the existing core suite. An isolated Win32 Release build covers the same tests.

## Frozen live screen

Root: `build/pvz-available-composition-20260926`. Complete source snapshots and
fresh reference/candidate DLLs are built from the same source, differing only
in `src/core/MacroPlanner.cpp`. The candidate does not include Pylon grace.

Three arms each play four McRaveZ games on Benzene/Destination and both host
sides, with fresh seeds 202609600–202609603 and a 30000-frame limit. Reference-b
uses the exact reference-a DLL and prepared components. Candidate differs only
in Protodd.dll. The repeated reference calibrates same-seed nondeterminism.
`comparison-plan.json` pins inputs, all compiled source, tests, reviewer and
supervisor. `scripts/continue-available-composition.ps1` owns the only run and
will write `comparison-status.json` and `comparison-report.json`.

Before launch, freeze these functional gates: all healthy paired results and
frame-8400 states; accepted new composition actions in at least three candidate
games; mean frame-8400 army at least one above both reference arms; no negative
mean army effect on either map; mean Probes at frames 6000/8400 within one of
the lower reference mean; median first defender no more than 120 frames after
the slower reference; no more missing Cores than the worse reference; mean
completed Core no more than 720 frames after the slower reference; total early
losses no more than two above the worse reference; no game ending over 2400
frames before both references; no fewer decisive wins than either reference.
Frame caps have no win label. Four independent seed groups permit a descriptive
bootstrap interval but cannot establish strength. A pass only justifies further
disjoint, larger evaluation; a failure is preserved without changing gates.

The hourly automation remains paused. This comparison was started by the
user's manual request. No tournament package or learned-model gate is changed.

The first supervisor launch used the legacy Windows PowerShell executable and
failed its hash preflight because that inherited environment could not load
Get-FileHash. No arena or game was started. Its status, lock and logs are archived
with `first-launch-` prefixes. The verified retry uses the installed PowerShell
7 runtime, owns PID 65764, and started reference-a at 00:57:49 UTC. The frozen
source, DLLs, plan and gates were unchanged.

## Completed screen — 26 September, 01:35 UTC

The owner completed all twelve games and its frozen reviewer wrote
`comparison-report.json`. All games were healthy, decisive losses with valid
matching seed/map/host inputs. Both reference arms and the candidate went 0/4.
The candidate had accepted intervention commands in all four games.

| Metric | Reference A | Same-DLL reference B | Candidate |
| --- | ---: | ---: | ---: |
| Mean army at frame 8400 | 5.25 | 5.00 | 5.75 |
| Mean Probes at frame 6000 | 14.00 | 15.25 | 15.25 |
| Mean Probes at frame 8400 | 21.75 | 23.00 | 22.50 |
| Mean completed Core frame (observed completions) | 9599.25 | 9985.00 | 10203.75 |
| Missing Core completions | 0 | 1 | 0 |
| Median first defender frame | 3281.5 | 3295.5 | 3280.0 |
| Total early losses | 1 | 1 | 0 |

**The functional screen failed.** Army gain was +0.50 against A and +0.75
against B, below the frozen requirement of +1 against both. All other declared
checks passed. The descriptive paired effects against the reference average
were [1, 0.5, 1, 0], with a four-seed bootstrap interval [0.25, 1]; that does
not override the gate or establish strength. No larger campaign is justified
by this result alone. Preserve the native bug reproduction and experiment,
but do not designate this change as an accepted strength candidate.

Final verification checked 260 hashes with no mismatches, recorded in
`build/deep-audit-20260926/composition-final-verification.json`. The working
source still contains the composition correction as an unvalidated development
change. The separate deep-audit fixes were made after these frozen DLLs were
built and had no exposure in these games. No promotion or automatic repeat
was launched.
