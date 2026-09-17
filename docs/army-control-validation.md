# Army control and validation — September 12, 2026

This follow-up addresses the reported whole-army stutter, rear units remaining
at home, and inefficient trades against BananaBrain. It preserves the existing
logging work and the earlier [logging-strength report](logging-strength.md).

## Actual results

There are **18 completed games: 3 wins and 15 losses**
in this pass. Against BananaBrain across these builds, the result is **0/13 wins**.
Correctness tests and longer survival are not evidence of a stronger win rate.

All development games use Benzene, requested and observed seed 43, fresh runtime
learning on both sides, and frozen DLLs. All completed BananaBrain tests use
the fixed `PvP_nzcore` opening. The later UAlbertaBot race checks are separate
regression opponents; their results do not establish strength against BananaBrain.
The same opening and seed do not reproduce identical movement or decisions.

Only the direct runner's completed, pre-cleanup result is counted. The in-game
limit is 43,200 frames. No timeout, cleanup result, score lead, or unfinished game
is counted as a win.

| Game | Result | Final frame | Peak army | Peak Nexuses |
| --- | --- | ---: | ---: | ---: |
| `win-pass-v1-benzene43` | Loss | 21,330 | 20 | 2 |
| `win-pass-v2-benzene43` | Loss | 22,756 | 30 | 1 |
| `win-pass-v3-benzene43` | Loss | 26,941 | 22 | 4 |
| `win-pass-v4-benzene43` | Loss | 20,462 | 26 | 1 |
| `win-pass-v6-benzene43` | Loss | 22,818 | 29 | 2 |
| `win-pass-v7-benzene43` | Loss | 19,811 | 23 | 2 |
| `win-pass-v8-benzene43` | Loss | 23,066 | 28 | 2 |
| `win-pass-v10-benzene43` | Loss | 21,051 | 24 | 2 |
| `win-pass-v11-benzene43` | Loss | 21,051 | 22 | 1 |
| `win-pass-v12-benzene43` | Loss | 19,284 | 28 | 1 |
| `win-pass-v15-benzene43` | Loss | 21,113 | 27 | 2 |
| `win-pass-v16-benzene43` | Loss | 22,570 | 29 | 3 |
| `win-pass-v16-uab-protoss43` | Win | 21,113 | 30 | 4 |
| `win-pass-v18-uab-terran43` | Loss | 24,368 | 20 | 1 |
| `win-pass-v18-uab-protoss43` | Win | 25,484 | 35 | 5 |
| `win-pass-v18-uab-zerg43` | Loss | 8,496 | 0 | 1 |
| `win-pass-v19-uab-protoss43` | Win | 24,740 | 34 | 4 |
| `win-pass-v19-benzene43` | Loss | 20,400 | 29 | 1 |

## Incomplete games

The v16 Terran regression reached frame 43,200 without a terminal result. The
army survived but remained contained after exhausting its main mineral line.
It is excluded from the win/loss total. The trace showed newly revealed enemies
causing defenders to be reassigned from the advance back to a tight Cannon
screen, weakening the remaining advancing squad and triggering its retreat.

## Control corrections

- A forward group of at least six fighters keeps its assault destination when
  a larger rear reinforcement group becomes the vanguard. Tiny detachments and
  trailing groups still regroup. The v18 Protoss win exposed seven sampled
  backward goals for thirteen-unit groups, motivating this correction.
- A strong mobile base-defense force with a stabilized engage decision and a
  ratio of at least 1.5 can clear the area within 960 pixels of its economy.
  This requires at least twelve local attack-capable fighters and mobile value
  at least three times static value. Small guards, detection failures and losing
  fights retain their screen. This is a bounded local policy, not an override
  granting the whole army a global attack.
- Empty Reavers and Carriers travel with legal Move orders and resume attack
  orders when their ammunition returns. The v16 Protoss win contained 1,392
  rejected empty-Reaver attack-moves, motivating this correction.
- Confirmed active attacks, movement and hold orders retain ownership without
  restarting the engine order. A stopped attacker outside range remains eligible
  for a path retry. Critical escapes still take priority.
- An own, just-issued attack already in range receives a bounded windup window.
  This prevents routine retargeting or a brief squad decision flip from cancelling
  the shot. Native attack frames remain separately protected. The observed rapid
  retargets establish command churn, not a measured count of cancelled shots.
- An expansion beyond the natural no longer redirects the entire main army.
  Economic defense allocation checks whether the army is actually present, while
  allowing an assembled force behind its own choke to break out.
- Retreat is independent of the forward pursuit boundary. Rear units in a losing
  squad no longer march back toward the forward terrain anchor. Retreat points
  remain near the defended economy, and threatened units use walkable local steps
  away from enemy fire when the nominal anchor is unsafe.
- Large ground armies receive stable, separated staging positions during clear
  defensive assembly. Positions exclude existing buildings and reserved Nexus
  footprints and respect the defensive area and walkable segments;
  active combat continues to use tactical targets.
- Visible Storms from either player enter the hazard map. Units react before the
  damage flag when the spell is visible, escape overlapping spells, and avoid
  stepping straight back into one during pursuit. Terrain and crowding affect the
  escape; the spell clearance radius is conservative.
