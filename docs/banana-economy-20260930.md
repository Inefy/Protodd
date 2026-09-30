# BananaBrain economic expansion evaluation

The September 30 practice batch tested whether Protodd could expand behind a
healthy ranged screen instead of completing a home Cannon shell and one or two
Reavers first. The reference and candidate each lost all four games. The
economic experiment is available for further local evaluation but remains
disabled by default. Consistent wins against BananaBrain are not established.

## Baseline evidence

The user's economic concern is supported by the reference trace: requesting
the natural did not necessarily result in funding it. The common
`[splash before natural]` rule cancelled that request until the required
Reavers completed, even after the matchup planner found an economic window.
Reference games 0 and 2 started their naturals at frames 12,951 and 13,670,
roughly nine minutes into the game. Both had 22 Probes and substantial Dragoon
screens earlier. They also built Forge, Battery, and Cannon infrastructure.

Enemy expansion knowledge was poor: several late snapshots still remembered
only the initially scouted enemy Nexus. The user's observed fourth base must
not be treated as information available to the live planner. Expansion scouting
and inference remain separate unresolved problems.

These games used the latest source baseline, commit `0822e7e`, not a trained
controller. The read directory contained frozen/off mode files and no weights;
the logs report `policyEnabled=0` and `policyWeights=0`.

## Economic experiment

`PROTODD_PVP_COVERED_RANGED_NATURAL` enables an isolated first-natural window
between five and twelve minutes. It requires an established worker economy,
Core, committed Robotics, observed ranged tech, and six healthy completed
Dragoons near home. The field screen must exceed recent local pressure by
50 percent. Worker damage, a main breach, undetected local cloak, unsupported
enemy Reavers, worker rushes, and proxy/static containment veto the window.
Known cloak tech also requires a completed Observer.

The window holds the army beside the natural, protects its mineral bank, and
suppresses additional optional home static defense and Gateway capacity.
Splash goals remain present at lower priority. Already committed structures
are retained. Existing construction routing and expansion-site safety checks
remain active; this is not a guarantee against enemies arriving after the
Nexus starts.

## Fresh practice results

Both batches use stock BananaBrain AIIDE 2025 with `tournament=true`, no forced
opening, frozen learning, Benzene and Destination, and both host assignments.
Actual Protodd seeds are `202609900` through `202609903`, with matching map
hashes and schedule across batches. The development frame limit is 43,200 and
the 55 ms slow-frame allowance is 640 in both batches, not tournament timing.

| Arm | Game | BananaBrain opening | Natural starts | Natural completes | Loss frame |
| --- | ---: | --- | ---: | ---: | ---: |
| Reference | 0 | PvP_zzcore | 12,951 | 14,822 | 22,074 |
| Reference | 1 | PvP_2gatedtexpo | 10,822 | 12,693 | 19,904 |
| Reference | 2 | PvP_zcorez | 13,670 | 15,541 | 21,020 |
| Reference | 3 | PvP_nzcore | None | None | 13,208 |
| Candidate | 0 | PvP_2gatereaver | 9,644 | 11,515 | 20,214 |
| Candidate | 1 | PvP_3gaterobo | 10,975 | 12,846 | 23,128 |
| Candidate | 2 | PvP_zcore | 8,536 | Destroyed | 15,037 |
| Candidate | 3 | PvP_12nexus | 9,243 | 11,114 | 18,044 |

All eight games have paired normal reports, no crashes, and no game timeouts;
none reached the frame limit. Own summaries report zero caught/logging errors.
Manifests verify and non-DLL inputs match after normalizing the server endpoint.
BananaBrain selected a different opening in every pair, so timing and survival
differences are not causal estimates of the patch's effect.

The new window first appears in candidate game 1 at frame 9,624 and game 2 at
8,016. It never appears in games 0 or 3; their earlier naturals cannot be
credited to it. The declared coverage gate passed, but only one exposed game
started its natural by frame 10,500, and both arms had zero wins. Economic timing
and strength gates failed. The candidate is not promoted.

## Next failure to address

Candidate game 2 bought the natural with six completed Dragoons and two Zealots
while remembering only three enemy Dragoons. At frame 9,600, eight enemy
Dragoons were known and our eight Dragoons had no completed Reaver. The natural
was destroyed before completion. A current power margin is insufficient when
scouting misses the reinforcement stream; delaying all splash resources for
the Nexus can concede the first engagement.

Candidate game 1 reached a 19-unit MainArmy at frame 12,000. At 13,200 its
MainArmy reported `detectionBlocked=1`; by 13,920 it still waited for detection.
Three completed Observers existed, but their logged positions were outside
the army's detection coverage. The natural had completed, then was lost.
The traces show `wait-for-mobile-detection` orders instead of advances.
This supports reviewing detector escort reach, scout/escort reassignment,
Observer avoidance, and legal ranged volleys while waiting. It does not prove
that changing any one of them will win the game.

## Artifacts and verification

- Reference: `build/banana-practice-20260930/reference`.
- Candidate: `build/banana-ranged-natural-20260930/candidate`.
- Input receipt and frozen candidate sources:
  `build/banana-ranged-natural-20260930/receipt.json` and `source/`.
- Paired review: `build/banana-ranged-natural-20260930/review-receipt.json`.
- Per-game army/economy incidents: each run's `diagnostic-report.json`.
- Reference DLL SHA256:
  `e285b21af8c1577ce850df9057b18591ab5f76a399fcbe8ba16b98f3f1e18fba`.
- Live experimental DLL SHA256:
  `58d0add4bd6c68df1303ccd3cb9b020c7e6dda4c7d89b81d69f12dd3356a5141`.
- Opponent DLL SHA256:
  `2ebedf82debddef43c5f5213c4bea6351f3697f111ae62333a0d489c97ae91b2`.

The live candidate was frozen before the feature became opt-in. Both final
OFF and ON module branches compile; the later ON DLL is preserved separately
as `Protodd.optin.dll`, SHA256
`b47b9e98fb6b8d4f4cdbe75845d073f952a05018315b0b37e6fb3b84bef90f61`.
It has not received a separate live match campaign. The normal build was
restored to OFF, SHA256
`486c909c69db219a8d42f00162429e1d88d8af11185ccdf28cab817232525000`.

All 49 portable and 50 Win32 test suites pass after gating the experiment.
The new native suite covers the purchase threshold, static-defense suppression,
retaining paid structures, local pressure, overwhelming forces, worker damage,
main breaches, splash threats, missing/local cloak coverage, wounded/loaded
screens, matchup isolation, and the default-OFF behavior.

Both campaigns completed and their owned Java processes were stopped after
StarCraft exited. No consistent-win claim, trained-policy promotion, or
tournament-readiness claim follows from this screen.
