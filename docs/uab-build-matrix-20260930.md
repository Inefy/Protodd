# UAlberta opening build diagnostics 30 September 2026

This development screen tests eight configured UAlberta opening variants,
then uses the losses to improve construction safety, detection timing and
early bio recognition. The initial nine games produced four clean wins,
two clean losses, two frame caps and one excluded opponent crash. These
small, single-map samples do not establish matchup win rates or universal
strength.

## Changes

- Home infrastructure now selects builders against the home construction
  anchor instead of the forward army rally. Nexus builders still use the
  expansion site; Cannon and Battery builders retain their defensive rally.
  Existing builder danger and route filters remain enabled. Pylon placement
  can still select a remote base; this does not prove every later Pylon route
  is aligned with its final footprint.
- Expansion funding waits when recent armed enemies occupy the site and the
  local available screen is insufficient. The existing combat evaluator is
  used without simulation. A distant home army does not count as coverage;
  powered local static defense does. Walking pending builders are released,
  the assembly target remains, and already-warping Nexuses are not canceled.
- Against scouted Protoss tech, the opening Core outranks extra Gateways and
  its bank is protected from routine Probe spending. After 3 minutes 30
  seconds, one observed Gateway plus Core with no observed Zealots or
  Dragoons requests one insurance Observer, provided our first Dragoon has
  been paid, 18 Probes are complete, and no immediate rush is threatening.
  Visible army and direct-pressure controls preserve defense spending.
- An enemy Marine first seen in the first two minutes supplies an early bio
  warning even if only one Barracks was scouted. The warning ends after six
  minutes and is overridden by observed Factory tech or a second Command
  Center. This addresses late fortification in the four-barracks loss.

## Inputs

All games use Protodd as Protoss, Destination map hash
`4e24f217d2fe4dbfa6799bc57f74d8dc939d425b`, and a 28,800-frame cap.
Eight alias packages use the same UAlberta DLL SHA-256
`A99DF20B634298D746114673596A1CE4D6A152D148CD086F8FA67BC803598183`.
They are opening configuration variants, not eight independently compiled
opponents. Strategy definitions were checked against their race.

`Strategy.UseEnemySpecificStrategy` and `Modules.UseStrategyIO` are false
to force each selected opening. Own learned reads start empty and frozen,
and manager bot-file learning is disabled. Earlier Terran diagnostics used
the original opponent strategy I/O settings, so comparison with those
results is not an isolated code A/B test. Seeds below were checked against
actual MATCH logs; the environment is not assumed deterministic.

Each campaign has a separately frozen DLL and source patch against base
`4949491`. No running campaign was updated midway through its games.

| Version | Changes | DLL SHA-256 | Passing Suite Executions |
| --- | --- | --- | --- |
| A | Construction anchors and site guard | `C3BEB763CB4B6BCEFB81A039BB5743FD9DD5100228CA103605235832065D30AE` | 47 portable + 48 Win32 |
| B | A plus cloak checkpoint and Core bank | `11CD47F6335F0829EBDBF73160A730C76A31493A6F2E9A631AF5C981DDC31EDD` | 48 portable + 49 Win32 |
| C | B plus early Marine warning | `E285B21AF8C1577CE850DF9057B18591AB5F76A399FCBE8BA16B98F3F1E18FBA` | 48 portable + 49 Win32 |

## Initial Matrix

Version A, `build/uab-construction-matrix-20260930/candidate/`:

| Game / Seed | Selected Opening | Protodd Host | Result | Own Final Frame |
| --- | --- | --- | --- | --- |
| 0 / 202609900 | Terran_MarineRush | Yes | Frame cap | 28,802 |
| 1 / 202609901 | Terran_VultureRush | No | Clean win | 18,416 |
| 2 / 202609902 | Terran_TankPush | Yes | Clean win | 22,911 |
| 3 / 202609903 | Terran_MarineRush | No | Opponent crash excluded | 19,594 |
| 4 / 202609904 | Protoss_DragoonRush | Yes | Clean win | 26,662 |
| 5 / 202609905 | Protoss_DTRush | No | Clean loss | 12,805 |
| 6 / 202609906 | Zerg_2HatchHydra | Yes | Frame cap | 28,802 |
| 7 / 202609907 | Zerg_3HatchMuta | No | Clean win | 23,035 |
| 8 / 202609908 | Terran_4RaxMarines | No | Clean loss | 12,898 |

Enemy lifecycle logs include Vultures at 5,596, Tank Mode at 6,985 and Siege
Mode at 8,344, Dragoons at 5,702, DTs at 7,546, Hydralisks at 6,635 and
Mutalisks at 16,685. Thus the named combat-unit openings were actually
encountered. Clean outcome pairs are NORMAL, agree on the winner, finish
below the cap, and show opponent army activity and a positive score.
Marine game 3 has an own win flag but opponent STARCRAFT_CRASH, so it is
excluded. The cap pairs both report false win flags and are not losses.

Every own summary has zero caught and logging errors. The site guard appears
in 32, 38, 49, 14 and 32 logged strategy samples in games 0 through 4. These
are sample counts, not durations. No home-tech/Pylon `build-no-builder`
samples appear in the Terran games, but this does not establish causal
performance improvement. An unfinished Nexus still died in Tank game 2:
the guard cannot prevent enemies arriving after construction has started.

## Loss Analysis

