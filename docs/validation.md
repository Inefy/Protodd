# Tournament validation

## Release candidate

- DLL: `build/tournament/Release/AstraBot.dll`
- SHA-256: `A62899AC090A9554277B0D40BC200E3FBEA1AA5BB18AF167F9394625BCE5C997`
- Target: Release, Win32, BWAPI 4.4.0

## Direct UAlbertaBot game

The release candidate was loaded in a two-process BWAPI Local PC game against
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
