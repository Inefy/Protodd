#include "protodd/Combat.hpp"
#include "protodd/CommandBus.hpp"
#include "protodd/Harassment.hpp"
#include "protodd/MacroPlanner.hpp"
#include "protodd/Navigation.hpp"
#include "protodd/Technology.hpp"
#include "protodd/Scouting.hpp"
#include "protodd/Workers.hpp"
#include "../src/bwapi/TechnologyProducer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <optional>
#include <string_view>

namespace {
using namespace protodd;
int failures{};
void check(bool value, std::string_view message) {
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
UnitSnapshot unit(int id, UnitKind kind, Position position = {500, 500}, bool ours = true) {
    UnitSnapshot u;
    u.id = id; u.kind = kind; u.position = position; u.ours = ours;
    u.visible = u.completed = u.powered = true;
    u.hitPoints = u.maxHitPoints = 100;
    u.topSpeed = 4.0;
    return u;
}
void navigation() {
    const NavigationGrid blocked{2, 2, 32, {1, 0, 0, 1}};
    check(!blocked.lineWalkable({16, 16}, {48, 48}), "diagonal cannot cut two blocked corners");
    const auto blockedWaypoint = blocked.nextWaypoint({16, 16}, {48, 48});
    check(blockedWaypoint.status == NavigationStatus::partial &&
              blockedWaypoint.waypoint == Position{16, 16},
          "isolated nearby goal holds safely instead of returning the destination");
    const NavigationGrid oneCorner{2, 2, 32, {1, 0, 1, 1}};
    check(!oneCorner.lineWalkable({16, 16}, {48, 48}), "diagonal cannot cut one blocked corner");
    check(oneCorner.findPath({16, 16}, {48, 48}).points.size() == 3,
          "legal orthogonal detour is preserved");
    const NavigationGrid open{3, 3, 32, std::vector<std::uint8_t>(9, 1)};
    check(open.lineWalkable({16, 16}, {80, 80}) && open.lineWalkable({80, 80}, {16, 16}),
          "open diagonal remains legal in both directions");
    check(open.lineWalkable({16, 16}, {80, 16}), "open horizontal remains legal");

    const auto tunnel = [](const int widthInWalkTiles) {
        constexpr auto width = 32;
        constexpr auto height = 24;
        std::vector<std::uint8_t> cells(width * height, 0U);
        for (auto y = 0; y < height; ++y) {
            for (auto x = 0; x < width; ++x) {
                if (x < 8 || x > 23) cells[static_cast<std::size_t>(y * width + x)] = 1U;
            }
        }
        const auto firstRow = 12 - widthInWalkTiles / 2;
        for (auto y = firstRow; y < firstRow + widthInWalkTiles; ++y)
            for (auto x = 8; x <= 23; ++x)
                cells[static_cast<std::size_t>(y * width + x)] = 1U;
        return NavigationGrid{width, height, 8, std::move(cells)};
    };
    const auto passes = [&tunnel](const int width, const MovementFootprint footprint) {
        const auto grid = tunnel(width);
        return grid.findPath({52, 100}, {204, 100}, 12000, footprint).reached();
    };
    const MovementFootprint probe{11, 11, 11, 11};
    const MovementFootprint zealot{11, 11, 5, 13};
    const MovementFootprint dragoon{15, 16, 15, 16};
    const MovementFootprint reaver{16, 15, 16, 15};
    check(!passes(2, probe) && passes(3, probe),
          "Probe collision bounds distinguish a blocked 16-pixel tunnel from a 24-pixel one");
    check(!passes(3, zealot) && passes(5, zealot),
          "Zealot footprint rejects a narrow Probe route but fits its wider lane");
    check(!passes(3, dragoon) && passes(5, dragoon),
          "Dragoon footprint rejects narrow passages and fits its wider lane");
    check(!passes(3, reaver) && passes(5, reaver),
          "Reaver footprint rejects routes sized only for smaller units and fits its wider lane");
}
void dynamicNavigation() {
    constexpr auto width = 64;
    constexpr auto height = 64;
    constexpr auto cellSize = 8;
    const Position from{64, 128};
    const Position to{448, 128};
    NavigationGrid grid{width, height, cellSize,
                        std::vector<std::uint8_t>(width * height, 1U)};
    std::vector<Position> originalRoute;
    check(grid.nextWaypoint(from, to, 7, 5000, {}, &originalRoute).waypoint == to &&
              originalRoute.size() == 2,
          "open terrain caches its direct route");
    const auto initialVersion = grid.obstacleVersion();
    const std::array pylonRoute{from, to};
    const auto remoteFrom = Position{64, 400};
    const auto remoteTo = Position{448, 400};
    std::vector<Position> remoteRoute;
    static_cast<void>(grid.nextWaypoint(remoteFrom, remoteTo, 7, 5000, {}, &remoteRoute));
    check(grid.updateDynamicObstacle(100, {248, 0, 263, 256}),
          "new Pylon footprint changes the obstacle version");
    check(!grid.lineWalkable(from, to),
          "new Pylon immediately closes the direct corridor");
    std::vector<Position> aroundPylon;
    const auto detour = grid.nextWaypoint(from, to, 7, 5000, {}, &aroundPylon);
    check(detour.hasUsableWaypoint() && !aroundPylon.empty() && aroundPylon != originalRoute,
          "cached route is replaced with a reachable Pylon detour");
    check(grid.routeAffectedSince(pylonRoute, from, to, {}, initialVersion),
          "Pylon closure invalidates a cached route through its cells");
    check(!grid.routeAffectedSince(remoteRoute, remoteFrom, remoteTo, {}, initialVersion),
          "distant cached routes survive a local Pylon update");

    const auto afterPylonVersion = grid.obstacleVersion();
    check(!grid.updateDynamicObstacle(100, {248, 0, 263, 256}) &&
              grid.obstacleVersion() == afterPylonVersion,
          "unchanged structure observations do not advance the path version");
    check(grid.updateDynamicObstacle(101, {248, 0, 263, 256}),
          "overlapping planned footprint is reference-counted");
    const auto overlappedVersion = grid.obstacleVersion();
    check(grid.removeDynamicObstacle(100) && grid.obstacleVersion() == overlappedVersion &&
              !grid.lineWalkable(from, to),
          "removing one overlapping footprint does not open a still-occupied route");
    check(grid.removeDynamicObstacle(101) && grid.lineWalkable(from, to),
          "removing the final footprint restores the direct route");

    check(grid.updateDynamicObstacle(200, {248, 112, 263, 144}),
          "remaining mineral field blocks its local walk cells");
    std::vector<Position> aroundMineral;
    static_cast<void>(grid.nextWaypoint(from, to, 7, 5000, {}, &aroundMineral));
    const auto mineralVersion = grid.obstacleVersion();
    check(!grid.lineWalkable(from, to) && !aroundMineral.empty(),
          "route detours around the remaining mineral field");
    check(grid.removeDynamicObstacle(200) && grid.lineWalkable(from, to),
          "depleted mineral field opens the shorter route immediately");
    check(grid.routeAffectedSince(aroundMineral, from, to, {}, mineralVersion),
          "mineral depletion invalidates only nearby cached paths");
    check(!grid.routeAffectedSince(remoteRoute, remoteFrom, remoteTo, {}, mineralVersion),
          "mineral depletion preserves a distant cached route");
}
void meleeTargets() {
    auto zealot = unit(1, UnitKind::zealot);
    zealot.groundWeapon = {8, 22, 0, 15, DamageType::normal, false, true, 2};
    auto near = unit(2, UnitKind::marine, {600, 500}, false);
    auto far = unit(3, UnitKind::marine, {850, 500}, false);
    CombatEvaluator evaluator;
    const auto select = [&](UnitSnapshot target, std::span<const TargetAllocation> allocation = {}) {
        const std::array enemies{target, far};
        const auto* chosen = evaluator.selectTarget(zealot, enemies, allocation);
        return chosen == nullptr ? -1 : chosen->id;
    };
    near.invincible = true;
    check(select(near) == far.id, "invincible nearby unit must not hide a legal melee target");
    near.invincible = false; near.loaded = true;
    check(select(near) == far.id, "loaded nearby unit must not hide a legal melee target");
    near.loaded = false; near.incomingDamage = 100;
    check(select(near) == far.id, "lethal incoming projectile must not suppress another melee target");
    near.incomingDamage = 0;
    const std::array allocation{TargetAllocation{near.id, 100}};
    check(select(near, allocation) == far.id, "fully allocated target must not suppress another melee target");
    check(select(near) == near.id, "a legal close enemy still takes precedence over distant pursuit");
}
void threatAndRoutes() {
    GameState state;
    state.mapWidthPixels = state.mapHeightPixels = 2048;
    auto dt = unit(10, UnitKind::darkTemplar, {512, 512}, false);
    dt.detected = false; dt.cloaked = true;
    dt.groundWeapon = {40, 30, 0, 15, DamageType::normal, false, true};
    state.enemy.units = {dt};
    InfluenceMap influence;
    influence.update(state);
    const auto full = influence.at(dt.position).groundThreat;
    state.enemy.units[0].hitPoints = 0;
    influence.update(state);
    check(full > 0 && std::abs(influence.at(dt.position).groundThreat - full) < 0.000001F,
          "unknown cloaked health must retain conservative map threat");
    check(state.enemy.units[0].hitPoints == 0 && !state.enemy.units[0].detected,
          "threat estimation cannot rewrite legal observations");
    state.enemy.units[0].detected = true;
    influence.update(state);
    check(influence.at(dt.position).groundThreat == 0, "known zero health must not be inflated");
    state.enemy.units[0] = dt; state.enemy.units[0].loaded = true;
    influence.update(state);
    check(influence.at(dt.position).groundThreat == 0, "loaded cargo cannot project weapon threat");

    auto raider = unit(20, UnitKind::zealot, {100, 512});
    auto cannon = unit(21, UnitKind::photonCannon, {600, 512}, false);
    cannon.groundWeapon = {20, 22, 0, 224, DamageType::normal, false, true};
    state.enemy.units = {cannon};
    check(!harassmentRouteSafe(state, raider, {1100, 512}, false), "powered Cannon blocks harassment route");
    state.enemy.units[0].powered = false;
    check(harassmentRouteSafe(state, raider, {1100, 512}, false), "unpowered Cannon cannot block route by weapon threat");
    state.enemy.units[0] = unit(22, UnitKind::marine, {600, 512}, false);
    state.enemy.units[0].groundWeapon = cannon.groundWeapon;
    state.enemy.units[0].hallucination = true;
    check(harassmentRouteSafe(state, raider, {1100, 512}, false), "hallucinated weapon cannot block harassment route");
    state.enemy.units[0].hallucination = false; state.enemy.units[0].loaded = true;
    check(harassmentRouteSafe(state, raider, {1100, 512}, false), "loaded cargo cannot block harassment route");
    state.enemy.units[0].loaded = false;
    raider.kind = UnitKind::darkTemplar; raider.cloaked = true;
    check(harassmentRouteSafe(state, raider, {1100, 512}, false),
          "covert DT may pass ordinary defenders without observed detection");
    raider.underAttack = true;
    check(!harassmentRouteSafe(state, raider, {1100, 512}, false),
          "real incoming attacks revoke the DT covert route assumption");
    raider.underAttack = false;
    state.enemy.units.push_back(unit(23, UnitKind::observer, {600, 512}, false));
    state.enemy.units.back().sightRange = 352;
    check(!harassmentRouteSafe(state, raider, {1100, 512}, false),
          "observed Observer blocks a covert DT route even without a role annotation");
    state.enemy.units.back().hallucination = true;
    check(harassmentRouteSafe(state, raider, {1100, 512}, false),
          "hallucinated Observer cannot detect a DT");
    state.enemy.units.clear(); state.enemy.id = 2;
    state.mapWidthPixels = 960; state.mapHeightPixels = 640;
    std::vector<std::uint8_t> cells(30 * 20, 1);
    for (int y = 0; y < 20; ++y) {
        if (y != 2) cells[y * 30 + 8] = cells[y * 30 + 20] = 0;
        if (y != 17) cells[y * 30 + 14] = 0;
    }
    const NavigationGrid winding{30, 20, 32, cells};
    raider.position = {80, 320}; raider.groundWeapon = dt.groundWeapon;
    BaseSnapshot economy; economy.ownerId = 2; economy.center = {860, 320};
    economy.mineralLine = {850, 320}; economy.mineralsRemaining = 5000; economy.lastScouted = 1;
    state.bases = {economy};
    const auto routed = harassmentOpportunity(state, raider, false, &winding);
    check(routed.target == economy.mineralLine && routed.waypoint.valid() && routed.probing,
          "covert DT routes through multiple terrain bends to a remembered economy");
    state.enemy.units.push_back(unit(25, UnitKind::probe, {100, 320}, false));
    check(harassmentOpportunity(state, raider, false, &winding).target == economy.mineralLine,
          "a lone enemy scout at home cannot bait the DT away from its economic raid");
    state.enemy.units.push_back(unit(24, UnitKind::observer, {270, 80}, false));
    state.enemy.units.back().sightRange = 352;
    check(!harassmentOpportunity(state, raider, false, &winding).target.valid(),
          "terrain path must still reject known detector coverage anywhere along its route");
}
void stormSafety() {
    auto templar = unit(30, UnitKind::highTemplar);
    templar.role = UnitRole::spellcaster; templar.energy = 100;
    std::vector<UnitSnapshot> enemies;
    for (int i = 0; i < 4; ++i) {
        auto muta = unit(40 + i, UnitKind::mutalisk, {650 + i * 8, 500}, false);
        muta.flying = true; enemies.push_back(muta);
    }
    auto ally = unit(50, UnitKind::carrier, {650, 500}); ally.flying = true;
    const std::array squad{templar};
    CombatEstimate estimate; estimate.decision = FightDecision::engage;
    InfluenceMap influence;
    const auto casts = [&](std::span<const UnitSnapshot> support, std::span<const UnitSnapshot> own) {
        const auto commands = TacticalController{}.control(squad, enemies, estimate, {900, 900},
            {100, 100}, influence, {500, 500}, 3, true, {}, TacticalIntent::battle, support, nullptr, own);
        return std::ranges::any_of(commands, [](const Command& c) {
            return c.technology == TechnologyKind::psionicStorm;
        });
    };
    check(casts({}, {}), "clear enemy cluster still receives Storm");
    check(!casts(std::array{ally}, {}), "Storm must protect an allied unit in another squad");
    check(!casts({}, std::array{ally}), "Storm must protect own units supplied by the live adapter");
    std::vector<UnitSnapshot> workers;
    for (int i = 0; i < 24; ++i) workers.push_back(unit(70 + i, UnitKind::probe, {650, 500}));
    check(!casts({}, workers), "Storm must protect workers outside combat squads");
    ally.loaded = true;
    check(casts(std::array{ally}, std::array{ally}), "loaded allies cannot receive Storm damage");
    auto light = unit(99, UnitKind::zealot, {650, 500});
    check(casts(std::array{light}, std::array{light}), "overlapping spans must not double count friendly fire");
}
void upgrades() {
    GameState state; state.self.supplyTotal = 40;
    state.self.units = {unit(1, UnitKind::nexus), unit(2, UnitKind::pylon),
        unit(3, UnitKind::gateway), unit(4, UnitKind::cyberneticsCore),
        unit(5, UnitKind::citadelOfAdun), unit(6, UnitKind::forge)};
    state.self.technologies = {{TechnologyKind::protossGroundWeapons, 1, false}};
    StrategicPlan plan;
    plan.goals = {{GoalKind::upgrade, UnitKind::unknown, 2, 100, true, "weapons", TechnologyKind::protossGroundWeapons}};
    ResourceLedger ledger{150, 200};
    auto actions = MacroPlanner{}.reconcile(state, plan, ledger);
    check(actions.size() == 1 && actions[0].target == UnitKind::templarArchives &&
          actions[0].reserved && actions[0].action == MacroActionKind::build,
          "level two ground upgrade must unlock its Archives before reserving upgrade cost");
    plan.goals[0].blocking = false; ledger = {150, 200};
    check(MacroPlanner{}.reconcile(state, plan, ledger).empty() && ledger.freeMinerals() == 150,
          "optional upgrade must wait for missing level prerequisite without trapping resources");
    plan.goals[0].blocking = true;
    auto archives = unit(7, UnitKind::templarArchives);
    archives.completed = false; archives.buildProgress = 0;
    state.self.units.push_back(archives); ledger = {150, 200};
    check(MacroPlanner{}.reconcile(state, plan, ledger).empty() && ledger.freeMinerals() == 150,
          "early Archives construction must not reserve an unusable upgrade");
    state.self.units.back().buildProgress = 90; ledger = {150, 200};
    actions = MacroPlanner{}.reconcile(state, plan, ledger);
    check(actions.size() == 1 && actions[0].reserved && !actions[0].executable &&
          actions[0].technology == TechnologyKind::protossGroundWeapons,
          "near-complete Archives can reserve but cannot execute level two");
    state.self.units.back().completed = true; ledger = {150, 200};
    actions = MacroPlanner{}.reconcile(state, plan, ledger);
    check(actions.size() == 1 && actions[0].reserved && actions[0].executable &&
          actions[0].technology == TechnologyKind::protossGroundWeapons, "completed Archives permits level two");
    state.self.technologies = {{TechnologyKind::protossGroundWeapons, 0, true}};
    plan.goals = {{GoalKind::upgrade, UnitKind::unknown, 1, 100, true, "armor", TechnologyKind::protossGroundArmor},
                  {GoalKind::train, UnitKind::probe, 1, 90, false, "workers"}};
    ledger = {100, 100}; actions = MacroPlanner{}.reconcile(state, plan, ledger);
    check(actions.size() == 1 && actions[0].target == UnitKind::probe,
          "existing busy-Forge protection keeps worker funding available");
    state.self.technologies.clear(); plan.goals.resize(1);
    auto secondForge = unit(8, UnitKind::forge);
    secondForge.completed = false; secondForge.buildProgress = 0;
    state.self.units.push_back(secondForge); ledger = {100, 100};
    actions = MacroPlanner{}.reconcile(state, plan, ledger);
    check(actions.size() == 1 && actions[0].reserved && actions[0].executable &&
          actions[0].technology == TechnologyKind::protossGroundArmor,
          "a new unfinished Forge cannot block the existing completed Forge");
}
void unavailableWorkers() {
    GameState state; state.self.id = 1; state.enemy.id = 2;
    state.bases = {{1, {500, 500}, {450, 500}, 8000, 5000, 1}};
    state.self.units = {unit(1, UnitKind::probe), unit(2, UnitKind::probe),
                       unit(3, UnitKind::probe), unit(4, UnitKind::probe),
                       unit(5, UnitKind::pylon)};
    state.self.units[0].loaded = true;
    state.self.units[1].disabled = true;
    state.self.units[2].hallucination = true;
    InfluenceMap influence; influence.update(state);
    const auto assignments = WorkerManager{}.assign(state, {}, influence);
    check(assignments.size() == 1 && assignments[0].worker == 4,
          "loaded, disabled and hallucinated workers cannot fill economic assignments");
    check(selectOpeningWorkerScout(state, {}, {}) == 4,
          "opening scout selection must skip unavailable workers");
    state.self.units[0].loaded = false;
    check(selectOpeningWorkerScout(state, {}, {}) == 1, "unloaded worker becomes eligible again");
}
void commandIdentity() {
    CommandBus bus; bus.beginFrame(100, 3);
    Command spell{1, CommandType::useTech, -1, {500, 500}, UnitKind::unknown,
                  98, 0, "spell", TechnologyKind::stasisField};
    bus.markIssued(spell); bus.beginFrame(101, 3);
    bus.submit(spell); check(bus.finalize().empty(), "identical spell remains suppressed within latency window");
    bus.beginFrame(101, 3); spell.technology = TechnologyKind::recall; bus.submit(spell);
    check(bus.finalize().size() == 1, "different technology is not the same command");
}
void navigationOutcomes() {
    constexpr auto width = 20;
    constexpr auto height = 20;
    constexpr auto cellSize = 8;
    const Position from{20, 20};
    const Position to{140, 140};
    NavigationGrid open{width, height, cellSize,
        std::vector<std::uint8_t>(width * height, 1U)};
    const auto reached = open.findPath(from, to, 400);
    check(reached.status == NavigationStatus::reached &&
              !reached.points.empty() && reached.points.back() == to,
          "complete A-star route reports reached");
    NavigationGrid boundedSearch{width, height, cellSize,
        std::vector<std::uint8_t>(width * height, 1U)};
    const auto partial = boundedSearch.findPath(from, to, 1);
    check(partial.status == NavigationStatus::partial && partial.points.size() >= 2,
          "bounded A-star returns a valid partial progress route when available");
    for (std::size_t i = 1; i < partial.points.size(); ++i)
        check(boundedSearch.lineWalkable(partial.points[i - 1], partial.points[i]),
              "partial route contains only walkable legs");
    std::vector<std::uint8_t> boundedDetour(width * height, 1U);
    for (auto y = 0; y < height; ++y) {
        if (y != 0)
            boundedDetour[static_cast<std::size_t>(y * width + width / 2)] = 0U;
    }
    NavigationGrid boundedGrid{width, height, cellSize, std::move(boundedDetour)};
    const auto partialWaypoint = boundedGrid.nextWaypoint({44, 84}, {116, 84}, 7, 1);
    check(partialWaypoint.status == NavigationStatus::partial &&
              partialWaypoint.hasUsableWaypoint() &&
              boundedGrid.lineWalkable({44, 84}, partialWaypoint.waypoint),
          "next waypoint exposes safe bounded-search progress");
    const auto exhaustedWaypoint = boundedGrid.nextWaypoint({44, 84}, {116, 84}, 7, 0);
    check(exhaustedWaypoint.status == NavigationStatus::budgetExhausted &&
              !exhaustedWaypoint.hasUsableWaypoint() &&
              exhaustedWaypoint.waypoint == Position{-1, -1},
          "budget exhaustion provides no unsafe destination fallback");
    check(open.findPath(from, to, 0).status == NavigationStatus::budgetExhausted,
          "zero expansions report an inconclusive budget exhaustion");
    check(open.findPath({-1, 20}, to).status == NavigationStatus::invalidInput,
          "out-of-map coordinates report invalid input");
    check(open.nextWaypoint(from, to, 7, -1).status == NavigationStatus::invalidInput,
          "negative search budget reports invalid input");
    check(NavigationGrid{}.nextWaypoint(from, to).status == NavigationStatus::invalidInput,
          "empty navigation grid does not return the destination as a fallback waypoint");

    constexpr auto disconnectedWidth = 80;
    constexpr auto disconnectedHeight = 20;
    std::vector<std::uint8_t> separated(disconnectedWidth * disconnectedHeight, 1U);
    for (auto y = 0; y < disconnectedHeight; ++y)
        separated[static_cast<std::size_t>(y * disconnectedWidth + disconnectedWidth / 2)] = 0U;
    NavigationGrid disconnected{disconnectedWidth, disconnectedHeight, cellSize,
                                std::move(separated)};
    const auto unreachable = disconnected.findPath({40, 80}, {604, 80}, 2000);
    check(unreachable.status == NavigationStatus::unreachable &&
              disconnected.nextWaypoint({40, 80}, {604, 80}, 7, 2000).waypoint ==
                  Position{-1, -1},
          "exhausted search distinguishes proven isolation from budget exhaustion");
    const std::array cleanupCandidates{Position{604, 80}, Position{180, 80}};
    check(disconnected.nearestReachableTarget({40, 80}, cleanupCandidates, 2000) ==
              Position{180, 80},
          "cleanup target selection skips a proven-unreachable objective for a reachable legal search site");
    const std::array isolatedCandidate{Position{604, 80}};
    check(!disconnected.nearestReachableTarget({40, 80}, isolatedCandidate, 2000).valid(),
          "cleanup target selection reports when no supplied objective is reachable");
}
void navigationWorkspaceReuse() {
    constexpr auto width = 73;
    constexpr auto height = 61;
    NavigationGrid grid{width, height, 8,
        std::vector<std::uint8_t>(width * height, 1U)};
    const Position from{36, 244};
    const Position to{548, 244};
    const auto before = NavigationGrid::diagnosticsForCurrentThread();
    const auto first = grid.findPath(from, to, 12000);
    const auto afterFirst = NavigationGrid::diagnosticsForCurrentThread();
    const auto second = grid.findPath(from, to, 12000);
    const auto afterSecond = NavigationGrid::diagnosticsForCurrentThread();
    check(first.reached() && second.reached() && first.points == second.points,
          "cached shared route preserves the completed path");
    check(afterFirst.searches == before.searches + 1U &&
              afterSecond.searches == afterFirst.searches &&
              afterSecond.routeCacheHits == afterFirst.routeCacheHits + 1U &&
              afterFirst.workspaceResizes == before.workspaceResizes + 1U &&
              afterSecond.workspaceResizes == afterFirst.workspaceResizes,
          "repeated exact query reuses a version-checked route without another A-star search");

    check(grid.updateDynamicObstacle(8400, {8, 8, 15, 15}),
          "distant dynamic blocker updates the route revision");
    const auto remoteChange = grid.findPath(from, to, 12000);
    const auto afterRemoteChange = NavigationGrid::diagnosticsForCurrentThread();
    check(remoteChange.points == first.points &&
              afterRemoteChange.searches == afterSecond.searches &&
              afterRemoteChange.routeCacheHits == afterSecond.routeCacheHits + 1U,
          "versioned cache retains a route untouched by a distant obstacle");

    check(grid.updateDynamicObstacle(8401, {280, 236, 295, 251}),
          "dynamic blocker updates the route revision");
    const auto detour = grid.findPath(from, to, 12000);
    const auto afterDetour = NavigationGrid::diagnosticsForCurrentThread();
    check(detour.reached(), "cached route invalidation still finds a complete detour");
    check(detour.points != first.points,
          "dynamic obstacle changes the cached route geometry");
    auto detourSafe = true;
    for (std::size_t i = 1; i < detour.points.size(); ++i)
        detourSafe = detourSafe && grid.lineWalkable(detour.points[i - 1U], detour.points[i]);
    check(detourSafe, "recomputed route clears the dynamic obstacle");
    check(afterDetour.searches == afterRemoteChange.searches + 1U &&
              afterDetour.routeCacheHits == afterRemoteChange.routeCacheHits &&
              afterDetour.workspaceResizes == afterFirst.workspaceResizes,
          "stale cached route is recomputed instead of returned as a cache hit");
    check(afterDetour.searchWorkspaceBytes ==
              static_cast<std::size_t>(width * height) * 20U &&
              afterDetour.searchWorkspaceBytes <= NavigationGrid::maximumSearchWorkspaceBytes &&
              afterDetour.cachedRouteBytes <= NavigationGrid::maximumCachedRouteBytes,
          "reusable search and route-cache storage obey their strict memory ceilings");

    // The engine assigns a fresh map into the same NavigationGrid member
    // between games. A previously safe detour remains walkable on an open
    // replacement map, so geometry validation alone cannot identify its age.
    grid = NavigationGrid{width, height, 8,
        std::vector<std::uint8_t>(width * height, 1U)};
    const auto beforeReplacement = NavigationGrid::diagnosticsForCurrentThread();
    const auto replacementPath = grid.findPath(from, to, 12000);
    const auto afterReplacement = NavigationGrid::diagnosticsForCurrentThread();
    NavigationGrid freshMap{width, height, 8,
        std::vector<std::uint8_t>(width * height, 1U)};
    const auto freshPath = freshMap.findPath(from, to, 12000);
    check(replacementPath.reached() && replacementPath.points == freshPath.points &&
              replacementPath.points != detour.points,
          "a fresh map at the same grid address discards the prior game's still-walkable detour");
    check(afterReplacement.searches == beforeReplacement.searches + 1U &&
              afterReplacement.routeCacheHits == beforeReplacement.routeCacheHits,
          "a new terrain lifetime cannot reuse a route solely because address and dimensions match");
    const auto repeatedReplacement = grid.findPath(from, to, 12000);
    const auto afterRepeatedReplacement = NavigationGrid::diagnosticsForCurrentThread();
    check(repeatedReplacement.points == replacementPath.points &&
              afterRepeatedReplacement.routeCacheHits == afterReplacement.routeCacheHits + 1U,
          "ordinary repeated queries still reuse the fresh map's route after replacement");

    std::optional<NavigationGrid> recycledGrid;
    recycledGrid.emplace(width, height, 8,
        std::vector<std::uint8_t>(width * height, 1U));
    recycledGrid->updateDynamicObstacle(8410, {280, 236, 295, 251});
    const auto recycledDetour = recycledGrid->findPath(from, to, 12000);
    check(recycledDetour.reached() && recycledDetour.points != freshPath.points,
          "the reconstructed-grid fixture first caches a real completed detour");
    recycledGrid.reset();
    recycledGrid.emplace(width, height, 8,
        std::vector<std::uint8_t>(width * height, 1U));
    const auto beforeReconstruction = NavigationGrid::diagnosticsForCurrentThread();
    const auto reconstructedPath = recycledGrid->findPath(from, to, 12000);
    const auto afterReconstruction = NavigationGrid::diagnosticsForCurrentThread();
    check(reconstructedPath.points == freshPath.points &&
              afterReconstruction.searches == beforeReconstruction.searches + 1U &&
              afterReconstruction.routeCacheHits == beforeReconstruction.routeCacheHits,
          "destroying and reconstructing terrain at the same address cannot inherit an old valid detour");

    NavigationGrid oversized{1025, 1024, 8, {}};
    check(oversized.empty(), "oversized navigation maps are rejected before scratch allocation");
}
void reachableTargetSnapping() {
    constexpr auto width = 30;
    constexpr auto height = 16;
    constexpr auto cellSize = 32;
    std::vector<std::uint8_t> walkable(width * height, 1U);
    for (auto y = 0; y < height; ++y)
        walkable[static_cast<std::size_t>(y * width + 15)] = 0U;
    walkable[static_cast<std::size_t>(8 * width + 17)] = 0U;
    NavigationGrid grid{width, height, cellSize, std::move(walkable)};
    const Position from{112, 272};
    const Position blockedTarget{560, 272};
    const auto radialFirst = grid.nearestWalkable(blockedTarget);
    check(radialFirst.valid() && radialFirst.x / cellSize > 15,
          "geometric radial scan selects its first target-side cell across the wall");

    const auto route = grid.findPath(from, blockedTarget, 4000);
    auto clear = route.hasUsablePath() && route.points.size() >= 2U &&
                 route.points.back().x / cellSize < 15;
    for (std::size_t i = 1; clear && i < route.points.size(); ++i)
        clear = grid.lineWalkable(route.points[i - 1U], route.points[i]);
    check(route.status == NavigationStatus::partial && clear,
          "blocked target snaps to safe progress in the origin's connected region");
    const auto reachable = grid.nearestReachable(from, blockedTarget, 4000);
    check(reachable.status == NavigationStatus::partial &&
              reachable.hasUsableWaypoint() && !route.points.empty() &&
              reachable.waypoint == route.points.back(),
          "nearest reachable target exposes the validated route endpoint");

    const Position isolatedWalkableTarget{560, 240};
    const auto isolated = grid.findPath(from, isolatedWalkableTarget, 4000);
    check(isolated.status == NavigationStatus::partial &&
              !isolated.points.empty() && isolated.points.back().x / cellSize < 15,
          "an exact but isolated target falls back to reachable nearby terrain");

    const MovementFootprint wide{20, 20, 20, 20};
    const auto clearance = grid.findPath(from, blockedTarget, 4000, wide);
    check(clearance.status == NavigationStatus::partial &&
              !clearance.points.empty() && clearance.points.back().x / cellSize < 14,
          "target-region search applies the mover's footprint at the wall edge");
}
void legalTechnologyProducer() {
    struct Producer {
        int id; bool alive; bool completed; bool powered; bool researching; bool upgrading;
        bool exists() const { return alive; }
        bool isCompleted() const { return completed; }
        bool isPowered() const { return powered; }
        int getID() const { return id; }
        // BWAPI canUpgrade can report true for an unpowered Forge. Power is
        // a separate execution guard, verified in the UMS engine fixture.
        bool legal() const { return !researching && !upgrading; }
    };
    Producer first{1, true, true, false, false, false};
    Producer second{2, true, true, true, false, false};
    const std::array producers{&first, &second};
    const auto choose = [&] {
        return bwapi::technologyProducer(producers, [](const Producer* p) { return p->legal(); });
    };
    check(choose() == &second, "unpowered lower-ID building cannot hide commandable producer");
    first.powered = true; first.researching = true;
    check(choose() == &second, "researching building cannot hide idle upgrade producer");
    first.researching = false; first.upgrading = true;
    check(choose() == &second, "upgrading building cannot hide idle research producer");
    first.upgrading = false;
    check(choose() == &first, "legal producer choice preserves stable ID ordering");
    first.alive = false; second.completed = false;
    check(choose() == nullptr, "dead and unfinished producers cannot receive commands");
}
void ammunitionAndTargetDomain() {
    auto corsair = unit(1, UnitKind::corsair); corsair.flying = true;
    corsair.airWeapon = {5, 8, 0, 160, DamageType::explosive, true, false};
    auto reaver = unit(2, UnitKind::reaver, {600, 500}, false);
    reaver.ammo = 0;
    reaver.groundWeapon = {100, 60, 0, 256, DamageType::normal, false, true};
    const std::array enemy{reaver};
    CombatEvaluator evaluator;
    check(evaluator.evaluate(std::array{corsair}, enemy, 1.2, 0.0, false).enemyPower == 0.0,
          "empty Reaver cannot acquire anti-air threat merely by running out of Scarabs");
    auto zealot = unit(3, UnitKind::zealot);
    zealot.groundWeapon = {8, 22, 0, 15, DamageType::normal, false, true, 2};
    check(evaluator.evaluate(std::array{zealot}, enemy, 1.2, 0.0, false).enemyPower > 0.0,
          "empty Reaver retains existing potential threat against compatible ground units");
}
}
int main() {
    navigation(); dynamicNavigation(); navigationOutcomes(); navigationWorkspaceReuse();
    reachableTargetSnapping(); meleeTargets(); threatAndRoutes(); stormSafety(); upgrades();
    commandIdentity(); unavailableWorkers();
    legalTechnologyProducer();
    ammunitionAndTargetDomain();
    if (!failures) std::cout << "Deep audit regression scenarios passed\n";
    return failures ? 1 : 0;
}
