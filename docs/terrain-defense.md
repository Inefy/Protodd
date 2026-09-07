# Terrain defense and visible decisions — September 7, 2026 UTC

The user observed excessive Zealot production, surrendering defensible
entrances, and no visible explanation of the bot's decisions. This pass adds
terrain-derived positions for flat chokes and high-ground approaches, corrects
composition spending, and exposes the actual strategic and tactical decisions
in the game. It has not established a win against BananaBrain.

## Army composition

Quiet PvP keeps one explicit Zealot bodyguard, then gives the first two
Dragoons priority over optional Robotics spending. Composition filling now
rejects a unit type already at its requested share. Previously it could keep
buying an affordable Zealot even when every composition deficit was negative;
the nominal Dragoon-heavy plan therefore produced a melee-heavy army.

Against observed Dragoon-heavy pressure, the response limits the melee screen
and saves a Dragoon production cycle even if its gas is not yet available.
The early response favors Dragoons with Reaver support. Observed melee rushes
retain their emergency response. The earlier opening-only experiment lost
despite producing its first Dragoon sooner; see
[opening tempo evidence](opening-tempo-review.md).

## Defensive positions

The bridge samples walkability and ground height, then examines routes from
resource sites toward map center and possible starting locations. It measures
passage width perpendicular to each route and favors a narrow passage or an
upper ground edge. The anchor stays on the home side of the entrance. A flat
choke does not need an elevation change to qualify.

Owned bases select among their approach candidates using visible ground
threats. Holding armies rally to the selected position before contact. Enemy
units near a distant entrance can activate base defense before reaching the
Nexus. Melee defenders do not pursue ranged bait across the entrance boundary;
ranged defenders can fire at enemies already within weapon range. Retreat and
kiting steps inside an active terrain defense stay on its protected side.
An actual breach near the economy still triggers an interception response.

Building placement preserves clearance around the selected entrance and anchor
candidates, in addition to the existing mineral lanes and producer exits.

This is a local defensive-position heuristic, not a complete terrain or
formation model. It uses 32-pixel cells, considers a bounded distance from
each base, and does not prove that every alternate approach is sealed.
Broad open plateaus are rejected when their frontage is too wide for this
defense model. Dynamic unit congestion, precise firing arcs, high-ground hit
probability in simulation, and defending arbitrary midfield objectives remain
limitations. The feature does not claim every map choke can be held at once.

## On-screen indicators

The overlay starts in compact mode. Enter commands in the game's chat input:

| Command | Display |
| --- | --- |
| `/debug` | Cycle compact, detailed, hidden |
| `/debug 0` | Hidden |
| `/debug 1` | Compact |
| `/debug 2` | Detailed |

The panel shows the chosen plan and posture, inferred enemy plan and its
uncertainty, completed Dragoon/Zealot/Reaver counts beside desired shares,
worker/base targets, rally coordinates, expansion selection, macro status,
and the actual queued action's cost and reason. `FUNDED` means resources are
reserved, not that a building has started. A pending expansion also shows its
Probe, remaining distance, lease age, and time since observed movement.

Squad rows show why the bot is holding terrain, retreating, firing and
repositioning, waiting for detection, intercepting a breach, or advancing.
Detailed mode adds local unit counts and the combat estimate beside the
required threshold. These ratios are heuristic estimates, not win
probabilities. No-enemy situations are labeled explicitly.

Cyan map markers show the rally and defensive anchor/area. Yellow marks the
entrance frontage and pursuit limit. Red marks the strategic attack target;
green marks the selected expansion site. Squad lines use green for engage,
yellow for kite, and red for retreat. Selecting an owned unit shows the reason
for its last successfully issued order. The panel sizes itself to its contents.

BWAPI must permit `Flag::UserInput` for command callbacks and selected-unit
inspection. The module requests it at startup. If the tournament host denies
it, the passive overlay remains visible and the footer says controls are
disabled. The commands change display state only.

## Validation and match evidence

All four Release CTest targets pass. The strict verifier passes the core and
adapter checks, 18 Python tests, privacy audit, and whitespace checks. New
regressions cover flat chokes, upper-ground anchors, open-ground rejection,
impassable approaches, pre-contact rallying, distant entrance defense,
melee pursuit limits, ranged shots across the entrance, protected cooldown
movement, interception after a bypass, and saving for ranged production.

The compact overlay was inspected in the running Benzene game. The earlier
detailed panel was visibly too tall; its fixed background has been replaced
with content-dependent sizing and compact mode is the default.

Competitive games fix BananaBrain to `PvP_nzcore` and reset both bots' runtime
learning. Manifests under `build/direct-logs/` are authoritative. The
Destination reference uses the same opponent configuration and requested seed;
different maps/seeds are separate coverage, not a paired strength comparison.

| Candidate / map / seed | Result | Peak Probes / army / Nexuses | First base breach |
| --- | --- | --- | --- |
| Earlier v7 / Destination / 42 | Loss, frame 15,316 | 22 / 16 / 1 | 11,112 |
| Terrain v1 / Destination / 42 | Loss, frame 21,268 | 22 / 14 / 1 | 20,016 |
| Terrain v2 / Benzene / 43 | Loss, frame 21,392 | 22 / 16 / 1 | 19,632 |

The Destination army assembled near the selected entrance at (1424, 3696),
ahead of its Nexus at (2112, 3824). It established a ranged screen and held
the base longer, but the planned expansion never started. The trace shows
repeated pending Nexus attempts while the approach remains contested, and
the army eventually loses to the opponent's larger force. This is evidence
of changed defensive behavior, not evidence of a winning macro transition or
a statistically improved win rate. Peak measured callback time was 28.443 ms,
with no caught errors or 55 ms threshold overruns.

Benzene also stayed on one base despite repeated expansion attempts. Its
peak measured callback time was 18.724 ms, with no caught errors or runtime
threshold overruns. The first completed Dragoon appeared at frame 5,544 and
range at 7,968. The two-map report is `build/terrain-audit/report.json`.

Frozen gameplay candidate v1:
`build/terrain-audit/terrain-v1.dll`, SHA-256
`132BAEDF6C14B586C6167546F37B3EC69BE51DC68F45094EF97973B28F15035E`.

Candidate v2 changes only the overlay presentation and pending-expansion
diagnostics: `build/terrain-audit/terrain-v2.dll`, SHA-256
`78B8105D20574B9AF40AAC51838360D17A589D3399E46FE79777002486CD2B6A`.

Final v3 additionally enables permitted BWAPI input events for the display
commands and reports when the host denies them. It makes no further strategic
or tactical changes. The default Release output is
`build/protodd-tournament/Release/Protodd.dll`, frozen as
`build/terrain-audit/terrain-v3.dll`, SHA-256
`C04F797F25CEE7ED336A224969E91D572FDA1D6B8490FB557116DD3B25DCF63E`.

A separate 120-second `terrain-v3-ui-controls-smoke` run verified the final
DLL's default compact display and a typed `/debug` transition to detailed
mode, including additional macro rows and squad details. This intentionally
bounded UI run is excluded from competitive results; its timeout cleanup
must not be counted as a loss.

The next strategic work needs to turn held territory into a completed second
base: coordinate construction with a cleared route and escort, avoid reserving
expansion resources indefinitely at a contested entrance, and preserve a
coherent ranged/splash army during the move. More opening and map coverage is
also needed before a tournament-strength claim is justified.