In DT game 5, the Core started at 4,974 and completed at 5,945. Robotics
started at 7,506 and completed at 8,777, after the first DT sighting at 7,546.
Neither an Observatory nor an Observer was produced. Workers fell from 22
at frame 7,800 to zero by 10,800. The one-Gateway/Core observation was not
covered by the existing two-Gateway detection insurance. The new checkpoint
and earlier Core address that missed timing; this is an evidence-led
hypothesis, not proof that every DT opening is solved.

In four-barracks game 8, the scout first saw a Marine at frame 2,220 but only
one Barracks. The spatial anti-bio warning activated around frame 4,560.
The Forge was then still unfinished at 5,160; the first Cannon lifecycle
create was 5,480 and its first completion was 6,752, while Marines were
already fighting nearby. The army peaked
at four and the Probe count at 18. Earlier recognition aims to fund the
static screen while the bio force is crossing the map.

## Cloak Retest

Version B, `build/uab-cloak-checkpoint-20260930/candidate/`:

| Game / Seed | Opening | Protodd Host | Result | Own Final Frame |
| --- | --- | --- | --- | --- |
| 4 / 202609904 | Protoss_DragoonRush | Yes | Clean win | 23,624 |
| 5 / 202609905 | Protoss_DTRush | No | Frame cap | 28,802 |
| 13 / 202609913 | Protoss_DTRush | Yes | Clean win | 17,083 |

The matched DT case started its Core at 3,828, Robotics at 5,463 and
Observatory at 6,799. Its first Observer completed at 7,928, shortly after
the first DT sighting at 7,574. The last sampled state has 48 Probes, 26 army
units and three Nexuses, compared with the earlier worker collapse and
loss. This supports improved survival on this case, not a decisive win or
a general strength claim. The Dragoon control remains a clean win.

The additional DT seed has no matched version A baseline. Two enemy DTs
were observed, first at 7,506, and the opponent scored 12,719. Probe samples
fell from 22 to 12 during the wave before recovering to 32. Its visible army
peak is only two, failing the generic four-unit activity screen despite
actual cloak combat. The result is structurally a clean win, but supplies
only a limited two-DT exposure check, not strong-opponent validation or a
training reward. Detection arrival and mineral-line coverage still need work.

## Final Build Retest

Version C, `build/uab-final-builds-20260930/candidate/`. These cases retain
version A's seeds, host sides and forced opponent configurations.

| Game / Seed | Opening | Result | Own Final Frame | Last Sample Army / Probes / Nexuses |
| --- | --- | --- | --- | --- |
| 0 / 202609900 | Terran_MarineRush | Clean win | 26,569 | 62 / 44 / 2 |
| 5 / 202609905 | Protoss_DTRush | Frame cap | 28,802 | 0 / 7 / 1 |
| 8 / 202609908 | Terran_4RaxMarines | Frame cap | 28,802 | 61 / 44 / 2 |

The DT case produced its first Observer at 7,832 after a first DT sighting
at 7,582. It survived the opening but later lost its army and two bases.
Its late army included DTs and Dragoons; visible enemy army peaked at 14.
This cap is a badly deteriorated position, not a successful defense of the
full game. Version B and C have identical PvP logic, so their different
late outcomes are not attributed to the Terran-only change.

The four-barracks case completed its first Cannon at 5,641 rather than
6,752 and reached a large army instead of the early collapse. Nevertheless,
it failed to eliminate the opponent before the cap. Its first Marine was
seen at 3,236; the MarineRush case first saw one at 3,545. Both are after
the new two-minute warning cutoff. Those outcomes therefore do not validate
the new warning's live impact. Native tests cover the missed frame-2,220
case and the later-Marine and Factory controls. Different observed timings
on repeated seeds limit causal claims about all of these comparisons.

Every own summary in all three campaigns has zero caught and logging errors.
No crash or timeout flags appear in the final campaign. The final gameplay
DLL matches the frozen C binary. All 97 final suite executions passed.

## Remaining Targets

1. Sustain mobile detection and defensive coverage across several mineral
   lines during repeated DT attacks. An earlier Observer alone is not enough.
2. Convert a large surviving army into a timely elimination. Four-barracks
   and Hydra caps are not wins, regardless of army size or score.
3. Exercise the new early-Marine condition in additional live cases, and
   repeat all openings across more seeds, host sides and maps before making
   strength claims. Only three openings were retested on the final DLL.
4. Reduce late supply stalls and prevent enemies arriving at a Nexus after
   the site guard has permitted construction.

## Evidence

The three campaign roots above contain frozen DLLs, source diffs, copies of
new source files, successful native test logs and preparation receipts. Their
`review-receipt.json` files pin configurations, actual match seeds, manifests,
raw result pairs, archived bot logs and opponent archives, review scripts,
and this report by SHA-256. Manifests verified before launch and after play;
audits also check map hashes, host sides and forced opening settings.

Preparation scripts: `build/prepare_uab_matrix_20260930.py`,
`build/prepare_uab_cloak_20260930.py`, `build/prepare_uab_final_20260930.py`.
Review scripts: `build/review_income_screen.py`,
`build/audit_uab_matrix_20260930.py`, `build/audit_uab_retests_20260930.py`.
The historical `build/review_uab_20260929.py` parser was not modified.
Large binaries, replays and local receipts remain ignored development
artifacts, not committed training data. All runs retain `training_ready=false`
and `strength_validated=false`; outcomes are not pooled across DLL versions.

All nine owned Java processes were stopped after their respective paired
reports arrived and StarCraft exited. No owned StarCraft or Java test process
remains running. Fifteen attempts were played across the three distinct DLLs
and eight opening variants; this is an iterative development screen, not a
held-out benchmark.