- Enemy range, damage, armor, shield upgrades and speed use the values BWAPI
  legally exposes for visible completed units. Fog retains the observed values.
  Enemy resources, queues and hidden research remain private.
- The bounded fight simulation includes discounted enemy-only splash from Reavers,
  Archons and Corsairs. Scarab ammunition is still consumed. It does not model
  complete movement, projectile collisions, future spell casts or every weapon's
  splash geometry; its ratios are not win probabilities.

## Economy and technology corrections

- An Assimilator no longer makes its base appear to have no geyser. In v7, the
  natural's gas count fell to zero after our refinery was built; the expansion
  mission moved from the natural at `416x2288` to `320x240` across the map. Static
  resource geometry now remains available after refinery construction.
- A visible attack on any owned economy pauses new worker/Nexus growth and
  prioritizes reinforcements, even when the main itself is clear.
- The ranged opener funds its Core once its Gateway and gas are committed,
  before the optional first melee unit. Robotics can overlap the last two of
  four committed ranged defenders. The first Observer and splash units have
  explicit funding checkpoints.
- An established Protoss combined army explicitly funds Storm and its first two
  Templar. A mature contained army can begin this transition without a second
  completed economy. A ready useful Storm takes priority over routine return to
  the defensive boundary.

These changes are cumulative development candidates, not controlled ablations.
Build-only candidates v5, v9, v13, v14 and v17 are not game results.

## Trace evidence and remaining weaknesses

The v18 Protoss win contains 318 empty-Reaver snapshots and 370 accepted reload
moves, with **zero Unable_To_Hit command rejections**. The earlier v16 win had
1,392 such rejections. This establishes that the invalid-command path was
exercised and corrected; it does not measure damage gained from the correction.

The v18 Protoss win had seven sampled large-group backward goals caused by
`join-vanguard`. Its 77 late-Attack snapshots had no sample with at least a
quarter of the older army at home. The subsequent v19 change targets those
backward goals, while retaining regrouping for small and trailing detachments.
The v19 Protoss repeat won at frame 24,740. Its 121 late-Attack snapshots had no
sample with a quarter of the older army at home. No large-group `join-vanguard`
backward goals recurred; the two remaining backward-goal samples were
`attack-target` updates of 200 and 155 pixels. `continue-assault` was exercised
once. The trace had 177 empty-Reaver snapshots, 166 accepted reload moves and
zero Unable_To_Hit rejections. These are bounded observations from one repeat.

The v18 Terran game lost at frame 24,368. The new perimeter rule appeared in
four sampled squad decisions, which does not establish that it improves wins.
Citadel construction began at frame 13,320, but the Archives still lacked its
200 gas requirement as the army weakened; Storm never completed. The v18 Zerg
game lost at frame 8,496, before these late-army policies could solve its opening
failure. Those matchup failures remain unresolved.

In the v16 BananaBrain loss, our observed combat losses cost 7,925 combined
minerals and gas; legally observed enemy combat losses cost 7,525. Seven Storm
commands were accepted, but the economy still collapsed. Better command
correctness and comparable observed trades did not turn into a win.

The final v19 BananaBrain rematch lost at frame 20,400 with a peak of 29 army
units, 22 Probes and one Nexus. No expansion completed. Mineral savings peaked
at 2,124 while technology and the breakout failed to convert that bank into a
second economy. Our known combat losses cost 7,525 combined resources versus
3,325 in legally observed enemy losses (31 own Dragoons lost, 11 enemy Dragoons
observed lost). This final test does **not** support a claim of improved
BananaBrain strength. Winning that matchup remains unfinished.

## Verification and artifacts

Release/Win32 compilation, all five CTest suites, and `scripts/verify.ps1` passed.
The strict verifier includes warnings-as-errors core and adapter compilation,
38 Python tests, privacy checks and whitespace checks. New core regressions cover
priority ownership, attack windup and emergency interruption, retreat across a
defensive boundary and on the next tick, overlapping Storm escape and re-entry,
stable staging positions, splash ammunition, reload travel, defensive perimeter
bounds, guard safety, forward-group ownership and funding milestones.

Current tested candidate: **v19**, DLL SHA-256
`7137f0c23c9a217a47df49931d256bd4f17c81c005340e985098688a113853e2`.

The default DLL at `build/tournament/Release/Protodd.dll` is installed with
this exact hash. The previous installed binary is preserved at
`build/win-pass/previous-installed.dll`.

Per-game manifests, raw/pre-cleanup traces, replays and offline decision reports
are in `build/direct-logs/win-pass-*`. The frozen DLLs, source ZIPs and file-hash
manifests are in `build/win-pass`; `metrics.json`, `unit-value.json` and
`candidates.json` retain the detailed evidence and build history. These local
runtime artifacts are intentionally ignored by Git. The final documentation
snapshot is `candidate-v19-final-source.zip`; its manifest verifies the same
build source and DLL alongside this finalized report.

Enemy losses and received damage are limited to legal observations; they are not
omniscient totals or unit-by-unit kill attribution. Pending games are excluded.
