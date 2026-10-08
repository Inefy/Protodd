#include "protodd/Workers.hpp"

#include <algorithm>
#include <iostream>

namespace {

protodd::UnitSnapshot makeUnit(const int id, const protodd::UnitKind kind,
                               const protodd::Position position) {
    protodd::UnitSnapshot unit;
    unit.id = id;
    unit.kind = kind;
    unit.position = position;
    unit.completed = true;
    unit.role = kind == protodd::UnitKind::probe ? protodd::UnitRole::worker :
        kind == protodd::UnitKind::nexus ? protodd::UnitRole::resourceDepot :
        protodd::UnitRole::groundArmy;
    return unit;
}

int transfersTo(const std::vector<protodd::WorkerAssignment>& assignments, const int baseId) {
    return static_cast<int>(std::ranges::count_if(assignments, [baseId](const auto& assignment) {
        return assignment.baseId == baseId && assignment.job == protodd::WorkerJob::transfer;
    }));
}

}  // namespace

int main() {
    using namespace protodd;
    int errors = 0;
    const auto check = [&](const bool value, const char* message) {
        if (!value) { ++errors; std::cerr << message << '\n'; }
    };

    GameState expansion;
    expansion.mapWidthPixels = 2400;
    expansion.mapHeightPixels = 1200;
    expansion.self.id = 1;
    expansion.enemy.id = 2;
    const BaseSnapshot main{1, {256, 512}, {300, 560}, 8000, 0, 1, 0,
                            true, false, 8, 0};
    const BaseSnapshot natural{2, {1900, 512}, {1850, 560}, 8000, 0, 1, 0,
                               false, false, 8, 0};
    expansion.bases = {main, natural};
    expansion.self.units.push_back(makeUnit(1, UnitKind::nexus, main.center));
    expansion.self.units.push_back(makeUnit(2, UnitKind::nexus, natural.center));
    for (int index = 0; index < 32; ++index) {
        expansion.self.units.push_back(makeUnit(10 + index, UnitKind::probe,
            {280 + (index % 8) * 8, 480 + (index / 8) * 8}));
    }
    InfluenceMap quiet;
    quiet.update(expansion);
    NavigationGrid openMap(75, 38, 32, std::vector<std::uint8_t>(75 * 38, 1));
    StrategicPlan growth;
    growth.expansionTarget = natural.center;
    const auto staged = WorkerManager{}.assign(expansion, growth, quiet, {}, false, false,
                                               &openMap);
    check(transfersTo(staged, natural.id) <= 8,
          "a new Nexus receives no more than one bounded group per worker update");
    check(transfersTo(staged, natural.id) > 0 &&
          std::ranges::count_if(staged, [&main](const auto& assignment) {
              return assignment.baseId == main.id &&
                  (assignment.job == WorkerJob::minerals || assignment.job == WorkerJob::transfer);
          }) >= 24,
          "staging leaves most of the established mineral line staffed");
    WorkerManager clearRouteManager;
    const auto clearRouteAssignments = clearRouteManager.assign(
        expansion, growth, quiet, {}, false, false, &openMap);
    check(clearRouteManager.routingStats().economicPathSearches == 0 &&
          clearRouteManager.routingStats().deferredChecks == 0 &&
          transfersTo(clearRouteAssignments, natural.id) > 0,
          "clear economic routes keep transfers active without A* work");

    auto routeLoad = expansion;
    routeLoad.mapWidthPixels = 4096;
    routeLoad.mapHeightPixels = 3072;
    routeLoad.bases = {{5, {3500, 512}, {3500, 560}, 8000, 0, 1, 0,
                       true, false, 8, 0}};
    std::vector<std::uint8_t> routeLoadTerrain(128U * 96U, 1U);
    for (int y = 0; y < 96; ++y)
        routeLoadTerrain[static_cast<std::size_t>(y * 128 + 64)] = 0;
    NavigationGrid routeLoadMap(128, 96, 32, std::move(routeLoadTerrain));
    InfluenceMap routeLoadInfluence;
    routeLoadInfluence.update(routeLoad);
    WorkerManager routeLoadManager;
    const auto beforeRouteLoad = NavigationGrid::diagnosticsForCurrentThread().searches;
    const auto routeLoadAssignments = routeLoadManager.assign(
        routeLoad, {}, routeLoadInfluence, {}, false, false, &routeLoadMap);
    const auto routeLoadSearches = NavigationGrid::diagnosticsForCurrentThread().searches - beforeRouteLoad;
    check(routeLoadSearches <= WorkerManager::maximumEconomicRouteSearches &&
          routeLoadManager.routingStats().economicPathSearches <= WorkerManager::maximumEconomicRouteSearches &&
          routeLoadManager.routingStats().deferredChecks > 0 &&
          routeLoadAssignments.size() == 32 && transfersTo(routeLoadAssignments, 5) == 0,
          "large disconnected economies bound aggregate route work and defer unproved transfers safely");

    GameState spawnAdjacentToNexus;
    spawnAdjacentToNexus.mapWidthPixels = 4096;
    spawnAdjacentToNexus.mapHeightPixels = 3584;
    spawnAdjacentToNexus.self.id = 1;
    spawnAdjacentToNexus.bases.push_back(
        {1, {3808, 464}, {3700, 520}, 8000, 0, 1, 0, true, false, 8, 0});
    auto startingProbe = makeUnit(20, UnitKind::probe, {3760, 520});
    startingProbe.dimensionLeft = 11;
    startingProbe.dimensionRight = 11;
    startingProbe.dimensionUp = 11;
    startingProbe.dimensionDown = 11;
    spawnAdjacentToNexus.self.units.push_back(startingProbe);
    NavigationGrid startClearance(
        512, 448, 8, std::vector<std::uint8_t>(512 * 448, 1));
    startClearance.updateDynamicObstacle(1, {3744, 416, 3872, 512});
    startClearance.updateDynamicObstacle(2, {3688, 500, 3712, 540});
    check(!startClearance.walkable(startingProbe.position,
              {11, 11, 11, 11}),
          "starting Probe footprint overlaps the Nexus-edge navigation cells");
    check(!startClearance.walkable(spawnAdjacentToNexus.bases.front().mineralLine),
          "a mineral-line anchor can lie inside a blocked resource cell");
    InfluenceMap spawnInfluence;
    spawnInfluence.update(spawnAdjacentToNexus);
    const auto spawnMining = WorkerManager{}.assign(
        spawnAdjacentToNexus, {}, spawnInfluence, {}, false, false, &startClearance);
    check(std::ranges::any_of(spawnMining, [](const WorkerAssignment& assignment) {
              return assignment.worker == 20 && assignment.job == WorkerJob::minerals;
          }), "a starting Probe can mine when its clearance footprint touches the Nexus obstacle");

    GameState nearbyExpansion = expansion;
    nearbyExpansion.bases[1].center = {800, 512};
    nearbyExpansion.bases[1].mineralLine = {760, 560};
    nearbyExpansion.self.units[1].position = nearbyExpansion.bases[1].center;
    InfluenceMap nearbyInfluence;
    nearbyInfluence.update(nearbyExpansion);
    const auto nearbyAssignments = WorkerManager{}.assign(
        nearbyExpansion, growth, nearbyInfluence, {}, false, false, &openMap);
    check(transfersTo(nearbyAssignments, natural.id) <= 8,
          "staging also limits transfers to an expansion within 640 pixels");

    GameState disconnected = expansion;
    disconnected.self.units.resize(10);
    for (std::size_t index = 0; index < disconnected.self.units.size(); ++index)
        disconnected.self.units[index].id = static_cast<int>(index + 1);
    disconnected.self.units.resize(10);
    disconnected.bases[1].center = {1600, 512};
    disconnected.bases[1].mineralLine = {1560, 560};
    disconnected.self.units[1].position = disconnected.bases[1].center;
    for (auto& unit : disconnected.self.units) {
        if (unit.kind == UnitKind::probe) unit.position = {300, 512};
    }
    InfluenceMap disconnectedInfluence;
    disconnectedInfluence.update(disconnected);
    std::vector<std::uint8_t> divided(60 * 32, 1);
    for (int y = 0; y < 32; ++y) divided[static_cast<std::size_t>(y * 60 + 30)] = 0;
    NavigationGrid dividedMap(60, 32, 32, std::move(divided));
    const auto disconnectedAssignments = WorkerManager{}.assign(
        disconnected, {}, disconnectedInfluence, {}, false, false, &dividedMap);
    check(transfersTo(disconnectedAssignments, disconnected.bases[1].id) == 0,
          "worker transfers require a reachable terrain path");

    GameState siegeRoute = expansion;
    std::vector<std::uint8_t> corridor(75 * 38, 1);
    for (int y = 0; y < 38; ++y) {
        if (y != 2) corridor[static_cast<std::size_t>(y * 75 + 37)] = 0;
    }
    NavigationGrid corridorMap(75, 38, 32, std::move(corridor));
    const auto requiredDetour = corridorMap.findPath({300, 512}, natural.mineralLine);
    check(requiredDetour.reached() && requiredDetour.points.size() > 20,
          "the second base route exercises the only open gap in a terrain wall");
    if (requiredDetour.reached()) {
        const auto gap = std::ranges::find_if(requiredDetour.points, [](const Position point) {
            return point.x / 32 == 37 && point.y / 32 == 2;
        });
        check(gap != requiredDetour.points.end(), "the terrain route crosses the designated gap");
        const auto siegePosition = gap != requiredDetour.points.end()
            ? *gap : requiredDetour.points[requiredDetour.points.size() / 2];
        auto tank = makeUnit(500, UnitKind::siegeTank, siegePosition);
        tank.groundWeapon = {.damage = 70, .cooldown = 75, .minRange = 64,
                             .maxRange = 384, .targetsGround = true};
        tank.hitPoints = tank.maxHitPoints = 160;
        tank.visible = true;
        siegeRoute.enemy.units = {tank};
        InfluenceMap routeInfluence;
        routeInfluence.update(siegeRoute);
        check(routeInfluence.maximumGroundThreat({300, 512}, natural.mineralLine) <= 0.25F,
              "the observed siege unit threatens the route detour rather than the straight line");
        const auto exposed = WorkerManager{}.assign(siegeRoute, growth, routeInfluence, {},
                                                     false, false, &corridorMap);
        check(transfersTo(exposed, natural.id) == 0,
              "route-level influence vetoes workers through an observed siege line");
    }

    GameState unready;
    unready.mapWidthPixels = 2400;
    unready.mapHeightPixels = 1200;
    unready.self.id = 1;
    unready.bases = {
        {1, {256, 512}, {300, 560}, 2400, 0, 1, 0, true, false, 8, 0},
        {3, {1900, 512}, {1850, 560}, 8000, 0, -1, 0, false, false, 8, 0},
    };
    unready.self.units.push_back(makeUnit(1, UnitKind::nexus, unready.bases[0].center));
    for (int index = 0; index < 12; ++index)
        unready.self.units.push_back(makeUnit(20 + index, UnitKind::probe, {300, 512}));
    InfluenceMap unreadyInfluence;
    unreadyInfluence.update(unready);
    StrategicPlan remotePlan;
    remotePlan.expansionTarget = unready.bases[1].center;
    NavigationGrid unreadyMap(75, 38, 32, std::vector<std::uint8_t>(75 * 38, 1));
    const auto unreadyAssignments = WorkerManager{}.assign(
        unready, remotePlan, unreadyInfluence, {}, false, true, &unreadyMap);
    check(transfersTo(unreadyAssignments, unready.bases[1].id) == 0,
          "planned expansion staging waits until the destination Nexus is complete and owned");

    auto depleted = unready;
    depleted.bases[0].mineralsRemaining = 0;
    InfluenceMap depletedInfluence;
    depletedInfluence.update(depleted);
    const auto emergencyRecovery = WorkerManager{}.assign(
        depleted, remotePlan, depletedInfluence, {}, false, true, &unreadyMap);
    check(transfersTo(emergencyRecovery, depleted.bases[1].id) == 8,
          "a fully depleted home bootstraps only a bounded worker group at safe remote minerals");

    GameState noRefinery = expansion;
    noRefinery.self.units.erase(std::remove_if(noRefinery.self.units.begin(),
        noRefinery.self.units.end(), [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::assimilator;
        }), noRefinery.self.units.end());
    StrategicPlan gasPlan;
    gasPlan.desiredGasWorkers = 6;
    const auto noGasTarget = WorkerManager{}.assign(noRefinery, gasPlan, quiet);
    check(std::ranges::none_of(noGasTarget, [](const WorkerAssignment& assignment) {
        return assignment.job == WorkerJob::gas;
    }), "worker assignments never target a refinery that no longer exists");

    if (!errors) std::cout << "Worker transfer safety scenarios passed\n";
    return errors ? 1 : 0;
}
