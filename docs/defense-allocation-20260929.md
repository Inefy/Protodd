# Defense allocation correction - 29 September 2026

The current allocator had two errors that can leave an economy defended by a
small detachment while other mobile fighters remain available:

- It valued an undetected enemy with unavailable HP/shields at the 15% vitality
  floor. Combat evaluation and the influence map already treat this observation
  as unknown health and estimate full vitality.
- It credited nearby Cannons against the entire attack, even when the attackers
  were outside their weapons' range. Multiple Cannons covering one attacker also
  paid for other attackers beyond their coverage.

`allocationPower` now uses conservative vitality for unavailable enemy health
without changing the legal snapshot or granting detection. This consistency
correction remains active in default control.

The static-credit variant requires a visible, detected attacker inside the actual
weapon range, including minimum range and the air/ground weapon choice. It caps
combined static credit by the covered attackers' power times the existing
defensive margin. Cannons retain their screen assignment; this changes mobile
reinforcement allocation. The completed screen did not show a win gain, so this
variant is now behind `PROTODD_STATIC_DEFENSE_COVERAGE`, **off by default**.

The native regression reproduced four failures before the fix. In its distant
three-Tank fixture, four rear Cannons previously reduced the mobile response
from six Dragoons to two. The corrected response remains six. Fully covered
attackers still permit a two-unit mobile guard; covering only one of the three
Tanks requires four Dragoons. Additional cases cover unavailable Dark Templar
health, known wounds, unpowered Cannons, air-weapon coverage, emergency posture,
and deterministic observation ordering. Default-mode tests also ensure that the
unknown-health correction stays active and static coverage stays off.

All 46 Win32 Release and 45 portable-core Release CTest suites passed, including
reruns after isolating the rejected static-credit variant.

## Matched development screen

Frozen packages are under `build/defense-allocation-20260929/`. The reference
was rebuilt from the current pre-fix source, including existing Observer safety
and favorable-front changes. This avoids attributing differences from the older
favorable-front DLL to this correction. The source patches are saved alongside
the packages. Both arms use Steamhammer Terran on Destination, both host sides,
the same schedule, opponent binary, read files and manager/client binaries.
The only package differences are Protodd's DLL and server connection ports.

- Reference DLL SHA-256:
  `2D1CF1251BCF63F663741BF635DDE60D1A2CD5C41AA01456645C45B3A3D6A259`.
- Candidate DLL SHA-256:
  `4D3BB88829CBD9733750512DF92DA9E1B71ADFD775960CA6D584C3AFC3BE60EC`.

Both arms completed with two healthy normal results and archived logs. Actual
seeds 202609900/202609901, map hashes, and starting Nexus positions agree by game
ID. `review.json` records the verified pairing, and both arms retain control and
outcome audits.

| Side | Reference Result | Candidate Result | Reference Peak Ground Army | Candidate Peak Ground Army |
| --- | --- | --- | --- | --- |
| 0 | Loss, frame 27,282 | Loss, frame 30,165 | 22 | 25 |
| 1 | Loss, frame 43,929 | Loss, frame 22,756 | 37 | 20 |

Both arms won **0/2** and neither established a third Nexus. The candidate's
first side lasted longer, but the second lost much earlier. Main-army
detection-blocked samples were 58/161 in the reference and 118/52 in the
candidate; these correlated samples do not measure independent missed wins.
The side-0 candidate still lost nine army units between frames 22,800 and
23,520, while sampled routes mainly covered an expansion that never became a
third economy. The outcome does not support promotion of the larger allocation
change. Preserve this negative result and stop this variant here.

The screened DLL combined the health and static-credit changes. The final
default DLL retains only the unknown-health consistency correction and has not
had a separate health-only live comparison. Its native health cases agree with
full-health threat allocation, but no live win gain is established for it.
The rebuilt default is saved at
`build/defense-allocation-20260929/default/Protodd.dll`, SHA-256
`06CB9BB992227073D74B8F39DAA2F8ED995392B1C1135D7E73F9EFB404B87EF9`.
Both evaluation-switch settings compile, and the development build cache has
been restored to `PROTODD_STATIC_DEFENSE_COVERAGE=OFF`.
The next control work must address detector-safe movement, local engagement,
and actual expansion construction together. Passing regressions or extending a
loss does not establish tournament strength or dominance over every opponent.
