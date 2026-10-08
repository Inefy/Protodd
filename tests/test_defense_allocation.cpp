#include "protodd/Squads.hpp"
#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <iostream>
#include <unordered_set>

namespace {
protodd::UnitSnapshot fighter(int id, protodd::UnitKind kind,
                              bool ours, protodd::Position position) {
    using namespace protodd;
    UnitSnapshot unit;
    unit.id = id;
    unit.kind = kind;
    unit.ours = ours;
    unit.position = position;
    unit.completed = unit.visible = unit.detected = unit.powered = true;
    unit.hitPoints = unit.maxHitPoints = 100;
    unit.groundWeapon = {.damage = 20, .cooldown = 30, .maxRange = 192,
                         .targetsGround = true};
    return unit;
}

std::size_t mobileDefenders(const std::vector<protodd::Squad>& squads) {
    using namespace protodd;
    auto count = std::size_t{0};
    for (const auto& squad : squads) {
        if (squad.role != SquadRole::baseDefense) continue;
        count += static_cast<std::size_t>(std::ranges::count_if(squad.units,
            [](const UnitSnapshot& unit) { return !isBuilding(unit.kind); }));
    }
    return count;
}
}

int main() {
    using namespace protodd;
    int failures = 0;
    const auto check = [&failures](bool condition, const char* message) {
        if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };
    GameState state;
    state.self.id = 1;
    state.enemy.id = 2;
    state.frame = 1000;
    state.mapWidthPixels = state.mapHeightPixels = 2048;
    BaseSnapshot base;
    base.id = 1;
    base.center = {500, 500};
    base.mineralLine = {400, 500};
    base.ownerId = state.self.id;
    state.bases = {base};
    StrategicPlan plan;
    plan.posture = Posture::hold;
    plan.rallyPoint = base.center;
    plan.attackTarget = {1800, 1800};
    SquadPlanner planner;
    std::vector<UnitSnapshot> army;
    for (int i = 0; i < 10; ++i)
        army.push_back(fighter(i, UnitKind::dragoon, true, {500 + i * 8, 500}));
    std::vector<UnitSnapshot> enemies;
    for (int i = 0; i < 3; ++i) {
        auto tank = fighter(100 + i, UnitKind::siegeTank, false, {1100, 480 + i * 24});
        tank.lastSeen = state.frame;
        tank.groundWeapon.maxRange = 384;
        enemies.push_back(tank);
    }
    const auto allocate = [&](const auto& friendly, const auto& hostile) {
        return planner.form(state, friendly, hostile, plan, base.center, nullptr, false);
    };
    const auto uncovered = mobileDefenders(allocate(army, enemies));
    check(uncovered == 6, "three healthy tanks request six mobile defenders");

    auto darkTemplars = enemies;
    for (auto& enemy : darkTemplars) {
        enemy.kind = UnitKind::darkTemplar;
        enemy.maxHitPoints = enemy.hitPoints = 80;
        enemy.maxShields = enemy.shields = 40;
        enemy.groundWeapon.maxRange = 15;
        enemy.detected = false;
        enemy.cloaked = true;
    }
    const auto cloakDefense = mobileDefenders(allocate(army, darkTemplars));
    check(cloakDefense == 5, "three healthy Dark Templar request five defenders");
    auto hidden = darkTemplars;
    for (auto& enemy : hidden) {
        enemy.hitPoints = enemy.shields = 0;
    }
    check(mobileDefenders(allocate(army, hidden)) == cloakDefense,
          "unavailable cloaked health cannot shrink the defensive force");
    check(mobileDefenders(planner.form(state, army, hidden, plan, base.center)) == cloakDefense,
          "unknown-health correction remains active in default allocation");
    check(hidden.front().durability() == 0 && !hidden.front().detected,
          "allocation preserves legal health and detection observations");
    auto remoteDt = fighter(300, UnitKind::darkTemplar, true, {1800, 1800});
    remoteDt.cloaked = true;
    auto localDt = remoteDt; localDt.id = 301; localDt.position = base.center;
    const std::vector covertArmy{remoteDt, localDt};
    const auto defending = [&](const auto& squads, int id) {
        return std::ranges::any_of(squads, [id](const Squad& squad) {
            return squad.role == SquadRole::baseDefense &&
                   std::ranges::any_of(squad.units, [id](const UnitSnapshot& member) { return member.id == id; });
        });
    };
    const auto unseenBreach = allocate(covertArmy, darkTemplars);
    check(!defending(unseenBreach, remoteDt.id) && defending(unseenBreach, localDt.id),
          "undetected breach retains nearby DTs without recalling a distant covert raider");
    auto revealedBreach = darkTemplars;
    for (auto& enemy : revealedBreach) enemy.detected = true;
    check(defending(allocate(covertArmy, revealedBreach), remoteDt.id),
          "a detected breach can still recall the DT raid for real defense");
    for (auto& enemy : hidden) enemy.hitPoints = 1;
    check(mobileDefenders(allocate(army, hidden)) == 2,
          "known wounded enemies retain the lower defensive demand");

    auto supported = army;
    for (int i = 0; i < 4; ++i) {
        auto cannon = fighter(200 + i, UnitKind::photonCannon, true, {420, 450 + i * 24});
        cannon.groundWeapon.maxRange = 224;
        supported.push_back(cannon);
    }
    check(mobileDefenders(allocate(supported, enemies)) == uncovered,
          "rear Cannons cannot replace mobile defenders against distant siege");
    check(mobileDefenders(planner.form(state, supported, enemies, plan, base.center)) == uncovered,
          "out-of-range Cannons do not receive defense credit by default");
    auto covered = enemies;
    for (auto& enemy : covered) enemy.position.x = 600;
    check(mobileDefenders(allocate(supported, covered)) == 2,
          "Cannons still reduce mobile demand when all attackers are in range");
    auto partial = enemies;
    partial.front().position.x = 600;
    check(mobileDefenders(allocate(supported, partial)) == 4,
          "four Cannons covering one attacker cannot pay for two uncovered tanks");
    check(mobileDefenders(planner.form(state, supported, partial, plan, base.center)) == 4,
          "default defense allocation credits only the one attacker currently covered");
    auto battery = fighter(250, UnitKind::shieldBattery, true, {430, 500});
    battery.groundWeapon = {};
    battery.airWeapon = {};
    auto batterySupported = army;
    batterySupported.push_back(battery);
    check(mobileDefenders(planner.form(state, batterySupported, enemies, plan, base.center)) ==
              uncovered,
          "Shield Batteries support recharge but cannot claim weapon coverage of attackers");
    for (auto& enemy : covered) enemy.detected = false;
    check(mobileDefenders(allocate(supported, covered)) == uncovered,
          "static credit requires currently targetable attackers");
    for (auto& cannon : supported)
        if (cannon.kind == UnitKind::photonCannon) cannon.powered = false;
    for (auto& enemy : covered) enemy.detected = true;
    check(mobileDefenders(allocate(supported, covered)) == uncovered,
          "unpowered Cannons provide no defensive credit");
    for (auto& cannon : supported)
        if (cannon.kind == UnitKind::photonCannon) cannon.powered = true;
    plan.posture = Posture::defend;
    check(mobileDefenders(allocate(supported, enemies)) == 6 &&
          mobileDefenders(allocate(supported, partial)) == 4,
          "emergency posture preserves coverage limits with its larger defense margin");
    plan.posture = Posture::hold;
    auto airArmy = army;
    auto airScreen = supported;
    for (auto& member : airArmy) {
        member.airWeapon = member.groundWeapon;
        member.airWeapon.targetsAir = true;
    }
    for (auto& member : airScreen) {
        member.airWeapon = member.groundWeapon;
        member.airWeapon.targetsAir = true;
    }
    auto airThreats = enemies;
    for (auto& enemy : airThreats) {
        enemy.kind = UnitKind::wraith;
        enemy.flying = true;
    }
    check(mobileDefenders(allocate(airScreen, airThreats)) ==
              mobileDefenders(allocate(airArmy, airThreats)),
          "distant air raiders cannot borrow rear Cannon ground-weapon coverage");

    GameState routeState;
    routeState.self.id = 1;
    routeState.enemy.id = 2;
    routeState.mapWidthPixels = 2048;
    routeState.mapHeightPixels = 1024;
    auto routeBase = base;
    routeBase.center = {800, 512};
    routeBase.mineralLine = {900, 512};
    routeState.bases = {routeBase};
    std::vector<std::uint8_t> terrain(64U * 32U, 1U);
    for (int y = 0; y < 32; ++y) {
        if (y != 1) terrain[static_cast<std::size_t>(y * 64 + 26)] = 0U;
        terrain[static_cast<std::size_t>(y * 64 + 40)] = 0U;
    }
    NavigationGrid routeGrid(64, 32, 32, std::move(terrain));
    auto slowRemote = fighter(201, UnitKind::dragoon, true, {880, 512});
    slowRemote.topSpeed = 2.1;
    auto fastLocal = fighter(202, UnitKind::zealot, true, {400, 512});
    fastLocal.topSpeed = 3.2;
    auto unreachable = fighter(203, UnitKind::zealot, true, {1600, 512});
    unreachable.topSpeed = 3.2;
    const auto remotePath = routeGrid.findPath(slowRemote.position, {656, 512});
    const auto localPath = routeGrid.findPath(fastLocal.position, {656, 512});
    const auto pathLength = [](const NavigationPathResult& path, const Position from) {
        auto length = 0.0;
        auto previous = from;
        for (const auto point : path.points) {
            length += distance(previous, point);
            previous = point;
        }
        return length;
    };
    auto nearbySlow = fighter(204, UnitKind::zealot, true, {720, 512});
    nearbySlow.topSpeed = 0.5;
    check(remotePath.reached() && localPath.reached() &&
              pathLength(localPath, fastLocal.position) / fastLocal.topSpeed <
                  pathLength(remotePath, slowRemote.position) / slowRemote.topSpeed,
          "fixture makes the farther fighter faster by reachable route arrival");
    const std::vector routeArmy{slowRemote, fastLocal, unreachable, nearbySlow};
    std::vector<UnitSnapshot> routeThreats;
    routeThreats.push_back(fighter(400, UnitKind::marine, false, {800, 480}));
    const auto routeDefense = planner.form(
        routeState, routeArmy, routeThreats, plan, routeBase.center, &routeGrid, false);
    const auto routeGuard = std::ranges::find(routeDefense, SquadRole::baseDefense,
                                               &Squad::role);
    check(routeGuard != routeDefense.end() && routeGuard->units.size() == 2 &&
              std::ranges::any_of(routeGuard->units, [&](const UnitSnapshot& member) {
                  return member.id == fastLocal.id;
              }) &&
              std::ranges::none_of(routeGuard->units, [&](const UnitSnapshot& member) {
                  return member.id == slowRemote.id || member.id == unreachable.id;
              }),
          "defense ranks footprint-clear path arrival over straight-line proximity and skips a disconnected fighter");

    GameState concurrent;
    // A large distant reserve must not multiply route searches without bound.
    std::vector<std::uint8_t> loadTerrain(160U * 128U, 1U);
    for (int y = 0; y < 128; ++y) loadTerrain[static_cast<std::size_t>(y * 160 + 40)] = 0U;
    NavigationGrid loadGrid(160, 128, 32, std::move(loadTerrain));
    std::vector<UnitSnapshot> loadArmy{
        fighter(800, UnitKind::dragoon, true, {640, 480}),
        fighter(801, UnitKind::dragoon, true, {640, 544})};
    for (int i = 0; i < 80; ++i)
        loadArmy.push_back(fighter(900 + i, UnitKind::dragoon, true,
            {1500 + (i % 10) * 64, 256 + (i / 10) * 64}));
    const auto beforeLoad = NavigationGrid::diagnosticsForCurrentThread().searches;
    const auto loadDefense = planner.form(routeState, loadArmy, routeThreats, plan,
        routeBase.center, &loadGrid, false);
    const auto loadSearches = NavigationGrid::diagnosticsForCurrentThread().searches - beforeLoad;
    check(loadSearches <= maximumDefensiveArrivalPathSearches && mobileDefenders(loadDefense) == 2,
          "large defense allocation bounds route searches and keeps confirmed local responders");
    std::ranges::reverse(loadArmy);
    const auto reversedLoadDefense = planner.form(routeState, loadArmy, routeThreats, plan,
        routeBase.center, &loadGrid, false);
    const auto loadIds = [](const auto& squads) {
        std::vector<UnitId> ids;
        for (const auto& squad : squads) if (squad.role == SquadRole::baseDefense)
            for (const auto& member : squad.units) ids.push_back(member.id);
        return ids;
    };
    check(loadIds(loadDefense) == loadIds(reversedLoadDefense),
          "bounded defensive arrival validation is deterministic across snapshot order");
    concurrent.self.id = 1;
    concurrent.enemy.id = 2;
    auto firstBase = base;
    firstBase.id = 1;
    firstBase.center = {500, 500};
    firstBase.mineralLine = {400, 500};
    auto secondBase = base;
    secondBase.id = 2;
    secondBase.center = {1800, 500};
    secondBase.mineralLine = {1900, 500};
    concurrent.bases = {firstBase, secondBase};
    const std::vector concurrentArmy{
        fighter(501, UnitKind::dragoon, true, {600, 500}),
        fighter(502, UnitKind::dragoon, true, {560, 520}),
        fighter(503, UnitKind::dragoon, true, {1700, 500}),
        fighter(504, UnitKind::dragoon, true, {1740, 520}),
    };
    std::vector<UnitSnapshot> concurrentThreats;
    for (int i = 0; i < 2; ++i) {
        const auto offset = i == 0 ? 0 : 1300;
        auto darkTemplar = fighter(600 + i, UnitKind::darkTemplar, false,
                                   {520 + offset, 500});
        darkTemplar.cloaked = true;
        darkTemplar.detected = false;
        concurrentThreats.push_back(darkTemplar);
    }
    auto firstObserver = fighter(701, UnitKind::observer, true, {600, 500});
    firstObserver.flying = true;
    firstObserver.sightRange = 288;
    auto secondObserver = fighter(702, UnitKind::observer, true, {1700, 500});
    secondObserver.flying = true;
    secondObserver.sightRange = 288;
    concurrent.self.units = {firstObserver, secondObserver};
    const auto concurrentDefense = planner.form(
        concurrent, concurrentArmy, concurrentThreats, plan, firstBase.center);
    std::unordered_set<UnitId> assignedDefenders;
    auto defenseSquads = 0;
    auto defenseMembers = 0;
    bool uniqueAssignment = true;
    for (const auto& squad : concurrentDefense) {
        if (squad.role != SquadRole::baseDefense) continue;
        ++defenseSquads;
        for (const auto& member : squad.units) {
            ++defenseMembers;
            uniqueAssignment = assignedDefenders.insert(member.id).second && uniqueAssignment;
        }
    }
    const auto detectorCoverage = planner.allocateDetectors(
        concurrent, concurrentDefense, InfluenceMap{});
    check(defenseSquads == 2 && defenseMembers == 4 && uniqueAssignment,
          "simultaneous base threats receive separate local response groups without double-assigning defenders");
    check(std::ranges::count_if(concurrentDefense, [](const Squad& squad) {
              return squad.role == SquadRole::baseDefense && squad.needsDetection;
          }) == 2,
          "simultaneous cloaked breaches retain independent detection demands");
    check(detectorCoverage.assignments.size() == 2 &&
              detectorCoverage.unmetDetectionDemands == 0 &&
              detectorCoverage.assignments[0].observerId !=
                  detectorCoverage.assignments[1].observerId,
          "detector support is also assigned uniquely across both base-defense commitments");

    std::ranges::reverse(army);
    std::ranges::reverse(enemies);
    check(mobileDefenders(allocate(army, enemies)) == uncovered,
          "allocation is deterministic across snapshot order");
    return failures == 0 ? 0 : 1;
}
