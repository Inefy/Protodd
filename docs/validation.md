# Tournament validation

## September 4 strength-audit candidate

- DLL: `build/tournament/Release/AstraBot.dll`
- Frozen copy: `build/strength-audit/candidate-v5-evidence.dll`
- SHA-256: `E73EE818DFBC130B1CCAE7A588C62EF221C7AA1B3067366A883B375E452A6E88`
- Target: Release, Win32, BWAPI 4.4.0

This is an audit candidate, not a statistically validated competition release.
The strict verifier passes, including core warnings-as-errors, x86 adapter
compilation, two log-analyzer tests, four direct-report tests, seven ladder
tests, privacy, and whitespace. All four Release CTest targets pass: core,
official BWAPI catalog, log analyzer, and direct report. The new core scenarios
contain 29 targeted competition assertions. The catalog compares 28 Protoss
unit/building prices, supply values, and construction times with BWAPI 4.4.0.

The source contains existing uncommitted work plus this audit's changes.
`build/strength-audit/before.patch` preserves the initial diff. These tests ran
on Windows 11 Education, version 10.0.26200. Native Windows 10 validation and
a broader modern-opponent benchmark remain outstanding.

### Iterative direct-match evidence

| Binary | Opponent / map | Result | Interpretation |
| --- | --- | --- | --- |
| Starting source, `546C1237...171` | UABTerran / Destination | Incomplete after 300 seconds | Cleanup wrote an apparent loss; excluded from competitive outcomes. |
| Audit v1, `A257DB50...941` | UABTerran / Destination | Win, frame 22,973 | Most execution/micro corrections loaded and completed a game. |
| Audit v2, `E5E6A128...455` | UABProtoss / Python | Loss, frame 9,705 | Exposed Probe/Battery starvation; corrected afterwards. |
| Audit v3, `EE5E4EFD...7D` | UABProtoss / Python | Incomplete after 900 seconds | Survived and recovered, but failed to close; last state snapshot at frame 56,880. |
| Pre-march candidate, `07CCE871...601` | UABZerg / Benzene | Win, frame 26,321 | Recovered after losing an attack and completed cleanup; precedes the v4 march change. |
| Audit v4, `FD5FF35F...AB9` | UABProtoss / Python, seed 1788550258 | Win, frame 28,677 | Movement revision reached the enemy main and completed cleanup on the earlier stalled run's seed. |
| Current v5, `E73EE818...E88` | UABProtoss / Python, seed 1788550258 | Win, frame 24,368 | Combined movement and evidence fixes completed the game 4,309 frames earlier than v4 on this seed. |
| Current v5, `E73EE818...E88` | BananaBrain AIIDE 2025 / Python, seed 1788550258 | Loss, frame 15,254 | Exposed the opening's poor economy and late ranged transition against stronger pressure. |

The v1 Terran game's measured peak callback was 25.468 ms; the v2 Protoss
game's peak was 0.606 ms. Neither recorded an over-42-ms callback. These are
observations on this machine and these games, not guarantees on tournament
hardware or larger engagements. Do not pool different binaries as evidence
for the frozen candidate's win rate. Unmatched starts and different time
limits also prevent these games from establishing a controlled improvement.

The v3 PvP run built its Battery and reached 12 Probes by frame 5,760, then
two bases with 28 Probes by frame 11,160. It eventually had a max-supply army
and a large bank without finishing the game. That unresolved movement/cleanup
failure prevents treating survival as successful PvP validation. Its raw
shutdown summary recorded a 25.096-ms peak and no threshold overruns; its
shutdown `END,loss,56936` is deliberately excluded from game outcomes.

The pre-march Zerg win recorded a 6.429-ms peak callback and no threshold
overruns. It still showed a late technology transition and one costly army
loss. This is a useful integration result, not evidence that those strategic
weaknesses are solved. The label `audit-final-zerg` was assigned before the
subsequent march change; its DLL hash, not its label, identifies what was tested.

