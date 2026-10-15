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

    auto balanced = expansion;
    for (auto& own : balanced.self.units) {
        if (own.kind == UnitKind::probe && own.id >= 26)
            own.position = {1880 + (own.id % 8) * 8, 480};
    }
    WorkerManager balancedManager;
    for (const auto frame : {0, 12, 24, 96}) {
        balanced.frame = frame;
        const auto assignments = balancedManager.assign(
            balanced, {}, quiet, {}, false, false, &openMap);
        check(transfersTo(assignments, main.id) == 0 &&
                  transfersTo(assignments, natural.id) == 0 &&
                  std::ranges::count(assignments, WorkerJob::minerals,
                                     &WorkerAssignment::job) == 32,
              "equally staffed mineral lines must not trade workers as iteration order rotates");
    }
    auto reversedBalanced = balanced;
    std::ranges::reverse(reversedBalanced.self.units);
    std::ranges::reverse(reversedBalanced.bases);
    const auto reversedJobs = WorkerManager{}.assign(
        reversedBalanced, {}, quiet, {}, false, false, &openMap);
    check(transfersTo(reversedJobs, main.id) == 0 && transfersTo(reversedJobs, natural.id) == 0,
          "balanced mineral staffing must not depend on unit or base enumeration");
    auto uneven = balanced;
    for (auto& own : uneven.self.units) {
        if (own.kind == UnitKind::probe && own.id >= 26 && own.id < 34)
            own.position = {280 + (own.id % 8) * 8, 480};
    }
    const auto unevenJobs = WorkerManager{}.assign(
        uneven, {}, quiet, {}, false, false, &openMap);
    check(transfersTo(unevenJobs, natural.id) > 0 &&
              transfersTo(unevenJobs, natural.id) <= 8 && transfersTo(unevenJobs, main.id) == 0,
          "an uneven economy transfers a bounded group only toward the understaffed base");

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

    // R278: defensive coverage ran72 A* searches while the economic counter
    // reported zero. The worker-phase allowance must cover every route category.
    GameState defenseLoad;
    defenseLoad.frame = 24000; defenseLoad.self.id = 1; defenseLoad.enemy.id = 2;
    defenseLoad.mapWidthPixels = defenseLoad.mapHeightPixels = 1024;
    defenseLoad.bases = {{1, {400, 512}, {384, 384}, 8000, 0, 1, 0, true, false, 8, 0}};
    auto miner = makeUnit(1, UnitKind::probe, {384, 384});
    miner.ours = miner.visible = miner.detected = miner.powered = true;
    miner.hitPoints = miner.maxHitPoints = 20; miner.shields = miner.maxShields = 20;
    miner.topSpeed = 4;
    miner.dimensionLeft = miner.dimensionRight = miner.dimensionUp = miner.dimensionDown = 11;
    miner.groundWeapon = {5, 22, 0, 15, DamageType::normal, false, true};
    defenseLoad.self.units.push_back(miner);
    for (int i = 0; i < 12; ++i) {
        auto defender = miner;
        defender.id = 100+i; defender.kind = UnitKind::zealot;
        defender.position = {400, 400+i*8};
        defender.hitPoints = defender.maxHitPoints = 100;
        defender.shields = defender.maxShields = 60;
        defender.dimensionLeft = defender.dimensionRight = 11;
        defender.dimensionUp = 5; defender.dimensionDown = 13;
        defender.groundWeapon = {8, 22, 0, 15, DamageType::normal, false, true, 2};
        defenseLoad.self.units.push_back(defender);
    }
    for (int i = 0; i < 6; ++i) {
        auto enemy = miner;
        enemy.id = 200+i; enemy.kind = UnitKind::zergling; enemy.ours = false;
        enemy.position = {620, 400+i*8}; enemy.topSpeed = 5;
        enemy.hitPoints = enemy.maxHitPoints = 35; enemy.shields = enemy.maxShields = 0;
        defenseLoad.enemy.units.push_back(enemy);
    }
    auto defenseCells = std::vector<std::uint8_t>(128*128, 1U);
    for (int y = 0; y < 128; ++y) defenseCells[y*128+64] = 0U;
    NavigationGrid defenseMap{128, 128, 8, std::move(defenseCells)};
    WorkerManager defenseManager;
    const auto defenseBefore = NavigationGrid::diagnosticsForCurrentThread().searches;
    const auto defenseJobs = defenseManager.assign(
        defenseLoad, {}, InfluenceMap{}, {}, false, false, &defenseMap);
    const auto defenseSearches = NavigationGrid::diagnosticsForCurrentThread().searches-defenseBefore;
    check(defenseSearches <= 16 && defenseJobs.size() == 1 &&
              defenseJobs.front().job == WorkerJob::minerals,
          "disconnected defender coverage is bounded without interrupting a safe local miner");
    for (const auto load : {RuntimeLoad::normal, RuntimeLoad::reduced, RuntimeLoad::emergency}) {
        const auto limits = workerRoutingLimits(load);
        WorkerManager limitedManager;
        const auto before = NavigationGrid::diagnosticsForCurrentThread();
        const auto jobs = limitedManager.assign(defenseLoad, {}, InfluenceMap{}, {}, false, false,
                                                &defenseMap, limits);
        const auto& stats = limitedManager.routingStats();
        const auto after = NavigationGrid::diagnosticsForCurrentThread();
        const auto searches = after.searches-before.searches;
        check(searches <= static_cast<std::uint64_t>(limits.totalQueries) &&
                  stats.totalPathQueries <= limits.totalQueries &&
                  stats.defensivePathSearches <= limits.defensiveQueries &&
                  stats.expandedNodes <= limits.totalExpansions &&
                  static_cast<std::uint64_t>(stats.expandedNodes) ==
                      after.totalExpansions-before.totalExpansions &&
                  stats.totalPathQueries == stats.economicPathSearches +
                      stats.defensivePathSearches + stats.evacuationPathSearches &&
                  stats.defensiveDeferredChecks > 0 && jobs.size() == 1 &&
                  jobs.front().job == WorkerJob::minerals,
              "every load profile bounds defensive searches, reports deferral and retains local mining");
    }
    auto coveredLoad = defenseLoad;
    coveredLoad.self.units.resize(3); coveredLoad.enemy.units.resize(1);
    coveredLoad.self.units[0].position = {495, 432};
    coveredLoad.self.units[1].position = {480, 400};
    coveredLoad.self.units[2].position = {480, 416};
    coveredLoad.enemy.units[0].position = {620, 432};
    auto coverCells = std::vector<std::uint8_t>(128*128, 1U);
    for (int y = 50; y <= 58; ++y) coverCells[y*128+64] = 0U;
    NavigationGrid coverMap{128, 128, 8, std::move(coverCells)};
    WorkerManager coverManager;
    const auto coveredJobs = coverManager.assign(coveredLoad, {}, InfluenceMap{}, {}, false, false,
        &coverMap, workerRoutingLimits(RuntimeLoad::emergency));
    check(coveredJobs.size() == 1 && coveredJobs.front().job == WorkerJob::minerals &&
              coverManager.routingStats().defensivePathSearches == 2,
          "two proved detours retain local protection and are reused in the militia coverage pass");
    WorkerRoutingLimits deniedLimits{};
    deniedLimits.totalQueries = deniedLimits.totalExpansions = 0;
    auto attackedLoad = coveredLoad;
    attackedLoad.self.units[0].underAttack = true;
    attackedLoad.enemy.units[0].orderTargetId = attackedLoad.self.units[0].id;
    WorkerManager attackedManager;
    const auto attackedJobs = attackedManager.assign(attackedLoad, {}, InfluenceMap{}, {}, false, false,
        &coverMap, deniedLimits);
    const MovementFootprint workerBody{11, 11, 11, 11};
    check(attackedJobs.size() == 1 && attackedJobs.front().job == WorkerJob::evacuate &&
              coverMap.lineWalkable(attackedLoad.self.units[0].position,
                                   attackedJobs.front().targetPosition, workerBody) &&
              attackedManager.routingStats().totalPathQueries == 0,
          "denied route work still gives a directly attacked worker a terrain-clear escape prefix");

    // Gas selection examines every candidate before mineral assignment. A
    // shared local detour must not consume one search for each healthy Probe.
    auto localDetour = expansion;
    localDetour.bases.resize(1);
    localDetour.self.units.erase(localDetour.self.units.begin() + 1);
    localDetour.self.units.push_back(makeUnit(90, UnitKind::assimilator, {340, 560}));
    std::vector<std::uint8_t> localTerrain(75 * 38, 1);
    for (int x = 0; x < 20; ++x) localTerrain[16 * 75 + x] = 0;
    NavigationGrid localDetourMap(75, 38, 32, std::move(localTerrain));
    InfluenceMap localQuiet;
    localQuiet.update(localDetour);
    StrategicPlan localGasPlan;
    localGasPlan.desiredGasWorkers = 3;
    WorkerManager localManager;
    const auto localJobs = localManager.assign(
        localDetour, localGasPlan, localQuiet, {}, false, false, &localDetourMap);
    check(std::ranges::count_if(localJobs, [](const auto& job) {
              return job.job == WorkerJob::gas;
          }) == 3 &&
          std::ranges::count_if(localJobs, [](const auto& job) {
              return job.job == WorkerJob::minerals;
          }) == 29,
          "gas route selection starved healthy local miners behind a shared detour");
    check(localManager.routingStats().economicPathSearches <=
              WorkerManager::maximumEconomicRouteSearches,
          "shared economic detours exceeded the existing search allowance");
    auto isolatedDetour = localDetour;
    isolatedDetour.self.units.push_back(makeUnit(100, UnitKind::probe, {1600, 480}));
    localDetourMap.updateDynamicObstacle(900, {960, 0, 991, 1215});
    const auto isolatedJobs = localManager.assign(
        isolatedDetour, localGasPlan, localQuiet, {}, false, false, &localDetourMap);
    check(std::ranges::none_of(isolatedJobs, [](const auto& job) {
              return job.worker == 100 &&
                  (job.job == WorkerJob::minerals || job.job == WorkerJob::gas);
          }), "a worker across an impassable wall joined another worker's economic proof");
    localDetourMap.removeDynamicObstacle(900);
    localDetour.frame += 12;
    localDetourMap.updateDynamicObstacle(901, {640, 512, 2399, 543});
    const auto closedJobs = localManager.assign(
        localDetour, localGasPlan, localQuiet, {}, false, false, &localDetourMap);
    check(std::ranges::none_of(closedJobs, [](const auto& job) {
              return job.job == WorkerJob::minerals || job.job == WorkerJob::gas;
          }), "economic proofs survived a newly closed detour on the next update");
    localDetourMap.removeDynamicObstacle(901);
    auto detourCannon = makeUnit(101, UnitKind::photonCannon, {680, 528});
    detourCannon.role = UnitRole::staticDefense;
    detourCannon.visible = true;
    detourCannon.hitPoints = detourCannon.maxHitPoints = 100;
    detourCannon.groundWeapon = {.damage = 20, .cooldown = 22,
                               .maxRange = 224, .targetsGround = true};
    localDetour.enemy.units.push_back(detourCannon);
    InfluenceMap threatenedDetour;
    threatenedDetour.update(localDetour);
    const auto threatenedJobs = localManager.assign(
        localDetour, localGasPlan, threatenedDetour, {}, false, false, &localDetourMap);
    check(std::ranges::none_of(threatenedJobs, [](const auto& job) {
              return job.job == WorkerJob::minerals || job.job == WorkerJob::gas;
          }), "economic proofs survived newly observed danger on the detour");

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



    // Neutral ownership metadata can lag an observed/memorized depot. No race
    // may receive bootstrap miners inside an enemy expansion footprint.
    for (const auto depotKind : {UnitKind::nexus, UnitKind::commandCenter,
                                UnitKind::hatchery, UnitKind::lair, UnitKind::hive}) {
        auto occupied = depleted;
        auto depot = makeUnit(800, depotKind, occupied.bases[1].center);
        depot.role = UnitRole::resourceDepot;
        occupied.enemy.units = {depot};
        InfluenceMap occupiedInfluence;
        occupiedInfluence.update(occupied);
        const auto assignments = WorkerManager{}.assign(
            occupied, remotePlan, occupiedInfluence, {}, false, true, &unreadyMap);
        check(transfersTo(assignments, occupied.bases[1].id) == 0,
              "remote mining entered a depot occupied by an enemy race");
        if (depotKind == UnitKind::commandCenter) {
            occupied.enemy.units[0].flying = true;
            const auto lifted = WorkerManager{}.assign(
                occupied, remotePlan, occupiedInfluence, {}, false, true, &unreadyMap);
            check(transfersTo(lifted, occupied.bases[1].id) == 8,
                  "a lifted depot incorrectly occupies a ground mining footprint");
        }
    }

    // A staged neutral-mining group needs time to arrive. Rotating economic
    // retry order must not withdraw healthy outbound workers on the next tick.
    auto bootstrap = depleted;
    WorkerManager bootstrapManager;
    const auto firstBootstrap = bootstrapManager.assign(
        bootstrap, remotePlan, depletedInfluence, {}, false, true, &unreadyMap);
    std::vector<UnitId> admittedBootstrap;
    for (const auto& assignment : firstBootstrap)
        if (assignment.job == WorkerJob::transfer &&
            assignment.baseId == bootstrap.bases[1].id)
            admittedBootstrap.push_back(assignment.worker);
    std::ranges::sort(admittedBootstrap);
    bool bootstrapStable = admittedBootstrap.size() == 8;
    for (int update = 1; update <= 6; ++update) {
        bootstrap.frame = update * 12;
        for (auto& unit : bootstrap.self.units)
            if (std::ranges::find(admittedBootstrap, unit.id) != admittedBootstrap.end())
                unit.position.x += 48;
        const auto assignments = bootstrapManager.assign(
            bootstrap, remotePlan, depletedInfluence, {}, false, true, &unreadyMap);
        std::vector<UnitId> continuing;
        for (const auto& assignment : assignments)
            if (assignment.job == WorkerJob::transfer &&
                assignment.baseId == bootstrap.bases[1].id)
                continuing.push_back(assignment.worker);
        std::ranges::sort(continuing);
        bootstrapStable = bootstrapStable && continuing == admittedBootstrap;
    }
    check(bootstrapStable,
          "healthy neutral-mining bootstrap workers were withdrawn by rotating staged admission");
    auto bootstrapThreat = makeUnit(900, UnitKind::photonCannon, bootstrap.bases[1].center);
    bootstrapThreat.role = UnitRole::staticDefense;
    bootstrapThreat.visible = true;
    bootstrapThreat.hitPoints = bootstrapThreat.maxHitPoints = 100;
    bootstrapThreat.groundWeapon = {.damage = 20, .cooldown = 22,
                                    .maxRange = 224, .targetsGround = true};
    bootstrap.enemy.units.push_back(bootstrapThreat);
    InfluenceMap bootstrapUnsafe;
    bootstrapUnsafe.update(bootstrap);
    const auto unsafeBootstrap = bootstrapManager.assign(
        bootstrap, remotePlan, bootstrapUnsafe, {}, false, true, &unreadyMap);
    check(transfersTo(unsafeBootstrap, bootstrap.bases[1].id) == 0,
          "retained neutral-mining bootstrap workers ignored newly observed route danger");

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


    {
        const auto actor = [](int id,UnitKind kind,Position position) {
            UnitSnapshot u;u.id=id;u.kind=kind;u.position=position;u.completed=true;
            u.role=kind==UnitKind::probe?UnitRole::worker:UnitRole::resourceDepot;
            u.hitPoints=u.maxHitPoints=20;u.shields=u.maxShields=20;u.topSpeed=4.92;return u;
        };
        const auto group=[](const std::vector<WorkerAssignment>& assignments,int base) {
            std::vector<UnitId> ids;for(const auto& a:assignments)
                if(a.job==WorkerJob::transfer&&a.baseId==base)ids.push_back(a.worker);
            std::ranges::sort(ids);return ids;
        };
    GameState s;s.frame=2400;s.mapWidthPixels=3072;s.mapHeightPixels=2048;s.self.id=1;s.enemy.id=2;
    s.bases.push_back({1,{192,192},{224,192},0,0,1,0,true,false,8,0});
    for(int i=0;i<8;++i)s.bases.push_back({2+i,{1808,192+i*128},{1840,192+i*128},8000,0,-1,0,false,false,8,0});
    s.bases.push_back({10,{800,192},{832,192},8000,0,-1,0,false,false,8,0});
    s.self.units.push_back(actor(1,UnitKind::nexus,s.bases[0].center));
    for(int i=0;i<16;++i)s.self.units.push_back(actor(100+i,UnitKind::probe,{192+(i%4)*8,192+(i/4)*8}));
    std::vector<std::uint8_t> terrain(96*64,1);
    for(int y=0;y<64;++y){terrain[y*96+40]=0;if(y!=20)terrain[y*96+12]=0;}
    NavigationGrid map(96,64,32,terrain);InfluenceMap quiet;quiet.update(s);
    check(!map.lineWalkable({192,192},{832,192})&&map.findPath({192,192},{832,192}).reached(),"control nearest line requires a complete safe detour");
    check(!map.findPath({192,192},{1840,192}).reached(),"control earlier neutral line is genuinely disconnected");
    StrategicPlan plan;plan.sustainEconomy=true;plan.expansionTarget=s.bases.back().center;
    WorkerManager manager;auto first=manager.assign(s,plan,quiet,{},false,true,&map);auto admitted=group(first,10);
    check(manager.routingStats().economicPathSearches<=WorkerManager::maximumEconomicRouteSearches,"economic recovery exceeded the existing eight-search budget");
    check(admitted.size()==8,"map-order disconnected bases starved the nearby neutral mining group");
    bool stable=admitted.size()==8;
    for(int i=1;i<=6;++i){s.frame+=12;auto a=manager.assign(s,plan,quiet,{},false,true,&map);stable=stable&&group(a,10)==admitted;}
    check(stable,"bounded neutral mining group never makes stable admission across worker updates");
    auto reversed=s;std::ranges::reverse(reversed.bases);WorkerManager reversedManager;
    auto reversedGroup=group(reversedManager.assign(reversed,plan,quiet,{},false,true,&map),10);
    check(reversedGroup.size()==8,"nearby neutral group failed when it appears first in map enumeration");
    const std::vector<UnitId> leased{100};auto leasedAssignments=WorkerManager{}.assign(s,plan,quiet,leased,false,true,&map);
    check(group(leasedAssignments,10).size()<=8&&
        std::ranges::none_of(leasedAssignments,[](const auto& a){return a.worker==100&&a.job==WorkerJob::transfer;}),"neutral recovery stole an active construction builder");
    auto danger=s;auto cannon=actor(900,UnitKind::photonCannon,{800,192});cannon.role=UnitRole::staticDefense;cannon.visible=true;
    cannon.groundWeapon={.damage=20,.cooldown=22,.maxRange=224,.targetsGround=true};danger.enemy.units.push_back(cannon);
    InfluenceMap unsafe;unsafe.update(danger);
    check(group(WorkerManager{}.assign(danger,plan,unsafe,{},false,true,&map),10).empty(),"neutral recovery ignored live danger at the nearby mining line");
    auto disconnected=s;disconnected.bases.pop_back();InfluenceMap isolated;isolated.update(disconnected);
    auto held=WorkerManager{}.assign(disconnected,plan,isolated,{},false,true,&map);
    check(std::ranges::none_of(held,[](const auto& a){return a.job==WorkerJob::transfer;}),"unreachable neutral lines received unproved transfers");
    auto funded=s;funded.bases.front().mineralsRemaining=8000;InfluenceMap fundedInfluence;fundedInfluence.update(funded);
    auto mining=WorkerManager{}.assign(funded,plan,fundedInfluence,{},false,true,&map);
    check(std::ranges::count_if(mining,[](const auto& a){return a.baseId==1&&a.job==WorkerJob::minerals;})==16,"neutral bootstrap withdrew an established live home mineral line");

        auto freshAtSameFrame=WorkerManager{}.assign(s,plan,quiet,{},false,true,&map);
        check(group(freshAtSameFrame,10)==reversedGroup,
              "neutral recovery depends on source base enumeration at the same frame");
        auto reincarnated=s;
        for(auto& u:reincarnated.self.units)if(std::ranges::find(admitted,u.id)!=admitted.end())u.firstSeen=2401;
        auto expectedFresh=WorkerManager{}.assign(reincarnated,plan,quiet,{},false,true,&map);
        auto afterLifetimeChange=manager.assign(reincarnated,plan,quiet,{},false,true,&map);
        check(group(afterLifetimeChange,10)==group(expectedFresh,10),
              "neutral recovery retained old worker IDs after lifetime changes");
    }

    // Short independent detours must share actual work, not pay the full
    // 12,000-node allowance even when they finish early or hit the route cache.
    for (const auto warmCache : {false, true}) {
        GameState recovery;
        recovery.frame = 2400;
        recovery.mapWidthPixels = recovery.mapHeightPixels = 1024;
        recovery.self.id = 1;
        recovery.enemy.id = 2;
        recovery.self.minerals = 400;
        const auto origin = Position{256, 512};
        recovery.self.units = {makeUnit(10, UnitKind::probe, origin)};
        for (int index = 0; index < 8; ++index) {
            const auto line = Position{480, 440 + index * 8};
            recovery.bases.push_back({index + 1, line, line, 8000, 0, -1, 0,
                                      false, false, 8, 0});
        }
        std::vector<std::uint8_t> cells(128 * 128, 1U);
        for (int row = 50; row <= 70; ++row) cells[row * 128 + 40] = 0U;
        NavigationGrid detours{128, 128, 8, std::move(cells)};
        for (const auto& base : recovery.bases) {
            check(!detours.lineWalkable(origin, base.mineralLine),
                  "economic throughput fixture requires independent detours");
            if (warmCache)
                check(detours.findPath(origin, base.mineralLine, 12000).reached(),
                      "economic throughput cache fixture has a complete route");
        }
        StrategicPlan rebuild;
        rebuild.expansionTarget = {480, 512};
        WorkerManager manager;
        const auto before = NavigationGrid::diagnosticsForCurrentThread();
        const auto jobs = manager.assign(recovery, rebuild, InfluenceMap{}, {}, false, false, &detours);
        const auto after = NavigationGrid::diagnosticsForCurrentThread();
        check(manager.routingStats().economicPathSearches == 8 &&
              jobs.size() == 1 && jobs.front().baseId == 8 &&
              jobs.front().targetPosition == recovery.bases.back().mineralLine,
              "cheap economic detours must reach the nearest rebuild site after eight route queries");
        const auto expanded = after.totalExpansions-before.totalExpansions;
        check(expanded == static_cast<std::uint64_t>(manager.routingStats().expandedNodes) &&
              expanded <= static_cast<std::uint64_t>(workerRoutingLimits(RuntimeLoad::normal).totalExpansions) &&
              (warmCache ? expanded == 0 : expanded > 0),
              "actual expansion accounting includes cold routes and returns unused cache-hit allowances");
        if (!warmCache) {
            // A fresh geometry identity prevents the previous proved paths
            // from turning the exhaustion control into a zero-work cache hit.
            std::vector<std::uint8_t> limitedCells(128 * 128, 1U);
            for (int row = 50; row <= 70; ++row) limitedCells[row * 128 + 40] = 0U;
            NavigationGrid limitedMap{128, 128, 8, std::move(limitedCells)};
            WorkerRoutingLimits limits;
            limits.totalExpansions = 97;
            WorkerManager limited;
            const auto denied = limited.assign(recovery, rebuild, InfluenceMap{}, {}, false, false,
                                                &limitedMap, limits);
            check(limited.routingStats().expandedNodes == 97 &&
                  limited.routingStats().totalPathQueries == 1 &&
                  limited.routingStats().economicDeferredChecks > 0 &&
                  denied.size() == 1 && denied.front().baseId == -1,
                  "exhausted actual node allowance denies unfinished routes without exceeding the cap");
        }
    }

    // One survivor's reachable rebuild site is not a route proof for another
    // survivor stranded across terrain or facing a dangerous approach.
    for (const auto dangerousApproach : {false, true}) {
        GameState recovery;
        recovery.frame = 2400;
        recovery.mapWidthPixels = recovery.mapHeightPixels = 1024;
        recovery.self.id = 1;
        recovery.enemy.id = 2;
        recovery.self.minerals = 400;
        recovery.bases = {{1, {384, 384}, {384, 384}, 8000, 0, -1, 0,
                           false, false, 8, 0}};
        recovery.self.units = {makeUnit(10, UnitKind::probe, {360, 384}),
                               makeUnit(11, UnitKind::probe, {dangerousApproach ? 900 : 700, 384})};
        std::vector<std::uint8_t> cells(128 * 128, 1U);
        if (!dangerousApproach)
            for (int row = 0; row < 128; ++row) cells[row * 128 + 64] = 0U;
        NavigationGrid recoveryMap{128, 128, 8, std::move(cells)};
        InfluenceMap routeInfluence;
        if (dangerousApproach) {
            auto cannon = makeUnit(90, UnitKind::photonCannon, {640, 384});
            cannon.visible = cannon.detected = cannon.powered = true;
            cannon.hitPoints = cannon.maxHitPoints = 100;
            cannon.groundWeapon = {.damage = 20, .cooldown = 22, .maxRange = 64, .targetsGround = true};
            recovery.enemy.units = {cannon};
            routeInfluence.update(recovery);
            check(routeInfluence.at(recovery.bases.front().center).groundThreat <= 0.25F &&
                  routeInfluence.maximumGroundThreat(recovery.self.units.back().position,
                                                    recovery.bases.front().mineralLine) > 0.25F,
                  "rebuild threat fixture has a safe destination and a dangerous remote approach");
        }
        StrategicPlan rebuild;
        rebuild.expansionTarget = recovery.bases.front().center;
        const auto jobs = WorkerManager{}.assign(
            recovery, rebuild, routeInfluence, {}, false, false, &recoveryMap);
        const auto safe = std::ranges::find(jobs, 10, &WorkerAssignment::worker);
        const auto stranded = std::ranges::find(jobs, 11, &WorkerAssignment::worker);
        check(safe != jobs.end() && safe->job == WorkerJob::rebuild && safe->baseId == 1,
              "reachable survivor keeps its affordable rebuild site");
        check(stranded != jobs.end() && stranded->job == WorkerJob::rebuild &&
              stranded->baseId == -1 && !stranded->targetPosition.valid(),
              "stranded survivor must not inherit another worker's rebuild route");
    }

    if (!errors) std::cout << "Worker transfer safety scenarios passed\n";
    return errors ? 1 : 0;
}
