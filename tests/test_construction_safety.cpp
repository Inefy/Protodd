#include "protodd/ConstructionAnchor.hpp"
#include "protodd/BuildTaskProgress.hpp"
#include "protodd/Navigation.hpp"
#include "protodd/Operations.hpp"
#include "protodd/PlacementSafety.hpp"
#include "protodd/RouteSafety.hpp"
#include "protodd/UnitCatalog.hpp"

#include <array>
#include <iostream>

int main() {
    using namespace protodd;
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { ++failures; std::cerr << message << '\n'; }
    };
    const Position home{2112, 3824}, rally{1424, 3696}, site{992, 3472};
    for (auto kind : {UnitKind::pylon, UnitKind::gateway, UnitKind::forge,
                     UnitKind::cyberneticsCore, UnitKind::roboticsFacility,
                     UnitKind::observatory, UnitKind::roboticsSupportBay,
                     UnitKind::stargate, UnitKind::citadelOfAdun,
                     UnitKind::templarArchives, UnitKind::fleetBeacon,
                     UnitKind::arbiterTribunal})
        check(constructionBuilderAnchor(kind, home, rally, site) == home,
              "home infrastructure selected its builder against the threatened forward rally");
    check(constructionBuilderAnchor(UnitKind::nexus, home, rally, site) == site,
          "Nexus builder selection lost its actual resource site");
    check(constructionBuilderAnchor(UnitKind::photonCannon, home, rally, site) == rally,
          "static intercept was relocated to the home tech anchor");
    check(constructionBuilderAnchor(UnitKind::shieldBattery, home, {-1, -1}, site) == home,
          "missing rally did not fall back to home");
    check(homeAnchoredSupplyPylon(UnitKind::pylon,
                                  "operational supply invariant", false),
          "operational supply Pylons share the home anchor used by their travel forecast");
    // An observed Probe can finish an intermediate Move outside the 96px
    // Build handoff radius. A nearby, proved final leg is a new destination,
    // even when it lies within the old 96px last-command deadband.
    NavigationGrid shortLegNavigation(64, 48, 8, std::vector<std::uint8_t>(64 * 48, 1));
    const Position pylonCenter{400, 200};
    shortLegNavigation.updateDynamicObstacle(91, {368, 168, 432, 232});
    const Position firstLeg{444, 92};
    const auto finalApproach = nearestGroundConstructionAccessPoint(
        &shortLegNavigation, firstLeg, pylonCenter, {32, 32, 32, 32}, {8, 8, 8, 8});
    const auto secondLeg = nextGroundRouteWaypoint(
        &shortLegNavigation, firstLeg, finalApproach, {8, 8, 8, 8});
    check(finalApproach.valid() && firstLeg.valid() && secondLeg.valid() &&
        firstLeg != secondLeg && distance(firstLeg, pylonCenter) > 96 &&
        distance(secondLeg, pylonCenter) <= 96 && distance(firstLeg, secondLeg) <= 96,
        "the short-leg fixture crosses the build handoff radius with two nearby proved waypoints");
    check(!activeConstructionWaypointOrder(true, false, firstLeg, secondLeg),
        "an idle Probe's completed Move cannot suppress the next construction route leg");
    check(!activeConstructionWaypointOrder(true, true, firstLeg, secondLeg),
        "a different short construction waypoint must not be mistaken for the old Move destination");
    check(activeConstructionWaypointOrder(true, true, firstLeg, firstLeg),
        "an actually active Move to the same construction waypoint is retained");
    check(!activeConstructionWaypointOrder(true, false, firstLeg, firstLeg),
        "an interrupted or completed same-target Move may be retried after the latency guard");
    check(!activeConstructionWaypointOrder(false, true, firstLeg, firstLeg),
        "a non-Move last command cannot own the construction waypoint");
    check(activeConstructionWaypointOrder(true, true, firstLeg,
                                          {firstLeg.x + 24, firstLeg.y}) &&
          !activeConstructionWaypointOrder(true, true, firstLeg,
                                           {firstLeg.x + 25, firstLeg.y}),
        "active construction orders tolerate small target jitter without suppressing a new leg");
    check(!activeConstructionWaypointOrder(true, true, {-1, -1}, {-1, -1}) &&
          !activeConstructionWaypointOrder(true, true, firstLeg, {-1, -1}),
        "invalid targets never masquerade as an active construction route");
    check(homeAnchoredSupplyPylon(UnitKind::pylon, "maintain a supply buffer", false),
          "ordinary strategy supply Pylons stay at home");
    check(homeAnchoredSupplyPylon(UnitKind::pylon,
                                  "restore power to disabled production", false),
          "unlocated power recovery falls back to a home Pylon");
    check(!homeAnchoredSupplyPylon(UnitKind::pylon,
                                   "power the new PvZ natural before pressure", false),
          "explicit natural power remains anchored at the expansion");
    check(!homeAnchoredSupplyPylon(UnitKind::pylon,
                                   "operational supply invariant", true),
          "site-scoped power recovery keeps its local task anchor");
    check(!homeAnchoredSupplyPylon(UnitKind::pylon,
                                   "give the forward defensive shell redundant power", false),
          "forward-defense Pylons keep their tactical anchor");
    check(!homeAnchoredSupplyPylon(UnitKind::gateway,
                                   "operational supply invariant", false),
          "the supply placement rule does not redirect other structures");
    check(buildTaskTravelDeadlineFrames(UnitKind::pylon, 256, 4920, false) == 8 * 24,
          "urgent local supply retains its short recovery deadline");
    const auto remotePylonDeadline =
        buildTaskTravelDeadlineFrames(UnitKind::pylon, 1280, 4920, false);
    check(remotePylonDeadline > 8 * 24 && remotePylonDeadline < 60 * 24,
          "a distant Pylon receives a route-scaled travel window");
    check(buildTaskTravelDeadlineFrames(UnitKind::photonCannon, 0, 4920, false) == 18 * 24,
          "other constructions retain a bounded minimum acknowledgment lease");
    check(buildTaskTravelDeadlineFrames(UnitKind::pylon, 5000, 4920, true) == 60 * 24,
          "a blocked or orbiting remote worker still hits the hard lease bound");
    check(buildTaskHardTravelDeadlineFrames(UnitKind::pylon, 256, false) == 45 * 24 &&
              buildTaskHardTravelDeadlineFrames(UnitKind::nexus, 1400, false) == 120 * 24,
          "local supply and remote expansion keep distinct absolute travel caps");
    BuildTravelProgress travelProgress{
        .travelDeadline = 1000 + 36 * 24,
        .hardDeadline = 1000 + 120 * 24,
        .bestDistancePixels = 1400};
    check(recordBuildTravelProgress(travelProgress, 1000 + 30 * 24, 1370) &&
              travelProgress.travelDeadline == 1000 + 54 * 24,
          "measurable movement toward a distant footprint extends its travel lease");
    const auto deadlineAfterProgress = travelProgress.travelDeadline;
    check(!recordBuildTravelProgress(travelProgress, 1000 + 40 * 24, 1390) &&
              travelProgress.travelDeadline == deadlineAfterProgress,
          "orbiting away from the best route distance cannot extend the lease");
    check(recordBuildTravelProgress(travelProgress, 1000 + 119 * 24, 1340) &&
              travelProgress.travelDeadline == travelProgress.hardDeadline &&
              !recordBuildTravelProgress(travelProgress, travelProgress.hardDeadline, 1300),
          "route progress extends only up to the absolute travel cap");

    constexpr int navWidth = 30;
    constexpr int navHeight = 12;
    const auto makeOpenNavigation = [] {
        return NavigationGrid(navWidth, navHeight, 32,
            std::vector<std::uint8_t>(navWidth * navHeight, 1));
    };
    auto placementNavigation = makeOpenNavigation();
    PlacementAccessRequirement mineralLane;
    mineralLane.alternatives.push_back({{80, 160}, {880, 160}, {}, {}, 0});
    std::vector<PlacementAccessRequirement> accessRequirements{mineralLane};
    capturePlacementAccessBaselines(placementNavigation, accessRequirements);
    const auto blockedLane = placementPreservesAccess(
        placementNavigation, std::uint64_t{1} << 62U, {480, 0, 511, 383},
        accessRequirements);
    check(!blockedLane && placementNavigation.findPath({80, 160}, {880, 160}).reached(),
          "a candidate that seals the only mineral corridor is rejected and its overlay is removed");
    const auto bypassLane = placementPreservesAccess(
        placementNavigation, std::uint64_t{1} << 62U, {480, 0, 511, 112},
        accessRequirements);
    check(bypassLane,
          "a candidate may touch the old route when the unit-footprint path can safely reroute");

    constexpr int routeWidth = 36;
    constexpr int routeHeight = 20;
    std::vector<std::uint8_t> routeWalkable(routeWidth * routeHeight, 1);
    for (auto y = 0; y < 16; ++y)
        routeWalkable[static_cast<std::size_t>(y * routeWidth + 17)] = 0;
    NavigationGrid constructionRoute(
        routeWidth, routeHeight, 32, std::move(routeWalkable));
    const Position routeStart{144, 336};
    const Position routeGoal{976, 336};
    const auto firstBuildWaypoint = nextGroundRouteWaypoint(
        &constructionRoute, routeStart, routeGoal, {}, 4);
    check(!constructionRoute.lineWalkable(routeStart, routeGoal) &&
              firstBuildWaypoint.valid() && firstBuildWaypoint != routeGoal &&
              constructionRoute.lineWalkable(routeStart, firstBuildWaypoint) &&
              distance(firstBuildWaypoint, routeGoal) < distance(routeStart, routeGoal),
          "a blocked long construction route yields a reachable intermediate command target");

    auto approachNavigation = makeOpenNavigation();
    const Position constructionCenter{480, 192};
    const MovementFootprint gatewayFootprint{64, 64, 48, 48};
    const MovementFootprint probeFootprint{8, 8, 8, 8};
    check(approachNavigation.updateDynamicObstacle(
              79, {constructionCenter.x - gatewayFootprint.left,
                   constructionCenter.y - gatewayFootprint.up,
                   constructionCenter.x + gatewayFootprint.right,
                   constructionCenter.y + gatewayFootprint.down}),
          "the pending structure reserves its exact footprint for route planning");
    const auto approachSearchesBefore = NavigationGrid::diagnosticsForCurrentThread().searches;
    const auto buildApproach = nearestGroundConstructionAccessPoint(
        &approachNavigation, {144, 192}, constructionCenter,
        gatewayFootprint, probeFootprint);
    check(NavigationGrid::diagnosticsForCurrentThread().searches == approachSearchesBefore,
          "a clear construction approach validates directly instead of searching every footprint side");
    check(buildApproach.valid() &&
              distance(buildApproach, constructionCenter) <= 96 &&
              approachNavigation.findPath({144, 192}, buildApproach, 12000,
                                          probeFootprint).reached(),
          "the final staged route ends beside the footprint within construction command range");

    auto builderScreening = makeOpenNavigation();
    const Position distantCenter{864, 192};
    const Position distantBuilder{80, 192};
    builderScreening.updateDynamicObstacle(90, {800, 144, 928, 240});
    InfluenceMap noThreat;
    const auto oldCenterRoute = selectSafeRouteAlternative(
        noThreat, distantBuilder, distantCenter, false, &builderScreening, probeFootprint);
    const auto screeningDestination = groundConstructionScreeningDestination(
        noThreat, &builderScreening, distantBuilder, distantCenter, gatewayFootprint, probeFootprint);
    const auto screenedRoute = selectSafeRouteAlternative(
        noThreat, distantBuilder, screeningDestination, false, &builderScreening,
        probeFootprint, 0.4125, {-1, -1}, 0.25);
    check(!oldCenterRoute.profile.reachable && screenedRoute.profile.reachable &&
          screeningDestination != distantCenter && distance(screeningDestination, distantCenter) <= 96 &&
          screenedRoute.profile.peakThreat <= 0.25,
          "a blocked construction center cannot reject a distant builder with a proved safe adjacent approach");
    const auto clearScreenSearches = NavigationGrid::diagnosticsForCurrentThread().searches;
    const auto clearCenter = groundConstructionScreeningDestination(
        noThreat, &builderScreening, distantBuilder, {480, 192}, gatewayFootprint, probeFootprint);
    check(clearCenter == Position{480, 192} &&
          NavigationGrid::diagnosticsForCurrentThread().searches == clearScreenSearches,
          "walkable construction centers preserve screening and add no access searches");
    GameState guardedApproach;
    guardedApproach.mapWidthPixels = 1024;
    guardedApproach.mapHeightPixels = 384;
    UnitSnapshot approachCannon;
    approachCannon.id = 91;
    approachCannon.kind = UnitKind::photonCannon;
    approachCannon.position = screeningDestination;
    approachCannon.completed = approachCannon.visible = approachCannon.detected = approachCannon.powered = true;
    approachCannon.hitPoints = approachCannon.maxHitPoints = 100;
    approachCannon.groundWeapon = {40, 15, 0, 320, DamageType::normal, false, true};
    guardedApproach.enemy.units.push_back(approachCannon);
    InfluenceMap approachThreat;
    approachThreat.update(guardedApproach);
    const auto unsafeScreenedRoute = selectSafeRouteAlternative(
        approachThreat, distantBuilder, screeningDestination, false, &builderScreening,
        probeFootprint, 0.4125, {-1, -1}, 0.25);
    check(!unsafeScreenedRoute.profile.reachable || unsafeScreenedRoute.profile.peakThreat > 0.25,
          "an occupied-center access point still fails builder screening when its approach is dangerous");
    InfluenceMap threatenedCenter;
    threatenedCenter.resize(1024, 384);
    const std::array centerStorm{Position{distantCenter.x + 80, distantCenter.y}};
    threatenedCenter.updateStorms(centerStorm);
    const auto safeOutsideStorm = selectSafeRouteAlternative(
        threatenedCenter, distantBuilder, screeningDestination, false, &builderScreening,
        probeFootprint, 0.4125, {-1, -1}, 0.25);
    check(safeOutsideStorm.profile.reachable && safeOutsideStorm.profile.peakThreat <= 0.25 &&
          !groundConstructionScreeningDestination(
              threatenedCenter, &builderScreening, distantBuilder, distantCenter,
              gatewayFootprint, probeFootprint).valid(),
          "a safe adjacent approach cannot bypass the original danger check at the structure center");
    builderScreening.updateDynamicObstacle(92, {480, 0, 511, 383});
    check(!groundConstructionScreeningDestination(
              noThreat, &builderScreening, distantBuilder, distantCenter, gatewayFootprint, probeFootprint).valid(),
          "a disconnected construction footprint cannot turn a partial route into a proved builder approach");
    check(groundConstructionScreeningDestination(
              noThreat, nullptr, distantBuilder, distantCenter, gatewayFootprint, probeFootprint) == distantCenter &&
          !groundConstructionScreeningDestination(
              noThreat, &builderScreening, {-1, -1}, distantCenter, gatewayFootprint, probeFootprint).valid(),
          "missing terrain preserves center screening while invalid builder observations stay invalid");

    auto plannedLayout = makeOpenNavigation();
    PlacementAccessRequirement producerExit;
    producerExit.alternatives.push_back({{80, 160}, {880, 160}, {}, {}, 0});
    std::vector<PlacementAccessRequirement> plannedRequirements{producerExit};
    capturePlacementAccessBaselines(plannedLayout, plannedRequirements);
    check(plannedLayout.updateDynamicObstacle(17, {480, 0, 511, 191}),
          "the first planned footprint is present while the second is evaluated");
    const auto combinedBlock = placementPreservesAccess(
        plannedLayout, std::uint64_t{1} << 62U, {480, 192, 511, 383},
        plannedRequirements);
    check(!combinedBlock && plannedLayout.findPath({80, 160}, {880, 160}).reached(),
          "multiple planned footprints cannot close the last producer exit together");
    static_cast<void>(plannedLayout.removeDynamicObstacle(17));
    check(plannedLayout.findPath({80, 160}, {880, 160}).reached(),
          "a failed planned structure releases its footprint and restores the producer exit");

    auto clearanceNavigation = makeOpenNavigation();
    check(clearanceNavigation.updateDynamicObstacle(31, {480, 0, 511, 127}) &&
              clearanceNavigation.updateDynamicObstacle(32, {480, 256, 511, 383}),
          "the baseline corridor has a producer-sized opening");
    PlacementAccessRequirement probeLane;
    probeLane.alternatives.push_back({{80, 144}, {880, 144}, {}, {}, 0});
    std::vector<PlacementAccessRequirement> probeRequirements{probeLane};
    PlacementAccessRequirement reaverExit;
    reaverExit.alternatives.push_back(
        {{80, 144}, {880, 144}, {16, 16, 16, 16}, {}, 0});
    std::vector<PlacementAccessRequirement> reaverRequirements{reaverExit};
    capturePlacementAccessBaselines(clearanceNavigation, probeRequirements);
    capturePlacementAccessBaselines(clearanceNavigation, reaverRequirements);
    check(probeRequirements.front().reachableBefore && reaverRequirements.front().reachableBefore,
          "both workers and large produced units can use the initial exit");
    const auto narrowedProbeLane = placementPreservesAccess(
        clearanceNavigation, std::uint64_t{1} << 62U, {480, 160, 511, 255},
        probeRequirements);
    const auto narrowedReaverExit = placementPreservesAccess(
        clearanceNavigation, std::uint64_t{1} << 62U, {480, 160, 511, 255},
        reaverRequirements);
    check(narrowedProbeLane && !narrowedReaverExit,
          "a corridor that remains open for Probes is rejected when it cannot fit Reavers");

    const auto fighter = [](int id, UnitKind kind, Position position) {
        UnitSnapshot unit;
        unit.id = id; unit.kind = kind; unit.position = position;
        unit.completed = unit.visible = true;
        unit.hitPoints = unit.maxHitPoints = kind == UnitKind::marine ? 40 : 100;
        unit.shields = unit.maxShields = kind == UnitKind::marine ? 0 : 80;
        unit.groundWeapon = {.damage = kind == UnitKind::marine ? 6 : 20,
                             .cooldown = kind == UnitKind::marine ? 15 : 30,
                             .maxRange = kind == UnitKind::marine ? 128 : 192,
                             .targetsGround = true};
        return unit;
    };
    GameState state; state.frame = 11000;
    StrategicPlan plan; plan.expansionTarget = site;
    auto marine = fighter(100, UnitKind::marine, {site.x + 200, site.y});
    marine.lastSeen = state.frame;
    state.enemy.units = {marine};
    for (int i = 0; i < 12; ++i)
        state.self.units.push_back(fighter(i, UnitKind::dragoon, home));
    ExpansionCoordinator coordinator;
    coordinator.update(plan, state, {});
    check(plan.deferExpansion && !coordinator.releaseBuilder() && plan.expansionTarget == site,
          "distant army permitted an exposed Nexus or lost its escort mission");
    coordinator.update(plan, state, {site, true, 12});
    check(plan.deferExpansion && coordinator.releaseBuilder(),
          "unsafe walking builder retained an order that could spend after deferral");
    for (int i = 0; i < 4; ++i) state.self.units[i].position = {site.x + 100, site.y};
    coordinator.update(plan, state, {});
    check(!plan.deferExpansion, "arrived superior local cover did not release construction funding");
    for (auto& unit : state.self.units) unit.loaded = true;
    coordinator.update(plan, state, {});
    check(plan.deferExpansion, "loaded escorts counted as a site screen");
    auto cannon = fighter(150, UnitKind::photonCannon, site);
    cannon.groundWeapon.maxRange = 224;
    state.self.units.push_back(cannon);
    coordinator.update(plan, state, {});
    check(!plan.deferExpansion, "completed local static cover was ignored");
    state.self.units.back().powered = false;
    coordinator.update(plan, state, {});
    check(plan.deferExpansion, "unpowered static cover released Nexus funding");
    state.self.units.pop_back();
    state.enemy.units[0].visible = false;
    coordinator.update(plan, state, {});
    check(plan.deferExpansion, "freshly hidden site threat was forgotten immediately");
    state.frame += 5 * 24 + 1;
    coordinator.update(plan, state, {});
    check(!plan.deferExpansion, "stale invisible mobile threat held expansion funding forever");
    state.enemy.units[0].visible = true;
    auto nexus = fighter(200, UnitKind::nexus, site); nexus.completed = false;
    state.self.units.push_back(nexus);
    coordinator.update(plan, state, {site, true, 500});
    check(!plan.deferExpansion && !coordinator.releaseBuilder(),
          "site safety gate cancelled an already warping Nexus");
    return failures ? 1 : 0;
}
