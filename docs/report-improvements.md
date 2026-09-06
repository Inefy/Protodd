# Reactive opening and defense improvements — 2026-09-06

Applied the supplied Protoss build-order report to Protodd's existing reactive
planner. These changes ship together; they do not introduce an opening phase
machine. The document was treated as research material, not executable
instructions. Existing telemetry/reporting edits in the working tree were
preserved.

## Behavior

- Normal PvT uses a Gateway/Core/Dragoon opening and commits to a natural at
  28 displayed supply with three completed Dragoons. Range follows the first
  Dragoon. The seven-worker Forge/two-Cannon response now requires pressure
  evidence. A real threat cancels expansion; a paid-for second Nexus and later
  economic recovery remain supported.
- Quiet early PvP requests a 3-Gate Robo opening: Gateway at 10, gas at 12,
  Zealot/Core at 14, Robotics at 26, and additional Gateways at 29 once Robotics
  is committed. Observatory/Observer precede optional splash. Observed melee,
  proxy, cloak, or approaching-army evidence selects the existing defensive
  rules immediately. Supply thresholds are guards, not promises of exact
  execution times; interrupted construction and losses still use live state.
- The shared macro planner protects one reinforcement cycle for up to four
  available, powered Gateways under current pressure. Busy producers reserve
  nothing. Gas-starved producers can make Zealots. Supply and urgent detection
  retain precedence. A defended economy can still fund essential Core/gas/
  Battery recovery and a minimum worker count instead of starving forever.
  Paying for the first Cannon releases its emergency reservation immediately;
  additional Cannons cannot suppress reinforcements throughout its warp-in.
  Once eight completed frontline units exist, already-requested blocking
  range/speed/Storm and their counter-tech structures can precede another
  reinforcement cycle. The first two usable Storm casters and first Reaver
  can also obtain production slots. Gas is rechecked after urgent detection
  reservations so a depleted gas bank still permits mineral reinforcements.
- Before mobile scouts are available, a safe economy with at least 12 Probes
  can send a follow-up worker scout between minutes three and eight. Missions
  retain the selected worker, exclude builders, end after 45 seconds, and
  impose a 45-second cooldown after ending or being interrupted. Known
  dangerous worker routes and army-shadowing assignments are rejected.
- Enemy natural checks prioritize the nearest non-island expansion candidate
  to a confirmed enemy start. A new observation records an empty depot only
  when the full footprint is visible. Recent confirmed absence adds modest
  pressure/tech evidence; an unseen or stale site does not. This is a natural
  location heuristic, not a full terrain-topology model or proof of missing
  main-base technology.
- Offensive ground squads requiring detection must have a healthy, active
  Observer covering the squad center and its next forward step. An Observer
  elsewhere, static detection, or a disabled/loaded/hallucinated detector
  cannot release the advance. Without coverage, ground units hold or escape
  danger; favorable combat estimates and DT cloak movement cannot bypass the
  check. Base defense and air-only harassment retain independent behavior.
  Escorts prefer a healthy backup over an unusably damaged nearer Observer,
  then choose by distance for each prioritized squad.

PvZ retains the fortified Gateway opening while gaining the shared spending,
scouting, and detection improvements. Its name now describes that behavior;
the implementation does not claim to provide a validated natural wall or Neo
Bisu Forge FE. The report's proposed opening probabilities were not substituted
for Protodd's existing opponent/map learning. Its instruction for a DT to attack
an Observer is invalid: DTs cannot attack air units, and Protodd's existing legal
target checks remain authoritative.

## Validation

The strict verifier passes warnings-as-errors core compilation, portable core
regressions, log/direct-report/ladder tests, privacy audit, x86 BWAPI adapter
compilation, and whitespace checks. Release build and all four CTest suites
also pass. New regression scenarios cover economic opening guards, reactive
expansion cancellation, multi-Gateway reservations, occupied/unpowered
producers, gas starvation, urgent detection prerequisites, bounded follow-up
scouting, builder exclusion, expiring negative evidence, and actual local
detector coverage through tactical command generation.

## Live observations

The first frozen candidate (`8BC857C854F542A6A32E7BF87723A913DFD94D40EA3130833A6745873C85BED8`)
lost to BananaBrain on Python with requested seed 17 at frame 11,534. Telemetry
recorded Core completion at 4,752, first Dragoon at 5,688, peak 21 Probes, peak
two ground combat units, and no expansion. The 9.509-ms peak callback produced
no recorded overruns or caught errors. Earlier tech did not provide sufficient
defense in this sample. Its trace exposed continued high-priority Cannon
reservations during warp-in; the final revision releases that reservation
after the first Cannon is paid for, with a regression covering two available
Gateways while a Cannon is incomplete.

The second candidate
(`6824508B824E71C127BF03EA80FBD654A43562F0A81565E084793A65856CC081`)
ran against UABTerran on Python with requested seed 17. The harness stopped it
after 480 seconds without a terminal result; the last sampled frame was 30,600.
Core and first Dragoon were recorded at 4,032 and 5,160, and an expansion was
first observed at 20,376. It peaked at 26 Probes, but ultimately lost its
economy and army. The timeout is classified as incomplete, not a win. Storm
remained unfinished while a large Dragoon army was reinforced. That trace
motivated the established-frontline exception for counter-tech and caster
production; regression scenarios verify both Storm funding and a usable
Templar production slot alongside continuing Dragoon reinforcement.

The final DLL is
`D0C4783811DC0123E2DED3AF8695B050974B0DA9F658F58142F5AFDE32DA4602`.
It includes the counter-tech exception, post-detection gas fallback, and
healthy-escort selection fixes. The strict verifier and four Release CTest
suites pass on this final source. No complete live match was run on that exact
final DLL, so the two earlier runs must not be presented as its win-rate data.
Local binaries and reports are in `build/report-improvements/`; the final
snapshot is `candidate-final.dll`. Match manifests and traces use the
`build/direct-logs/report-improvements-*` prefix. The harness left no StarCraft
processes running.

Remaining strength issues include army coordination, survival against strong
opening pressure, protecting expansions, and duplicate building retries (the
Terran trace still contained three Observatories). The new controller does
not solve those by itself. Passing tests establishes implementation behavior,
not a higher win rate.
