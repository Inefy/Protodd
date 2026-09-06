# BananaBrain source review — 2026-09-06

Reviewed the local AIIDE 2025 source in `ladder/bots/BananaBrain/src/Source`.
This pass builds on the existing, uncommitted Stardust work. It does not replace
that work or establish a stronger win rate by itself.

## Source comparison and implemented changes

| BananaBrain implementation | Gap in Protodd | Change in this pass |
| --- | --- | --- |
| `Macro.cpp`, `TrainingManager::is_training_queue_empty` and `apply_worker_train_orders` (1034, 1174); `BananaBrain.cpp` (217–227) | Protodd ran macro frequently, but both its snapshot and command adapter still considered a producer unavailable until training finished. | A shared training-slot predicate admits one successor when completion falls inside remaining latency. Recent commands and a second queue entry prevent duplicate spending. This applies to Nexuses and combat producers. |
| `Macro.cpp`, `BuildingManager::update_supply_requests` (129) | A fixed supply buffer underestimates several fast producers; an unfinished Pylon suppressed all additional requests. | Forecast supply consumption over Pylon construction plus a 96-frame builder allowance, using each producer's normalized composition and unit build times. Credit pending supply; another Pylon is allowed if that supply still falls short. |
| `Worker.cpp`, `defend_base_with_workers_if_needed` (1385) and `is_dangerous_unit` (2035) | Worker escape was coupled to the early militia cutoff; a distant undetected DT could evacuate every Probe. | Evaluate melee danger beyond the opening cutoff, choose the nearby threat for each worker, and stop escape assignments once separation is restored. Keep the existing small militia policy. |
| Worker orders and local defense decisions in `Worker.cpp` | The adapter alternated move/gather during command latency and sent fleeing workers to the same farthest mineral patch. | Respect pending commands, mineral-walk to safer patches with an assignment-load penalty, check other visible attackers at the destination, and use the explicit escape step when no suitable patch exists. |
| `ProtossStrategy.cpp`, `mode_main` (3769–3875) | Army posture automatically cancelled economic growth even after saturation. | A guarded economy may reserve a natural while its army stays defensive. The opening base cap, active rush/containment, breach, and missing cloak detection still veto growth. |
| `Worker.cpp`, `WorkerAllocation::max_workers` (79) derives its cap from allocated resources | Desired-but-unstarted expansions allowed 40 Probes on one base in the third test. | Worker targets use actual Nexus count, including construction, rather than desired base count. Protodd retains its approximate 22-workers-per-base limit; it does not copy BananaBrain's three-workers-per-resource calculation. |
| `ProtossStrategy.cpp` selects distinct opponent responses and tech requests | Every visible ground combat unit counted as current melee pressure. | Only visible Zealots/DTs activate that contact signal. Correct an adjacent Cannon-count branch that could request more Cannons when the second was judged unaffordable. |

The review also exposed two bugs in the previous Protodd changes. Deferred
structures refreshed their own last-request timestamp forever; now only an
explicit strategic request refreshes the 30-second memory, and cancelled
expansions release it immediately. Different upgrades share an unknown unit
target; demand merging now includes technology identity, so range does not
erase speed or other upgrades.

These are independent implementations of the inspected ideas. No BananaBrain
source blocks or precomputed mining data were copied. Its `License.txt` allows
reuse subject to conditions, including author permission for public tournament
submissions containing substantial portions of its software.

## Important remaining differences

BananaBrain's `Strategy::attack_check_condition` (117) keeps an attack committed
between separate entry and exit army-supply thresholds, correcting for excess
air-only units. Protodd's director mainly has a timed post-defense hold, and its
matchup rules still contain many competing overrides. This pass separates
growth from defense but does not replace that whole strategic controller.

`WorkerManager::is_dangerous_unit` asks a local enemy cluster whether the front
units should win. Protodd's revised escape checks use proximity and its influence
map, not that cluster simulation. The 160-pixel melee escape distance and
mining floor need live tuning; path safety between a worker and a selected
mineral patch is still approximate.

BananaBrain's `BaseState::update_next_available_bases` (177) uses BWEM base and
area ownership, reachable ground distances, distance from enemy bases, and an
explicit preference for the natural. Protodd still uses its resource-site
discovery and placement/navigation checks. Porting the topology model would
be a separate architectural change.

