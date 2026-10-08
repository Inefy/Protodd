# Replay-Derived Aggressive Openings

## Evidence

The local archive contains 10,002 PvP, 10,007 TvP and 10,010 ZvP recordings.
The raw miner examined 21,798 unique eligible games after excluding reserved
validation/test duplicate groups, games shorter than three minutes, and games
without a matched player MMR claim of at least 2,000. At least one player is
qualified; the acting Protoss player's MMR can be unknown. Both winning and
losing Protoss sides are included. Replay hashes match the read-only audit DB.
No reserved validation/test groups were opened.

Families describe raw requests, not accepted production, a pure all-in or a
confirmed attack. Repeated placements are deduplicated. Outcomes are source
winner-race claims in cross-race games and conservative single-quit inferences
in PvP. Conflicts and missing evidence remain unknown. Skill, map, opponent,
archive selection and correlated PvP perspectives confound these rates. This is
human ladder data, not bot tournament evidence or a causal strength ranking.

| Matchup | Family | Perspectives | Known Outcomes | Wins | Descriptive Rate |
| --- | --- | ---: | ---: | ---: | ---: |
| PvP | Gasless two-Gate Zealot | 3,422 | 2,736 | 1,519 | 55.5% |
| PvP | One-base Reaver | 2,129 | 1,638 | 851 | 52.0% |
| PvP | Three-Gate Dragoon | 1,061 | 827 | 416 | 50.3% |
| PvP | Four-Gate Dragoon | 291 | 231 | 117 | 50.6% |
| PvP | One-base DT | 669 | 510 | 256 | 50.2% |
| PvT | One-base DT | 246 | 246 | 95 | 38.6% |
| PvT | One-base Reaver | 138 | 138 | 54 | 39.1% |
| PvT | Gasless two-Gate Zealot | 452 | 452 | 148 | 32.7% |
| PvZ | Gasless two-Gate Zealot | 696 | 696 | 237 | 34.1% |

Two-Gate is the first PvP candidate: largest sample and strongest well-supported
descriptive rate. Tiny apparent 100% families are not selected. PvT/PvZ evidence
does not justify automatic all-ins. Reaver remains a later transport/splash
candidate; it is not implemented in the current repertoire.

The state-based pass found 3,443 commitments in 11,183 training perspectives.
Every known result was a win: this extracted cohort selects qualified winners'
perspectives. Its apparent 100% rates are invalid rankings. Completed states
give useful winning-example timings only. Medians are conditional on a checkpoint
being observed, not a single reconstructed build order or proof of execution.

| PvP Winning Examples | Four Zealots / Four Dragoons / First DT | Second Nexus Starts |
| --- | ---: | ---: |
| Two-Gate Zealot | 4,656 frames (3:14) | 7,824 (5:26) |
| Three-Gate Dragoon | 7,056 (4:54) | 9,804 (6:48.5) |
| One-base DT | 7,608 (5:17) | 9,036 (6:16.5) |

Two-Gate examples start gas at median frame 4,392 and Core at 5,472. Gasless
means two Gateways precede gas, not that the army stays gasless indefinitely.

## Implementation

`AllInOpeningPlanner` owns production and strategic intent in the weighted local
hybrid. Native workers, construction, scouting, legal observation, tactical
safety and emergency defense remain. Weights are the continued six-slot export;
the openings are replay-derived native rules, not newly trained neural weights.

| Profile | Worker Cap | Gateways | Completed Launch Milestone | Pressure Window | Hard Deadline |
| --- | ---: | ---: | --- | ---: | ---: |
| `two-gate-zealot` | 14 | 2 | Four Zealots | 120 seconds | 9,000 frames |
| `three-gate-dragoon` | 20 | 3 | Four Dragoons | 120 seconds | 12,000 |
| `four-gate-dragoon` | 20 | 4 | Six Dragoons | 150 seconds | 13,200 |
| `dt-pressure` | 18 | 2 | One DT | 90 seconds | 10,800 |

Launch is sticky. A pressure timeout, hard deadline or severe post-launch army
loss ends the one-base commitment permanently. Workers and at least two bases
resume when the immediate economy is safe. Surviving fighters continue pressure
while growing; native fight evaluation can retreat from a losing encounter.
Speculative Cannons, Forge and tech demands are explicitly retired, including
macro memory. Observed cloak threats retain detection. One-base assembly has no
expansion target, so its army cannot guard an unrequested natural. Generic legacy
policy defend cannot erase the opening intent; emergency stabilization remains.

Two-Gate adds gas behind the first three Zealots, then Core, Dragoons and range.
Recovery restores a safe neutral expansion site if native rules left it unset,
and prioritizes the Nexus over optional splash tech. `auto` chooses two-Gate only
in PvP; other matchups remain standard pending prospective evidence. Each profile
can be forced for frozen development evaluation.

The subsequent Pluto/BananaBrain investigation tightened DT funding: two
Dragoons are the escort cap, the second Gateway waits for Archives funding,
and generic reinforcement filling no longer demotes the opening's tech outside
an emergency. DT raids ignore an isolated scouting Probe, can follow a
multi-bend terrain route to a remembered enemy economy, and are not recalled
across the map to attack an undetected breach they cannot target. Observed
detectors and actual hits still block covert routes. A visible enemy DT near a
Nexus requests one immediate detection Cannon; ordinary ground pressure and
harmless Observers do not trigger that opening detection anchor.

These execution fixes do not establish a winning build. The separate local
opponent investigation records the tested DLLs and results in
`docs/pluto-banana-allins-20261001.md`; the initial pilot below remains historical.

## Reproduction