The v4 PvP run's requested and observed seed both equal 1788550258, and its
starting Nexus at `(288, 2800)` matches v3. Its peak callback was 12.449 ms with
no threshold overruns. This supports the movement correction, but one replayed
seed does not establish a win-rate improvement or full opponent determinism.
It also exposed the old rush-label hold persisting at maximum uncertainty;
v5 adds the evidence requirement for that hold.

The current v5 DLL also matched the requested seed and start. It recorded a
10.966-ms peak callback and no threshold overruns. Its win was about three
in-game minutes earlier than v4's on this single seed. This timing observation
does not establish a general win-rate increase. PvT and PvZ still require
repeat testing on the exact v5 binary; their earlier wins used other hashes.

### Stronger-opponent diagnostic

The official AIIDE 2025 BananaBrain package was imported into the ignored
local ladder and run with its published AI configuration/pretraining files.
Its BWAPI 4.4.0 DLL hash is
`2EBEDF82DEBDDEF43C5F5213C4BEA6351F3697F111AE62333A0D489C97AE91B2`.
The manifest records every AI component hash. No BananaBrain implementation
is included in Astra or its source package.

At frame 10,800, Astra had one base, 12 Probes, eight completed Zealots, and
its first Dragoon still in production. BananaBrain had several visible
Dragoons applying pressure. Astra lost the mobile army, then its workers and
static defenses; a Reaver was also visible during the final breakthrough.
The loss completed normally with a 2.766-ms peak callback and no threshold
overruns. This was a playing-strength failure, not a recorded timeout or crash.

The next PvP experiments should fund ranged technology and a sustainable
economy earlier, and keep defensive melee units from following kiting enemies
far beyond Cannon support. Measure those changes against both the UAlbertaBot
rush and BananaBrain across balanced starts. The current candidate should not
be represented as championship-ready based on its UAlbertaBot wins.

Full hashes, opponent/map hashes, timestamps, and outcomes are in
`build/direct-logs/audit-*.json`. Pre-cleanup traces use `.log`; raw cleanup
traces use `.raw.log`. `tools/direct_report.py` treats the JSON manifest as
authoritative and reports per-binary/opponent/map confidence intervals.

## Historical pre-audit Zerg validation

The previously tested DLL had SHA-256
`A62899AC090A9554277B0D40BC200E3FBEA1AA5BB18AF167F9394625BCE5C997`.
It was loaded in a two-process BWAPI Local PC game against
the UAlbertaBot Zerg module on `(2)Benzene.scx`. This avoids treating a
Tournament Manager launch failure as a game result: both StarCraft processes
entered the match, Astra produced live state records, and the opponent used
its race-specific Zerg module.

Result: **AstraBot win at frame 15,688**.

Relevant trace milestones:

- frame 720: the first Pylon was already the protected next macro action at 78
  minerals; it was under construction by frame 1,080;
- frame 2,520: first-seen Pool timing classified `FastRush` before contact and
  switched the plan to the emergency hold;
- frame 2,880: both defensive Cannons were under construction;
- frame 3,600: both Cannons were complete;
- frames 4,320-4,680: the recovery Pylon started and completed without its
  builder being stolen by another structure order;
- frame 6,120: the economy had recovered to 12 Probes while both Gateways and
  the Shield Battery were operating;
- frame 15,689 performance summary: 0.226 ms average, 3.058 ms maximum, and no
  recorded budget overruns or skipped updates.

The full local trace is archived at
`build/direct-logs/release-uab-zerg-win.log` (ignored from source packages).
One game proves the integration path and the tested rush response; it is not a
claim of statistical dominance across every map, spawn, race, or bot.

## Automated checks

`scripts/verify.ps1` passes the C++ core scenarios, ladder-tool tests, log
analyzer tests, and privacy audit. `scripts/build-tournament.ps1` then builds
and tests the actual 32-bit Release/BWAPI configuration used for the DLL.