BananaBrain also has persistent worker orders and map-specific optimal-mining
data. Protodd retains mineral assignments but does not have that movement/cargo
optimization. Its new supply forecast is a throughput estimate with a fixed
travel allowance, not BananaBrain's measured builder travel estimate or a
complete future resource scheduler.

`BananaBrain::after` (211–227) explicitly switches combat training ahead of
optional construction/research when `prioritize_training` is enabled. Its
Protoss main mode enables this for rushes, containment, anti-air needs, or an
oversized opponent army. Protodd still expresses most of this through individual
goal priorities. The new traces show why a coherent reinforcement budget is
the next useful scheduling change: expensive tech can finish while the mobile
screen and Gateways are being lost. Fixing latency does not fix that spending
decision.

## Verification

The Release/BWAPI build and all four CTest suites pass. New regressions cover
latency-window training, duplicate-command suppression, abandoned-request
expiry, immediate expansion cancellation, distinct concurrent upgrades,
high-throughput supply demand, pending Pylon credit, distant/nearby DT escape,
return to mining, post-opening melee protection, defensive expansion, and
cancellation on a real breach. Additional cases ensure that an unstarted
natural cannot raise the worker cap, while a warping Nexus can.

Direct games use BananaBrain AIIDE 2025 on Python, with learning reset by the
match harness. The tested candidate DLL is
`ADA0CFF2A5A2514DA60B47ABA9373627C9FD1467E9E48F0A3902872C29BB8B3B`.
The runtime implementation is the same for the first two candidate games.
Final review then corrected duplicate explicit requests overwriting the
strongest remembered priority. The third game used
`81564F47B4DF778E42165851F7E80DA6BC25391438B0DC8B76E78679754AEBB5`.
That game exposed worker oversaturation, leading to the construction-based
worker cap. The final DLL is
`4C002D4433E31412A2E516531C9D8F5BE03CADA64751421704240DA824B3E2BE`.

| Build / seed | Outcome | Peak sampled Probes | Last sampled mined minerals | Peak bases |
| --- | --- | ---: | ---: | ---: |
| Previous `stardust-source-v181-seed17` / 17 | Loss at 9,085 | 16 | 2,426 | 1 |
| Candidate `banana-review-v183-seed17` / 17 | Loss at 16,060 | 20 | 5,826 | 1 |
| Previous `stardust-source-v182-seed20260984` / 20260984 | Loss at 9,488 | 17 | 2,434 | 1 |
| Candidate `banana-review-v184-seed20260984` / 20260984 | Loss at 12,743 | 17 | 3,954 | 1 |
| Intermediate `banana-review-v185-seed17` / 17 | Loss at 24,740 | 40 | 13,170 | 1 |
| Final `banana-review-v186-seed17` / 17 | Loss at 15,657 | 18 | 5,106 | 1 |

In the candidate's seed-17 trace, a completed Core is first sampled at 8,280,
Robotics at 9,720, and Observatory at 10,440. Mining continues through the
defense instead of stopping at 2,426 minerals. The army is subsequently reduced
to one combat unit, the Core has to be rebuilt, and the Nexus falls before
an expansion is established. No counterattack is recorded. This supports an
economy/survival improvement in this run, not a win-rate claim or a successful
live expansion claim. A fixed seed is not a guarantee of identical execution
in this harness; repeated balanced-start comparisons remain necessary.

The second seed first samples a completed Core at 7,920 and Robotics at
10,800. It also loses its army and Gateways before establishing a natural.
Neither candidate game demonstrates successful live expansion or a win.

The third game reached 30 combat units and recorded its first counterattack
at 16,101. It reserved a natural but never built it, while worker production
continued to 40 on one base. That is a macro failure, not evidence of economic
success. It also sampled two completed Observatories: duplicate building
execution/retry behavior remains an investigation item beyond request merging.

The final build's game peaks at 18 Probes, completes the Core (first sampled
at 7,920), and later loses the economy to the attack. It never reaches the
new cap, so the cap's before/after construction behavior is verified by the
regression scenarios, not by this game. It records no counterattack or
expansion.

All four development games completed: zero wins, four losses, no timeouts.
They span three DLLs and must not be treated as a controlled win-rate estimate
for the final build. The final candidate has one full-game loss. All four
CTest suites pass on that build, `git diff --check` is clean, and the match
harness left no StarCraft processes running. The principal unresolved issues
are reinforcement spending under pressure, army coordination, completing the
transition to a second base, and duplicate building retries.