```powershell
./build/model-venv/Scripts/python.exe -m training.mine_allin_requests `
  --cohort artifacts/replay-learning/protoss-training-v2-20260920/cohort.json `
  --audit artifacts/replay-learning/audit-protoss-20260920/audit.sqlite `
  --replays artifacts/cwal-dataset --parser build/replay-tools/screp.exe `
  --output artifacts/replay-learning/allin-requests-NEW --workers 4

./scripts/build-trained-controller.ps1 `
  -Weights 'path/to/export-package/weights.bin' `
  -BuildDirectory build/trained-allins
```

`training.arena prepare --all-in-opening PROFILE` freezes the local profile,
read files and DLL. The all-in CMake flag defaults OFF and requires the weighted
local hybrid evaluation controller. The trained-controller script enables it;
`-StandardOpenings` disables it and `-Exclusive` restores research-only model
control. This optional evaluation build requires a separately retained weights
export and Python dependencies; neither is bundled with the tournament source
archive. Neither the repertoire nor weights are promoted to tournament use.

## Retained Evidence

Raw report: `artifacts/replay-learning/allin-requests-20261001/report.json`.
SHA256: `563db8a1b2b01388237667f162783bd5d3d44978c1a630f18ffcf349376f9371`.
Raw profiles: `0d45d8d949aaf72f27fc5eaf95d6a54616f64cd012e199097fc8793eaf822a84`.
State report: `artifacts/replay-learning/allin-mining-v2-20261001/report.json`.
SHA256: `58941871cb2b2014cc748c7fff232314e445e554f6b71bcf28347ff27c1210c8`.
These local replay artifacts are not committed.

Initial pilot: `build/allin-banana-20261001/two-gate`, fixed stock BananaBrain
`PvP_3gaterobo`, Benzene, both host sides, full 86,400-frame ceiling, frozen
learning, development slow-frame allowance 640. DLL:
`e8bc2b153c843568f008167d150997aa383e571786c5fc110479d0cfc3e5bc48`.
Pre-launch source and build record are retained. This revision stayed gasless
too long and could leave recovery without an expansion target. Its results
must not be attributed to the subsequent corrected revision.

The initial revision finished 0/2, normal paired results without caps, crashes or
timeouts. Both launched at frame 4,800 and transitioned at 7,680. Four fighters
were observed at least 1,200 pixels from home at frames 5,263 / 7,183. This movement
proxy is not proof of a successful attack. First Dragoons completed at 9,583 /
9,679; neither game completed a second Nexus. Loss frames: 19,501 / 14,355.
Both strict 42 ms callback gates passed (24.319 / 18.205 ms maximum). Review:
`build/allin-banana-20261001/two-gate-review.json`.

Corrected revision DLL:
`9072340858698eb6d8c0b791360fb3a0a075b10f13e52c7e953b603de72cee44`.
Source fingerprint:
`3df323d53f5b301973968dddc00bcc541f0cb5a94cfdd03efd8b2f3a7d56f7a8`.
Separate frozen campaigns at `build/allin-banana-v2-20261001/{two-gate,three-gate}`
use that same DLL and weights. They are separate from the defective first pilot.
The pre-launch source snapshot and build record are retained at this root.

Corrected two-Gate finished 0/2 normal uncapped losses, at frames 20,276 / 18,881.
Four Zealots completed at 4,807 in both games. First Dragoons completed at
7,015 / 6,991, and second Nexuses at 10,255 / 10,279. Both games completed two
bases, versus one in the initial revision; four fighters were observed at least
1,200 pixels from home at 5,263 / 5,911. This verifies timing and transition
behavior, not improved win rate. Peak completed Probes: 32 / 28; Dragoons: 10 / 11.
The model proposed no legal attack targets. Review:
`build/allin-banana-v2-20261001/two-gate-review.json`.

Corrected three-Gate also finished 0/2 normal uncapped losses, at frames
23,004 / 18,385. Four Dragoons completed at 7,567 / 7,351; launch decisions
were at 7,560 / 7,344. The pressure windows ended at 10,440 / 10,224. Second
Nexuses completed at 12,535 / 12,607. Four fighters were observed at least
1,200 pixels from home at 7,879 / 7,639. Peak completed Probes: 36 / 30;
Dragoons: 14 / 16. It researched range but did not convert pressure into wins.
Both strict 42 ms callback gates failed (93.134 / 89.608 ms maximum); normal
development results under the relaxed allowance do not establish ladder readiness.
The model proposed no legal attack targets. Review:
`build/allin-banana-v2-20261001/three-gate-review.json`.

Both corrected arms have matching actual seeds 202609900 / 202609901, map hashes,
host sides, weights and DLL. Only the frozen opening profile differs, alongside
the necessarily different local run time. Corrected two-Gate passed the strict
callback gate (29.236 / 26.568 ms maximum). All owned arena processes were stopped
after completed games. No heavy replay-mining/build job ran during these corrected
games, but this is still a local pilot, not an isolated performance benchmark.

All six all-in pilot games lost, including the defective first revision. This
work establishes a reproducible repertoire and verifies attack/economic phase
execution, not improved tournament strength. Four-Gate and DT have deterministic
core coverage but no prospective matches in this pilot. The major unresolved
work is attack conversion, scouting-informed profile selection, and large-fight
callback cost. The replay-derived rules cannot compensate for those weaknesses.

Verification: the corrected Win32 build passed all 61 CTest groups. The six
opening/mining/hybrid/arena review groups also passed after the final report-tool
update. `git diff --check` passed. No model promotion receipt or online reward
labels were created from these hybrid games.
