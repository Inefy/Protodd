#include "protodd/Combat.hpp"
#include "protodd/Diagnostics.hpp"
#include "protodd/Operations.hpp"
#include "protodd/CommandBus.hpp"
#include "protodd/GameState.hpp"
#include "protodd/Geometry.hpp"
#include "protodd/InfluenceMap.hpp"
#include "protodd/Information.hpp"
#include "protodd/Learning.hpp"
#include "protodd/MacroPlanner.hpp"
#include "protodd/Navigation.hpp"
#include "protodd/Scouting.hpp"
#include "protodd/Runtime.hpp"
#include "protodd/Strategy.hpp"
#include "protodd/Squads.hpp"
#include "protodd/UnitCatalog.hpp"
#include "protodd/Technology.hpp"
#include "protodd/Transport.hpp"
#include "protodd/Workers.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void testGeometry() {
    expect(std::abs(protodd::distance({0, 0}, {3, 4}) - 5.0) < 0.001,
           "Euclidean distance");
    expect(protodd::moveToward({0, 0}, {100, 0}, 32.0) == protodd::Position{32, 0},
           "bounded movement toward a target");
    const protodd::BuildingFootprint gateway{{100, 100}, 128, 96};
    expect(!protodd::separatedByGap(gateway, {{228, 100}, 64, 64}, 32),
           "adjacent structures cannot close a production exit");
    expect(!protodd::separatedByGap(gateway, {{244, 100}, 64, 64}, 32),
           "a sixteen-pixel slit is insufficient for a Dragoon corridor");
    expect(protodd::separatedByGap(gateway, {{260, 100}, 64, 64}, 32) &&
               protodd::separatedByGap(gateway, {{100, 228}, 64, 64}, 32),
           "a full tile of clearance permits passage on either axis");
}

void testSnapshots() {
    protodd::UnitSnapshot dragoon;
    dragoon.id = 7;
    dragoon.hitPoints = 80;
    dragoon.maxHitPoints = 100;
    dragoon.shields = 40;
    dragoon.maxShields = 80;
    dragoon.groundWeapon = {.damage = 20, .targetsGround = true};

    protodd::UnitSnapshot enemy;
    enemy.id = 9;

    expect(std::abs(dragoon.healthFraction() - (120.0 / 180.0)) < 0.001,
           "combined shield and hit point fraction");
    expect(dragoon.canAttack(enemy), "ground target compatibility");

    protodd::GameState state;
    state.self.units.push_back(dragoon);
    state.enemy.units.push_back(enemy);
    expect(state.findUnit(9).has_value(), "unit lookup across both players");
    expect(!state.findUnit(42).has_value(), "missing unit lookup");

    auto reaver = dragoon;
    reaver.kind = protodd::UnitKind::reaver;
    reaver.ammo = 0;
    expect(!reaver.canAttack(enemy), "empty Reaver cannot promise a combat volley");
    reaver.ammo = 1;
    expect(reaver.canAttack(enemy), "armed Reaver exposes its Scarab attack");
}

void testNavigation() {
    constexpr auto width = 7;
    constexpr auto height = 5;
    std::vector<std::uint8_t> walkable(static_cast<std::size_t>(width * height), 1U);
    for (auto y = 0; y < height; ++y) {
        if (y != 2) walkable[static_cast<std::size_t>(y * width + 3)] = 0U;
    }
    protodd::NavigationGrid navigation(width, height, 32, walkable);
    expect(!navigation.lineWalkable({16, 16}, {208, 16}),
           "terrain line test detects a blocking cliff");
    const auto path = navigation.findPath({16, 16}, {208, 16});
    expect(!path.empty() && std::ranges::any_of(path, [](const protodd::Position point) {
               return point.y == 80;
           }),
           "A* routes a ground army through the available choke");
    const auto waypoint = navigation.nextWaypoint({16, 16}, {208, 16}, 3);
    expect(waypoint.valid() && waypoint != protodd::Position{208, 16},
           "long blocked route yields an intermediate waypoint");

    for (auto y = 0; y < height; ++y) {
        walkable[static_cast<std::size_t>(y * width + 3)] = 0U;
    }
    protodd::NavigationGrid disconnected(width, height, 32, walkable);
    expect(disconnected.findPath({16, 16}, {208, 16}).empty(),
           "disconnected terrain fails safely without inventing a route");
}

protodd::UnitSnapshot unit(
    const protodd::UnitId id,
    const protodd::UnitKind kind,
    const bool ours,
    const protodd::Position position = {128, 128}) {
    protodd::UnitSnapshot result;
    result.id = id;
    result.kind = kind;
    result.ours = ours;
    result.position = position;
    result.lastPosition = position;
    result.visible = true;
    result.completed = true;
    result.hitPoints = 100;
    result.maxHitPoints = 100;
    result.topSpeed = 4.0;
    return result;
}

void testCatalog() {
    expect(protodd::unitStats(protodd::UnitKind::probe).minerals == 50,
           "Probe catalog cost");
    expect(protodd::unitStats(protodd::UnitKind::overlord).name == "Overlord",
           "catalog enum and table remain aligned");
    expect(protodd::isCombatUnit(protodd::UnitKind::dragoon), "Dragoon combat classification");
    expect(!protodd::isCombatUnit(protodd::UnitKind::pylon), "Pylon combat classification");
    expect(!protodd::isCombatUnit(protodd::UnitKind::observer),
           "Observer remains support rather than attack army");
    expect(protodd::isCombatUnit(protodd::UnitKind::valkyrie) &&
               protodd::isCombatUnit(protodd::UnitKind::guardian) &&
               protodd::isCombatUnit(protodd::UnitKind::spiderMine),
           "late-game flyers and Spider Mines remain visible to combat evaluation");
    expect(protodd::isCombatUnit(protodd::UnitKind::zergling) &&
               protodd::isCombatUnit(protodd::UnitKind::scourge) &&
               protodd::isCombatUnit(protodd::UnitKind::broodling),
           "low-cost combat units are never filtered by an arbitrary value cutoff");
    expect(protodd::isBuilding(protodd::UnitKind::scienceFacility) &&
               protodd::isBuilding(protodd::UnitKind::defilerMound),
           "advanced enemy tech structures remain visible to inference");
    const auto tribunalRequirements = protodd::unitPrerequisites(
        protodd::UnitKind::arbiterTribunal);
    expect(std::ranges::find(tribunalRequirements, protodd::UnitKind::stargate) !=
               tribunalRequirements.end() &&
               std::ranges::find(tribunalRequirements, protodd::UnitKind::templarArchives) !=
                   tribunalRequirements.end(),
           "Arbiter Tribunal requires both branches of its tech tree");
    const auto batteryRequirements = protodd::unitPrerequisites(
        protodd::UnitKind::shieldBattery);
    expect(batteryRequirements.size() == 1 &&
               batteryRequirements.front() == protodd::UnitKind::gateway,
           "Shield Battery follows the actual Gateway prerequisite");
    expect(protodd::technologyStats(protodd::TechnologyKind::protossGroundWeapons)
                       .mineralCost(2) == 150,
           "repeatable upgrades use next-level pricing");
}

void testOpponentInferenceAndStrategy() {
    protodd::GameState unseen;
    unseen.enemy.race = protodd::Race::terran;
    protodd::OpponentModel unseenModel;
    unseenModel.update(unseen);
    expect(unseenModel.mostLikelyPlan() == protodd::EnemyPlan::unknown,
           "unscouted opponents remain unknown instead of defaulting to worker rush");

    auto distantOpening = unseen;
    distantOpening.enemy.race = protodd::Race::protoss;
    distantOpening.frame = 3000;
    distantOpening.self.units = {unit(1, protodd::UnitKind::nexus, true, {256, 256})};
    distantOpening.enemy.units = {unit(2, protodd::UnitKind::zealot, false, {3000, 3000})};
    protodd::OpponentModel distantModel;
    distantModel.update(distantOpening);
    const auto singleObservation = distantModel.probability(protodd::EnemyPlan::fastRush);
    for (int i = 0; i < 100; ++i) {
        distantOpening.frame += 6;
        distantModel.update(distantOpening);
    }
    expect(std::abs(distantModel.probability(protodd::EnemyPlan::fastRush) -
                    singleObservation) < 0.0001 &&
               distantModel.assessment().aggression < 0.6,
           "repeated snapshots of one remote Zealot cannot manufacture certainty about a rush");
    expect(protodd::StrategyEngine{}.plan(distantOpening, distantModel.assessment()).posture !=
               protodd::Posture::defend,
           "an ordinary first enemy Zealot does not cancel the opening technology plan");

    protodd::GameState state;
    state.frame = 3 * 60 * 24;
    state.mapWidthPixels = 4096;
    state.mapHeightPixels = 4096;
    state.self.id = 1;
    state.self.race = protodd::Race::protoss;
    state.enemy.id = 2;
    state.enemy.race = protodd::Race::zerg;
    auto nexus = unit(1, protodd::UnitKind::nexus, true, {256, 256});
    nexus.role = protodd::UnitRole::resourceDepot;
    state.self.units.push_back(nexus);
    for (int i = 0; i < 8; ++i) {
        auto zergling = unit(100 + i, protodd::UnitKind::zergling, false,
                            {300 + i * 4, 300});
        zergling.role = protodd::UnitRole::groundArmy;
        zergling.groundWeapon = {.damage = 5, .cooldown = 8, .maxRange = 32,
                                .targetsGround = true};
        state.enemy.units.push_back(zergling);
    }

    protodd::OpponentModel model;
    model.update(state);
    expect(model.mostLikelyPlan() == protodd::EnemyPlan::fastRush,
           "early zerglings classify as fast rush");
    expect(model.assessment().immediateGround > 0.3,
           "rush produces immediate-ground warning");

    protodd::GameState earlyPoolState = state;
    earlyPoolState.enemy.units.clear();
    auto earlyPool = unit(120, protodd::UnitKind::spawningPool, false,
                          {2400, 2400});
    earlyPool.firstSeen = 2'700;
    earlyPool.lastSeen = earlyPoolState.frame;
    earlyPoolState.enemy.units.push_back(earlyPool);
    protodd::OpponentModel earlyPoolModel;
    earlyPoolModel.update(earlyPoolState);
    expect(earlyPoolModel.mostLikelyPlan() == protodd::EnemyPlan::fastRush,
           "an early scouted Spawning Pool warns of a rush before contact");

    auto developingPool = earlyPool;
    developingPool.completed = false;
    developingPool.firstSeen = 2500;
    developingPool.buildProgress = -1;
    developingPool.constructionStartUpperBound = 2500;
    auto completedPool = developingPool;
    completedPool.completed = true;
    completedPool.lastSeen = 4000;
    completedPool.constructionStartUpperBound = 4000 -
        protodd::unitStats(protodd::UnitKind::spawningPool).buildTime;
    completedPool.inheritObservationHistory(developingPool);
    earlyPoolState.enemy.units = {completedPool};
    protodd::OpponentModel normalPoolModel;
    normalPoolModel.update(earlyPoolState);
    expect(normalPoolModel.mostLikelyPlan() != protodd::EnemyPlan::fastRush,
           "later Pool completion does not move its start before the first unfinished observation");
    auto oldDrone = developingPool;
    oldDrone.kind = protodd::UnitKind::drone;
    oldDrone.firstSeen = 100;
    developingPool.inheritObservationHistory(oldDrone);
    expect(developingPool.firstSeen == 2500,
           "a Zerg structure does not inherit the first-seen timestamp of its Drone");

    protodd::StrategyEngine strategy;
    const auto plan = strategy.plan(state, model.assessment());
    expect(plan.posture == protodd::Posture::defend, "PvZ rush switches to defense");
    const auto emergencyZealots = std::ranges::find_if(
        plan.goals,
        [](const protodd::ProductionGoal& goal) {
            return goal.target == protodd::UnitKind::zealot && goal.blocking;
        });
    expect(emergencyZealots != plan.goals.end(), "rush plan contains blocking zealots");
    expect(std::ranges::any_of(plan.goals, [](const protodd::ProductionGoal& goal) {
               return goal.target == protodd::UnitKind::shieldBattery && goal.blocking;
           }),
           "opening anti-ling response adds a blocking Shield Battery");

    protodd::GameState approaching = state;
    approaching.enemy.units.clear();
    for (int i = 0; i < 4; ++i) {
        auto zergling = unit(130 + i, protodd::UnitKind::zergling, false,
                            {1180 + i * 8, 256});
        zergling.lastPosition = {1220 + i * 8, 256};
        zergling.role = protodd::UnitRole::groundArmy;
        approaching.enemy.units.push_back(zergling);
    }
    for (int i = 0; i < 3; ++i) {
        auto hatchery = unit(140 + i, protodd::UnitKind::hatchery, false,
                             {2400 + i * 160, 2400});
        hatchery.role = protodd::UnitRole::production;
        approaching.enemy.units.push_back(hatchery);
    }
    protodd::OpponentModel approachModel;
    approachModel.update(approaching);
    expect(approachModel.assessment().approachingCombatEnemies == 4 &&
               approachModel.assessment().approachingArmyValue > 0.0,
           "enemy motion toward the main is recognized before base contact");
    expect(approachModel.assessment().enemyProductionCapacity >= 3.0,
           "scouted production is retained as an explicit capacity estimate");

    auto terranPressure = state;
    terranPressure.enemy.race = protodd::Race::terran;
    terranPressure.enemy.units.clear();
    terranPressure.bases.push_back(
        {1, {256, 256}, {300, 260}, 8000, 5000, terranPressure.self.id,
         terranPressure.frame, true, false, 8, 1});
    auto marine = unit(150, protodd::UnitKind::marine, false, {320, 300});
    marine.role = protodd::UnitRole::groundArmy;
    marine.groundWeapon = {.damage = 6, .cooldown = 15, .maxRange = 128,
                           .targetsGround = true};
    terranPressure.enemy.units.push_back(marine);
    for (int id = 160; id < 172; ++id) {
        auto defender = unit(id, protodd::UnitKind::probe, true, {260, 260});
        defender.role = protodd::UnitRole::worker;
        terranPressure.self.units.push_back(defender);
    }
    protodd::ThreatAssessment visiblePressure;
    visiblePressure.combatEnemiesNearMain = 1;
    const auto economicUnderAttack = strategy.plan(
        terranPressure, visiblePressure, protodd::OpeningStyle::economic);
    expect(economicUnderAttack.posture == protodd::Posture::defend &&
               economicUnderAttack.desiredBases == 1 &&
               economicUnderAttack.desiredWorkers <= 14,
           "learned economic style cannot override visible main-base pressure");

    protodd::GameState workerRush;
    workerRush.frame = 2 * 60 * 24;
    workerRush.self.id = 1;
    workerRush.enemy.id = 2;
    workerRush.enemy.race = protodd::Race::protoss;
    auto remoteProbe = unit(1, protodd::UnitKind::probe, true, {3000, 3000});
    remoteProbe.role = protodd::UnitRole::worker;
    auto homeNexus = unit(2, protodd::UnitKind::nexus, true, {256, 256});
    homeNexus.role = protodd::UnitRole::resourceDepot;
    workerRush.self.units = {remoteProbe, homeNexus};
    for (int i = 0; i < 4; ++i) {
        workerRush.enemy.units.push_back(
            unit(20 + i, protodd::UnitKind::probe, false, {300 + i * 12, 280}));
    }
    protodd::OpponentModel workerModel;
    workerModel.update(workerRush);
    expect(workerModel.mostLikelyPlan() == protodd::EnemyPlan::workerRush &&
               workerModel.assessment().workerRush > 0.3,
           "worker rush inference anchors to the Nexus rather than unit ordering");

    protodd::GameState normalScout = workerRush;
    normalScout.enemy.units.resize(1);
    protodd::OpponentModel normalScoutModel;
    normalScoutModel.update(normalScout);
    expect(normalScoutModel.mostLikelyPlan() != protodd::EnemyPlan::workerRush &&
               normalScoutModel.assessment().workerRush < 0.3,
           "one scouting worker is not misclassified as a worker rush");
    const auto normalScoutPlan = strategy.plan(
        normalScout, normalScoutModel.assessment());
    expect(normalScoutPlan.posture != protodd::Posture::defend,
           "one scouting worker does not force the matchup plan into emergency defense");

    for (auto& enemy : state.enemy.units) {
        enemy.visible = false;
        enemy.lastSeen = state.frame;
    }
    for (int update = 1; update <= 30; ++update) {
        state.frame += 31 * 24;
        model.update(state);
    }
    expect(model.assessment().aggression < 0.6,
           "remembered opening units age out of the active pressure estimate");

    protodd::GameState cannonRush = workerRush;
    cannonRush.enemy.units.clear();
    auto cannon = unit(80, protodd::UnitKind::photonCannon, false, {480, 300});
    cannon.completed = false;
    cannon.buildProgress = 40;
    cannonRush.enemy.units.push_back(cannon);
    protodd::OpponentModel cannonModel;
    cannonModel.update(cannonRush);
    expect(cannonModel.mostLikelyPlan() == protodd::EnemyPlan::staticContain,
           "nearby opening cannon classifies as a static contain");
    const auto containPlan = strategy.plan(cannonRush, cannonModel.assessment(),
                                           protodd::OpeningStyle::economic);
    expect(containPlan.posture == protodd::Posture::defend &&
               containPlan.desiredBases == 1 &&
               std::ranges::none_of(containPlan.goals,
                                    [](const protodd::ProductionGoal& candidate) {
                                        return candidate.goal == protodd::GoalKind::expand;
                                    }),
           "static contain overrides learned greed and suppresses expansion");

    protodd::GameState adaptive;
    adaptive.frame = 11 * 60 * 24;
    adaptive.self.id = 1;
    adaptive.self.race = protodd::Race::protoss;
    adaptive.self.supplyUsed = 100;
    adaptive.self.supplyTotal = 150;
    adaptive.enemy.id = 2;
    adaptive.enemy.race = protodd::Race::zerg;
    auto adaptiveNexus = unit(300, protodd::UnitKind::nexus, true, {256, 256});
    adaptiveNexus.role = protodd::UnitRole::resourceDepot;
    adaptive.self.units.push_back(adaptiveNexus);
    for (int i = 0; i < 8; ++i) {
        auto hydralisk = unit(400 + i, protodd::UnitKind::hydralisk, false,
                              {1200 + i * 8, 1200});
        hydralisk.role = protodd::UnitRole::groundArmy;
        hydralisk.lastSeen = adaptive.frame;
        adaptive.enemy.units.push_back(hydralisk);
    }
    for (int i = 0; i < 6; ++i) {
        auto guardian = unit(500 + i, protodd::UnitKind::guardian, false,
                             {1300 + i * 8, 1250});
        guardian.role = protodd::UnitRole::airArmy;
        guardian.flying = true;
        guardian.lastSeen = adaptive.frame;
        adaptive.enemy.units.push_back(guardian);
    }
    const auto adaptivePlan = strategy.plan(adaptive, {});
    expect(std::ranges::any_of(adaptivePlan.goals, [](const protodd::ProductionGoal& goal) {
               return goal.target == protodd::UnitKind::reaver && goal.desiredCount >= 2;
           }),
           "observed hydralisk mass adds a reaver splash counter");
    expect(std::ranges::any_of(adaptivePlan.goals, [](const protodd::ProductionGoal& goal) {
               return goal.target == protodd::UnitKind::corsair && goal.desiredCount >= 7 &&
                      goal.blocking;
           }),
           "observed Zerg air mass increases blocking air-control production");
}

void testSupplyPlanning() {
    protodd::GameState state;
    state.self.id = 1;
    state.self.race = protodd::Race::protoss;
    state.enemy.race = protodd::Race::terran;
    state.self.supplyUsed = 12;
    state.self.supplyTotal = 18;
    auto nexus = unit(1, protodd::UnitKind::nexus, true);
    nexus.role = protodd::UnitRole::resourceDepot;
    state.self.units.push_back(nexus);

    protodd::StrategyEngine strategy;
    const auto opening = strategy.plan(state, {});
    const auto pylonGoal = std::ranges::find(
        opening.goals, protodd::UnitKind::pylon, &protodd::ProductionGoal::target);
    expect(pylonGoal != opening.goals.end() && pylonGoal->desiredCount == 1 &&
               pylonGoal->blocking,
           "opening supply logic reserves the first pylon before a supply block");
    state.self.minerals = 100;
    protodd::ResourceLedger openingLedger{state.self.minerals, state.self.gas};
    const auto openingActions = protodd::MacroPlanner{}.reconcile(
        state, opening, openingLedger);
    expect(!openingActions.empty() &&
               openingActions.front().action == protodd::MacroActionKind::build &&
               openingActions.front().target == protodd::UnitKind::pylon &&
               openingActions.front().reserved,
           "live opening state turns the blocking pylon goal into the first command");

    auto pendingPylon = unit(2, protodd::UnitKind::pylon, true);
    pendingPylon.completed = false;
    pendingPylon.buildProgress = 20;
    state.self.units.push_back(pendingPylon);
    const auto constructing = strategy.plan(state, {});
    const auto constructingGoal = std::ranges::find(
        constructing.goals, protodd::UnitKind::pylon, &protodd::ProductionGoal::target);
    expect(constructingGoal != constructing.goals.end() &&
               constructingGoal->desiredCount == 1,
           "pending pylon supply prevents a duplicate construction order");

    state.self.supplyUsed = 40;
    state.self.supplyTotal = 60;
    state.self.units.pop_back();
    state.self.units.push_back(unit(20, protodd::UnitKind::pylon, true));
    state.self.units.push_back(unit(21, protodd::UnitKind::pylon, true));
    for (int id = 10; id < 14; ++id) {
        state.self.units.push_back(unit(id, protodd::UnitKind::gateway, true));
        state.self.queuedUnits.push_back(protodd::UnitKind::dragoon);
    }
    const auto productionForecast = strategy.plan(state, {});
    const auto forecastPylon = std::ranges::find(
        productionForecast.goals, protodd::UnitKind::pylon,
        &protodd::ProductionGoal::target);
    expect(forecastPylon != productionForecast.goals.end() &&
               forecastPylon->desiredCount == 2,
           "supply forecast does not count in-production units twice when headroom is sufficient");
    state.self.supplyUsed = 52;
    const auto nextCycle = strategy.plan(state, {});
    const auto nextPylon = std::ranges::find(nextCycle.goals, protodd::UnitKind::pylon,
                                            &protodd::ProductionGoal::target);
    expect(nextPylon != nextCycle.goals.end() && nextPylon->desiredCount == 3,
           "supply forecasting still funds the next production cycle before a block");

    protodd::GameState banked;
    banked.frame = 4 * 60 * 24;
    banked.self.race = protodd::Race::protoss;
    banked.enemy.race = protodd::Race::terran;
    banked.self.minerals = 700;
    banked.self.supplyUsed = 24;
    banked.self.supplyTotal = 34;
    banked.self.units = {
        unit(30, protodd::UnitKind::nexus, true),
        unit(31, protodd::UnitKind::pylon, true),
        unit(32, protodd::UnitKind::gateway, true),
    };
    banked.bases.push_back(
        {1, {128, 128}, {160, 128}, 8000, 5000, banked.self.id,
         banked.frame, true, false, 8, 1});
    const auto spendingPlan = strategy.plan(banked, {});
    expect(std::ranges::any_of(spendingPlan.goals, [](const protodd::ProductionGoal& goal) {
               return goal.target == protodd::UnitKind::gateway &&
                      goal.desiredCount >= 2 && goal.priority == 62;
           }),
           "sustained mineral surplus adds production instead of banking indefinitely");

    banked.frame = 5 * 60 * 24;
    banked.self.minerals = 0;
    for (int id = 33; id < 45; ++id) {
        auto worker = unit(id, protodd::UnitKind::probe, true);
        worker.role = protodd::UnitRole::worker;
        banked.self.units.push_back(worker);
    }
    banked.self.supplyUsed = 56;
    for (int id = 60; id < 63; ++id)
        banked.self.units.push_back(unit(id, protodd::UnitKind::dragoon, true));
    const auto expansionPlan = strategy.plan(banked, {});
    expect(expansionPlan.desiredBases >= 2,
           "PvT natural becomes due once the supply and Dragoon commitments are met");
    expect(std::ranges::any_of(
               expansionPlan.goals, [](const protodd::ProductionGoal& goal) {
                   return goal.goal == protodd::GoalKind::expand && goal.blocking &&
                          goal.priority > 74;
               }),
           "safe due expansion reserves its bank ahead of routine Probe production");

    protodd::ThreatAssessment threatenedExpansion;
    threatenedExpansion.combatEnemiesNearMain = 4;
    threatenedExpansion.immediateGround = 0.8;
    const auto defensePlan = strategy.plan(banked, threatenedExpansion);
    expect(std::ranges::none_of(
               defensePlan.goals, [](const protodd::ProductionGoal& goal) {
                   return goal.goal == protodd::GoalKind::expand && goal.blocking;
               }),
           "immediate pressure cancels expansion banking in favor of defenders");

    auto learnedPressure = threatenedExpansion;
    learnedPressure.combatEnemiesNearMain = 0;
    learnedPressure.immediateGround = 0.0;
    learnedPressure.mostLikely = protodd::EnemyPlan::fastRush;
    const auto cautiousTransition = strategy.plan(banked, learnedPressure);
    expect(cautiousTransition.posture == protodd::Posture::hold &&
               cautiousTransition.minimumAttackSize >= 14,
           "a learned rush keeps a decisive army on its defensive screen between waves");

    auto chainedExpansion = banked;
    chainedExpansion.frame = 11 * 60 * 24;
    auto pendingNexus = unit(45, protodd::UnitKind::nexus, true, {640, 640});
    pendingNexus.role = protodd::UnitRole::resourceDepot;
    pendingNexus.completed = false;
    chainedExpansion.self.units.push_back(pendingNexus);
    const auto chainedPlan = strategy.plan(chainedExpansion, {});
    expect(std::ranges::none_of(
               chainedPlan.goals, [](const protodd::ProductionGoal& goal) {
                   return goal.goal == protodd::GoalKind::expand && goal.blocking &&
                          goal.desiredCount >= 3;
               }),
           "an unfinished expansion cannot immediately reserve a third Nexus");

    protodd::GameState throughput;
    throughput.frame = 4 * 60 * 24;
    throughput.self.race = protodd::Race::protoss;
    throughput.enemy.race = protodd::Race::protoss;
    throughput.self.supplyUsed = 40;
    throughput.self.supplyTotal = 50;
    throughput.self.units = {
        unit(40, protodd::UnitKind::nexus, true),
        unit(41, protodd::UnitKind::pylon, true),
        unit(42, protodd::UnitKind::gateway, true),
    };
    throughput.self.units.front().role = protodd::UnitRole::resourceDepot;
    for (int id = 50; id < 68; ++id) {
        auto worker = unit(id, protodd::UnitKind::probe, true);
        worker.role = protodd::UnitRole::worker;
        throughput.self.units.push_back(worker);
    }
    throughput.bases.push_back(
        {1, {128, 128}, {160, 128}, 8000, 5000, throughput.self.id,
         throughput.frame, true, false, 8, 1});
    const auto throughputPlan = strategy.plan(throughput, {});
    expect(std::ranges::any_of(
               throughputPlan.goals, [](const protodd::ProductionGoal& goal) {
                   return goal.target == protodd::UnitKind::gateway &&
                          goal.desiredCount >= 3 && goal.priority == 73;
               }),
           "saturated one-base economy proactively scales army throughput");

    protodd::ThreatAssessment productionThreat;
    productionThreat.enemyProductionCapacity = 3.0;
    throughput.self.units.erase(throughput.self.units.begin() + 3,
                                throughput.self.units.end());
    const auto parityPlan = strategy.plan(throughput, productionThreat);
    expect(std::ranges::any_of(
               parityPlan.goals, [](const protodd::ProductionGoal& goal) {
                   return goal.target == protodd::UnitKind::gateway &&
                          goal.desiredCount == 3 && goal.priority == 78;
               }),
           "scouted enemy production prevents an underbuilt one-base response");
}

void testStrategicDirector() {
    protodd::StrategicDirector director;
    protodd::GameState state;
    state.frame = 100;
    protodd::StrategicPlan pressure;
    pressure.name = "pressure";
    pressure.posture = protodd::Posture::pressure;
    expect(director.stabilize(pressure, state, {}).posture == protodd::Posture::pressure,
           "strategic director accepts the initial map-level intent");

    protodd::StrategicPlan defense = pressure;
    defense.posture = protodd::Posture::defend;
    protodd::ThreatAssessment breach;
    breach.combatEnemiesNearMain = 2;
    state.frame += 24;
    expect(director.stabilize(defense, state, breach).posture == protodd::Posture::defend,
           "strategic emergencies override an attack immediately");

    state.frame += 4 * 24;
    const auto regrouping = director.stabilize(pressure, state, {});
    expect(regrouping.posture == protodd::Posture::defend &&
               regrouping.attackThreshold >= 1.40,
           "one clear observation cannot relaunch an army after base defense");

    state.frame += 5 * 24;
    expect(director.stabilize(pressure, state, {}).posture == protodd::Posture::pressure,
           "sustained safety releases the regrouped army");
}

void testOpeningMilestones() {
    protodd::GameState state;
    state.self.id = 1;
    state.self.race = protodd::Race::protoss;
    state.enemy.id = 2;
    state.enemy.race = protodd::Race::terran;
    state.self.supplyTotal = 34;
    auto nexus = unit(1, protodd::UnitKind::nexus, true);
    nexus.role = protodd::UnitRole::resourceDepot;
    state.self.units.push_back(nexus);
    protodd::StrategyEngine strategy;

    state.self.supplyUsed = 14;
    const auto beforeGateway = strategy.plan(state, {});
    const auto openingCannons = std::ranges::find_if(
        beforeGateway.goals, [](const protodd::ProductionGoal& goal) {
            return goal.target == protodd::UnitKind::photonCannon;
        });
    const auto openingGateway = std::ranges::find_if(
        beforeGateway.goals, [](const protodd::ProductionGoal& goal) {
            return goal.target == protodd::UnitKind::gateway;
        });
    expect(openingCannons == beforeGateway.goals.end() &&
               openingGateway == beforeGateway.goals.end() && beforeGateway.desiredWorkers > 7,
           "unscouted PvT grows its economy before the ten-supply Gateway");

    state.self.supplyUsed = 20;
    const auto gatewayTiming = strategy.plan(state, {});
    expect(std::ranges::any_of(gatewayTiming.goals, [](const protodd::ProductionGoal& goal) {
               return goal.target == protodd::UnitKind::gateway && goal.blocking;
           }),
           "ten-supply Gateway becomes a mandatory opening milestone");

    state.self.supplyUsed = 20;
    protodd::ThreatAssessment earlyBio;
    earlyBio.combatEnemiesNearMain = 3;
    const auto terranSafety = strategy.plan(state, earlyBio);
    expect(std::ranges::any_of(terranSafety.goals, [](const protodd::ProductionGoal& goal) {
               return goal.target == protodd::UnitKind::zealot && goal.blocking;
           }),
           "confirmed PvT pressure banks an opening bodyguard before dragoon tech");

    state.enemy.race = protodd::Race::protoss;
    state.self.supplyUsed = 14;
    const auto zealotTiming = strategy.plan(state, {});
    expect(zealotTiming.desiredWorkers > 7 &&
               std::ranges::none_of(zealotTiming.goals, [](const protodd::ProductionGoal& goal) {
                   return goal.target == protodd::UnitKind::forge ||
                          goal.target == protodd::UnitKind::photonCannon;
               }), "PvP keeps growing its opening income instead of buying blind static defense");
    state.self.supplyUsed = 20;
    const auto mobileOpening = strategy.plan(state, {});
    expect(std::ranges::any_of(mobileOpening.goals, [](const protodd::ProductionGoal& goal) {
               return goal.target == protodd::UnitKind::gateway && goal.blocking &&
                      goal.priority >= 100;
           }), "PvP reserves its first Gateway at ten supply before routine spending");

    state.self.supplyUsed = 28;
    state.self.units.push_back(unit(2, protodd::UnitKind::gateway, true));
    state.self.units.push_back(unit(3, protodd::UnitKind::gateway, true));
    state.self.units.push_back(unit(4, protodd::UnitKind::zealot, true));
    const auto coreTiming = strategy.plan(state, {});
    expect(std::ranges::any_of(coreTiming.goals, [](const protodd::ProductionGoal& goal) {
               return goal.target == protodd::UnitKind::cyberneticsCore && goal.blocking;
           }),
           "fourteen-supply Core becomes a mandatory opening milestone");
    expect(coreTiming.desiredGasWorkers == 3,
           "gas mining starts before gas-dependent Dragoon technology");
    state.self.supplyUsed = 28;
    const auto rangeTiming = strategy.plan(state, {});
    const auto rangeGoal = std::ranges::find_if(
        rangeTiming.goals, [](const protodd::ProductionGoal& goal) {
            return goal.technology == protodd::TechnologyKind::singularityCharge;
        });
    expect(rangeGoal == rangeTiming.goals.end() || !rangeGoal->blocking,
           "normal PvP does not reserve range before its first Dragoon");

    protodd::ThreatAssessment baseBreach;
    baseBreach.combatEnemiesNearMain = 3;
    const auto emergencyTiming = strategy.plan(state, baseBreach);
    expect(emergencyTiming.composition.size() == 1 &&
               emergencyTiming.composition.front().kind == protodd::UnitKind::zealot &&
               std::ranges::none_of(
                   emergencyTiming.goals, [](const protodd::ProductionGoal& goal) {
                       return goal.target == protodd::UnitKind::cyberneticsCore ||
                              goal.technology ==
                                  protodd::TechnologyKind::singularityCharge;
                   }),
           "PvP base breach cannot reserve tech ahead of continuous defenders");

    auto marineFlood = state;
    marineFlood.enemy.race = protodd::Race::terran;
    marineFlood.frame = 6 * 60 * 24;
    const auto fortifiedTerran = strategy.plan(marineFlood, baseBreach);
    expect(std::ranges::any_of(
               fortifiedTerran.goals, [](const protodd::ProductionGoal& goal) {
                   return goal.target == protodd::UnitKind::photonCannon &&
                          goal.desiredCount >= 4 && goal.priority > 100;
               }) && std::ranges::any_of(
               fortifiedTerran.goals, [](const protodd::ProductionGoal& goal) {
                   return goal.target == protodd::UnitKind::zealot &&
                          goal.desiredCount >= 10 && goal.blocking;
               }),
           "sustained PvT bio pressure scales both static and mobile throughput");
    const auto emergencyGateway = std::ranges::find_if(
        fortifiedTerran.goals, [](const protodd::ProductionGoal& goal) {
            return goal.target == protodd::UnitKind::gateway &&
                   goal.desiredCount >= 2;
        });
    const auto sustainedZealots = std::ranges::find_if(
        fortifiedTerran.goals, [](const protodd::ProductionGoal& goal) {
            return goal.target == protodd::UnitKind::zealot &&
                   goal.desiredCount >= 10;
        });
    expect(emergencyGateway != fortifiedTerran.goals.end() &&
               sustainedZealots != fortifiedTerran.goals.end() &&
               emergencyGateway->priority > sustainedZealots->priority,
           "PvT pressure adds production before repeatedly reserving individual bodies");
    marineFlood.self.units.push_back(
        unit(9, protodd::UnitKind::photonCannon, true));
    marineFlood.self.units.push_back(
        unit(10, protodd::UnitKind::photonCannon, true));
    marineFlood.self.units.push_back(
        unit(11, protodd::UnitKind::photonCannon, true));
    marineFlood.self.units.push_back(
        unit(13, protodd::UnitKind::photonCannon, true));
    marineFlood.self.units.push_back(
        unit(12, protodd::UnitKind::cyberneticsCore, true));
    for (int id = 20; id < 27; ++id) {
        marineFlood.enemy.units.push_back(
            unit(id, protodd::UnitKind::marine, false, {320 + id * 4, 320}));
    }
    for (int id = 30; id < 33; ++id) {
        marineFlood.self.units.push_back(
            unit(id, protodd::UnitKind::zealot, true, {220 + id * 4, 220}));
    }
    marineFlood.frame = 8 * 60 * 24;
    const auto antiBioTech = strategy.plan(marineFlood, baseBreach);
    expect(std::ranges::any_of(
               antiBioTech.goals, [](const protodd::ProductionGoal& goal) {
                   return goal.target == protodd::UnitKind::citadelOfAdun &&
                          goal.priority >= 99 && goal.blocking;
               }),
           "a completed PvT defensive screen converts its window into cloak tech");
    expect(std::ranges::any_of(
               antiBioTech.goals, [](const protodd::ProductionGoal& goal) {
                   return goal.target == protodd::UnitKind::dragoon &&
                          goal.desiredCount == 1 && goal.priority >= 104 &&
                          !goal.blocking;
               }),
           "gas-starved PvT does not freeze Cannon recovery for its first Dragoon");
    auto gasReadyBioTech = marineFlood;
    gasReadyBioTech.self.gas = 50;
    const auto gasReadyBioPlan = strategy.plan(gasReadyBioTech, baseBreach);
    expect(std::ranges::any_of(
               gasReadyBioPlan.goals, [](const protodd::ProductionGoal& goal) {
                   return goal.target == protodd::UnitKind::dragoon &&
                          goal.desiredCount == 1 && goal.priority >= 104 &&
                          goal.blocking;
               }),
           "a powered defensive screen reserves its first Dragoon once gas is ready");
    expect(std::ranges::any_of(
               antiBioTech.goals, [](const protodd::ProductionGoal& goal) {
                   return goal.target == protodd::UnitKind::cyberneticsCore &&
                          goal.priority >= 99 && goal.blocking;
               }),
           "an established screen advances ranged tech after emergency production");
    expect(std::ranges::any_of(
               antiBioTech.goals, [](const protodd::ProductionGoal& goal) {
                   return goal.target == protodd::UnitKind::pylon &&
                          goal.desiredCount >= 2 && goal.priority >= 102 &&
                          goal.blocking;
               }),
           "the PvT defensive shell receives redundant forward power");
    auto committedCloak = marineFlood;
    std::erase_if(committedCloak.self.units, [](const protodd::UnitSnapshot& candidate) {
        return protodd::isCombatUnit(candidate.kind);
    });
    committedCloak.self.units.push_back(
        unit(38, protodd::UnitKind::zealot, true));
    committedCloak.self.units.push_back(
        unit(39, protodd::UnitKind::citadelOfAdun, true));
    const auto committedCloakPlan = strategy.plan(committedCloak, baseBreach);
    expect(std::ranges::any_of(
               committedCloakPlan.goals, [](const protodd::ProductionGoal& goal) {
                   return goal.target == protodd::UnitKind::templarArchives &&
                          goal.priority >= 99 && goal.blocking;
               }),
           "an initiated cloak transition persists through frontline losses");
    auto stormState = marineFlood;
    for (int id = 33; id < 37; ++id) {
        stormState.self.units.push_back(
            unit(id, protodd::UnitKind::zealot, true, {220 + id * 4, 220}));
    }
    const auto stormPlan = strategy.plan(stormState, baseBreach);
    const auto stormGoal = std::ranges::find_if(
        stormPlan.goals, [](const protodd::ProductionGoal& goal) {
            return goal.technology == protodd::TechnologyKind::psionicStorm;
        });
    const auto routineDragoon = std::ranges::find_if(
        stormPlan.goals, [](const protodd::ProductionGoal& goal) {
            return goal.target == protodd::UnitKind::dragoon &&
                   goal.desiredCount >= 8;
        });
    expect(stormGoal != stormPlan.goals.end() && stormGoal->blocking &&
               routineDragoon != stormPlan.goals.end() &&
               stormGoal->priority > routineDragoon->priority,
           "observed bio reserves Storm before unbounded Dragoon production");

    auto staticRecovery = marineFlood;
    std::erase_if(staticRecovery.self.units, [](const protodd::UnitSnapshot& candidate) {
        return protodd::isCombatUnit(candidate.kind);
    });
    for (int id = 40; id < 45; ++id) {
        auto probe = unit(id, protodd::UnitKind::probe, true, {160 + id, 160});
        probe.role = protodd::UnitRole::worker;
        staticRecovery.self.units.push_back(probe);
    }
    const auto staticRecoveryPlan = strategy.plan(staticRecovery, baseBreach);
    expect(std::ranges::any_of(
               staticRecoveryPlan.goals, [](const protodd::ProductionGoal& goal) {
                   return goal.target == protodd::UnitKind::probe &&
                          goal.desiredCount >= 8 && goal.priority == 105 &&
                          goal.blocking;
               }),
           "three completed Cannons permit critical Probe recovery under pressure");

    state.enemy.race = protodd::Race::protoss;
    state.frame = 5 * 60 * 24;
    state.bases.push_back(
        {1, {128, 128}, {160, 128}, 8000, 5000, 1, state.frame,
         true, false, 8, 1});
    const auto oneBasePlan = strategy.plan(state, {});
    expect(oneBasePlan.desiredBases == 1 && oneBasePlan.desiredWorkers <= 22,
           "one-base plans stop Probe production at a useful saturation cap");

    state.enemy.race = protodd::Race::zerg;
    state.self.supplyUsed = 20;
    std::erase_if(state.self.units, [](const protodd::UnitSnapshot& candidate) {
        return candidate.id == 3;
    });
    const auto safePvZ = strategy.plan(state, {});
    expect(std::ranges::any_of(safePvZ.goals, [](const protodd::ProductionGoal& goal) {
               return goal.target == protodd::UnitKind::gateway &&
                      goal.desiredCount >= 2 && goal.blocking;
           }),
           "PvZ secures two-gate throughput before exposing the economy");
    expect(std::ranges::any_of(safePvZ.goals, [](const protodd::ProductionGoal& goal) {
               return goal.target == protodd::UnitKind::forge && goal.blocking;
           }) &&
               std::ranges::any_of(
                   safePvZ.goals, [](const protodd::ProductionGoal& goal) {
                       return goal.target == protodd::UnitKind::photonCannon &&
                              goal.desiredCount >= 1 && goal.blocking;
                   }),
           "PvZ establishes a fortified anchor before exposing its economy");

    protodd::GameState poolFirst;
    poolFirst.self.id = 1;
    poolFirst.self.race = protodd::Race::protoss;
    poolFirst.enemy.id = 2;
    poolFirst.enemy.race = protodd::Race::zerg;
    poolFirst.self.supplyUsed = 14;
    poolFirst.self.supplyTotal = 18;
    auto poolNexus = unit(80, protodd::UnitKind::nexus, true, {128, 128});
    poolNexus.role = protodd::UnitRole::resourceDepot;
    poolFirst.self.units.push_back(poolNexus);
    for (int id = 81; id < 88; ++id) {
        auto poolProbe = unit(id, protodd::UnitKind::probe, true, {128, 128});
        poolProbe.role = protodd::UnitRole::worker;
        poolFirst.self.units.push_back(poolProbe);
    }
    const auto poolFirstPlan = strategy.plan(poolFirst, {});
    expect(poolFirstPlan.desiredWorkers == 8 &&
               std::ranges::any_of(
                   poolFirstPlan.goals, [](const protodd::ProductionGoal& goal) {
                       return goal.target == protodd::UnitKind::gateway &&
                              goal.blocking;
                   }),
           "pool-first-safe opening banks a Gateway before resuming Probe growth");

    protodd::ThreatAssessment earlyZergThreat;
    earlyZergThreat.immediateGround = 0.35;
    earlyZergThreat.combatEnemiesNearMain = 6;
    const auto pressuredPoolFirstPlan = strategy.plan(poolFirst, earlyZergThreat);
    expect(std::ranges::any_of(
               pressuredPoolFirstPlan.goals, [](const protodd::ProductionGoal& goal) {
                   return goal.target == protodd::UnitKind::photonCannon &&
                          goal.desiredCount >= 2 && goal.priority == 100;
               }),
           "confirmed early Zerg pressure immediately doubles static coverage");

    auto fortifiedPoolFirst = poolFirst;
    fortifiedPoolFirst.self.units.push_back(
        unit(90, protodd::UnitKind::forge, true, {160, 160}));
    fortifiedPoolFirst.self.units.push_back(
        unit(91, protodd::UnitKind::photonCannon, true, {180, 160}));
    fortifiedPoolFirst.self.units.push_back(
        unit(92, protodd::UnitKind::photonCannon, true, {200, 160}));
    const auto recoveryBehindCannons = strategy.plan(
        fortifiedPoolFirst, earlyZergThreat);
    expect(std::ranges::any_of(
               recoveryBehindCannons.goals, [](const protodd::ProductionGoal& goal) {
                   return goal.target == protodd::UnitKind::probe &&
                          goal.desiredCount >= 10 && goal.priority == 100 &&
                          goal.blocking;
               }),
           "completed anti-rush Cannons immediately restore Probe production");
}

void testStrategicTargeting() {
    protodd::GameState state;
    state.self.id = 1;
    state.self.race = protodd::Race::protoss;
    state.enemy.id = 2;
    state.enemy.race = protodd::Race::terran;
    state.bases = {
        {1, {256, 256}, {280, 256}, 8000, 5000, 1, 100, true, false},
        {2, {1800, 1800}, {1760, 1800}, 8000, 5000, -1, 0, true, false},
    };
    auto nexus = unit(1, protodd::UnitKind::nexus, true, {256, 256});
    nexus.role = protodd::UnitRole::resourceDepot;
    state.self.units.push_back(nexus);

    protodd::StrategyEngine strategy;
    const auto search = strategy.plan(state, {});
    expect(search.attackTarget == protodd::Position{1800, 1800},
           "unknown enemy search excludes our owned start location");
    expect(search.rallyPoint != nexus.position &&
               protodd::distance(search.rallyPoint, nexus.position) > 128.0 &&
               protodd::distance(search.rallyPoint, search.attackTarget) <
                   protodd::distance(nexus.position, search.attackTarget),
           "defensive rally screens the mineral line toward the enemy approach");

    auto hiddenTech = unit(20, protodd::UnitKind::factory, false, {1500, 1400});
    hiddenTech.visible = false;
    state.enemy.units.push_back(hiddenTech);
    const auto cleanup = strategy.plan(state, {});
    expect(cleanup.attackTarget == hiddenTech.position,
           "cleanup objective retains a remembered enemy structure");
}

void testEconomicRecovery() {
    protodd::GameState state;
    state.frame = 10 * 60 * 24;
    state.self.id = 1;
    state.self.race = protodd::Race::protoss;
    state.enemy.id = 2;
    state.enemy.race = protodd::Race::terran;
    state.self.supplyUsed = 30;
    state.self.supplyTotal = 50;
    state.bases.push_back(
        {1, {256, 256}, {280, 260}, 6000, 5000, -1, 0, true, false, 8, 1});
    for (int id = 1; id <= 5; ++id) {
        auto probe = unit(id, protodd::UnitKind::probe, true, {256 + id * 4, 256});
        probe.role = protodd::UnitRole::worker;
        state.self.units.push_back(probe);
    }
    protodd::StrategyEngine strategy;
    const auto lostMain = strategy.plan(state, {});
    expect(lostMain.posture == protodd::Posture::recover &&
               std::ranges::any_of(lostMain.goals, [](const protodd::ProductionGoal& goal) {
                   return goal.target == protodd::UnitKind::nexus && goal.blocking &&
                          goal.priority == 100;
               }),
           "surviving workers trigger an emergency Nexus rebuild");

    state.self.units.clear();
    state.bases.clear();
    for (int id = 1; id <= 3; ++id) {
        auto nexus = unit(id, protodd::UnitKind::nexus, true, {id * 400, 256});
        nexus.role = protodd::UnitRole::resourceDepot;
        state.self.units.push_back(nexus);
        state.bases.push_back(
            {id, {id * 400, 256}, {id * 400 + 30, 256}, 0, 0, 1, 0,
             id == 1, false, 0, 1});
    }
    for (int id = 10; id < 30; ++id) {
        auto probe = unit(id, protodd::UnitKind::probe, true);
        probe.role = protodd::UnitRole::worker;
        state.self.units.push_back(probe);
    }
    const auto depleted = strategy.plan(state, {});
    expect(depleted.desiredBases >= 4 &&
               std::ranges::any_of(depleted.goals, [](const protodd::ProductionGoal& goal) {
                   return goal.target == protodd::UnitKind::nexus &&
                          goal.desiredCount >= 4 && goal.blocking;
               }),
           "mined-out Nexuses do not prevent replacement expansion");

    auto disabledGateway = unit(50, protodd::UnitKind::gateway, true);
    disabledGateway.powered = false;
    state.self.units.push_back(disabledGateway);
    const auto repower = strategy.plan(state, {});
    expect(std::ranges::any_of(repower.goals, [](const protodd::ProductionGoal& goal) {
               return goal.target == protodd::UnitKind::pylon && goal.priority == 98 &&
                      goal.blocking;
           }),
           "unpowered production triggers mandatory local repowering");
}

void testMacroReservations() {
    protodd::GameState state;
    state.self.minerals = 200;
    protodd::StrategicPlan plan;
    plan.goals = {
        {protodd::GoalKind::build, protodd::UnitKind::pylon, 1, 100, true, "supply"},
        {protodd::GoalKind::build, protodd::UnitKind::gateway, 1, 90, false, "production"},
    };
    protodd::ResourceLedger ledger{state.self.minerals, 0};
    protodd::MacroPlanner planner;
    const auto actions = planner.reconcile(state, plan, ledger);
    expect(actions.size() == 1, "only affordable macro goal is emitted");
    expect(actions.front().target == protodd::UnitKind::pylon && actions.front().reserved,
           "higher-priority pylon reserves first");
    expect(ledger.freeMinerals() == 100, "resource reservation is explicit");

    state.self.units.push_back(unit(1, protodd::UnitKind::pylon, true));
    state.self.units.push_back(unit(2, protodd::UnitKind::nexus, true));
    protodd::StrategicPlan emergency;
    emergency.goals = {
        {protodd::GoalKind::build, protodd::UnitKind::gateway, 1, 100, true, "emergency"},
        {protodd::GoalKind::train, protodd::UnitKind::probe, 1, 50, false, "worker"},
    };
    protodd::ResourceLedger poor{100, 0};
    const auto waiting = planner.reconcile(state, emergency, poor);
    expect(waiting.size() == 2 &&
               std::ranges::any_of(waiting, [](const protodd::MacroAction& action) {
                   return action.target == protodd::UnitKind::probe && action.reserved &&
                          action.executable;
               }),
           "unaffordable structural goals preserve continuous worker production");
    expect(poor.freeMinerals() == 0 && poor.reservedMinerals == 100,
           "blocking reservation plus the worker cycle accounts for the full bank");

    protodd::GameState queuedState;
    queuedState.self.minerals = 50;
    queuedState.self.units.push_back(unit(2, protodd::UnitKind::nexus, true));
    queuedState.self.queuedUnits.push_back(protodd::UnitKind::probe);
    protodd::StrategicPlan queuedPlan;
    queuedPlan.goals = {
        {protodd::GoalKind::train, protodd::UnitKind::probe, 1, 80, false, "worker"},
    };
    protodd::ResourceLedger queuedLedger{50, 0};
    expect(planner.reconcile(queuedState, queuedPlan, queuedLedger).empty(),
           "queued production counts toward macro targets");

    protodd::GameState duplicateState;
    duplicateState.self.minerals = 800;
    duplicateState.self.units.push_back(unit(3, protodd::UnitKind::nexus, true));
    protodd::StrategicPlan duplicatePlan;
    duplicatePlan.goals = {
        {protodd::GoalKind::expand, protodd::UnitKind::nexus, 2, 90, false, "expand"},
        {protodd::GoalKind::expand, protodd::UnitKind::nexus, 2, 70, false, "economic style"},
    };
    protodd::ResourceLedger duplicateLedger{800, 0};
    const auto expansions = planner.reconcile(duplicateState, duplicatePlan, duplicateLedger);
    expect(expansions.size() == 1 && expansions.front().target == protodd::UnitKind::nexus,
           "overlapping strategic goals reserve only one missing structure");

    // Saving for a long-horizon expansion must not suppress the next Probe.
    // The Nexus reservation protects the bank, but leaves one worker cycle
    // available until the full 400-mineral cost can actually be issued.
    protodd::GameState expansionMacroState;
    expansionMacroState.self.minerals = 136;
    expansionMacroState.self.supplyTotal = 82;
    expansionMacroState.self.supplyUsed = 76;
    expansionMacroState.self.units = {unit(4, protodd::UnitKind::nexus, true)};
    protodd::StrategicPlan expansionMacroPlan;
    expansionMacroPlan.goals = {
        {protodd::GoalKind::expand, protodd::UnitKind::nexus, 2, 95, true,
         "safe natural"},
        {protodd::GoalKind::train, protodd::UnitKind::probe, 22, 93, false,
         "continuous workers"},
    };
    protodd::ResourceLedger expansionMacroLedger{136, 0};
    const auto expansionMacro = planner.reconcile(
        expansionMacroState, expansionMacroPlan, expansionMacroLedger);
    expect(std::ranges::any_of(expansionMacro, [](const protodd::MacroAction& action) {
               return action.target == protodd::UnitKind::probe && action.reserved &&
                      action.executable;
           }),
           "a pending expansion leaves one mineral cycle for continuous Probe production");

    protodd::GameState techMacroState;
    techMacroState.self.minerals = 120;
    techMacroState.self.supplyTotal = 34;
    techMacroState.self.supplyUsed = 20;
    techMacroState.self.units = {
        unit(5, protodd::UnitKind::nexus, true),
        unit(6, protodd::UnitKind::pylon, true),
        unit(7, protodd::UnitKind::gateway, true),
    };
    protodd::StrategicPlan techMacroPlan;
    techMacroPlan.goals = {
        {protodd::GoalKind::build, protodd::UnitKind::cyberneticsCore, 1, 95, true,
         "ranged transition"},
        {protodd::GoalKind::train, protodd::UnitKind::probe, 18, 93, false,
         "continuous workers"},
    };
    protodd::ResourceLedger techMacroLedger{120, 0};
    const auto techMacro = planner.reconcile(techMacroState, techMacroPlan, techMacroLedger);
    expect(std::ranges::any_of(techMacro, [](const protodd::MacroAction& action) {
               return action.target == protodd::UnitKind::probe && action.reserved &&
                      action.executable;
           }),
           "a pending tech transition leaves one mineral cycle for continuous Probe production");

    // An active melee emergency must fund its static anchor before another
    // Probe or Battery can consume the bank.  Otherwise a 120-mineral bank
    // can oscillate below the 150-mineral Forge cost indefinitely.
    protodd::GameState urgentAnchorState;
    urgentAnchorState.self.minerals = 120;
    urgentAnchorState.self.supplyTotal = 66;
    urgentAnchorState.self.supplyUsed = 30;
    urgentAnchorState.self.units = {
        unit(8, protodd::UnitKind::nexus, true),
        unit(9, protodd::UnitKind::pylon, true),
        unit(10, protodd::UnitKind::gateway, true),
    };
    protodd::StrategicPlan urgentAnchorPlan;
    urgentAnchorPlan.goals = {
        {protodd::GoalKind::build, protodd::UnitKind::forge, 1, 112, true,
         "anchor the mineral line"},
        {protodd::GoalKind::train, protodd::UnitKind::probe, 20, 93, false,
         "keep workers producing"},
    };
    protodd::ResourceLedger urgentAnchorLedger{120, 0};
    const auto urgentAnchor = planner.reconcile(
        urgentAnchorState, urgentAnchorPlan, urgentAnchorLedger);
    expect(std::ranges::none_of(urgentAnchor, [](const protodd::MacroAction& action) {
               return action.target == protodd::UnitKind::probe && action.reserved &&
                      action.executable;
           }) && urgentAnchorLedger.freeMinerals() == 0,
           "urgent static anchors take the current bank before worker refills");

    protodd::GameState blockedDuplicateState;
    blockedDuplicateState.self.minerals = 100;
    blockedDuplicateState.self.units = {
        unit(30, protodd::UnitKind::pylon, true),
        unit(31, protodd::UnitKind::gateway, true),
    };
    protodd::StrategicPlan blockedDuplicatePlan;
    blockedDuplicatePlan.goals = {
        {protodd::GoalKind::build, protodd::UnitKind::cyberneticsCore, 1, 95, true,
         "first core goal"},
        {protodd::GoalKind::build, protodd::UnitKind::cyberneticsCore, 1, 90, true,
         "overlapping core goal"},
    };
    protodd::ResourceLedger blockedDuplicateLedger{100, 0};
    const auto blockedCoreActions = planner.reconcile(
        blockedDuplicateState, blockedDuplicatePlan, blockedDuplicateLedger);
    expect(blockedCoreActions.size() == 1 &&
               blockedCoreActions.front().target == protodd::UnitKind::cyberneticsCore,
           "unaffordable blocking goals are deduplicated in one macro pass");

    protodd::GameState busyProducerState;
    busyProducerState.self.minerals = 250;
    busyProducerState.self.units = {
        unit(35, protodd::UnitKind::pylon, true),
        unit(36, protodd::UnitKind::gateway, true),
    };
    busyProducerState.self.queuedUnits = {protodd::UnitKind::zealot};
    protodd::StrategicPlan busyProducerPlan;
    busyProducerPlan.goals = {
        {protodd::GoalKind::train, protodd::UnitKind::zealot, 3, 98, true,
         "more defenders"},
        {protodd::GoalKind::build, protodd::UnitKind::gateway, 2, 97, true,
         "increase throughput"},
    };
    protodd::ResourceLedger busyProducerLedger{250, 0};
    const auto busyProducerActions = planner.reconcile(
        busyProducerState, busyProducerPlan, busyProducerLedger);
    expect(busyProducerActions.size() == 1 &&
               busyProducerActions.front().target == protodd::UnitKind::gateway &&
               busyProducerActions.front().reserved,
           "busy producers do not reserve queued units ahead of new throughput");

    busyProducerState.self.queuedUnits.clear();
    busyProducerState.self.busyProducers = {protodd::UnitKind::gateway};
    protodd::ResourceLedger latencyBusyLedger{250, 0};
    const auto latencyBusyActions = planner.reconcile(
        busyProducerState, busyProducerPlan, latencyBusyLedger);
    expect(latencyBusyActions.size() == 1 &&
               latencyBusyActions.front().target == protodd::UnitKind::gateway,
           "isTraining occupancy closes the BWAPI queue-visibility latency gap");

    protodd::GameState techState;
    techState.self.minerals = 100;
    protodd::StrategicPlan techPlan;
    techPlan.goals = {
        {protodd::GoalKind::train, protodd::UnitKind::dragoon, 1, 90, false, "tech unit"},
    };
    protodd::ResourceLedger techLedger{100, 0};
    const auto techActions = planner.reconcile(techState, techPlan, techLedger);
    expect(techActions.size() == 1 && techActions.front().target == protodd::UnitKind::pylon,
           "unreachable unit goals build the next missing prerequisite first");

    protodd::GameState pendingPrerequisite;
    pendingPrerequisite.self.minerals = 150;
    auto unfinishedPylon = unit(38, protodd::UnitKind::pylon, true);
    unfinishedPylon.completed = false;
    pendingPrerequisite.self.units.push_back(unfinishedPylon);
    protodd::StrategicPlan pendingPrerequisitePlan;
    pendingPrerequisitePlan.goals = {
        {protodd::GoalKind::train, protodd::UnitKind::zealot, 1, 99, true,
         "opening defender"},
    };
    protodd::ResourceLedger pendingPrerequisiteLedger{150, 0};
    const auto chainedActions = planner.reconcile(
        pendingPrerequisite, pendingPrerequisitePlan, pendingPrerequisiteLedger);
    expect(chainedActions.size() == 1 && chainedActions.front().reserved &&
               chainedActions.front().action == protodd::MacroActionKind::build &&
               chainedActions.front().target == protodd::UnitKind::gateway,
           "an in-progress Pylon advances reservation to the Gateway, not an impossible Zealot");

    protodd::GameState finishingGateway;
    finishingGateway.self.minerals = 150;
    auto incompleteGateway = unit(39, protodd::UnitKind::gateway, true);
    incompleteGateway.completed = false;
    finishingGateway.self.units.push_back(incompleteGateway);
    finishingGateway.self.units.push_back(
        unit(40, protodd::UnitKind::nexus, true));
    protodd::StrategicPlan finishingGatewayPlan;
    finishingGatewayPlan.goals = {
        {protodd::GoalKind::train, protodd::UnitKind::zealot, 1, 99, true,
         "reserve first defender"},
        {protodd::GoalKind::train, protodd::UnitKind::probe, 1, 74, false,
         "spend safe surplus"},
    };
    protodd::ResourceLedger finishingGatewayLedger{150, 0};
    const auto finishingGatewayActions = planner.reconcile(
        finishingGateway, finishingGatewayPlan, finishingGatewayLedger);
    expect(finishingGatewayActions.size() == 2 &&
               finishingGatewayActions.front().target == protodd::UnitKind::zealot &&
               finishingGatewayActions.front().reserved &&
               !finishingGatewayActions.front().executable &&
               finishingGatewayActions.back().target == protodd::UnitKind::probe &&
               finishingGatewayActions.back().reserved &&
               finishingGatewayActions.back().executable,
           "future unit reservation does not block executable surplus production");

    protodd::GameState compositionState;
    compositionState.self.minerals = 125;
    compositionState.self.gas = 50;
    compositionState.self.supplyTotal = 20;
    compositionState.self.units = {
        unit(4, protodd::UnitKind::gateway, true),
        unit(5, protodd::UnitKind::cyberneticsCore, true),
    };
    protodd::StrategicPlan compositionPlan;
    compositionPlan.composition = {{protodd::UnitKind::dragoon, 1.0}};
    protodd::ResourceLedger compositionLedger{125, 50};
    const auto compositionActions = planner.reconcile(
        compositionState, compositionPlan, compositionLedger);
    expect(compositionActions.size() == 1 &&
               compositionActions.front().target == protodd::UnitKind::dragoon,
           "remaining resources continuously reinforce the planned composition");

    compositionState.self.minerals = 100;
    compositionState.self.gas = 0;
    compositionPlan.composition = {
        {protodd::UnitKind::dragoon, 0.9}, {protodd::UnitKind::zealot, 0.1},
    };
    protodd::ResourceLedger fallbackLedger{100, 0};
    const auto fallbackActions = planner.reconcile(
        compositionState, compositionPlan, fallbackLedger);
    expect(fallbackActions.size() == 1 &&
               fallbackActions.front().target == protodd::UnitKind::zealot,
           "composition production falls back instead of idling on an unaffordable unit");
    compositionState.self.units.push_back(unit(99, protodd::UnitKind::zealot, true));
    fallbackLedger = {100, 0};
    const auto saturatedFallback = planner.reconcile(compositionState, compositionPlan, fallbackLedger);
    expect(std::ranges::none_of(saturatedFallback, [](const protodd::MacroAction& action) {
        return action.action == protodd::MacroActionKind::train && action.target == protodd::UnitKind::zealot;
    }), "gas shortage cannot buy excess Zealots beyond the requested army mix");

    protodd::GameState parallelProduction;
    parallelProduction.self.minerals = 300;
    parallelProduction.self.supplyTotal = 40;
    parallelProduction.self.units = {
        unit(20, protodd::UnitKind::gateway, true),
        unit(21, protodd::UnitKind::gateway, true),
        unit(22, protodd::UnitKind::gateway, true),
    };
    protodd::StrategicPlan parallelPlan;
    parallelPlan.composition = {{protodd::UnitKind::zealot, 1.0}};
    protodd::ResourceLedger parallelLedger{300, 0};
    const auto parallelActions = planner.reconcile(
        parallelProduction, parallelPlan, parallelLedger);
    expect(parallelActions.size() == 3 &&
               std::ranges::all_of(parallelActions, [](const protodd::MacroAction& action) {
                   return action.action == protodd::MacroActionKind::train &&
                          action.target == protodd::UnitKind::zealot && action.reserved;
               }),
           "one macro pass fills every affordable idle Gateway");

    parallelProduction.self.queuedUnits.push_back(protodd::UnitKind::zealot);
    protodd::ResourceLedger partlyBusyLedger{300, 0};
    expect(planner.reconcile(parallelProduction, parallelPlan, partlyBusyLedger).size() == 2,
           "existing queues consume producer slots before parallel pumping");

    parallelProduction.self.queuedUnits.clear();
    parallelProduction.self.supplyUsed = 36;
    protodd::ResourceLedger supplyBoundLedger{300, 0};
    expect(planner.reconcile(parallelProduction, parallelPlan, supplyBoundLedger).size() == 1,
           "parallel pumping never overcommits the remaining supply");

    protodd::GameState upgradeState;
    upgradeState.self.minerals = 150;
    upgradeState.self.gas = 150;
    upgradeState.self.units.push_back(unit(8, protodd::UnitKind::cyberneticsCore, true));
    protodd::StrategicPlan upgradePlan;
    upgradePlan.goals = {
        {protodd::GoalKind::upgrade, protodd::UnitKind::unknown, 1, 95, true,
         "dragoon range", protodd::TechnologyKind::singularityCharge},
    };
    protodd::ResourceLedger upgradeLedger{150, 150};
    const auto upgradeActions = planner.reconcile(upgradeState, upgradePlan, upgradeLedger);
    expect(upgradeActions.size() == 1 && upgradeActions.front().reserved &&
               upgradeActions.front().action == protodd::MacroActionKind::upgrade &&
               upgradeActions.front().technology ==
                   protodd::TechnologyKind::singularityCharge,
           "strategic upgrades reserve resources as executable macro actions");

    upgradeState.self.technologies.push_back(
        {protodd::TechnologyKind::singularityCharge, 0, true});
    protodd::ResourceLedger duplicateUpgradeLedger{150, 150};
    expect(planner.reconcile(upgradeState, upgradePlan, duplicateUpgradeLedger).empty(),
           "in-progress technology is never issued twice");

    upgradeState.self.technologies.clear();
    upgradeState.self.minerals = 100;
    upgradeState.self.gas = 0;
    upgradePlan.goals.push_back(
        {protodd::GoalKind::train, protodd::UnitKind::probe, 1, 30, false, "worker"});
    protodd::ResourceLedger savingLedger{100, 0};
    const auto savingActions = planner.reconcile(upgradeState, upgradePlan, savingLedger);
    expect(savingActions.size() == 1 && !savingActions.front().reserved &&
               savingActions.front().technology ==
                   protodd::TechnologyKind::singularityCharge,
           "mandatory technology preserves its bank instead of leaking to cheap production");

    upgradeState.self.units.push_back(unit(9, protodd::UnitKind::nexus, true));
    upgradeState.self.minerals = 250;
    protodd::ResourceLedger surplusLedger{250, 0};
    const auto surplusActions = planner.reconcile(upgradeState, upgradePlan, surplusLedger);
    expect(std::ranges::any_of(surplusActions, [](const protodd::MacroAction& action) {
               return action.target == protodd::UnitKind::probe && action.reserved;
           }) && surplusLedger.reservedMinerals == 200,
           "gas-starved technology protects its cost while surplus minerals keep probes flowing");

    protodd::GameState supplyInvariant;
    supplyInvariant.self.id = 1;
    supplyInvariant.self.race = protodd::Race::protoss;
    supplyInvariant.self.minerals = 100;
    supplyInvariant.self.supplyUsed = 12;
    supplyInvariant.self.supplyTotal = 18;
    supplyInvariant.self.units.push_back(unit(40, protodd::UnitKind::nexus, true));
    protodd::ResourceLedger supplyLedger{100, 0};
    const auto protectedSupply = planner.reconcile(supplyInvariant, {}, supplyLedger);
    expect(protectedSupply.size() == 1 && protectedSupply.front().reserved &&
               protectedSupply.front().blocksLowerPriority &&
               protectedSupply.front().target == protodd::UnitKind::pylon,
           "macro safety layer prevents a supply deadlock even with an empty strategy");

    auto pendingSupply = unit(41, protodd::UnitKind::pylon, true);
    pendingSupply.completed = false;
    supplyInvariant.self.units.push_back(pendingSupply);
    protodd::ResourceLedger pendingSupplyLedger{100, 0};
    expect(planner.reconcile(supplyInvariant, {}, pendingSupplyLedger).empty(),
           "supply invariant does not duplicate an in-progress pylon");
}

void testOpponentLearning() {
    protodd::OpponentHistory history;
    history.parse(
        "Bot,Map,standard,8,2\n"
        "Bot,Map,aggressive,2,8\n"
        "Bot,Map,economic,3,7\n"
        "Bot,Map,deceptive,1,9\n");
    expect(history.choose("Bot", "Map", 7) == protodd::OpeningStyle::standard,
           "UCB learning exploits clearly successful opening");
    history.record("Bot", "Map", protodd::OpeningStyle::standard, true);
    const auto encoded = history.serialize();
    protodd::OpponentHistory restored;
    restored.parse(encoded);
    expect(restored.lookup("Bot", "Map", protodd::OpeningStyle::standard).wins == 9,
           "opponent history round-trips through tournament CSV");

    restored.merge("Bot,Map,standard,11,2\nBot,Map,aggressive,1,4\n");
    expect(restored.lookup("Bot", "Map", protodd::OpeningStyle::standard).wins == 11 &&
               restored.lookup("Bot", "Map", protodd::OpeningStyle::standard).losses == 2 &&
               restored.lookup("Bot", "Map", protodd::OpeningStyle::aggressive).losses == 8,
           "read and write learning snapshots merge without losing cumulative results");

    protodd::OpponentHistory fresh;
    const auto exploration = fresh.choose("NewBot", "Map", 3);
    expect(exploration == protodd::OpeningStyle::standard,
           "fresh opponent starts from the robust baseline opening");
    fresh.record("NewBot", "Map", protodd::OpeningStyle::standard, true);
    expect(fresh.choose("NewBot", "Map", 3) != protodd::OpeningStyle::standard,
           "learning explores an untried style after collecting baseline evidence");
}

void testInfluenceAndCombat() {
    protodd::GameState state;
    state.mapWidthPixels = 1024;
    state.mapHeightPixels = 1024;
    auto enemy = unit(20, protodd::UnitKind::hydralisk, false, {512, 512});
    enemy.role = protodd::UnitRole::groundArmy;
    enemy.groundWeapon = {.damage = 10, .cooldown = 15, .maxRange = 128,
                          .targetsGround = true};
    state.enemy.units.push_back(enemy);
    protodd::InfluenceMap influence;
    influence.update(state);
    expect(influence.at({512, 512}).groundThreat > 0.0F,
           "enemy weapon contributes local ground threat");

    auto staleState = state;
    staleState.frame = 24 * 60;
    staleState.enemy.units.front().visible = false;
    staleState.enemy.units.front().lastSeen = 0;
    influence.update(staleState);
    expect(influence.at({512, 512}).groundThreat < 0.01F,
           "stale mobile enemies decay out of the fog-of-war threat field");
    auto cannon = staleState.enemy.units.front();
    cannon.kind = protodd::UnitKind::photonCannon;
    cannon.role = protodd::UnitRole::staticDefense;
    staleState.enemy.units = {cannon};
    influence.update(staleState);
    expect(influence.at({512, 512}).groundThreat > 0.0F,
           "remembered static defenses persist until their tile is cleared");

    auto detectorState = state;
    auto observer = unit(22, protodd::UnitKind::observer, false, {512, 512});
    observer.role = protodd::UnitRole::detector;
    observer.flying = true;
    observer.sightRange = 11 * 32;
    detectorState.enemy.units = {observer};
    influence.update(detectorState);
    expect(influence.at({800, 512}).detection > 0.0F,
           "mobile detection field uses the observer's real sight radius");

    std::vector<protodd::UnitSnapshot> friendly;
    for (int i = 0; i < 4; ++i) {
        auto dragoon = unit(30 + i, protodd::UnitKind::dragoon, true, {400, 400 + i * 8});
        dragoon.role = protodd::UnitRole::groundArmy;
        dragoon.shields = 80;
        dragoon.maxShields = 80;
        dragoon.groundWeapon = {.damage = 20, .cooldown = 30, .maxRange = 192,
                                .targetsGround = true};
        friendly.push_back(dragoon);
    }
    protodd::CombatEvaluator evaluator;
    const auto estimate = evaluator.evaluate(friendly, state.enemy.units, 1.1, 0.1);
    expect(estimate.decision == protodd::FightDecision::engage,
           "overwhelming dragoon force elects to engage");
    expect(estimate.simulatedEnemyRemaining < 0.001,
           "bounded combat simulation predicts lethal focus-fire volleys");
    expect(evaluator.selectTarget(friendly.front(), state.enemy.units) != nullptr,
           "combat target selection finds compatible target");

    auto hallucination = enemy;
    hallucination.hallucination = true;
    const std::vector<protodd::UnitSnapshot> hallucinations{hallucination};
    const auto hallucinationEstimate = evaluator.evaluate(
        friendly, hallucinations, 1.1, 0.0);
    expect(hallucinationEstimate.enemyPower == 0.0 &&
               evaluator.selectTarget(friendly.front(), hallucinations) == nullptr,
           "known hallucinations neither deter the army nor consume volleys");

    auto wounded = enemy;
    wounded.id = 21;
    wounded.hitPoints = 10;
    const std::vector<protodd::UnitSnapshot> targetChoices{wounded, enemy};
    const protodd::TargetAllocation lethalVolley[]{
        {wounded.id, 20},
    };
    expect(evaluator.selectTarget(friendly.front(), targetChoices, lethalVolley)->id == enemy.id,
           "focus fire redirects once a target has lethal committed damage");

    auto firstCorsair = unit(70, protodd::UnitKind::corsair, true, {400, 400});
    firstCorsair.role = protodd::UnitRole::airArmy;
    firstCorsair.flying = true;
    firstCorsair.airWeapon = {.damage = 5, .cooldown = 8, .maxRange = 160,
                              .targetsAir = true, .hits = 2};
    auto secondCorsair = firstCorsair;
    secondCorsair.id = 71;
    auto firstScourge = unit(72, protodd::UnitKind::scourge, false, {450, 400});
    firstScourge.role = protodd::UnitRole::airArmy;
    firstScourge.flying = true;
    firstScourge.hitPoints = 8;
    firstScourge.maxHitPoints = 25;
    auto secondScourge = firstScourge;
    secondScourge.id = 73;
    secondScourge.position = {455, 405};
    const std::vector<protodd::UnitSnapshot> corsairs{firstCorsair, secondCorsair};
    const std::vector<protodd::UnitSnapshot> scourge{firstScourge, secondScourge};
    protodd::CombatEstimate volleyEstimate;
    volleyEstimate.decision = protodd::FightDecision::engage;
    protodd::InfluenceMap volleyInfluence;
    protodd::TacticalController volleyTactics;
    const auto volleyOrders = volleyTactics.control(
        corsairs, scourge, volleyEstimate, {900, 900}, {100, 100}, volleyInfluence);
    expect(volleyOrders.size() == 2 &&
               volleyOrders[0].targetUnit != volleyOrders[1].targetUnit,
           "multi-hit volleys reserve exact lethal damage and avoid overkill");

    auto explosiveAttacker = friendly.front();
    explosiveAttacker.groundWeapon = {
        .damage = 100, .cooldown = 1000, .maxRange = 192,
        .damageType = protodd::DamageType::explosive, .targetsGround = true,
    };
    auto smallTarget = enemy;
    smallTarget.hitPoints = 100;
    smallTarget.maxHitPoints = 100;
    smallTarget.size = protodd::UnitSize::small;
    auto largeTarget = smallTarget;
    largeTarget.size = protodd::UnitSize::large;
    const std::vector<protodd::UnitSnapshot> oneAttacker{explosiveAttacker};
    const std::vector<protodd::UnitSnapshot> smallForce{smallTarget};
    const std::vector<protodd::UnitSnapshot> largeForce{largeTarget};
    const auto versusSmall = evaluator.evaluate(oneAttacker, smallForce, 1.0, 0.0);
    const auto versusLarge = evaluator.evaluate(oneAttacker, largeForce, 1.0, 0.0);
    expect(versusSmall.simulatedEnemyRemaining > versusLarge.simulatedEnemyRemaining,
           "simulation applies Brood War damage-type modifiers by unit size");

    auto singleHit = friendly.front();
    singleHit.groundWeapon = {.damage = 6, .cooldown = 30, .maxRange = 192,
                              .targetsGround = true, .hits = 1};
    auto multiHit = singleHit;
    multiHit.groundWeapon.hits = 4;
    const std::vector<protodd::UnitSnapshot> singleHitForce{singleHit};
    const std::vector<protodd::UnitSnapshot> multiHitForce{multiHit};
    const auto singleHitEstimate = evaluator.evaluate(
        singleHitForce, smallForce, 1.0, 0.0, false);
    const auto multiHitEstimate = evaluator.evaluate(
        multiHitForce, smallForce, 1.0, 0.0, false);
    expect(multiHitEstimate.friendlyPower > singleHitEstimate.friendlyPower,
           "fast combat estimate values every hit in a multi-hit weapon");

    auto armoredTarget = smallTarget;
    armoredTarget.armor = 5;
    auto fourSmallHits = singleHit;
    fourSmallHits.groundWeapon.cooldown = 1000;
    fourSmallHits.groundWeapon.hits = 4;
    auto oneLargeHit = fourSmallHits;
    oneLargeHit.groundWeapon.damage = 24;
    oneLargeHit.groundWeapon.hits = 1;
    const std::vector<protodd::UnitSnapshot> armoredForce{armoredTarget};
    const std::vector<protodd::UnitSnapshot> smallHitsForce{fourSmallHits};
    const std::vector<protodd::UnitSnapshot> largeHitForce{oneLargeHit};
    const auto smallHitsEstimate = evaluator.evaluate(
        smallHitsForce, armoredForce, 1.0, 0.0);
    const auto largeHitEstimate = evaluator.evaluate(
        largeHitForce, armoredForce, 1.0, 0.0);
    expect(smallHitsEstimate.simulatedEnemyRemaining >
               largeHitEstimate.simulatedEnemyRemaining,
           "armor is applied independently to every hit in a volley");

    auto kiter = friendly.front();
    kiter.position = {200, 200};
    kiter.weaponCooldown = 10;
    auto melee = unit(50, protodd::UnitKind::zergling, false, {250, 200});
    melee.role = protodd::UnitRole::groundArmy;
    melee.groundWeapon = {.damage = 5, .cooldown = 8, .maxRange = 32,
                          .targetsGround = true};
    protodd::CombatEstimate kiteEstimate;
    kiteEstimate.decision = protodd::FightDecision::kite;
    protodd::InfluenceMap emptyInfluence;
    protodd::TacticalController tactics;
    const std::vector<protodd::UnitSnapshot> kitingForce{kiter};
    const std::vector<protodd::UnitSnapshot> meleeForce{melee};
    const auto kiteOrders = tactics.control(
        kitingForce, meleeForce, kiteEstimate, {900, 900}, {100, 200}, emptyInfluence);
    expect(kiteOrders.size() == 1 && kiteOrders.front().type == protodd::CommandType::move &&
               kiteOrders.front().targetPosition.x < kiter.position.x,
           "ranged cooldown micro steps directly away from a nearby melee threat");

    auto firing = kiter;
    firing.attackFrame = true;
    const std::vector<protodd::UnitSnapshot> firingForce{firing};
    expect(tactics.control(firingForce, meleeForce, kiteEstimate,
                           {900, 900}, {100, 200}, emptyInfluence).empty(),
           "attack-frame protection does not cancel a committed volley");

    auto lockedAttacker = friendly.front();
    auto equalFirst = enemy;
    equalFirst.id = 80;
    equalFirst.position = {320, 320};
    auto equalLocked = equalFirst;
    equalLocked.id = 81;
    lockedAttacker.orderTargetId = equalLocked.id;
    const std::vector<protodd::UnitSnapshot> equalTargets{equalFirst, equalLocked};
    expect(evaluator.selectTarget(lockedAttacker, equalTargets)->id == equalLocked.id,
           "equal-value focus fire retains the current target instead of oscillating");

    auto zealotDefender = unit(82, protodd::UnitKind::zealot, true, {200, 200});
    zealotDefender.groundWeapon = {.damage = 8, .cooldown = 22, .maxRange = 32,
                                   .targetsGround = true, .hits = 2};
    auto nearbyLing = melee;
    nearbyLing.id = 83;
    nearbyLing.position = {280, 200};
    auto distantLing = nearbyLing;
    distantLing.id = 84;
    distantLing.position = {700, 200};
    distantLing.hitPoints = 1;
    const std::vector<protodd::UnitSnapshot> splitRush{distantLing, nearbyLing};
    expect(evaluator.selectTarget(zealotDefender, splitRush)->id == nearbyLing.id,
           "melee defenders do not chase a tempting distant target out of the base");

    auto templar = unit(60, protodd::UnitKind::highTemplar, true, {600, 500});
    templar.role = protodd::UnitRole::spellcaster;
    const std::vector<protodd::UnitSnapshot> casters{templar};
    const auto casterOrders = tactics.control(
        casters, meleeForce, estimate, {900, 900}, {100, 100}, emptyInfluence, {400, 400});
    expect(casterOrders.size() == 1 && casterOrders.front().source == "spellcaster-screen",
           "high-value spellcasters stay behind the formation screen");

    auto stormTemplar = templar;
    stormTemplar.energy = 100;
    stormTemplar.position = {500, 500};
    std::vector<protodd::UnitSnapshot> stormTargets;
    for (int i = 0; i < 4; ++i) {
        auto marine = unit(90 + i, protodd::UnitKind::marine, false,
                           {650 + i * 12, 500 + (i % 2) * 12});
        marine.role = protodd::UnitRole::groundArmy;
        stormTargets.push_back(marine);
    }
    const std::vector<protodd::UnitSnapshot> stormCasters{stormTemplar};
    const auto stormOrders = tactics.control(
        stormCasters, stormTargets, estimate, {900, 900}, {100, 100},
        emptyInfluence, {500, 500}, 2, true);
    expect(stormOrders.size() == 1 &&
               stormOrders.front().type == protodd::CommandType::useTech &&
               stormOrders.front().technology == protodd::TechnologyKind::psionicStorm,
           "researched High Templar cast safe high-value Psionic Storms tactically");

    auto friendlyDragoon = unit(95, protodd::UnitKind::dragoon, true,
                                stormTargets.front().position);
    const std::vector<protodd::UnitSnapshot> unsafeCasters{stormTemplar, friendlyDragoon};
    const auto unsafeStorm = tactics.control(
        unsafeCasters, stormTargets, estimate, {900, 900}, {100, 100},
        emptyInfluence, {500, 500}, 2, true);
    expect(std::ranges::none_of(unsafeStorm, [](const protodd::Command& command) {
               return command.type == protodd::CommandType::useTech;
           }),
           "storm targeting rejects clusters with excessive friendly fire");

    protodd::EngagementTracker engagement;
    expect(engagement.stabilize(77, protodd::FightDecision::engage, 1.3, 1.2, 100) ==
               protodd::FightDecision::engage,
           "first local combat estimate establishes a squad decision");
    expect(engagement.stabilize(77, protodd::FightDecision::kite, 1.0, 1.2, 102) ==
               protodd::FightDecision::engage &&
               engagement.stabilize(77, protodd::FightDecision::kite, 1.0, 1.2, 104) ==
                   protodd::FightDecision::engage &&
               engagement.stabilize(77, protodd::FightDecision::kite, 1.0, 1.2, 148) ==
                   protodd::FightDecision::kite,
           "borderline simulation noise cannot reverse a squad on one frame");
    expect(engagement.stabilize(77, protodd::FightDecision::retreat, 0.4, 1.2, 150) ==
               protodd::FightDecision::retreat,
           "catastrophic local odds bypass combat hysteresis immediately");
}

void testCommandArbitration() {
    protodd::CommandBus bus;
    bus.beginFrame(100, 2);
    bus.submit({7, protodd::CommandType::move, -1, {400, 400},
                protodd::UnitKind::unknown, 20, 0, "patrol"});
    bus.submit({7, protodd::CommandType::attackUnit, 9, {-1, -1},
                protodd::UnitKind::unknown, 80, 0, "combat"});
    bus.submit({7, protodd::CommandType::recharge, 10, {-1, -1},
                protodd::UnitKind::shieldBattery, 95, 0, "recharge"});
    auto selected = bus.finalize();
    expect(selected.size() == 1 && selected.front().type == protodd::CommandType::recharge,
           "shield preservation can override a routine attack per actor");

    bus.clear();
    bus.beginFrame(100, 2);
    bus.submit({7, protodd::CommandType::attackUnit, 9, {-1, -1},
                protodd::UnitKind::unknown, 80, 0, "combat"});
    selected = bus.finalize();
    bus.markIssued(selected.front());

    bus.beginFrame(101, 2);
    bus.submit(selected.front());
    expect(bus.finalize().empty(), "latency-window duplicate is suppressed");

    bus.beginFrame(107, 2);
    bus.submit(selected.front());
    expect(bus.finalize().empty(), "attack orders remain stable through the firing window");

    bus.beginFrame(109, 2);
    bus.submit(selected.front());
    expect(bus.finalize().empty(), "stable attack order is not spammed mid-cooldown");

    bus.beginFrame(119, 2);
    bus.submit(selected.front());
    expect(bus.finalize().size() == 1, "stable attack order refreshes after a full firing window");

    bus.clear();
    for (int cycle = 0; cycle < 2; ++cycle) {
        bus.beginFrame(200 + cycle, 0);
        for (int actor = 1; actor <= 4; ++actor) {
            bus.submit({actor, protodd::CommandType::move, -1, {actor * 32, 100},
                        protodd::UnitKind::unknown, 50, 0, "budget"});
        }
        const auto budgeted = bus.finalize(2);
        expect(budgeted.size() == 2, "combat command budget is enforced");
        if (cycle == 0) {
            expect(budgeted.front().actor == 1 && budgeted.back().actor == 2,
                   "first command-budget slice is deterministic");
        } else {
            expect(budgeted.front().actor == 3 && budgeted.back().actor == 4,
                   "equal-priority commands rotate fairly across ticks");
        }
    }
}

void testFrameBudget() {
    protodd::FrameBudget budget;
    expect(budget.load(100) == protodd::RuntimeLoad::normal,
           "frame budget begins at full quality");
    budget.record(100, 30000);
    expect(budget.load(101) == protodd::RuntimeLoad::reduced &&
               !budget.allowSimulation(101) &&
               budget.expensiveCadenceMultiplier(101) == 2,
           "slow frame temporarily sheds expensive optional work");
    expect(budget.load(400) == protodd::RuntimeLoad::normal,
           "quality automatically recovers after the cooldown window");
    budget.record(500, 56000);
    expect(budget.load(501) == protodd::RuntimeLoad::emergency &&
               budget.combatCommandLimit(501) == 40U &&
               budget.navigationInterval(501) == 96,
           "dangerous frame time enters the emergency budget");
    expect(budget.stats().over42ms == 1U && budget.stats().over55ms == 1U,
           "AIIDE frame-time thresholds are counted explicitly");
}

void testWorkersAndScouts() {
    const std::vector<protodd::MineralPatchCandidate> unbalancedPatches{
        {10, {300, 256}, 2}, {11, {340, 256}, 0}, {12, {380, 256}, 1},
    };
    expect(protodd::selectMineralPatch(unbalancedPatches, {340, 256}, {256, 256}, 10) == 11,
           "mineral assignment fills the least-saturated patch first");
    const std::vector<protodd::MineralPatchCandidate> balancedPatches{
        {10, {300, 256}, 1}, {11, {340, 256}, 1}, {12, {380, 256}, 1},
    };
    expect(protodd::selectMineralPatch(balancedPatches, {340, 256}, {256, 256}, 12) == 12,
           "balanced mineral assignment retains its current patch");

    protodd::MineralAllocator mineralAllocator;
    std::vector<protodd::MineralWorker> miningWorkers;
    for (int i = 0; i < 6; ++i) {
        miningWorkers.push_back({i, {256, 256}, {340, 256}, 10 + i % 3});
    }
    const auto miningTargets = mineralAllocator.assign(miningWorkers, balancedPatches);
    for (auto& worker : miningWorkers) {
        worker.currentTarget = -1;  // The order now targets the cargo depot.
        worker.position = {300 + (5 - worker.id) * 16, 260};
    }
    expect(mineralAllocator.assign(miningWorkers, balancedPatches) == miningTargets,
           "returning mineral cargo does not erase patch loads and reshuffle active miners");
    miningWorkers.erase(miningWorkers.begin());
    const auto afterWorkerLoss = mineralAllocator.assign(miningWorkers, balancedPatches);
    expect(!afterWorkerLoss.contains(0) && afterWorkerLoss.size() == 5,
           "dead or reassigned workers release their mineral reservations");
    const std::vector<protodd::MineralPatchCandidate> depletedPatches{
        {11, {340, 256}, 0}, {12, {380, 256}, 0},
    };
    const auto afterDepletion = mineralAllocator.assign(miningWorkers, depletedPatches);
    expect(afterDepletion.size() == 5 && std::ranges::none_of(afterDepletion,
               [](const auto& target) { return target.second == 10; }),
           "depleted patches release their workers for useful reassignment");
    miningWorkers.front().mineralLine = {1800, 1800};
    expect(!mineralAllocator.assign(miningWorkers, depletedPatches).contains(miningWorkers.front().id),
           "an explicit base transfer cannot retain a patch at the old base");

    protodd::GameState state;
    state.frame = 5000;
    state.mapWidthPixels = 2048;
    state.mapHeightPixels = 2048;
    state.self.id = 1;
    state.enemy.id = 2;
    state.bases.push_back({1, {256, 256}, {300, 260}, 8000, 5000, 1, 4000,
                           true, false, 8, 1});
    state.bases.push_back({2, {1700, 1700}, {1680, 1700}, 8000, 5000, -1, 0, true, false});
    auto probe = unit(5, protodd::UnitKind::probe, true, {260, 260});
    probe.role = protodd::UnitRole::worker;
    state.self.units.push_back(probe);
    auto observer = unit(6, protodd::UnitKind::observer, true, {300, 300});
    observer.flying = true;
    observer.role = protodd::UnitRole::detector;
    state.self.units.push_back(observer);
    state.self.units.push_back(unit(7, protodd::UnitKind::assimilator, true, {320, 256}));
    for (int i = 0; i < 8; ++i) {
        auto miner = probe;
        miner.id = 20 + i;
        state.self.units.push_back(miner);
    }

    protodd::InfluenceMap influence;
    influence.update(state);
    protodd::StrategicPlan plan;
    plan.desiredGasWorkers = 1;
    protodd::WorkerManager workers;
    const auto assignments = workers.assign(state, plan, influence);
    expect(assignments.size() == 9 && assignments.front().job == protodd::WorkerJob::gas,
           "gas policy assigns requested worker count");

    auto gasState = state;
    auto existingGasProbe = probe;
    existingGasProbe.id = 8;
    existingGasProbe.position = {420, 256};
    existingGasProbe.orderTargetId = 7;
    gasState.self.units.push_back(existingGasProbe);
    const auto stableGas = workers.assign(gasState, plan, influence);
    expect(std::ranges::any_of(stableGas, [](const protodd::WorkerAssignment& assignment) {
               return assignment.worker == 8 && assignment.job == protodd::WorkerJob::gas;
           }) && std::ranges::none_of(stableGas, [](const protodd::WorkerAssignment& assignment) {
               return assignment.worker == 5 && assignment.job == protodd::WorkerJob::gas;
           }),
           "gas rebalance preserves an existing refinery worker instead of oscillating jobs");

    gasState.self.units.back().orderTargetId = -1;
    gasState.self.units.back().gatheringGas = true;
    gasState.self.units.back().carryingResources = true;
    const auto returningGas = workers.assign(gasState, plan, influence);
    expect(std::ranges::any_of(returningGas, [](const protodd::WorkerAssignment& assignment) {
               return assignment.worker == 8 && assignment.job == protodd::WorkerJob::gas;
           }) && std::ranges::none_of(returningGas, [](const protodd::WorkerAssignment& assignment) {
               return assignment.worker == 5 && assignment.job == protodd::WorkerJob::gas;
           }), "a gas worker returning cargo retains its refinery slot instead of pulling in a mineral worker");

    gasState.self.minerals = 50;
    gasState.self.gas = 400;
    plan.posture = protodd::Posture::defend;
    const auto mineralRecovery = workers.assign(gasState, plan, influence);
    expect(std::ranges::none_of(
               mineralRecovery, [](const protodd::WorkerAssignment& assignment) {
                   return assignment.job == protodd::WorkerJob::gas;
               }),
           "mineral-starved defense releases gas workers after a sufficient gas bank");
    plan.posture = protodd::Posture::hold;
    auto assembling = workers.assign(gasState, plan, influence);
    expect(std::ranges::none_of(assembling, [](const protodd::WorkerAssignment& assignment) {
               return assignment.job == protodd::WorkerJob::gas;
           }), "one-base assembly spends its excess gas bank while redirecting mining to minerals");
    gasState.self.gas = 140;
    assembling = workers.assign(gasState, plan, influence);
    expect(std::ranges::any_of(assembling, [](const protodd::WorkerAssignment& assignment) {
               return assignment.job == protodd::WorkerJob::gas;
           }), "gas collection resumes at the lower refill threshold");
    auto raided = gasState;
    raided.self.units = {existingGasProbe, unit(7, protodd::UnitKind::assimilator, true, {320, 256})};
    raided.self.gas = 0;
    const auto lastWorker = workers.assign(raided, plan, influence);
    expect(lastWorker.size() == 1 && lastWorker.front().job == protodd::WorkerJob::minerals,
           "the last worker mines minerals to rebuild the economy even with an empty gas bank");

    protodd::GameState militiaState;
    militiaState.frame = 4 * 60 * 24;
    militiaState.self.id = 1;
    militiaState.enemy.id = 2;
    militiaState.mapWidthPixels = 2048;
    militiaState.mapHeightPixels = 2048;
    militiaState.bases.push_back(
        {1, {256, 256}, {300, 260}, 8000, 5000, 1, 0, true, false, 8, 1});
    for (int i = 0; i < 10; ++i) {
        auto defender = unit(100 + i, protodd::UnitKind::probe, true,
                             {240 + i * 8, 260});
        defender.role = protodd::UnitRole::worker;
        militiaState.self.units.push_back(defender);
    }
    auto tank = unit(200, protodd::UnitKind::siegeTank, false, {400, 260});
    tank.role = protodd::UnitRole::groundArmy;
    tank.groundWeapon = {.damage = 70, .cooldown = 75, .maxRange = 384,
                         .targetsGround = true};
    militiaState.enemy.units.push_back(tank);
    protodd::InfluenceMap militiaInfluence;
    militiaInfluence.update(militiaState);
    const auto tankResponse = workers.assign(militiaState, {}, militiaInfluence);
    expect(std::ranges::none_of(tankResponse, [](const protodd::WorkerAssignment& assignment) {
               return assignment.job == protodd::WorkerJob::defend;
           }),
           "worker militia never charges a siege tank");

    auto marineThreat = unit(207, protodd::UnitKind::marine, false, {360, 260});
    marineThreat.role = protodd::UnitRole::groundArmy;
    marineThreat.groundWeapon = {.damage = 6, .cooldown = 15, .maxRange = 128,
                                 .targetsGround = true};
    auto cannonScreen = unit(208, protodd::UnitKind::photonCannon, true, {300, 300});
    cannonScreen.role = protodd::UnitRole::staticDefense;
    cannonScreen.groundWeapon = {.damage = 20, .cooldown = 22, .maxRange = 224,
                                 .targetsGround = true};
    militiaState.self.units.push_back(cannonScreen);
    militiaState.enemy.units = {marineThreat};
    militiaInfluence.update(militiaState);
    const auto screenedMarineResponse = workers.assign(
        militiaState, {}, militiaInfluence);
    expect(std::ranges::none_of(
               screenedMarineResponse, [](const protodd::WorkerAssignment& assignment) {
                   return assignment.job == protodd::WorkerJob::defend;
               }) && std::ranges::any_of(
               screenedMarineResponse, [](const protodd::WorkerAssignment& assignment) {
                   return assignment.job == protodd::WorkerJob::evacuate;
               }),
           "Probes mineral-walk behind a completed static screen instead of charging Marines");
    militiaState.self.units.pop_back();

    auto loneScout = unit(202, protodd::UnitKind::probe, false, {350, 260});
    loneScout.role = protodd::UnitRole::worker;
    militiaState.enemy.units = {loneScout};
    militiaInfluence.update(militiaState);
    const auto scoutResponse = workers.assign(militiaState, {}, militiaInfluence);
    expect(std::ranges::none_of(
               scoutResponse, [](const protodd::WorkerAssignment& assignment) {
                   return assignment.job == protodd::WorkerJob::defend;
               }),
           "one enemy scout does not pull a Probe away from mining");

    auto zealotThreat = unit(203, protodd::UnitKind::zealot, false, {300, 260});
    zealotThreat.role = protodd::UnitRole::groundArmy;
    zealotThreat.groundWeapon = {.damage = 16, .cooldown = 22, .maxRange = 32,
                                 .targetsGround = true, .hits = 2};
    militiaState.enemy.units = {zealotThreat};
    militiaState.self.units.front().underAttack = true;
    militiaState.self.units.front().hitPoints = 60;
    militiaInfluence.update(militiaState);
    const auto woundedResponse = workers.assign(militiaState, {}, militiaInfluence);
    expect(std::ranges::any_of(
               woundedResponse, [zealotThreat](const protodd::WorkerAssignment& assignment) {
                   return assignment.worker == 100 &&
                          assignment.job == protodd::WorkerJob::evacuate &&
                          assignment.targetUnit == zealotThreat.id;
               }),
           "a Probe wounded by melee pressure disengages after the first hit");
    militiaState.self.units.front().underAttack = false;
    militiaState.self.units.front().hitPoints = 100;

    for (auto& worker : militiaState.self.units) {
        if (worker.kind == protodd::UnitKind::probe) worker.carryingResources = true;
    }
    auto firstLing = unit(204, protodd::UnitKind::zergling, false, {300, 252});
    firstLing.role = protodd::UnitRole::groundArmy;
    firstLing.groundWeapon = {.damage = 5, .cooldown = 8, .maxRange = 32,
                              .targetsGround = true};
    auto secondLing = firstLing;
    secondLing.id = 205;
    secondLing.position = {304, 268};
    militiaState.enemy.units = {firstLing, secondLing};
    militiaInfluence.update(militiaState);
    const auto cargoMilitia = workers.assign(militiaState, {}, militiaInfluence);
    expect(std::ranges::count(cargoMilitia, protodd::WorkerJob::defend,
                              &protodd::WorkerAssignment::job) == 4,
           "mineral-carrying Probes still join an emergency anti-ling surround");
    for (auto& worker : militiaState.self.units) {
        if (worker.kind == protodd::UnitKind::probe) worker.carryingResources = false;
    }

    auto secondZealot = zealotThreat;
    secondZealot.id = 206;
    secondZealot.position = {332, 260};
    militiaState.enemy.units = {zealotThreat, secondZealot};
    militiaInfluence.update(militiaState);
    const auto zealotMilitia = workers.assign(militiaState, {}, militiaInfluence);
    expect(std::ranges::count(zealotMilitia, protodd::WorkerJob::defend,
                              &protodd::WorkerAssignment::job) == 6,
           "a melee breach commits enough healthy Probes to form a surround");

    // Once a real mobile screen is already trading with a three-Zealot wave,
    // the economy should mineral-walk away instead of waiting for the last
    // Probe to become militia.  This is deliberately just before the six
    // minute militia-demand cutoff, matching the live opening pressure window.
    militiaState.frame = 5 * 60 * 24 + 12 * 24;
    auto screenZealot = zealotThreat;
    screenZealot.id = 210;
    screenZealot.position = {336, 260};
    auto screenZealotTwo = screenZealot;
    screenZealotTwo.id = 211;
    screenZealotTwo.position = {352, 260};
    auto screenZealotThree = screenZealot;
    screenZealotThree.id = 212;
    screenZealotThree.position = {368, 260};
    auto screenUnit = unit(213, protodd::UnitKind::zealot, true, {328, 260});
    screenUnit.role = protodd::UnitRole::groundArmy;
    auto screenUnitTwo = screenUnit;
    screenUnitTwo.id = 214;
    screenUnitTwo.position = {344, 260};
    militiaState.self.units.push_back(screenUnit);
    militiaState.self.units.push_back(screenUnitTwo);
    militiaState.enemy.units = {screenZealot, screenZealotTwo, screenZealotThree};
    militiaInfluence.update(militiaState);
    const auto evacuatedScreen = workers.assign(militiaState, {}, militiaInfluence);
    expect(std::ranges::count(evacuatedScreen, protodd::WorkerJob::evacuate,
                               &protodd::WorkerAssignment::job) >= 2 &&
               std::ranges::count(evacuatedScreen, protodd::WorkerJob::evacuate,
                                  &protodd::WorkerAssignment::job) <= 4 &&
               std::ranges::none_of(
                   evacuatedScreen, [](const protodd::WorkerAssignment& assignment) {
                       return assignment.job == protodd::WorkerJob::defend;
                   }),
           "a screened three-Zealot wave evacuates only the exposed edge while keeping a mining floor");

    militiaState.self.units.erase(
        std::remove_if(militiaState.self.units.begin(), militiaState.self.units.end(),
                       [](const protodd::UnitSnapshot& candidate) {
                           return candidate.id == 213 || candidate.id == 214;
                       }),
        militiaState.self.units.end());
    militiaState.frame = 4 * 60 * 24;

    auto proxyCannon = unit(201, protodd::UnitKind::photonCannon, false, {420, 280});
    proxyCannon.completed = false;
    proxyCannon.buildProgress = 35;
    militiaState.enemy.units = {loneScout};
    militiaState.enemy.units.push_back(proxyCannon);
    militiaInfluence.update(militiaState);
    const auto cannonResponse = workers.assign(militiaState, {}, militiaInfluence);
    expect(std::ranges::count(cannonResponse, protodd::WorkerJob::defend,
                              &protodd::WorkerAssignment::job) == 4 &&
               std::ranges::all_of(cannonResponse, [](const protodd::WorkerAssignment& assignment) {
                   return assignment.job != protodd::WorkerJob::defend ||
                          assignment.targetUnit == 201;
               }),
           "four healthy Probes focus an unfinished proxy cannon");

    const protodd::UnitId reservedProbe[]{probe.id};
    const auto leased = workers.assign(state, plan, influence, reservedProbe);
    expect(leased.size() == 9 && leased.front().worker == probe.id &&
               leased.front().job == protodd::WorkerJob::build,
           "leased scout or builder probe cannot be reclaimed by mining");

    protodd::GameState openingScoutState;
    openingScoutState.frame = 120;
    openingScoutState.self.units.push_back(probe);
    expect(protodd::selectOpeningWorkerScout(openingScoutState) == -1,
           "worker scouting waits until the opening pylon has started");
    openingScoutState.self.units.push_back(
        unit(8, protodd::UnitKind::pylon, true, {300, 300}));
    expect(protodd::selectOpeningWorkerScout(openingScoutState, {}, reservedProbe) == -1,
           "worker scouting never overwrites a reserved builder order");
    auto alternateProbe = probe;
    alternateProbe.id = 9;
    openingScoutState.self.units.push_back(alternateProbe);
    expect(protodd::selectOpeningWorkerScout(openingScoutState, {}, reservedProbe) == 9,
           "worker scouting selects a non-builder after pylon construction begins");
    auto knownEnemyMain = unit(10, protodd::UnitKind::commandCenter, false, {1600, 1600});
    knownEnemyMain.role = protodd::UnitRole::resourceDepot;
    openingScoutState.enemy.units.push_back(knownEnemyMain);
    expect(protodd::selectOpeningWorkerScout(openingScoutState) == -1,
           "worker scouting stops once the enemy main has been located");

    protodd::GameState noNexus;
    noNexus.self.id = 1;
    noNexus.mapWidthPixels = 2048;
    noNexus.mapHeightPixels = 2048;
    noNexus.bases.push_back(
        {1, {256, 256}, {300, 260}, 6000, 5000, -1, 0, true, false, 8, 1});
    auto survivor = unit(70, protodd::UnitKind::probe, true, {260, 260});
    survivor.role = protodd::UnitRole::worker;
    noNexus.self.units.push_back(survivor);
    protodd::InfluenceMap recoveryInfluence;
    recoveryInfluence.update(noNexus);
    const auto recoveryMining = workers.assign(noNexus, {}, recoveryInfluence);
    expect(recoveryMining.size() == 1 &&
               recoveryMining.front().job == protodd::WorkerJob::minerals,
           "surviving Probes keep mining while a replacement Nexus is built");

    const auto scoutState = state;
    state.bases[1].ownerId = 1;
    state.bases[1].mineralPatches = 8;
    for (int i = 0; i < 16; ++i) {
        auto extra = unit(20 + i, protodd::UnitKind::probe, true, {260 + i, 270});
        extra.role = protodd::UnitRole::worker;
        state.self.units.push_back(extra);
    }
    plan.desiredGasWorkers = 0;
    const auto balanced = workers.assign(state, plan, influence);
    expect(std::ranges::any_of(balanced, [](const protodd::WorkerAssignment& assignment) {
               return assignment.job == protodd::WorkerJob::transfer && assignment.baseId == 2;
           }),
           "oversaturated mineral lines transfer workers to an owned expansion");

    const protodd::UnitId scouts[]{6};
    protodd::ScoutManager scouting;
    const auto orders = scouting.assign(scoutState, scouts, influence, {});
    expect(orders.size() == 1 && orders.front().target == protodd::Position{1700, 1700},
           "scout prioritizes stale unexplored start location");

    protodd::GameState riskState;
    riskState.mapWidthPixels = 2048;
    riskState.mapHeightPixels = 2048;
    riskState.self.id = 1;
    riskState.enemy.id = 2;
    auto riskProbe = unit(300, protodd::UnitKind::probe, true, {128, 128});
    riskProbe.role = protodd::UnitRole::worker;
    riskState.self.units.push_back(riskProbe);
    riskState.bases.push_back(
        {3, {900, 128}, {900, 128}, 8000, 0, -1, 0, false, false, 8, 0});
    riskState.bases.push_back(
        {4, {128, 1800}, {128, 1800}, 8000, 0, -1, 0, false, false, 8, 0});
    auto corridorTank = unit(301, protodd::UnitKind::siegeTank, false, {520, 128});
    corridorTank.role = protodd::UnitRole::groundArmy;
    corridorTank.groundWeapon = {.damage = 70, .cooldown = 75, .maxRange = 384,
                                 .targetsGround = true};
    riskState.enemy.units.push_back(corridorTank);
    protodd::InfluenceMap riskInfluence;
    riskInfluence.update(riskState);
    protodd::ScoutManager riskScouting;
    const protodd::UnitId riskScoutIds[]{300};
    const auto safeProbeOrder = riskScouting.assign(riskState, riskScoutIds,
                                                    riskInfluence, {});
    expect(safeProbeOrder.size() == 1 &&
               safeProbeOrder.front().target == protodd::Position{128, 1800},
           "ground scout rejects a shorter route through siege-tank influence");

    riskState.self.units.front().kind = protodd::UnitKind::observer;
    riskState.self.units.front().role = protodd::UnitRole::detector;
    riskState.self.units.front().flying = true;
    riskScouting.reset();
    const auto flyingOrder = riskScouting.assign(riskState, riskScoutIds,
                                                 riskInfluence, {});
    expect(flyingOrder.size() == 1 &&
               flyingOrder.front().target == protodd::Position{520, 128} &&
               flyingOrder.front().purpose == protodd::ScoutPurpose::watchArmy,
           "flying scout ignores ground-only danger and shadows the army");
}

void testLocalSquadsAndDetection() {
    protodd::GameState state;
    state.mapWidthPixels = 2048;
    state.mapHeightPixels = 2048;
    state.self.id = 1;
    state.enemy.id = 2;
    state.bases.push_back({1, {256, 256}, {300, 260}, 8000, 5000, 1, 0, true, false});

    std::vector<protodd::UnitSnapshot> friendly;
    for (int i = 0; i < 6; ++i) {
        auto dragoon = unit(10 + i, protodd::UnitKind::dragoon, true,
                            i < 3 ? protodd::Position{300 + i * 24, 300}
                                  : protodd::Position{1500 + i * 24, 1500});
        dragoon.role = protodd::UnitRole::groundArmy;
        dragoon.groundWeapon = {.damage = 20, .cooldown = 30, .maxRange = 192,
                                .targetsGround = true};
        friendly.push_back(dragoon);
    }
    auto lurker = unit(90, protodd::UnitKind::lurker, false, {380, 320});
    lurker.role = protodd::UnitRole::groundArmy;
    lurker.burrowed = true;
    lurker.detected = false;
    lurker.groundWeapon = {.damage = 20, .cooldown = 37, .maxRange = 192,
                           .targetsGround = true};
    const std::vector<protodd::UnitSnapshot> enemy{lurker};

    protodd::StrategicPlan plan;
    plan.rallyPoint = {256, 256};
    plan.attackTarget = {1800, 1800};
    protodd::SquadPlanner planner;
    auto squads = planner.form(state, friendly, enemy, plan, {256, 256});
    const auto defense = std::ranges::find_if(squads, [](const protodd::Squad& squad) {
        return squad.role == protodd::SquadRole::baseDefense;
    });
    expect(defense != squads.end() && defense->needsDetection,
           "cloaked base threat creates detection-aware defense squad");
    expect(defense != squads.end() &&
               protodd::distance(defense->retreat, state.bases.front().mineralLine) >
                   protodd::distance(state.bases.front().center,
                                   state.bases.front().mineralLine) &&
               defense->requiredRatio < 0.6,
           "base defense screens on the safe side of the economy instead of retreating through workers");

    auto breachedDefense = *defense;
    auto visibleLing = unit(91, protodd::UnitKind::zergling, false,
                            breachedDefense.retreat);
    visibleLing.role = protodd::UnitRole::groundArmy;
    visibleLing.groundWeapon = {.damage = 5, .cooldown = 8, .maxRange = 32,
                                .targetsGround = true};
    breachedDefense.enemies = {visibleLing};
    expect(protodd::SquadPlanner::mustHoldDefensiveScreen(breachedDefense),
           "base defenders stop retreating once melee attackers breach the economy screen");
    breachedDefense.retreat = {600, 500};
    breachedDefense.defense = {{600, 500}, 256, {350, 500}};
    visibleLing.position = {180, 500};
    breachedDefense.enemies = {visibleLing};
    expect(protodd::SquadPlanner::mustHoldDefensiveScreen(breachedDefense),
           "a mineral-line breach triggers a stand even outside the forward retreat anchor");
    visibleLing.position = {1800, 1800};
    breachedDefense.enemies = {visibleLing};
    expect(!protodd::SquadPlanner::mustHoldDefensiveScreen(breachedDefense),
           "base defenders can still disengage before a distant threat reaches the economy");

    std::vector<protodd::UnitSnapshot> heavyThreats;
    for (int i = 0; i < 3; ++i) {
        auto tank = unit(110 + i, protodd::UnitKind::siegeTank, false,
                         {400 + i * 24, 320});
        tank.role = protodd::UnitRole::groundArmy;
        tank.groundWeapon = {.damage = 70, .cooldown = 75, .maxRange = 384,
                             .targetsGround = true};
        heavyThreats.push_back(tank);
    }
    const auto heavyDefense = planner.form(
        state, friendly, heavyThreats, plan, {256, 256});
    const auto committed = std::ranges::find_if(
        heavyDefense, [](const protodd::Squad& squad) {
            return squad.role == protodd::SquadRole::baseDefense;
        });
    expect(committed != heavyDefense.end() && committed->units.size() == friendly.size(),
           "base defense commits enough army value to answer heavy units, not a fixed headcount");

    auto observer = unit(100, protodd::UnitKind::observer, true, {200, 200});
    observer.flying = true;
    observer.role = protodd::UnitRole::detector;
    state.self.units.push_back(observer);
    protodd::InfluenceMap influence;
    influence.update(state);
    const auto escorts = planner.detectorEscorts(state, squads, influence);
    expect(!escorts.empty() && escorts.front().actor == observer.id,
           "observer is assigned to highest-priority detection squad");

    auto localCannon = unit(101, protodd::UnitKind::photonCannon, true, {280, 280});
    localCannon.role = protodd::UnitRole::staticDefense;
    localCannon.groundWeapon = {.damage = 20, .cooldown = 22, .maxRange = 224,
                                .targetsGround = true};
    friendly.push_back(localCannon);
    squads = planner.form(state, friendly, {}, plan, {256, 256});
    const auto mainGroups = std::ranges::count_if(squads, [](const protodd::Squad& squad) {
        return squad.role == protodd::SquadRole::mainArmy;
    });
    expect(mainGroups == 2 && std::ranges::none_of(
               squads, [](const protodd::Squad& squad) {
                   return std::ranges::any_of(squad.units, [](const protodd::UnitSnapshot& member) {
                       return protodd::isStaticDefense(member.kind);
                   });
               }),
           "static defenses cannot glue disconnected mobile armies into one squad");

    const auto* vanguard = protodd::SquadPlanner::selectVanguard(squads, plan.attackTarget);
    expect(vanguard != nullptr && vanguard->center.x > 1000,
           "the strongest forward mobile component becomes the reinforcement vanguard");

    const auto firstSignature = squads.front().signature;
    std::ranges::reverse(friendly);
    const auto reordered = planner.form(state, friendly, {}, plan, {256, 256});
    expect(!reordered.empty() && reordered.front().signature == firstSignature,
           "squad signature is deterministic across observation order");
    plan.attackTarget = {1700, 1500};
    const auto rerouted = planner.form(state, friendly, {}, plan, {256, 256});
    expect(!rerouted.empty() && rerouted.front().signature != firstSignature,
           "squad route identity changes immediately when its objective changes");
}

void testTransportMissions() {
    protodd::GameState state;
    state.mapWidthPixels = 2048;
    state.mapHeightPixels = 2048;
    auto shuttle = unit(200, protodd::UnitKind::shuttle, true, {100, 100});
    shuttle.flying = true;
    shuttle.role = protodd::UnitRole::transport;
    shuttle.cargoSpace = 8;
    auto reaver = unit(201, protodd::UnitKind::reaver, true, {300, 100});
    reaver.role = protodd::UnitRole::groundArmy;
    state.self.units = {shuttle, reaver};
    protodd::InfluenceMap influence;
    influence.update(state);
    protodd::TransportController transports;

    auto orders = transports.control(state, {1000, 100}, {100, 100}, influence);
    expect(std::ranges::any_of(orders, [](const protodd::Command& command) {
               return command.actor == 200 && command.source == "shuttle-rendezvous";
           }) && std::ranges::any_of(orders, [](const protodd::Command& command) {
               return command.actor == 201 && command.source == "reaver-rendezvous";
           }),
           "shuttle and reaver rendezvous under persistent mission ownership");

    state.frame = 1;
    state.self.units[1].position = {150, 100};
    orders = transports.control(state, {1000, 100}, {100, 100}, influence);
    expect(std::ranges::any_of(orders, [](const protodd::Command& command) {
               return command.type == protodd::CommandType::load && command.targetUnit == 201;
           }),
           "nearby reaver receives a transport load command");

    state.frame = 2;
    state.self.units[1].loaded = true;
    state.self.units[1].transportId = 200;
    state.self.units[1].position = state.self.units[0].position;
    orders = transports.control(state, {1000, 100}, {100, 100}, influence);
    expect(std::ranges::any_of(orders, [](const protodd::Command& command) {
               return command.source == "shuttle-attack-route";
           }),
           "loaded shuttle begins its threat-aware attack transit");

    state.frame = 3;
    state.self.units[0].position = {900, 100};
    state.self.units[1].position = {900, 100};
    orders = transports.control(state, {1000, 100}, {100, 100}, influence);
    expect(std::ranges::any_of(orders, [](const protodd::Command& command) {
               return command.type == protodd::CommandType::unload &&
                      command.source == "reaver-drop";
           }),
           "shuttle unloads the reaver at the mission objective");

    state.frame = 4;
    state.self.units[1].loaded = false;
    state.self.units[1].transportId = -1;
    static_cast<void>(transports.control(state, {1000, 100}, {100, 100}, influence));
    state.frame = 4 + 7 * 24;
    orders = transports.control(state, {1000, 100}, {100, 100}, influence);
    expect(std::ranges::any_of(orders, [](const protodd::Command& command) {
               return command.type == protodd::CommandType::load &&
                      command.source == "reaver-extract";
           }),
           "drop mission extracts its reaver after the bounded firing window");
}

}  // namespace

void testCompetitionRegressions() {
    using namespace protodd;
    auto attacker = unit(1, UnitKind::dragoon, true, {100, 100});
    attacker.groundWeapon = {.damage = 20, .cooldown = 30, .maxRange = 192,
                            .damageType = DamageType::explosive, .targetsGround = true};
    auto target = unit(2, UnitKind::zealot, false, {200, 100});
    target.size = UnitSize::small;
    target.armor = 4;
    expect(attackDamage(attacker, target) == 8.0,
           "HP armor is subtracted before explosive size reduction");
    target.shields = 60;
    target.maxShields = 60;
    expect(attackDamage(attacker, target) == 20.0,
           "explosive damage is not reduced by size or HP armor against shields");
    target.shieldArmor = 2;
    expect(attackDamage(attacker, target) == 18.0,
           "shield upgrades reduce each shield hit independently");
    expect(attackDamage(attacker, target, 100.0) == 8.0,
           "simulation switches to HP armor after shields have depleted");
    attacker.groundWeapon = {.damage = 8, .cooldown = 22, .maxRange = 32,
                             .targetsGround = true, .hits = 2};
    target.shields = 8;
    target.shieldArmor = 0;
    expect(attackDamage(attacker, target) == 12.0,
           "multi-hit volleys cross the shield boundary hit by hit");
    attacker.dimensionRight = 16;
    target.dimensionLeft = 16;
    expect(weaponDistance(attacker, target) == 68.0,
           "weapon range uses collision edges rather than unit centers");
    target.invincible = true;
    expect(CombatEvaluator{}.selectTarget(attacker, std::vector{target}) == nullptr,
           "invincible or stasised enemies do not attract focus fire");

    GameState state;
    state.self.supplyTotal = 80;
    state.self.supplyUsed = 20;
    state.self.units = {unit(10, UnitKind::nexus, true), unit(11, UnitKind::pylon, true),
                        unit(12, UnitKind::gateway, true),
                        unit(13, UnitKind::cyberneticsCore, true)};
    StrategicPlan plan;
    plan.goals = {{GoalKind::train, UnitKind::dragoon, 4, 100, true, "ranged"},
                  {GoalKind::train, UnitKind::zealot, 4, 99, true, "melee"},
                  {GoalKind::train, UnitKind::probe, 12, 90, false, "economy"}};
    ResourceLedger ledger{225, 50};
    const auto actions = MacroPlanner{}.reconcile(state, plan, ledger);
    expect(std::ranges::count_if(actions, [](const MacroAction& action) {
               return action.reserved && action.action == MacroActionKind::train;
           }) == 2 && std::ranges::any_of(actions, [](const MacroAction& action) {
               return action.target == UnitKind::probe && action.reserved;
           }), "one Gateway reserves one unit while the Nexus keeps producing");
    state.self.units[2].powered = false;
    ResourceLedger unpoweredLedger{225, 50};
    const auto unpowered = MacroPlanner{}.reconcile(state, plan, unpoweredLedger);
    expect(std::ranges::none_of(unpowered, [](const MacroAction& action) {
               return action.action == MacroActionKind::train &&
                      action.target != UnitKind::probe;
           }), "unpowered Gateways do not trap the macro resource budget");
    state.self.units[2].powered = true;
    state.self.supplyUsed = 78;
    ResourceLedger supplyLedger{225, 50};
    const auto supplyActions = MacroPlanner{}.reconcile(state, plan, supplyLedger);
    expect(supplyActions.size() == 1 && supplyActions.front().target == UnitKind::probe,
           "explicit unit goals respect shared supply and permit a smaller legal unit");
    state.self.supplyUsed = 20;
    state.self.units.push_back(unit(14, UnitKind::forge, true));
    state.self.technologies = {{TechnologyKind::protossGroundWeapons, 0, true}};
    plan.goals = {{GoalKind::upgrade, UnitKind::unknown, 1, 100, true, "armor",
                   TechnologyKind::protossGroundArmor},
                  {GoalKind::train, UnitKind::probe, 12, 90, false, "economy"}};
    ResourceLedger busyTechLedger{100, 100};
    const auto busyTech = MacroPlanner{}.reconcile(state, plan, busyTechLedger);
    expect(busyTech.size() == 1 && busyTech.front().target == UnitKind::probe,
           "busy research buildings cannot reserve a second simultaneous upgrade");
    state.self.units.push_back(unit(15, UnitKind::stargate, true));
    state.self.units.push_back(unit(16, UnitKind::arbiterTribunal, true));
    state.self.busyProducers = {UnitKind::stargate};
    plan.goals = {{GoalKind::train, UnitKind::arbiter, 2, 100, true, "arbiter"}};
    ResourceLedger arbiterLedger{100, 350};
    expect(MacroPlanner{}.reconcile(state, plan, arbiterLedger).empty(),
           "Arbiters obey Stargate occupancy");

    CommandBus bus;
    Command retreat{1, CommandType::move, -1, {50, 50}, UnitKind::unknown, 100, 0, "retreat"};
    Command attack{1, CommandType::attackUnit, 2, {-1, -1}, UnitKind::unknown, 80, 0, "attack"};
    bus.beginFrame(100, 3);
    bus.markIssued(retreat);
    bus.beginFrame(102, 3);
    bus.submit(retreat);
    bus.submit(attack);
    expect(bus.finalize().empty(),
           "deduplicated high-priority retreat still excludes a lower-priority attack");

    auto templar = unit(30, UnitKind::highTemplar, true, {500, 500});
    templar.energy = 100;
    templar.role = UnitRole::spellcaster;
    std::vector<UnitSnapshot> mutas;
    for (int i = 0; i < 4; ++i) {
        auto muta = unit(40 + i, UnitKind::mutalisk, false, {650 + i * 8, 500});
        muta.flying = true;
        mutas.push_back(muta);
    }
    CombatEstimate estimate;
    estimate.decision = FightDecision::engage;
    InfluenceMap empty;
    TacticalController tactics;
    const auto storm = tactics.control(std::vector{templar}, mutas, estimate,
                                      {900, 900}, {100, 100}, empty, {500, 500}, 3, true);
    expect(storm.size() == 1 && storm.front().technology == TechnologyKind::psionicStorm,
           "Storm targets clustered Mutalisks as well as ground armies");
    auto friendlyAir = unit(50, UnitKind::carrier, true, {650, 500});
    friendlyAir.flying = true;
    const auto unsafe = tactics.control(std::vector{templar, friendlyAir}, mutas, estimate,
                                       {900, 900}, {100, 100}, empty, {500, 500}, 3, true);
    expect(std::ranges::none_of(unsafe, [](const Command& command) {
               return command.technology == TechnologyKind::psionicStorm;
           }), "Storm evaluates friendly air casualties");
    attacker.underStorm = true;
    attacker.attackFrame = true;
    const auto escape = tactics.control(std::vector{attacker}, {}, estimate,
                                        {900, 900}, {50, 50}, empty);
    expect(escape.size() == 1 && escape.front().source == "storm-escape",
           "escaping active Storm overrides attack animation preservation");

    std::vector<UnitSnapshot> marching;
    for (int i = 0; i < 4; ++i) {
        marching.push_back(unit(51 + i, UnitKind::zealot, true, {800 + i * 32, 500}));
    }
    const Position marchTarget{1800, 500};
    const auto march = tactics.control(marching, {}, estimate, marchTarget,
                                       {100, 500}, empty, {250, 500});
    expect(march.size() == marching.size() && std::ranges::all_of(march,
               [marchTarget](const Command& command) {
                   return command.type == CommandType::attackMove &&
                          command.targetPosition == marchTarget;
               }), "uncontested travel does not pull advancing units back toward a stale centroid");

    GameState detectionState;
    detectionState.mapWidthPixels = 2048;
    detectionState.mapHeightPixels = 2048;
    auto cannon = unit(60, UnitKind::photonCannon, false, {512, 512});
    cannon.role = UnitRole::staticDefense;
    cannon.sightRange = 320;
    detectionState.enemy.units = {cannon};
    InfluenceMap detection;
    detection.update(detectionState);
    expect(detection.at(cannon.position).detection > 0.0F,
           "Photon Cannons threaten cloaked harassment through detection");
    detectionState.enemy.units.front().powered = false;
    detection.update(detectionState);
    expect(detection.at(cannon.position).detection == 0.0F,
           "unpowered Cannons do not project detection");

    GameState raidState;
    raidState.self.id = 1;
    raidState.bases = {{1, {256, 256}, {200, 256}, 8000, 5000, 1},
                       {2, {2500, 256}, {2600, 256}, 8000, 5000, 1}};
    std::vector<UnitSnapshot> defenders;
    std::vector<UnitSnapshot> raiders;
    for (int i = 0; i < 6; ++i) {
        auto defender = unit(70 + i, UnitKind::dragoon, true,
                             {i < 3 ? 300 : 2500, 300 + i * 8});
        defender.groundWeapon = {.damage = 20, .cooldown = 30, .maxRange = 192,
                                 .targetsGround = true};
        defenders.push_back(defender);
    }
    for (int i = 0; i < 2; ++i) {
        auto raider = unit(80 + i, UnitKind::marine, false, {i == 0 ? 400 : 2400, 300});
        raider.groundWeapon = {.damage = 6, .cooldown = 15, .maxRange = 128,
                              .targetsGround = true};
        raiders.push_back(raider);
    }
    const auto defense = SquadPlanner{}.form(raidState, defenders, raiders, {}, {256, 256});
    expect(std::ranges::count(defense, SquadRole::baseDefense, &Squad::role) == 2,
           "simultaneous raids receive separate base defense detachments");
    raidState.self.units = {unit(83, UnitKind::photonCannon, true, {300, 320}),
                            unit(84, UnitKind::photonCannon, true, {2500, 320})};
    const auto reserveArea = SquadPlanner::defensiveArea(raidState, {400, 256});
    expect(reserveArea.center == Position{300, 320} && reserveArea.pursuitRadius == 256,
           "unassigned main-army reserves also hold the local Cannon screen");
    auto reaverThreat = unit(82, UnitKind::reaver, false, {400, 300});
    reaverThreat.groundWeapon = {.damage = 100, .cooldown = 60, .maxRange = 256,
                                 .targetsGround = true};
    const auto reaverDefense = SquadPlanner{}.form(raidState, defenders,
                                                  std::vector{reaverThreat}, {}, {256, 256});
    expect(std::ranges::count(reaverDefense, SquadRole::baseDefense, &Squad::role) == 1,
           "missing current ammunition does not hide an enemy Reaver raid from base defense");
    plan.attackTarget = {4000, 4000};
    cannon.position = plan.attackTarget;
    const auto distant = SquadPlanner{}.form(raidState, defenders, std::vector{cannon},
                                             plan, {256, 256});
    expect(std::ranges::all_of(distant, [](const Squad& squad) { return squad.enemies.empty(); }),
           "distant objective defenses do not poison local combat estimates");

    expect(OpponentHistory::filename("alias-A") != OpponentHistory::filename("alias-B") &&
               OpponentHistory::filename("../alias").find('/') == std::string::npos,
           "tournament learning uses separate path-safe alias filenames");
    GameState gasState;
    gasState.frame = 6 * 60 * 24;
    gasState.self.race = Race::protoss;
    gasState.enemy.race = Race::zerg;
    gasState.self.units = state.self.units;
    ThreatAssessment airRush;
    airRush.combatEnemiesNearMain = 3;
    airRush.air = 0.8;
    const auto airPlan = StrategyEngine{}.plan(gasState, airRush);
    expect(airPlan.desiredGasWorkers >= 3 && std::ranges::any_of(airPlan.goals,
               [](const ProductionGoal& goal) { return goal.target == UnitKind::assimilator; }),
           "emergency anti-air reaction funds its gas-dependent counter units");
    const auto workerGoal = std::ranges::find(airPlan.goals, UnitKind::probe,
                                              &ProductionGoal::target);
    expect(workerGoal != airPlan.goals.end() && workerGoal->desiredCount <= airPlan.desiredWorkers,
           "final worker cap reaches production goals after emergency reactions");

    GameState mirror;
    mirror.frame = 5 * 60 * 24;
    mirror.self.race = Race::protoss;
    mirror.enemy.race = Race::protoss;
    mirror.self.supplyUsed = 16;
    mirror.self.supplyTotal = 34;
    mirror.self.units = {unit(90, UnitKind::nexus, true), unit(91, UnitKind::pylon, true),
                         unit(92, UnitKind::gateway, true), unit(93, UnitKind::gateway, true),
                         unit(94, UnitKind::photonCannon, true),
                         unit(95, UnitKind::photonCannon, true), unit(96, UnitKind::zealot, true)};
    for (int i = 0; i < 6; ++i) {
        auto probe = unit(100 + i, UnitKind::probe, true);
        probe.role = UnitRole::worker;
        mirror.self.units.push_back(probe);
    }
    ThreatAssessment mirrorRush;
    mirrorRush.combatEnemiesNearMain = 6;
    const auto mirrorPlan = StrategyEngine{}.plan(mirror, mirrorRush);
    ResourceLedger mirrorLedger{250, 0};
    const auto mirrorActions = MacroPlanner{}.reconcile(mirror, mirrorPlan, mirrorLedger);
    expect(std::ranges::any_of(mirrorActions, [](const MacroAction& action) {
               return action.target == UnitKind::probe && action.reserved && action.priority >= 104;
           }) && std::ranges::any_of(mirrorActions, [](const MacroAction& action) {
               return action.target == UnitKind::shieldBattery && action.reserved;
           }), "a held mirror rush funds worker recovery and Battery before endless Zealot replacement");

    mirror.frame = 9 * 60 * 24;
    for (int i = 0; i < 10; ++i) {
        auto probe = unit(110 + i, UnitKind::probe, true);
        probe.role = UnitRole::worker;
        mirror.self.units.push_back(probe);
    }
    ThreatAssessment staleRush;
    staleRush.mostLikely = EnemyPlan::fastRush;
    staleRush.uncertainty = 1.0;
    expect(StrategyEngine{}.plan(mirror, staleRush).posture == Posture::pressure,
           "a stale rush label with maximum uncertainty does not delay the attack until minute 16");
    staleRush.uncertainty = 0.2;
    expect(StrategyEngine{}.plan(mirror, staleRush).posture == Posture::hold,
           "confident rush evidence retains a defensive assembly posture");
    staleRush.uncertainty = 1.0;
    staleRush.combatEnemiesNearMain = 6;
    expect(StrategyEngine{}.plan(mirror, staleRush).posture == Posture::defend,
           "a current visible threat overrides uncertainty and keeps emergency defense active");
}

void testEconomicTargeting() {
    using namespace protodd;
    Squad squad;
    auto attacker = unit(1, UnitKind::zealot, true, {500, 500});
    attacker.groundWeapon = {.damage = 8, .cooldown = 22, .maxRange = 32,
                             .targetsGround = true, .hits = 2};
    squad.units = {attacker};
    auto worker = unit(2, UnitKind::probe, false, {540, 500});
    auto production = unit(3, UnitKind::gateway, false, {650, 500});
    auto remote = unit(4, UnitKind::probe, false, {1600, 500});
    auto hidden = unit(5, UnitKind::probe, false, {550, 500});
    hidden.visible = false;
    auto immortal = unit(6, UnitKind::pylon, false, {560, 500});
    immortal.invincible = true;
    auto targets = SquadPlanner::tacticalTargets(squad,
        std::vector{worker, production, remote, hidden, immortal});
    const auto estimate = CombatEvaluator{}.evaluate(squad.units, squad.enemies, 1.2, 0.0);
    expect(targets.size() == 2 && estimate.enemyPower == 0.0,
           "local worker and production targets do not add distant or hidden units to a fight");
    const InfluenceMap influence;
    auto orders = TacticalController{}.control(squad.units, targets, estimate,
                                                {1000, 500}, {200, 500}, influence);
    expect(orders.size() == 1 && orders.front().type == CommandType::attackUnit &&
               orders.front().targetUnit == worker.id,
           "an army explicitly attacks an exposed worker instead of relying on attack-move");
    targets = SquadPlanner::tacticalTargets(squad, std::vector{production});
    orders = TacticalController{}.control(squad.units, targets, estimate,
                                          {1000, 500}, {200, 500}, influence);
    expect(orders.size() == 1 && orders.front().targetUnit == production.id,
           "cleanup explicitly targets a surviving production building");
    auto proxy = unit(7, UnitKind::photonCannon, false, {570, 500});
    proxy.completed = false;
    targets = SquadPlanner::tacticalTargets(squad, std::vector{proxy});
    orders = TacticalController{}.control(squad.units, targets, estimate,
                                          {1000, 500}, {200, 500}, influence);
    expect(orders.size() == 1 && orders.front().targetUnit == proxy.id,
           "defenders can destroy an unfinished proxy before its weapon becomes active");
    squad.enemies = {proxy};
    expect(SquadPlanner::tacticalTargets(squad, std::vector{proxy}).size() == 1,
           "a hostile already in the combat set is not duplicated for focus-fire allocation");
}

void testReserveCounterattack() {
    using namespace protodd;
    Squad reserves;
    reserves.role = SquadRole::mainArmy;
    for (int i = 0; i < 14; ++i) {
        auto fighter = unit(i, UnitKind::zealot, true, {500 + i * 2, 500});
        fighter.groundWeapon = {.damage = 8, .cooldown = 22, .maxRange = 32,
                               .targetsGround = true, .hits = 2};
        reserves.units.push_back(fighter);
    }
    reserves.enemies = {unit(100, UnitKind::zergling, false, {900, 500})};
    StrategicPlan plan;
    plan.posture = Posture::defend;
    plan.minimumAttackSize = 14;
    CombatEstimate advantage;
    advantage.decision = FightDecision::engage;
    advantage.ratio = 1.9;
    const auto canAdvance = SquadPlanner::canCounterattack(reserves, advantage, plan);
    const auto orders = TacticalController{}.control(reserves.units, reserves.enemies,
        advantage, {1400, 500}, {500, 500}, InfluenceMap{}, {}, 3, false,
        canAdvance ? DefenseArea{} : DefenseArea{{500, 500}, 256});
    expect(canAdvance && !orders.empty() && orders.front().type == CommandType::attackUnit,
           "a strong independent reserve force can break a contain despite the global defense posture");
    reserves.role = SquadRole::baseDefense;
    expect(!SquadPlanner::canCounterattack(reserves, advantage, plan),
           "counterattack permission does not release the assigned base defenders");
    reserves.role = SquadRole::mainArmy;
    advantage.ratio = 1.2;
    expect(!SquadPlanner::canCounterattack(reserves, advantage, plan),
           "a marginal fight estimate cannot override the defensive posture");
    advantage.ratio = 1.9;
    reserves.units.back() = unit(99, UnitKind::highTemplar, true, {500, 500});
    expect(!SquadPlanner::canCounterattack(reserves, advantage, plan),
           "support casters do not satisfy the minimum counterattacking fighter count");
    reserves.units.pop_back();
    for (int i = 0; i < 4; ++i) {
        auto cannon = unit(200 + i, UnitKind::photonCannon, true);
        cannon.groundWeapon = {.damage = 20, .cooldown = 22, .maxRange = 224,
                              .targetsGround = true};
        reserves.units.push_back(cannon);
    }
    expect(!SquadPlanner::canCounterattack(reserves, advantage, plan),
           "static defenses cannot inflate a breakout force");
}

void testRangedDefense() {
    using namespace protodd;
    const DefenseArea area{{500, 500}, 320};
    auto defender = unit(1, UnitKind::zealot, true, {500, 500});
    defender.groundWeapon = {.damage = 8, .cooldown = 22, .maxRange = 32,
                             .targetsGround = true, .hits = 2};
    auto bait = unit(2, UnitKind::dragoon, false, {900, 500});
    bait.groundWeapon = {.damage = 20, .cooldown = 30, .maxRange = 192,
                         .targetsGround = true};
    CombatEstimate engage;
    engage.decision = FightDecision::engage;
    TacticalController tactics;
    InfluenceMap influence;
    auto covert = unit(6, UnitKind::darkTemplar, true, {500, 500});
    covert.cloaked = true;
    covert.groundWeapon = {.damage = 40, .cooldown = 30, .maxRange = 32,
                           .targetsGround = true};
    auto contain = unit(7, UnitKind::marine, false, {900, 500});
    contain.groundWeapon = {.damage = 6, .cooldown = 15, .maxRange = 128,
                            .targetsGround = true};
    CombatEstimate losing;
    losing.decision = FightDecision::retreat;
    losing.ratio = 0.1;
    auto ranged = unit(9, UnitKind::dragoon, true, {500, 500});
    ranged.groundWeapon = bait.groundWeapon;
    auto pursuer = bait;
    pursuer.position = {660, 500};
    const auto retreatOrders = [&]() {
        return tactics.control(std::vector{ranged}, std::vector{pursuer}, losing,
            pursuer.position, {350, 500}, influence, {}, 3, false, area);
    };
    auto firing = retreatOrders();
    expect(firing.size() == 1 && firing.front().type == CommandType::attackUnit,
           "a retreating Dragoon takes an available shot instead of conceding free damage");
    ranged.weaponCooldown = 20;
    firing = retreatOrders();
    expect(firing.size() == 1 && firing.front().type == CommandType::move,
           "a Dragoon falls back between retreat volleys");
    ranged.weaponCooldown = 0;
    pursuer.position = {900, 500};
    firing = retreatOrders();
    expect(firing.size() == 1 && firing.front().type == CommandType::move,
           "retreat fire never chases a pursuer outside weapon range");
    pursuer.position = {660, 500};
    ranged.kind = UnitKind::reaver;
    ranged.ammo = 0;
    firing = retreatOrders();
    expect(firing.size() == 1 && firing.front().type == CommandType::move,
           "an empty Reaver keeps retreating while Scarabs replenish");
    ranged.ammo = 2;
    firing = retreatOrders();
    expect(firing.size() == 1 && firing.front().type == CommandType::attackUnit,
           "a loaded army Reaver can fire during a losing defensive engagement");
    ranged.hitPoints = 1;
    ranged.shields = 0;
    firing = retreatOrders();
    expect(firing.size() == 1 && firing.front().type == CommandType::move,
           "critically wounded ranged units prioritize survival over a retreat volley");
    auto covertOrders = tactics.control(std::vector{covert}, std::vector{contain}, losing,
        contain.position, area.center, influence, {}, 3, false, area);
    expect(covertOrders.size() == 1 && covertOrders.front().type == CommandType::attackUnit,
           "a cloaked Dark Templar can challenge an undetected contain beyond Cannon support");
    covert.underAttack = true;
    covertOrders = tactics.control(std::vector{covert}, std::vector{contain}, losing,
        contain.position, area.center, influence, {}, 3, false, area);
    expect(covertOrders.size() == 1 && covertOrders.front().source == "combat-retreat",
           "actual incoming attacks cancel the covert-advance assumption");
    covert.underAttack = false;
    GameState detectedState;
    detectedState.mapWidthPixels = 2048;
    detectedState.mapHeightPixels = 2048;
    auto detector = unit(8, UnitKind::missileTurret, false, {550, 500});
    detector.role = UnitRole::detector;
    detectedState.enemy.units = {detector};
    InfluenceMap detectionInfluence;
    detectionInfluence.update(detectedState);
    covertOrders = tactics.control(std::vector{covert}, std::vector{contain}, losing,
        contain.position, area.center, detectionInfluence, {}, 3, false, area);
    expect(covertOrders.size() == 1 && covertOrders.front().source == "combat-retreat",
           "observed detector coverage prevents covert advances into a losing fight");
    auto wounded = unit(4, UnitKind::dragoon, true, {500, 500});
    wounded.maxHitPoints = 100;
    wounded.hitPoints = 20;
    wounded.maxShields = 80;
    wounded.shields = 0;
    auto battery = unit(5, UnitKind::shieldBattery, true, {550, 500});
    battery.energy = 100;
    CommandBus recovery;
    recovery.beginFrame(100, 3);
    for (const auto& order : tactics.control(std::vector{wounded}, {}, engage,
                                              {900, 500}, {400, 500}, influence)) {
        recovery.submit(order);
    }
    for (const auto& order : tactics.recharge(std::vector{wounded, battery}, true)) {
        recovery.submit(order);
    }
    const auto recoveryOrders = recovery.finalize();
    expect(recoveryOrders.size() == 1 && recoveryOrders.front().type == CommandType::recharge,
           "a critically wounded Dragoon can recharge instead of being locked into endless retreat");
    battery.powered = false;
    expect(tactics.recharge(std::vector{wounded, battery}, true).empty(),
           "unpowered Batteries cannot divert damaged units away from retreat");
    auto orders = tactics.control(std::vector{defender}, std::vector{bait}, engage,
                                  bait.position, area.center, influence, {}, 3, false, area);
    expect(orders.size() == 1 && orders.front().type == CommandType::hold,
           "a defender holds its screen instead of chasing ranged bait outside the base");
    auto intruder = unit(3, UnitKind::zealot, false, {600, 500});
    orders = tactics.control(std::vector{defender}, std::vector{bait, intruder}, engage,
                             bait.position, area.center, influence, {}, 3, false, area);
    expect(orders.size() == 1 && orders.front().targetUnit == intruder.id,
           "an excluded high-value ranged target does not hide a legal base intruder");
    const DefenseArea offsetCannons{{500, 500}, 256, {250, 500}};
    intruder.position = {180, 500};
    orders = tactics.control(std::vector{defender}, std::vector{intruder}, engage,
                             intruder.position, {250, 500}, influence, {}, 3, false, offsetCannons);
    expect(orders.size() == 1 && orders.front().targetUnit == intruder.id,
           "Cannon pursuit limits still allow defending the uncovered side of the mineral line");
    defender.position = {900, 500};
    orders = tactics.control(std::vector{defender}, std::vector{bait}, engage,
                             bait.position, area.center, influence, {}, 3, false, area);
    expect(orders.size() == 1 && orders.front().source == "defense-return",
           "an already drawn-out defender returns even when the local fight looks favorable");
    defender.position = {750, 500};
    defender.kind = UnitKind::dragoon;
    defender.groundWeapon = bait.groundWeapon;
    orders = tactics.control(std::vector{defender}, std::vector{bait}, engage,
                             bait.position, area.center, influence, {}, 3, false, area);
    expect(orders.size() == 1 && orders.front().type == CommandType::attackUnit,
           "a ranged defender can shoot beyond the pursuit boundary without chasing");
    defender.position = {950, 500};
    defender.attackFrame = true;
    expect(tactics.control(std::vector{defender}, std::vector{bait}, engage,
                            bait.position, area.center, influence, {}, 3, false, area).empty(),
           "returning to defense does not cancel a shot already firing");
    defender.attackFrame = false;
    orders = tactics.control(std::vector{defender}, std::vector{bait}, engage,
                             bait.position, area.center, influence);
    expect(orders.size() == 1 && orders.front().type == CommandType::attackUnit,
           "attacking squads remain free to pursue targets beyond a defensive perimeter");

    GameState state;
    state.frame = 5 * 60 * 24;
    state.self.race = Race::protoss;
    state.enemy.race = Race::protoss;
    state.self.supplyUsed = 28;
    state.self.supplyTotal = 66;
    state.self.units = {unit(10, UnitKind::nexus, true), unit(11, UnitKind::pylon, true),
                        unit(12, UnitKind::forge, true), unit(13, UnitKind::gateway, true),
                        unit(14, UnitKind::gateway, true), unit(15, UnitKind::photonCannon, true),
                        unit(16, UnitKind::photonCannon, true), unit(17, UnitKind::zealot, true),
                        unit(18, UnitKind::shieldBattery, true)};
    for (int i = 0; i < 12; ++i) {
        auto probe = unit(20 + i, UnitKind::probe, true);
        probe.role = UnitRole::worker;
        state.self.units.push_back(probe);
    }
    // Scouting a Core under construction is enough to anticipate ranged units.
    auto enemyCore = unit(40, UnitKind::cyberneticsCore, false);
    enemyCore.completed = false;
    state.enemy.units = {enemyCore};
    ThreatAssessment pressure;
    pressure.combatEnemiesNearMain = 4;
    auto plan = StrategyEngine{}.plan(state, pressure);
    expect(plan.posture == Posture::defend && plan.desiredWorkers >= 20 &&
               plan.desiredGasWorkers >= 3,
           "a scouted ranged transition restores sustainable income during sustained defense");
    ResourceLedger coreLedger{250, 0};
    auto actions = MacroPlanner{}.reconcile(state, plan, coreLedger);
    expect(std::ranges::any_of(actions, [](const MacroAction& action) {
               return action.target == UnitKind::cyberneticsCore && action.reserved;
           }) && std::ranges::any_of(actions, [](const MacroAction& action) {
               return action.target == UnitKind::probe && action.reserved;
           }), "two Cannons and a mobile screen fund the counter's Core alongside worker growth");
    state.self.units.push_back(unit(41, UnitKind::cyberneticsCore, true));
    state.self.gas = 50;
    plan = StrategyEngine{}.plan(state, pressure);
    ResourceLedger gasLedger{150, 0};
    actions = MacroPlanner{}.reconcile(state, plan, gasLedger);
    expect(std::ranges::any_of(actions, [](const MacroAction& action) {
               return action.target == UnitKind::assimilator && action.reserved;
           }), "the ranged counter's gas supply cannot be starved by repeated Zealot goals");
    state.self.units.push_back(unit(42, UnitKind::assimilator, true));
    ResourceLedger armyLedger{175, 50};
    actions = MacroPlanner{}.reconcile(state, plan, armyLedger);
    expect(std::ranges::any_of(actions, [](const MacroAction& action) {
               return action.target == UnitKind::dragoon && action.reserved;
           }) && std::ranges::none_of(actions, [](const MacroAction& action) {
               return action.target == UnitKind::zealot && action.reserved;
           }), "the first ranged defender takes priority over a replenishment Zealot");
    state.self.units.erase(std::remove_if(state.self.units.begin(), state.self.units.end(),
        [](const UnitSnapshot& candidate) { return candidate.kind == UnitKind::zealot; }),
        state.self.units.end());
    expect(StrategyEngine{}.plan(state, pressure).desiredWorkers >= 20,
           "losing a screening Zealot does not undo an established technology transition");
    state.enemy.units.clear();
    state.self.units.erase(std::remove_if(state.self.units.begin(), state.self.units.end(),
        [](const UnitSnapshot& candidate) { return candidate.kind == UnitKind::cyberneticsCore; }),
        state.self.units.end());
    plan = StrategyEngine{}.plan(state, pressure);
    expect(plan.desiredWorkers <= 12 && plan.desiredGasWorkers == 0,
           "a pure melee rush without a stable mobile screen retains emergency mineral defense");

    state.enemy.units = {unit(50, UnitKind::gateway, false),
                         unit(51, UnitKind::gateway, false)};
    state.self.id = 1;
    state.bases = {{1, {256, 256}, {180, 256}, 8000, 5000, 1}};
    state.self.units.push_back(unit(52, UnitKind::zealot, true));
    state.self.units.push_back(unit(53, UnitKind::zealot, true));
    plan = StrategyEngine{}.plan(state, {});
    expect(plan.desiredBases == 1 && std::ranges::any_of(plan.goals,
               [](const ProductionGoal& goal) {
                   return goal.target == UnitKind::photonCannon && goal.blocking;
           }), "scouted double melee production adds support before risking an expansion");
    for (int i = 0; i < 8; ++i) {
        auto probe = unit(60 + i, UnitKind::probe, true);
        probe.role = UnitRole::worker;
        state.self.units.push_back(probe);
    }
    for (const auto style : {OpeningStyle::standard, OpeningStyle::economic}) {
        const auto constrained = StrategyEngine{}.plan(state, {}, style);
        expect(constrained.desiredBases == 1 && std::ranges::none_of(constrained.goals,
                   [](const ProductionGoal& goal) {
                       return goal.goal == GoalKind::expand && goal.desiredCount > 1;
                   }), "saturation and learned economy cannot override a scouted one-base defense");
    }
    state.enemy.units.push_back(enemyCore);
    plan = StrategyEngine{}.plan(state, {});
    expect(std::ranges::none_of(plan.goals, [](const ProductionGoal& goal) {
               return goal.target == UnitKind::photonCannon && goal.blocking;
           }), "observed ranged tech changes the double-Gateway response back to mobile counters");
}

void testBananaBrainMacroRegressions() {
    using namespace protodd;
    expect(trainingSlotAvailable(false, 0, 0, 6, false), "idle Nexus accepts a Probe");
    expect(trainingSlotAvailable(true, 1, 4, 6, false),
           "next Probe is scheduled before the active Probe finishes within latency");
    expect(!trainingSlotAvailable(true, 1, 7, 6, false) &&
               !trainingSlotAvailable(true, 2, 4, 6, false) &&
               !trainingSlotAvailable(false, 0, 0, 6, true),
           "early, duplicate queued, and unacknowledged train commands cannot spend twice");

    GameState state;
    state.frame = 100;
    state.self.units = {unit(1, UnitKind::nexus, true), unit(2, UnitKind::pylon, true)};
    StrategicPlan request;
    request.goals = {{GoalKind::build, UnitKind::forge, 1, 110, true, "temporary threat"}};
    MacroPlanner planner;
    ResourceLedger emptyBank{};
    expect(!planner.reconcile(state, request, emptyBank).empty(), "record a deferred structure");
    for (state.frame = 124; state.frame <= 820; state.frame += 24) {
        ResourceLedger bank{};
        expect(!planner.reconcile(state, {}, bank).empty(), "deferred structure survives brief plan churn");
    }
    state.frame = 844;
    ResourceLedger expiredBank{};
    expect(planner.reconcile(state, {}, expiredBank).empty(),
           "repeated reconciliation cannot renew an abandoned structure forever");

    state.frame = 850;
    request.goals.push_back({GoalKind::build, UnitKind::forge, 1, 60, true, "routine forge"});
    MacroPlanner duplicatePlanner;
    ResourceLedger renewedBank{};
    (void)duplicatePlanner.reconcile(state, request, renewedBank);
    state.frame = 853;
    ResourceLedger rememberedBank{};
    const auto remembered = duplicatePlanner.reconcile(state, {}, rememberedBank);
    expect(remembered.size() == 1 && remembered.front().priority == 110,
           "duplicate explicit requests remember the strongest current checkpoint");

    request.desiredBases = 2;
    request.goals = {{GoalKind::expand, UnitKind::nexus, 2, 120, true, "safe natural"}};
    state.frame = 900;
    ResourceLedger expansionBank{};
    (void)planner.reconcile(state, request, expansionBank);
    state.frame = 903;
    StrategicPlan emergency;
    emergency.posture = Posture::defend;
    ResourceLedger emergencyBank{400, 0};
    expect(planner.reconcile(state, emergency, emergencyBank).empty() &&
               emergencyBank.reservedMinerals == 0,
           "new defense cancels an unstarted expansion reservation immediately");

    state.self.units.push_back(unit(3, UnitKind::cyberneticsCore, true));
    state.self.units.push_back(unit(4, UnitKind::citadelOfAdun, true));
    request.goals = {
        {GoalKind::upgrade, UnitKind::unknown, 1, 100, false, "range", TechnologyKind::singularityCharge},
        {GoalKind::upgrade, UnitKind::unknown, 1, 90, false, "speed", TechnologyKind::legEnhancements}};
    ResourceLedger upgradeBank{1000, 1000};
    auto actions = MacroPlanner{}.reconcile(state, request, upgradeBank);
    expect(std::ranges::count_if(actions, [](const MacroAction& action) {
               return action.reserved && action.technology != TechnologyKind::none;
           }) == 2, "distinct upgrades survive demand merging and use separate producers");

    state.self.race = Race::protoss;
    state.self.supplyTotal = 100;
    state.self.supplyUsed = 80;
    for (int i = 0; i < 8; ++i) state.self.units.push_back(unit(10 + i, UnitKind::gateway, true));
    request.goals.clear();
    request.desiredWorkers = 22;
    request.composition = {{UnitKind::zealot, 1.0}};
    ResourceLedger supplyBank{100, 0};
    actions = MacroPlanner{}.reconcile(state, request, supplyBank);
    expect(std::ranges::any_of(actions, [](const MacroAction& action) {
               return action.target == UnitKind::pylon && action.reserved;
           }), "eight Gateways forecast supply beyond the old fixed sixteen-supply buffer");
    auto pendingPylon = unit(30, UnitKind::pylon, true);
    pendingPylon.completed = false;
    state.self.units.push_back(pendingPylon);
    ResourceLedger pendingBank{100, 0};
    actions = MacroPlanner{}.reconcile(state, request, pendingBank);
    expect(std::ranges::none_of(actions, [](const MacroAction& action) {
               return action.target == UnitKind::pylon;
           }), "forecast credits a pending Pylon instead of buying redundant supply");

    GameState workersState;
    workersState.frame = 13 * 60 * 24;
    workersState.self.id = 1;
    workersState.enemy.id = 2;
    workersState.bases = {{1, {512, 512}, {512, 560}, 8000, 5000, 1}};
    for (int i = 0; i < 10; ++i) {
        auto probe = unit(100 + i, UnitKind::probe, true, {512 + i * 4, 560});
        probe.role = UnitRole::worker;
        workersState.self.units.push_back(probe);
    }
    auto dt = unit(200, UnitKind::darkTemplar, false, {1600, 512});
    dt.detected = false;
    dt.groundWeapon = {.damage = 40, .cooldown = 30, .maxRange = 32, .targetsGround = true};
    workersState.enemy.units = {dt};
    InfluenceMap influence;
    influence.update(workersState);
    const WorkerManager workers;
    auto assignments = workers.assign(workersState, {}, influence);
    expect(std::ranges::all_of(assignments, [](const WorkerAssignment& assignment) {
               return assignment.job == WorkerJob::minerals;
           }), "a distant undetected DT does not evacuate a safe mineral line");
    workersState.enemy.units.front().position = {540, 560};
    influence.update(workersState);
    assignments = workers.assign(workersState, {}, influence);
    expect(std::ranges::any_of(assignments, [](const WorkerAssignment& assignment) {
               return assignment.job == WorkerJob::evacuate;
           }), "nearby undetected DT still triggers escape");
    workersState.enemy.units.front().position = {1000, 560};
    influence.update(workersState);
    assignments = workers.assign(workersState, {}, influence);
    expect(std::ranges::all_of(assignments, [](const WorkerAssignment& assignment) {
               return assignment.job == WorkerJob::minerals;
           }), "workers resume mining once separation is restored even while a DT remains visible");
    workersState.enemy.units.front().kind = UnitKind::zealot;
    workersState.enemy.units.front().detected = true;
    workersState.enemy.units.front().position = {540, 560};
    influence.update(workersState);
    assignments = workers.assign(workersState, {}, influence);
    expect(std::ranges::any_of(assignments, [](const WorkerAssignment& assignment) {
               return assignment.job == WorkerJob::evacuate;
           }), "melee worker protection still works after the opening militia cutoff");

    GameState economy;
    economy.frame = 12 * 60 * 24;
    economy.self.id = 1;
    economy.self.race = Race::protoss;
    economy.enemy.id = 2;
    economy.enemy.race = Race::protoss;
    economy.self.supplyUsed = 64;
    economy.self.supplyTotal = 100;
    economy.self.minerals = 450;
    economy.self.units = {
        unit(1, UnitKind::nexus, true, {512, 512}),
        unit(2, UnitKind::pylon, true), unit(3, UnitKind::gateway, true),
        unit(4, UnitKind::cyberneticsCore, true), unit(5, UnitKind::roboticsFacility, true),
        unit(6, UnitKind::observatory, true), unit(7, UnitKind::roboticsSupportBay, true),
        unit(8, UnitKind::observer, true)};
    economy.self.units.front().role = UnitRole::resourceDepot;
    economy.bases = {{1, {512, 512}, {512, 560}, 8000, 5000, 1, 0, true, false, 8, 1},
                     {2, {1800, 512}, {1800, 560}, 8000, 5000, -1, 0, false, false, 8, 1}};
    for (int i = 0; i < 20; ++i) {
        auto probe = unit(100 + i, UnitKind::probe, true, {512, 560});
        probe.role = UnitRole::worker;
        economy.self.units.push_back(probe);
    }
    for (int i = 0; i < 6; ++i) {
        auto dragoon = unit(200 + i, UnitKind::dragoon, true, {700, 512});
        dragoon.role = UnitRole::groundArmy;
        economy.self.units.push_back(dragoon);
    }
    auto perimeter = unit(300, UnitKind::dragoon, false, {1200, 512});
    perimeter.role = UnitRole::groundArmy;
    economy.enemy.units = {perimeter};
    const auto growth = StrategyEngine{}.plan(economy, {});
    expect(growth.sustainEconomy && growth.breakContainment &&
               growth.desiredBases == 2 && growth.desiredWorkers <= 22 &&
               std::ranges::any_of(growth.goals, [](const ProductionGoal& goal) {
                   return goal.goal == GoalKind::expand && goal.desiredCount == 2 && goal.blocking;
           }), "a saturated army with local superiority banks a natural while contesting containment");
    economy.enemy.units.clear();
    const auto savingNatural = StrategyEngine{}.plan(economy, {});
    expect(savingNatural.desiredBases >= 2 && savingNatural.desiredWorkers <= 22,
           "planning a natural cannot overproduce workers for an unstarted Nexus");
    auto warpingNexus = unit(400, UnitKind::nexus, true, {1800, 512});
    warpingNexus.completed = false;
    warpingNexus.role = UnitRole::resourceDepot;
    economy.self.units.push_back(warpingNexus);
    const auto growingNatural = StrategyEngine{}.plan(economy, {});
    expect(growingNatural.desiredWorkers > 22 && growingNatural.desiredWorkers <= 44,
           "an actual warping Nexus releases the next base's worker growth");
    economy.self.units.pop_back();
    economy.enemy.units = {perimeter};
    ThreatAssessment breach;
    breach.combatEnemiesNearMain = 1;
    economy.enemy.units.front().position = {600, 512};
    const auto defense = StrategyEngine{}.plan(economy, breach);
    expect(defense.desiredBases == 1 && defense.desiredWorkers <= 22 &&
               std::ranges::none_of(defense.goals, [](const ProductionGoal& goal) {
                   return goal.goal == GoalKind::expand && goal.desiredCount > 1;
               }), "a real breach cancels growth and caps workers to the remaining base");
}

void testReportImprovements() {
    using namespace protodd;
    GameState state;
    state.frame = 4 * 60 * 24;
    state.self.id = 1;
    state.enemy.id = 2;
    state.self.race = Race::protoss;
    state.enemy.race = Race::terran;
    state.self.supplyUsed = 56;
    state.self.supplyTotal = 100;
    state.self.units = {unit(1, UnitKind::nexus, true, {512, 512}),
        unit(2, UnitKind::pylon, true), unit(3, UnitKind::gateway, true),
        unit(4, UnitKind::cyberneticsCore, true), unit(5, UnitKind::assimilator, true)};
    state.self.units.front().role = UnitRole::resourceDepot;
    for (int i = 0; i < 16; ++i) {
        auto probe = unit(100 + i, UnitKind::probe, true, {512, 560});
        probe.role = UnitRole::worker;
        state.self.units.push_back(probe);
    }
    state.bases = {{1, {512, 512}, {512, 560}, 8000, 5000, 1, 0, true, false, 8, 1},
        {2, {2500, 2500}, {2500, 2550}, 8000, 5000, 2, state.frame, true, false, 8, 1},
        {3, {1900, 2500}, {1900, 2550}, 8000, 5000, -1, state.frame, false, false, 8, 1}};
    StrategyEngine strategy;
    auto plan = strategy.plan(state, {});
    expect(plan.desiredBases == 1 && plan.desiredWorkers > 7 &&
        std::ranges::none_of(plan.goals, [](const ProductionGoal& goal) {
            return goal.target == UnitKind::forge || goal.target == UnitKind::photonCannon;
        }), "normal PvT preserves income and waits for its army before the Nexus");
    for (int i = 0; i < 3; ++i) state.self.units.push_back(unit(200 + i, UnitKind::dragoon, true));
    plan = strategy.plan(state, {});
    expect(plan.desiredBases == 2, "28 supply and three completed Dragoons unlock the natural");
    ThreatAssessment pressure;
    pressure.combatEnemiesNearMain = 3;
    pressure.immediateGround = 0.8;
    plan = strategy.plan(state, pressure);
    expect(plan.desiredBases == 1 && plan.prioritizeReinforcements,
           "observed pressure cancels the natural and enables the reinforcement budget");

    state.enemy.race = Race::protoss;
    state.self.supplyUsed = 40;
    plan = strategy.plan(state, {});
    expect(plan.name.find("ranged economy") != std::string::npos &&
        std::ranges::none_of(plan.goals, [](const ProductionGoal& goal) {
            return goal.target == UnitKind::roboticsFacility && goal.blocking;
        }), "quiet PvP funds its fourth Dragoon before committing to Robotics");
    auto robo = unit(6, UnitKind::roboticsFacility, true);
    robo.completed = false;
    state.self.units.push_back(robo);
    state.self.supplyUsed = 44;
    plan = strategy.plan(state, {});
    expect(std::ranges::any_of(plan.goals, [](const ProductionGoal& goal) {
        return goal.target == UnitKind::gateway && goal.desiredCount == 2 && goal.blocking;
    }) && std::ranges::any_of(plan.goals, [](const ProductionGoal& goal) {
        return goal.target == UnitKind::roboticsSupportBay && goal.blocking;
    }), "the second Gateway and splash support are scheduled during the first Robotics cycle");
    state.self.supplyUsed = 64;
    plan = strategy.plan(state, {});
    expect(std::ranges::any_of(plan.goals, [](const ProductionGoal& goal) {
        return goal.target == UnitKind::gateway && goal.desiredCount == 3 && goal.blocking;
    }), "army supply unlocks three-Gateway production without requiring an Observer");

    state.self.units.pop_back();
    state.self.units.push_back(unit(7, UnitKind::gateway, true));
    StrategicPlan budget;
    budget.prioritizeReinforcements = true;
    budget.goals = {{GoalKind::build, UnitKind::roboticsSupportBay, 1, 120, true, "optional splash"}};
    ResourceLedger bank{350, 100};
    MacroPlanner macro;
    auto actions = macro.reconcile(state, budget, bank);
    expect(std::ranges::count_if(actions, [](const MacroAction& action) {
        return action.target == UnitKind::dragoon && action.reserved && action.executable;
    }) == 2, "two usable Gateways receive a reinforcement cycle before optional tech");
    expect(bank.reservedMinerals <= bank.minerals && bank.reservedGas <= bank.gas,
           "defensive reservations never double-spend resources");
    auto cannon = unit(50, UnitKind::photonCannon, true);
    cannon.completed = false;
    state.self.units.push_back(cannon);
    budget.goals.push_back({GoalKind::build, UnitKind::photonCannon, 3, 118, true, "static screen"});
    bank = {350, 100};
    actions = macro.reconcile(state, budget, bank);
    expect(std::ranges::count_if(actions, [](const MacroAction& action) {
        return action.target == UnitKind::dragoon && action.reserved;
    }) == 2, "a warping Cannon releases the bank for mobile reinforcements");
    budget.goals.pop_back();
    state.self.units.pop_back();
    state.self.busyProducers = {UnitKind::gateway};
    state.self.units.back().powered = false;
    bank = {350, 100};
    actions = macro.reconcile(state, budget, bank);
    expect(std::ranges::none_of(actions, [](const MacroAction& action) {
        return action.reason == "protect a defensive reinforcement cycle";
    }), "busy and unpowered Gateways cannot hoard a reinforcement budget");
    state.self.busyProducers.clear();
    state.self.units.back().powered = true;
    bank = {250, 0};
    actions = macro.reconcile(state, budget, bank);
    expect(std::ranges::count_if(actions, [](const MacroAction& action) {
        return action.target == UnitKind::zealot && action.reserved;
    }) == 2, "gas-starved defense fields mineral units rather than reserving impossible Dragoons");
    budget.requireMobileDetection = true;
    budget.goals.push_back({GoalKind::train, UnitKind::observer, 1, 100, true, "urgent detection"});
    bank = {300, 250};
    actions = macro.reconcile(state, budget, bank);
    expect(std::ranges::any_of(actions, [](const MacroAction& action) {
        return action.target == UnitKind::roboticsFacility && action.reserved && action.priority >= 124;
    }), "urgent detection prerequisites survive the reinforcement budget");
    bank = {450, 200};
    actions = macro.reconcile(state, budget, bank);
    expect(std::ranges::count_if(actions, [](const MacroAction& action) {
        return action.target == UnitKind::zealot && action.reserved;
    }) == 2, "detection consuming the gas budget still leaves both Gateways able to reinforce");

    auto established = state;
    for (int i = 0; i < 6; ++i)
        established.self.units.push_back(unit(700 + i, UnitKind::dragoon, true));
    established.self.units.push_back(unit(710, UnitKind::templarArchives, true));
    StrategicPlan stormPlan;
    stormPlan.prioritizeReinforcements = true;
    stormPlan.goals = {{GoalKind::research, UnitKind::unknown, 1, 100, true,
                       "counter observed bio mass", TechnologyKind::psionicStorm}};
    bank = {325, 250};
    actions = macro.reconcile(established, stormPlan, bank);
    expect(std::ranges::any_of(actions, [](const MacroAction& action) {
        return action.technology == TechnologyKind::psionicStorm && action.reserved;
    }) && std::ranges::any_of(actions, [](const MacroAction& action) {
        return action.target == UnitKind::dragoon && action.reserved;
    }), "an established frontline funds required Storm while still reinforcing from the surplus");
    established.self.technologies.push_back({TechnologyKind::psionicStorm, 1, false});
    stormPlan.goals.push_back({GoalKind::train, UnitKind::highTemplar, 4, 98, false, "Storm support"});
    bank = {325, 250};
    actions = macro.reconcile(established, stormPlan, bank);
    expect(std::ranges::any_of(actions, [](const MacroAction& action) {
        return action.target == UnitKind::highTemplar && action.reserved;
    }) && std::ranges::any_of(actions, [](const MacroAction& action) {
        return action.target == UnitKind::dragoon && action.reserved;
    }), "reinforcement reservations leave a Gateway slot for the first usable Storm caster");

    auto enemyDepot = unit(500, UnitKind::nexus, false, {2500, 2500});
    enemyDepot.role = UnitRole::resourceDepot;
    state.enemy.units = {enemyDepot};
    ScoutManager scouts;
    const UnitId reserved[]{100};
    const auto workerScout = scouts.selectWorkerScout(state, {}, {}, reserved);
    expect(workerScout >= 0 && workerScout != 100,
           "follow-up worker scout rechecks a known enemy without stealing the builder");
    ++state.frame;
    expect(scouts.selectWorkerScout(state, {}, {}, reserved) == workerScout,
           "follow-up scout keeps its worker assignment");
    expect(scouts.selectWorkerScout(state, pressure, {}, reserved) == -1,
           "a base breach releases the follow-up worker scout");
    ++state.frame;
    expect(scouts.selectWorkerScout(state, {}, {}, reserved) == -1,
           "an interrupted scout is not replaced every callback");
    state.frame += 46 * 24;
    expect(scouts.selectWorkerScout(state, {}, {}, reserved) >= 0,
           "follow-up scouting resumes after its cooldown when the economy is safe");
    state.frame += 46 * 24;
    expect(scouts.selectWorkerScout(state, {}, {}, reserved) == -1,
           "worker scouting has a bounded mission duration");

    OpponentModel model;
    model.update(state);
    expect(!model.assessment().enemyNaturalCheckedEmpty,
           "seeing a base center is not proof that its full depot footprint is empty");
    const auto priorPressure = model.probability(EnemyPlan::heavyPressure);
    state.bases.back().lastConfirmedEmpty = state.frame;
    ++state.frame;
    model.update(state);
    expect(model.assessment().enemyNaturalCheckedEmpty &&
           model.probability(EnemyPlan::heavyPressure) > priorPressure,
           "fresh confirmed-empty natural modestly raises one-base pressure belief");
    state.frame += 46 * 24;
    model.update(state);
    expect(!model.assessment().enemyNaturalCheckedEmpty, "negative scouting evidence expires");

    Squad squad;
    squad.center = {1000, 1000};
    squad.objective = {1800, 1000};
    squad.retreat = {512, 512};
    squad.needsDetection = true;
    auto dragoon = unit(600, UnitKind::dragoon, true, squad.center);
    squad.units = {dragoon};
    auto observer = unit(601, UnitKind::observer, true, {512, 512});
    observer.sightRange = 288;
    observer.flying = true;
    state.self.units.push_back(observer);
    expect(!SquadPlanner::mobileDetectionReady(state, squad),
           "an Observer at home cannot authorize a distant army advance");
    state.self.units.back().position = {1020, 1000};
    expect(SquadPlanner::mobileDetectionReady(state, squad),
           "healthy Observer covering the army and next step releases the advance");
    state.self.units.back().disabled = true;
    expect(!SquadPlanner::mobileDetectionReady(state, squad),
           "disabled detectors do not satisfy mission coverage");
    state.self.units.back().kind = UnitKind::photonCannon;
    state.self.units.back().disabled = false;
    expect(!SquadPlanner::mobileDetectionReady(state, squad),
           "static detection cannot authorize an offensive ground mission");
    CombatEstimate estimate;
    estimate.decision = FightDecision::engage;
    estimate.advanceBlocked = true;
    InfluenceMap influence;
    state.mapWidthPixels = 4096;
    state.mapHeightPixels = 4096;
    influence.update(state);
    auto woundedEscort = unit(650, UnitKind::observer, true, squad.center);
    woundedEscort.hitPoints = 1;
    state.self.units.push_back(woundedEscort);
    state.self.units.push_back(unit(651, UnitKind::observer, true, {512, 512}));
    const auto escorts = SquadPlanner{}.detectorEscorts(state, std::span<const Squad>(&squad, 1), influence);
    expect(!escorts.empty() && escorts.front().actor == 651,
           "a healthy backup replaces a closer Observer that cannot satisfy mission readiness");
    const auto orders = TacticalController{}.control(squad.units, {}, estimate,
        squad.objective, squad.retreat, influence, squad.center, 0, false);
    expect(!orders.empty() && orders.front().type == CommandType::hold,
           "a favorable fight estimate cannot bypass missing mobile detection");
}

void testLadderSourceImprovements() {
    using namespace protodd;
    GameState state;
    auto dragoon = unit(10, UnitKind::dragoon, true, {400, 400});
    dragoon.groundWeapon = {20, 30, 0, 192, DamageType::explosive, false, true};
    auto dying = unit(20, UnitKind::hydralisk, false, {480, 400});
    dying.hitPoints = 20;
    auto healthy = unit(21, UnitKind::hydralisk, false, {480, 430});
    state.self.units = {dragoon};
    state.enemy.units = {dying, healthy};
    const std::vector<IncomingProjectile> shots{{1, 10, 20}, {1, 10, 20}};
    accountIncomingDamage(state, shots);
    CombatEvaluator evaluator;
    expect(state.enemy.units[0].incomingDamage == 20.0 &&
               state.enemy.units[0].hitPoints == 20,
           "an observed projectile reserves damage without changing observed hit points");
    const auto selected = evaluator.selectTarget(dragoon, state.enemy.units);
    expect(selected != nullptr && selected->id == healthy.id,
           "a Dragoon shoots a live target instead of wasting a shot on an incoming kill");
    accountIncomingDamage(state, {});
    expect(state.enemy.units[0].incomingDamage == 0.0,
           "vanished or missed projectiles cannot retain stale target reservations");

    state.enemy.units[0].hitPoints = 100;
    state.enemy.units[0].shields = 5;
    state.enemy.units[0].armor = 2;
    state.enemy.units[0].size = UnitSize::small;
    state.self.units[0].groundWeapon.hits = 2;
    accountIncomingDamage(state, shots);
    expect(std::abs(state.enemy.units[0].incomingDamage - 11.5) < 0.001,
           "duplicate projectile IDs count once and one projectile is one armor-adjusted hit");
    accountIncomingDamage(state, std::vector<IncomingProjectile>{{2, 10, 20}, {1, 10, 20}});
    expect(std::abs(state.enemy.units[0].incomingDamage - 20.5) < 0.001,
           "successive incoming shots deplete shields before applying size and armor");
    state.enemy.units[0].visible = false;
    accountIncomingDamage(state, shots);
    expect(state.enemy.units[0].incomingDamage == 0.0,
           "fogged targets do not inherit projectile predictions");
    state.enemy.units[0].visible = true;
    accountIncomingDamage(state, std::vector<IncomingProjectile>{{3, 999, 20}});
    expect(state.enemy.units[0].incomingDamage == 0.0,
           "unknown projectile sources cannot fabricate upgraded weapon damage");

    auto carrier = unit(22, UnitKind::carrier, false, {1100, 400});
    carrier.flying = true;
    dragoon.airWeapon = {20, 30, 0, 192, DamageType::explosive, true, false};
    const std::vector<UnitSnapshot> targetChoice{carrier, healthy};
    const auto localShot = evaluator.selectTarget(dragoon, targetChoice);
    expect(localShot != nullptr && localShot->id == healthy.id,
           "ranged units take available shots instead of chasing distant expensive units");

    auto corsair = unit(30, UnitKind::corsair, true, {400, 400});
    corsair.flying = true;
    corsair.airWeapon = {5, 8, 0, 160, DamageType::explosive, true, false};
    auto zealot = unit(31, UnitKind::zealot, false, {450, 400});
    zealot.groundWeapon = {8, 22, 0, 32, DamageType::normal, false, true, 2};
    const auto harmlessGround = evaluator.evaluate(std::vector<UnitSnapshot>{corsair},
        std::vector<UnitSnapshot>(40, zealot), 1.2, 0.0, false);
    expect(harmlessGround.enemyPower == 0.0 && harmlessGround.decision == FightDecision::engage,
           "ground-only armies do not threaten an independent air squad");
    auto cannon = unit(32, UnitKind::photonCannon, true, {400, 400});
    cannon.topSpeed = 0;
    cannon.groundWeapon = {20, 22, 0, 224, DamageType::normal, false, true};
    auto tank = unit(33, UnitKind::siegeTank, false, {730, 400});
    tank.topSpeed = 0;
    tank.groundWeapon = {70, 75, 64, 384, DamageType::explosive, false, true};
    const auto siege = evaluator.evaluate(std::vector<UnitSnapshot>{cannon},
        std::vector<UnitSnapshot>{tank}, 1.2, 0.0, false);
    expect(siege.friendlyPower == 0.0 && siege.enemyPower > 0.0,
           "a Cannon cannot provide imaginary support against an outranging siege line");

    auto reaver = unit(40, UnitKind::reaver, true, {400, 400});
    reaver.ammo = 1;
    reaver.groundWeapon = {100, 60, 0, 256, DamageType::normal, false, true};
    auto target = unit(41, UnitKind::ultralisk, false, {450, 400});
    target.hitPoints = target.maxHitPoints = 250;
    const auto oneScarab = evaluator.evaluate(std::vector<UnitSnapshot>{reaver},
        std::vector<UnitSnapshot>{target}, 1.2, 0.0);
    reaver.ammo = 3;
    const auto threeScarabs = evaluator.evaluate(std::vector<UnitSnapshot>{reaver},
        std::vector<UnitSnapshot>{target}, 1.2, 0.0);
    expect(oneScarab.simulatedEnemyRemaining > 0.0 && threeScarabs.simulatedEnemyRemaining == 0.0,
           "combat simulation spends each Scarab once instead of granting unlimited ammunition");

    StrategicPlan plan;
    plan.posture = Posture::hold;
    plan.attackTarget = {3000, 3000};
    auto dt = unit(50, UnitKind::darkTemplar, true, {400, 400});
    auto distantCorsair = corsair;
    distantCorsair.id = 51;
    distantCorsair.position = {2000, 2000};
    const std::vector<UnitSnapshot> harassers{dt, corsair, distantCorsair};
    const auto harassment = SquadPlanner{}.form(state, harassers, {}, plan, {100, 100});
    expect(harassment.size() == 3U && std::ranges::all_of(harassment, [](const Squad& squad) {
               return squad.role == SquadRole::harassment && squad.units.size() == 1U;
           }),
           "air and ground harassment split both by movement domain and local connectivity");

    const auto original = SquadPlanner{}.form(state, std::vector<UnitSnapshot>{dragoon}, {}, plan, {100, 100});
    auto reinforcement = dragoon;
    reinforcement.id = 60;
    plan.attackTarget.x += 8;
    const auto reinforced = SquadPlanner{}.form(state,
        std::vector<UnitSnapshot>{reinforcement, dragoon}, {}, plan, {100, 100});
    expect(original.size() == 1U && reinforced.size() == 1U &&
               original[0].signature != reinforced[0].signature &&
               original[0].engagementKey == reinforced[0].engagementKey,
           "reinforcements and target motion refresh routes without erasing engagement memory");

    GameState macro;
    macro.self.minerals = 200;
    macro.self.supplyTotal = 100;
    auto gateway = unit(70, UnitKind::gateway, true);
    gateway.disabled = true;
    macro.self.units = {gateway, unit(71, UnitKind::nexus, true)};
    StrategicPlan production;
    production.goals = {{GoalKind::train, UnitKind::zealot, 2, 100, true, "disabled producer"},
                        {GoalKind::train, UnitKind::probe, 1, 90, false, "usable producer"}};
    ResourceLedger ledger{200, 0, 0, 0};
    const auto actions = MacroPlanner{}.reconcile(macro, production, ledger);
    expect(std::ranges::none_of(actions, [](const MacroAction& action) {
               return action.target == UnitKind::zealot && action.reserved;
           }) && std::ranges::any_of(actions, [](const MacroAction& action) {
               return action.target == UnitKind::probe && action.reserved;
           }),
           "a disabled producer cannot reserve resources needed by a usable Nexus");
}

void testContainmentRecovery() {
    using namespace protodd;
    GameState state;
    state.frame = 9 * 60 * 24;
    state.self.id = 1;
    state.self.race = Race::protoss;
    state.enemy.id = 2;
    state.enemy.race = Race::protoss;
    state.self.supplyTotal = 120;
    state.self.supplyUsed = 70;
    state.self.units = {unit(1, UnitKind::nexus, true, {512, 512}),
        unit(2, UnitKind::pylon, true), unit(3, UnitKind::gateway, true),
        unit(4, UnitKind::cyberneticsCore, true), unit(5, UnitKind::observer, true)};
    state.bases = {{1, {512, 512}, {512, 600}, 8000, 5000, 1, 0, true, false, 8, 1}};
    state.bases.push_back({2, {1536, 512}, {1536, 600}, 8000, 5000, -1, 0, false, false, 8, 1});
    for (int i = 0; i < 20; ++i) {
        auto probe = unit(100 + i, UnitKind::probe, true, {512, 600});
        probe.role = UnitRole::worker;
        state.self.units.push_back(probe);
    }
    std::vector<UnitSnapshot> army;
    for (int i = 0; i < 8; ++i) {
        auto dragoon = unit(200 + i, UnitKind::dragoon, true, {700 + i * 12, 512});
        dragoon.role = UnitRole::groundArmy;
        dragoon.groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
        army.push_back(dragoon);
        state.self.units.push_back(dragoon);
    }
    state.enemy.units = {unit(300, UnitKind::dragoon, false, {1200, 512}),
                        unit(301, UnitKind::nexus, false, {3000, 512}),
                        unit(302, UnitKind::nexus, false, {3000, 1600}),
                        unit(303, UnitKind::photonCannon, false, {2900, 512}),
                        unit(304, UnitKind::photonCannon, false, {3100, 512})};
    state.enemy.units[1].role = state.enemy.units[2].role = UnitRole::resourceDepot;
    for (auto& enemy : state.enemy.units) {
        if (isCombatUnit(enemy.kind) || isStaticDefense(enemy.kind))
            enemy.groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
    }
    for (auto& fighter : army)
        fighter.groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
    ThreatAssessment threat;
    threat.combatEnemiesNearMain = 1;
    threat.approachingArmyValue = 12;
    threat.immediateGround = 0.65;
    auto plan = StrategyEngine{}.plan(state, threat);
    expect(plan.sustainEconomy && plan.breakContainment && plan.desiredBases == 2 &&
               !plan.prioritizeReinforcements,
           "a small perimeter force cannot veto a superior army's economic transition");
    expect(plan.expansionTarget == Position{1536, 512} && plan.rallyPoint == plan.expansionTarget,
           "the army and expansion builder share a concrete economic objective");
    expect(plan.attackTarget == Position{3000, 1600},
           "the field army targets an exposed expansion before a defended main");
    const auto squads = SquadPlanner{}.form(state, army, state.enemy.units, plan, {512, 512});
    expect(squads.size() == 1 && squads.front().role == SquadRole::mainArmy &&
               squads.front().units.size() == army.size(),
           "breaking containment keeps the assembled army together and releases its base leash");
    auto timing = plan;
    timing.breakContainment = false;
    timing.minimumAttackSize = 8;
    const auto timingSquads = SquadPlanner{}.form(state, army, {}, timing, {512, 512});
    expect(timingSquads.size() == 1 && timingSquads.front().units.size() == 8,
           "a home guard cannot remove the units required to launch the declared timing");
    auto fog = state.enemy.units.front();
    fog.visible = false;
    fog.lastSeen = state.frame - 24;
    auto fogSquads = SquadPlanner{}.form(state, army, std::vector{fog}, plan, {512, 512});
    expect(fogSquads.size() == 1 && fogSquads.front().enemies.size() == 1 &&
               !fogSquads.front().enemies.front().visible &&
               CombatEvaluator{}.selectTarget(army.front(), fogSquads.front().enemies) == nullptr,
           "retreating out of vision retains enemy risk without targeting hidden units");
    fog.lastSeen = state.frame - 9 * 24;
    fogSquads = SquadPlanner{}.form(state, army, std::vector{fog}, plan, {512, 512});
    expect(fogSquads.front().enemies.empty(),
           "short combat memory expires instead of creating a permanent ghost contain");
    StrategicDirector director;
    static_cast<void>(director.stabilize({}, state, {}));
    expect(director.stabilize(plan, state, threat).posture == Posture::pressure,
           "a covered approach does not reset the strategic emergency timer");
    state.enemy.units.front().position = {600, 512};
    plan = StrategyEngine{}.plan(state, threat);
    expect(!plan.sustainEconomy && !plan.breakContainment && plan.prioritizeReinforcements,
           "an actual mineral-line breach still interrupts the economic transition");

    GameState macro;
    macro.self.supplyTotal = 120;
    macro.self.units = {unit(1, UnitKind::nexus, true), unit(2, UnitKind::gateway, true),
                       unit(3, UnitKind::cyberneticsCore, true),
                       unit(4, UnitKind::roboticsFacility, true)};
    macro.self.units.back().completed = false;
    macro.self.units.back().buildProgress = 20;
    StrategicPlan production;
    production.goals = {{GoalKind::train, UnitKind::observer, 1, 124, true, "future detection"},
                       {GoalKind::train, UnitKind::dragoon, 1, 112, true, "current defense"}};
    ResourceLedger ledger{150, 100};
    auto actions = MacroPlanner{}.reconcile(macro, production, ledger);
    expect(std::ranges::any_of(actions, [](const MacroAction& action) {
               return action.target == UnitKind::dragoon && action.reserved;
           }) && std::ranges::none_of(actions, [](const MacroAction& action) {
               return action.target == UnitKind::observatory && action.reserved;
           }), "a distant detection deadline cannot freeze a usable Gateway");
    macro.self.units.back().buildProgress = 95;
    ledger = {150, 100};
    actions = MacroPlanner{}.reconcile(macro, production, ledger);
    expect(std::ranges::any_of(actions, [](const MacroAction& action) {
               return action.target == UnitKind::observatory && action.reserved;
           }), "the detection reservation resumes before Robotics finishes");

    macro.enemy.race = Race::protoss;
    macro.self.units.pop_back();
    macro.self.units.push_back(unit(10, UnitKind::photonCannon, true));
    for (int i = 0; i < 4; ++i)
        macro.self.units.push_back(unit(20 + i, UnitKind::dragoon, true));
    production.prioritizeReinforcements = true;
    production.goals = {{GoalKind::build, UnitKind::roboticsFacility, 1, 113, true,
                        "protected splash checkpoint"}};
    ledger = {200, 200};
    actions = MacroPlanner{}.reconcile(macro, production, ledger);
    expect(std::ranges::any_of(actions, [](const MacroAction& action) {
               return action.target == UnitKind::roboticsFacility && action.reserved;
           }), "an established screen funds splash instead of reserving endless Gateway cycles");

    GameState transportState;
    transportState.mapWidthPixels = transportState.mapHeightPixels = 2048;
    transportState.self.units = {unit(1, UnitKind::shuttle, true, {512, 512}),
                                unit(2, UnitKind::reaver, true, {540, 512})};
    transportState.self.units.front().flying = true;
    InfluenceMap influence;
    influence.update(transportState);
    TransportController transports;
    auto orders = transports.control(transportState, {1800, 1800}, {512, 512}, influence, 2);
    expect(orders.empty(), "a Shuttle cannot steal the first defensive Reaver for a raid");
    transportState.self.units.back().loaded = true;
    transportState.self.units.back().transportId = 1;
    orders = transports.control(transportState, {1800, 1800}, {512, 512}, influence, 2);
    expect(std::ranges::any_of(orders, [](const Command& order) {
               return order.type == CommandType::unload && order.source == "reaver-return";
           }), "a reserved Reaver already aboard is returned to the defense");
    transportState.self.units.back().loaded = false;
    orders = transports.control(transportState, {1800, 1800}, {512, 512}, influence, 2);
    expect(orders.empty(), "a returned army Reaver is released rather than immediately reloaded");

    Squad travelling;
    travelling.role = SquadRole::mainArmy;
    travelling.center = {1100, 512};
    travelling.units = army;
    expect(SquadPlanner::supportRendezvous(transportState, travelling, {1800, 512}) ==
               Position{796, 512},
           "an uncontested army waits for nearby trailing Reaver support instead of outrunning it");
    travelling.enemies.push_back(unit(10, UnitKind::dragoon, false, {1200, 512}));
    expect(SquadPlanner::supportRendezvous(transportState, travelling, {1800, 512}) ==
               Position{1800, 512},
           "support assembly does not override an active combat decision");
}

void testOpeningRangedCommitment() {
    using namespace protodd;
    GameState state;
    state.frame = 4000;
    state.self.id = 1;
    state.enemy.id = 2;
    state.self.race = state.enemy.race = Race::protoss;
    state.self.supplyUsed = 40;
    state.self.supplyTotal = 66;
    state.self.units = {unit(1, UnitKind::nexus, true, {512, 512}),
        unit(2, UnitKind::pylon, true), unit(3, UnitKind::gateway, true),
        unit(4, UnitKind::cyberneticsCore, true), unit(5, UnitKind::assimilator, true),
        unit(6, UnitKind::zealot, true)};
    for (int i = 0; i < 18; ++i) {
        auto worker = unit(100 + i, UnitKind::probe, true, {512, 560});
        worker.role = UnitRole::worker;
        state.self.units.push_back(worker);
    }
    state.bases = {{1, {512, 512}, {512, 600}, 8000, 5000, 1, 0, true, false, 8, 1}};
    state.self.busyProducers = {UnitKind::nexus};
    state.self.units[3].completed = false;
    state.self.units[3].buildProgress = 50;
    MacroPlanner macro;
    auto plan = StrategyEngine{}.plan(state, {});
    ResourceLedger bank{125, 200};
    auto actions = macro.reconcile(state, plan, bank);
    expect(std::ranges::none_of(actions, [](const MacroAction& action) {
        return action.action == MacroActionKind::train && action.target == UnitKind::zealot;
    }), "quiet Core construction cannot fill the first Dragoon's Gateway slot with extra Zealots");
    state.frame += 600;
    state.self.units[3].completed = true;
    plan = StrategyEngine{}.plan(state, {});
    bank = {200, 200};
    actions = macro.reconcile(state, plan, bank);
    expect(std::ranges::any_of(actions, [](const MacroAction& action) {
        return action.target == UnitKind::dragoon && action.reserved && action.executable;
    }) && std::ranges::none_of(actions, [](const MacroAction& action) {
        return action.target == UnitKind::roboticsFacility && action.reserved;
    }), "the first ranged defender receives its bank before the optional Robotics investment");
    ThreatAssessment rush;
    rush.immediateGround = 0.9;
    rush.combatEnemiesNearMain = 3;
    plan = StrategyEngine{}.plan(state, rush);
    expect(plan.prioritizeReinforcements && std::ranges::any_of(plan.goals,
        [](const ProductionGoal& demand) {
            return demand.goal == GoalKind::train && demand.target == UnitKind::zealot;
        }), "observed rushes retain the separate emergency melee response");
}

void testDefensiveTerrain() {
    using namespace protodd;
    constexpr int width = 64, height = 40;
    std::vector<std::uint8_t> cells(width * height, 1);
    NavigationGrid open(width, height, 32, cells);
    const Position home{256, 640}, outside{1664, 640};
    expect(!open.defensivePosition(home, outside).valid(), "open terrain is not mislabeled as a defensive choke");
    for (int y = 0; y < height; ++y)
        for (int x = 30; x <= 32; ++x)
            if (y < 18 || y > 21) cells[static_cast<std::size_t>(y * width + x)] = 0;
    NavigationGrid choke(width, height, 32, cells);
    const auto position = choke.defensivePosition(home, outside);
    expect(position.valid() && position.width <= 160 && position.anchor.x < position.entrance.x,
           "a flat narrow passage has a defensible position on the home side");
    std::vector<std::uint8_t> heights(width * height, 0);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < 32; ++x) heights[static_cast<std::size_t>(y * width + x)] = 2;
    NavigationGrid ramp(width, height, 32, cells, heights);
    const auto upper = ramp.defensivePosition(home, outside);
    expect(upper.valid() && upper.highGround && upper.anchor.x < 32 * 32,
           "an elevation advantage keeps the defensive anchor above the approach");
    for (int y = 18; y <= 21; ++y) cells[static_cast<std::size_t>(y * width + 31)] = 0;
    expect(!NavigationGrid(width, height, 32, cells).defensivePosition(home, outside).valid(),
           "an impassable wall is not selected as a usable army entrance");

    GameState state;
    state.frame = 7200;
    state.self.id = 1;
    state.enemy.id = 2;
    state.self.race = state.enemy.race = Race::protoss;
    state.self.supplyUsed = 40;
    state.self.supplyTotal = 66;
    state.bases = {{1, home, {224, 640}, 8000, 5000, 1, 0, true, false, 8, 1}};
    state.bases.front().defense = position;
    state.self.units = {unit(1, UnitKind::nexus, true, home),
        unit(2, UnitKind::gateway, true, home), unit(3, UnitKind::cyberneticsCore, true, home)};
    state.self.units.front().role = UnitRole::resourceDepot;
    std::vector<UnitSnapshot> army;
    for (int i = 0; i < 18; ++i) {
        auto worker = unit(100 + i, UnitKind::probe, true, home);
        worker.role = UnitRole::worker;
        state.self.units.push_back(worker);
    }
    for (int i = 0; i < 4; ++i) {
        auto goon = unit(20 + i, UnitKind::dragoon, true, position.anchor);
        goon.groundWeapon = {.damage=20, .cooldown=30, .maxRange=192, .targetsGround=true};
        army.push_back(goon);
        state.self.units.push_back(goon);
    }
    auto enemy = unit(50, UnitKind::dragoon, false, {position.entrance.x + 128, position.entrance.y});
    enemy.groundWeapon = army.front().groundWeapon;
    const auto plan = StrategyEngine{}.plan(state, {});
    expect(plan.rallyPoint == position.anchor, "a holding army assembles at defensible terrain before contact");
    state.enemy.units = {enemy};
    const auto squads = SquadPlanner{}.form(state, army, state.enemy.units, plan, home);
    const auto defense = std::ranges::find(squads, SquadRole::baseDefense, &Squad::role);
    expect(defense != squads.end() && defense->defense.front == position.entrance &&
               defense->retreat == position.anchor && !SquadPlanner::mustHoldDefensiveScreen(*defense),
           "threats at a distant choke activate defense without forcing a losing downhill charge");
    if (defense != squads.end()) {
        expect(!defense->defense.contains(enemy.position) && defense->defense.contains(position.anchor),
               "the pursuit boundary excludes the enemy side while retaining the home-side formation");
        auto melee = unit(60, UnitKind::zealot, true, position.anchor);
        melee.groundWeapon = {.damage=8, .cooldown=22, .maxRange=32, .targetsGround=true, .hits=2};
        CombatEstimate engage;
        engage.decision = FightDecision::engage;
        InfluenceMap influence;
        auto orders = TacticalController{}.control(std::vector{melee}, state.enemy.units, engage,
            enemy.position, position.anchor, influence, position.anchor, 3, false, defense->defense);
        expect(orders.size() == 1 && orders.front().type == CommandType::hold,
               "melee defenders do not chase ranged bait through the choke");
        auto rangedTarget = enemy;
        rangedTarget.position = {position.entrance.x + 48, position.entrance.y};
        auto dragoon = army.front();
        CombatEstimate retreat;
        retreat.decision = FightDecision::retreat;
        retreat.ratio = 0.5;
        orders = TacticalController{}.control(std::vector{dragoon}, std::vector{rangedTarget}, retreat,
            rangedTarget.position, position.anchor, influence, position.anchor, 3, false, defense->defense);
        expect(orders.size() == 1 && orders.front().type == CommandType::attackUnit &&
                   orders.front().targetUnit == rangedTarget.id,
               "ranged defenders fire across the choke boundary even while holding an unfavorable fight");
        dragoon.weaponCooldown = 20;
        orders = TacticalController{}.control(std::vector{dragoon}, std::vector{rangedTarget}, retreat,
            rangedTarget.position, position.anchor, influence, position.anchor, 3, false, defense->defense);
        expect(orders.size() == 1 && orders.front().type == CommandType::move &&
                   defense->defense.contains(orders.front().targetPosition),
               "a cooling-down ranged defender repositions on the protected side of the choke");
        auto breached = *defense;
        breached.enemies.front().position = home;
        expect(SquadPlanner::mustHoldDefensiveScreen(breached),
               "an enemy that bypasses the choke and reaches the economy forces interception");
    }
    state.self.units.push_back(unit(61, UnitKind::zealot, true, position.anchor));
    state.self.units.push_back(unit(62, UnitKind::zealot, true, position.anchor));
    for (int i = 1; i < 3; ++i) { enemy.id = 50 + i; state.enemy.units.push_back(enemy); }
    ThreatAssessment pressure;
    pressure.immediateGround = 0.8;
    auto response = StrategyEngine{}.plan(state, pressure);
    ResourceLedger bank{300, 0};
    const auto actions = MacroPlanner{}.reconcile(state, response, bank);
    expect(std::ranges::none_of(actions, [](const MacroAction& action) {
        return action.target == UnitKind::zealot && action.reserved;
    }), "a ranged mirror with its melee screen already filled saves for Dragoons instead of adding more Zealots");
}

void testEconomicHarassment() {
    using namespace protodd;
    GameState state;
    state.frame = 10000;
    state.self.id = 1;
    state.enemy.id = 2;
    state.mapWidthPixels = state.mapHeightPixels = 4096;
    const Position home{300, 300};
    std::vector<UnitSnapshot> army;
    for (int i = 0; i < 12; ++i) {
        auto fighter = unit(100 + i, UnitKind::dragoon, true, {500 + i * 8, 500});
        fighter.groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
        army.push_back(fighter);
    }
    state.enemy.units = {unit(200, UnitKind::probe, false, {2400, 800}),
                        unit(201, UnitKind::probe, false, {2450, 820})};
    expect(harassmentOpportunity(state, army.front()).economicTargets == 2,
           "an observed exposed worker cluster creates a harassment opportunity");
    auto defender = unit(210, UnitKind::dragoon, false, {1400, 640});
    defender.groundWeapon = army.front().groundWeapon;
    state.enemy.units.push_back(defender);
    expect(!harassmentOpportunity(state, army.front()).target.valid(),
           "a ground contain along the route vetoes a suicidal worker raid");
    expect(harassmentOpportunity(state, army.front(), true).target.valid(),
           "an air transport can bypass a ground-only route obstruction");
    state.enemy.units.back().airWeapon = defender.groundWeapon;
    expect(!harassmentOpportunity(state, army.front(), true).target.valid(),
           "air transport routes respect observed anti-air coverage");
    state.enemy.units.pop_back();
    StrategicPlan plan;
    plan.minimumAttackSize = 8;
    HarassmentPlanner raids;
    expect(raids.update(state, std::span<const UnitSnapshot>{army.data(), 8}, plan, home, false).members.empty(),
           "harassment cannot cannibalize an undersized main army");
    auto mission = raids.update(state, army, plan, home, false);
    expect(mission.members.size() == 2 && !mission.withdrawing,
           "two spare fighters launch an economic raid while ten remain");
    const auto members = mission.members;
    state.frame += 24;
    army.push_back(unit(99, UnitKind::zealot, true, {500, 500}));
    mission = raids.update(state, army, plan, home, false);
    expect(mission.members == members, "new reinforcements cannot churn active raid membership");
    state.enemy.units.push_back(defender);
    state.frame += 24;
    mission = raids.update(state, army, plan, home, false);
    expect(mission.withdrawing, "defenders on the route trigger a latched raid withdrawal");
    state.enemy.units.pop_back();
    state.frame += 24;
    expect(raids.update(state, army, plan, home, false).withdrawing,
           "defenders disappearing does not restart an extracting raid");
    for (auto& fighter : army)
        if (std::ranges::find(members, fighter.id) != members.end()) fighter.position = home;
    state.frame += 24;
    expect(raids.update(state, army, plan, home, false).members.empty(),
           "returning raiders are released at home");
    state.frame += 24;
    expect(raids.update(state, army, plan, home, false).members.empty(),
           "raid cooldown prevents immediate reuse of the same tired detachment");
    raids.reset();
    expect(raids.update(state, army, plan, home, true).members.empty(),
           "an actual base threat vetoes spare-unit harassment");

    auto corsair = unit(300, UnitKind::corsair, true, {600, 600});
    corsair.flying = true;
    corsair.airWeapon = {5, 8, 0, 160, DamageType::normal, true, false};
    auto overlord = unit(301, UnitKind::overlord, false, {2000, 600});
    overlord.flying = true;
    state.enemy.units.push_back(overlord);
    expect(harassmentOpportunity(state, corsair).target == overlord.position,
           "Corsairs hunt exposed air logistics instead of unattackable workers");
    state.enemy.units.back().visible = false;
    state.enemy.units.back().lastSeen = state.frame - 30 * 24;
    expect(!harassmentOpportunity(state, corsair).target.valid(),
           "stale hidden economic targets do not support a new raid");

    auto shuttle = unit(400, UnitKind::shuttle, true, {600, 600});
    shuttle.flying = true;
    auto reaver = unit(401, UnitKind::reaver, true, {630, 600});
    reaver.groundWeapon = {100, 60, 0, 256, DamageType::normal, false, true};
    reaver.ammo = 2;
    state.self.units = {shuttle, reaver};
    InfluenceMap influence;
    influence.update(state);
    TransportController transports;
    auto orders = transports.control(state, {3500, 3500}, home, influence, 0, true);
    expect(std::ranges::any_of(orders, [](const Command& order) { return order.type == CommandType::load; }),
           "spare Reaver loads for an exposed worker line");
    state.self.units[1].loaded = true;
    state.self.units[1].transportId = 400;
    state.self.units[0].position = {2250, 800};
    state.frame += 24;
    orders = transports.control(state, {3500, 3500}, home, influence, 0, true);
    expect(std::ranges::any_of(orders, [](const Command& order) { return order.type == CommandType::unload; }),
           "Reaver drops at the selected economy target rather than the main army objective");
}

void testScoutHarassmentAndContainment() {
    using namespace protodd;
    GameState state;
    state.frame = 1000;
    state.latencyFrames = 2;
    state.mapWidthPixels = state.mapHeightPixels = 2048;
    state.self.id = 1;
    state.enemy.id = 2;
    state.self.race = state.enemy.race = Race::protoss;
    state.self.units = {unit(1, UnitKind::nexus, true, {256, 256}),
        unit(2, UnitKind::probe, true, {1200, 1200}), unit(3, UnitKind::pylon, true, {300, 300})};
    state.enemy.units = {unit(10, UnitKind::nexus, false, {1450, 1300}),
                        unit(11, UnitKind::probe, false, {1240, 1200})};
    state.self.units.front().role = state.enemy.units.front().role = UnitRole::resourceDepot;
    state.self.units[1].groundWeapon = {5, 22, 0, 32, DamageType::normal, false, true};
    InfluenceMap influence;
    influence.update(state);
    ProbeHarasser harass;
    auto order = harass.control(state, 2, {1450, 1300}, influence);
    expect(order && order->type == CommandType::attackUnit && order->targetUnit == 11,
           "opening Probe tags a visible worker while scouting its base");
    state.self.units[1].attackFrame = true;
    state.self.units[1].weaponCooldown = 20;
    expect(!harass.control(state, 2, {1450, 1300}, influence), "Probe finishes a safe attack frame");
    state.enemy.units[1].orderTargetId = 2;
    state.frame += 2;
    order = harass.control(state, 2, {1450, 1300}, influence);
    expect(order && order->source == "probe-harass-evade-chaser" &&
        distance(order->targetPosition, state.enemy.units[1].position) >
            distance(state.self.units[1].position, state.enemy.units[1].position),
           "a chasing worker immediately makes the Probe open distance");
    state.self.units[1].attackFrame = false;
    state.self.units[1].weaponCooldown = 0;
    state.enemy.units[1].orderTargetId = -1;
    state.frame += 12;
    order = harass.control(state, 2, {1450, 1300}, influence);
    expect(order && order->type == CommandType::move, "Probe does not reverse during its escape interval");
    state.frame += 24;
    order = harass.control(state, 2, {1450, 1300}, influence);
    expect(order && order->type == CommandType::attackUnit, "Probe re-engages after the chase stops");
    state.enemy.units.push_back(unit(12, UnitKind::zealot, false, {1500, 1300}));
    order = harass.control(state, 2, {1450, 1300}, influence);
    expect(order && order->source == "probe-harass-withdraw", "first completed fighter ends opening harassment");
    state.enemy.units.pop_back();
    state.frame += 24;
    order = harass.control(state, 2, {1450, 1300}, influence);
    expect(order && order->source == "probe-harass-withdraw", "fog cannot restart harassment after a fighter was observed");
    state.self.units[1].position = {300, 300};
    expect(!harass.control(state, 2, {1450, 1300}, influence) && harass.finished(),
           "surviving scout is released to the economy at home");
    harass.reset();
    state.self.units[1].position = {1200, 1200};
    state.self.units[1].hitPoints = 10;
    order = harass.control(state, 2, {1450, 1300}, influence);
    expect(order && order->source == "probe-harass-withdraw", "damaged Probe preserves its life before another tag");

    ScoutManager scouts;
    state.enemy.units.clear();
    state.self.units[1].hitPoints = state.self.units[1].maxHitPoints;
    const std::vector<UnitId> builder{2};
    expect(scouts.selectWorkerScout(state, {}, {}, builder) < 0, "scout harassment cannot steal a builder");
    expect(scouts.selectWorkerScout(state, {}) == 2, "one available Probe owns the opening mission");
    state.enemy.units = {unit(10, UnitKind::nexus, false, {1450, 1300})};
    state.self.units[1].underAttack = true;
    expect(scouts.selectWorkerScout(state, {}) == 2 && scouts.openingScout() == 2,
           "finding the enemy and being chased do not release the Probe to conflicting mining orders");

    state.frame = 7 * 60 * 24;
    state.self.supplyUsed = 60;
    state.self.supplyTotal = 100;
    state.self.units[1].underAttack = false;
    state.self.units.push_back(unit(4, UnitKind::cyberneticsCore, true, {280, 400}));
    state.self.units.push_back(unit(5, UnitKind::gateway, true, {320, 400}));
    state.self.units.push_back(unit(6, UnitKind::assimilator, true, {200, 320}));
    state.self.units.push_back(unit(7, UnitKind::roboticsFacility, true, {400, 400}));
    for (int i = 0; i < 4; ++i) {
        state.self.units.push_back(unit(30 + i, UnitKind::dragoon, true, {400, 400}));
        state.enemy.units.push_back(unit(50 + i, UnitKind::dragoon, false, {1000, 500}));
        state.enemy.units.back().groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
    }
    state.bases = {{1, {256, 256}, {256, 320}, 8000, 5000, 1, 0, true, false, 8, 1}};
    ThreatAssessment containment;
    containment.mostLikely = EnemyPlan::heavyPressure;
    containment.approachingArmyValue = 8;
    containment.immediateGround = 0.7;
    const auto wants = [](const StrategicPlan& plan, const UnitKind kind) {
        return std::ranges::any_of(plan.goals, [kind](const ProductionGoal& demand) {
            return demand.target == kind && demand.desiredCount > 0;
        });
    };
    auto plan = StrategyEngine{}.plan(state, containment);
    expect(!wants(plan, UnitKind::photonCannon) && !wants(plan, UnitKind::forge),
           "ranged containment funds a mobile breakout instead of home static defense");
    expect(!wants(plan, UnitKind::observer) && !wants(plan, UnitKind::observatory),
           "optional Observer cannot consume the four-Dragoon army's next investment");
    MacroPlanner macro;
    StrategicPlan old;
    old.goals = {{GoalKind::build, UnitKind::photonCannon, 2, 120, true, "old static response"}};
    ResourceLedger bank{0, 0};
    static_cast<void>(macro.reconcile(state, old, bank));
    state.frame += 24;
    bank = {500, 500};
    const auto actions = macro.reconcile(state, plan, bank);
    for (const auto& action : actions) {
        if (action.target == UnitKind::forge || action.target == UnitKind::photonCannon ||
            action.target == UnitKind::observer || action.target == UnitKind::observatory)
            std::cerr << "Containment action: " << unitStats(action.target).name << " / " << action.reason << '\n';
    }
    expect(std::ranges::none_of(actions, [](const MacroAction& action) {
        return action.target == UnitKind::forge || action.target == UnitKind::photonCannon ||
               action.target == UnitKind::observer || action.target == UnitKind::observatory;
    }), "cancelled containment investments cannot return through macro demand memory");
    containment.cloak = 0.8;
    state.enemy.units.push_back(unit(90, UnitKind::darkTemplar, false, {1000, 600}));
    plan = StrategyEngine{}.plan(state, containment);
    expect(plan.requireMobileDetection && wants(plan, UnitKind::observer),
           "observed cloak still overrides the optional detection delay");
    expect(!wants(plan, UnitKind::photonCannon),
           "mobile detection does not reopen home Cannon spending against a ranged perimeter contain");
    state.enemy.units.clear();
    state.self.units.push_back(unit(80, UnitKind::dragoon, true));
    state.self.units.push_back(unit(81, UnitKind::dragoon, true));
    plan = StrategyEngine{}.plan(state, {});
    expect(plan.desiredBases >= 2 && wants(plan, UnitKind::observer),
           "six Dragoons unlock optional detection and expansion without waiting for an Observer");

    EngagementTracker engagements;
    std::vector<UnitSnapshot> members = {unit(101, UnitKind::dragoon, true),
        unit(102, UnitKind::dragoon, true), unit(103, UnitKind::dragoon, true)};
    const auto key = engagements.identify(members, 100);
    static_cast<void>(engagements.stabilize(key, FightDecision::engage, 1.5, 1.2, 100));
    expect(engagements.stabilize(key, FightDecision::retreat, 0.5, 1.2, 102) == FightDecision::retreat,
           "overwhelming danger always permits an immediate exit");
    members.erase(members.begin());
    expect(engagements.identify(members, 104) == key, "losing the lead unit preserves surviving squad fight history");
    expect(engagements.stabilize(key, FightDecision::engage, 10.0, 1.2, 104, false) == FightDecision::retreat,
           "losing local contact does not turn retreat into immediate forward travel");
    for (Frame frame = 106; frame <= 150; frame += 2)
        expect(engagements.stabilize(key, FightDecision::engage, 1.8, 1.2, frame) == FightDecision::retreat,
               "frequent callbacks cannot shorten the regroup interval");
    expect(engagements.stabilize(key, FightDecision::engage, 1.8, 1.2, 174) == FightDecision::engage,
           "a sustained advantage reopens combat after the regroup interval");
    members = {unit(201, UnitKind::dragoon, true), unit(202, UnitKind::dragoon, true)};
    expect(engagements.identify(members, 176) != key, "unrelated squads do not inherit each other's retreat");

    std::vector<UnitSnapshot> defenders{unit(400, UnitKind::dragoon, true, {500, 500})};
    defenders.front().groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
    std::vector<UnitSnapshot> attackers{unit(500, UnitKind::dragoon, false, {640, 500})};
    CombatEstimate losing;
    losing.decision = FightDecision::retreat;
    losing.ratio = 0.3;
    losing.holdScreen = true;
    InfluenceMap quiet;
    auto orders = TacticalController{}.control(defenders, attackers, losing, {1500, 500}, {500, 500}, quiet);
    expect(!orders.empty() && orders.front().source == "screen-intercept" &&
           orders.front().type == CommandType::attackUnit,
           "a losing defense keeps a ready volley against enemies already in range");
    attackers.front().position = {1000, 500};
    orders = TacticalController{}.control(defenders, attackers, losing, {1500, 500}, {500, 500}, quiet);
    expect(!orders.empty() && orders.front().type == CommandType::move && orders.front().targetPosition == Position{500, 500},
           "protecting an economy cannot force an outward chase into overwhelming odds");
    defenders.front().kind = UnitKind::zealot;
    defenders.front().groundWeapon.maxRange = 32;
    attackers.front().position = {545, 500};
    orders = TacticalController{}.control(defenders, attackers, losing, {1500, 500}, {500, 500}, quiet);
    expect(!orders.empty() && orders.front().type == CommandType::attackUnit,
           "the last screen still intercepts a melee intruder within reach");
}

void testDecisionDiagnosticsAndOperations() {
    using namespace protodd;
    GasBankController gasPolicy;
    GameState economy;
    StrategicPlan gasPlan;
    gasPlan.desiredGasWorkers = 6;
    gasPlan.posture = Posture::pressure;
    economy.self.minerals = 100;
    economy.self.gas = 350;
    expect(gasPolicy.target(economy, gasPlan) == 0, "surplus gas pauses collection while a field army needs minerals");
    economy.frame = 24;
    economy.self.minerals = 175;
    economy.self.gas = 250;
    gasPlan.posture = Posture::hold;
    expect(gasPolicy.target(economy, gasPlan) == 0, "small bank and posture changes do not rotate gas workers");
    economy.frame = 48;
    economy.self.gas = 140;
    expect(gasPolicy.target(economy, gasPlan) == 6, "gas collection resumes before the reserve is exhausted");
    economy.self.minerals = 100;
    economy.self.gas = 350;
    gasPlan.goals = {{GoalKind::train, UnitKind::arbiter, 1, 120, true, "gas-heavy tech"}};
    expect(gasPolicy.target(economy, gasPlan) == 6, "an unmet expensive unit raises the protected gas reserve");

    CommandBus measuredBus;
    measuredBus.beginFrame(0, 3);
    measuredBus.submit({1, CommandType::hold, -1, {}, UnitKind::unknown, 90, 0, "hold"});
    measuredBus.submit({1, CommandType::move, -1, {10, 10}, UnitKind::unknown, 50, 0, "move"});
    measuredBus.submit({2, CommandType::hold, -1, {}, UnitKind::unknown, 80, 0, "hold"});
    const auto selected = measuredBus.finalize(1);
    expect(selected.size() == 1 && measuredBus.stats().proposed == 3 &&
           measuredBus.stats().superseded == 1 && measuredBus.stats().budgetDeferred == 1,
           "command diagnostics distinguish arbitration from budget pressure");
    FrameIntegral idle;
    idle.sample(0, 2);
    idle.sample(24, 1);
    idle.sample(24, 1);
    idle.sample(72, 0);
    expect(idle.total() == 96, "idle duration integrates unit frames without duplicate-sample inflation");
    idle.sample(0, 1);
    idle.sample(24, 0);
    expect(idle.total() == 24, "a new game resets diagnostic durations");

    GameState state;
    state.frame = 1000;
    state.self.race = Race::protoss;
    state.self.supplyTotal = 100;
    state.self.minerals = 550;
    state.self.units = {unit(1, UnitKind::nexus, true, {100, 100}),
                        unit(2, UnitKind::gateway, true, {100, 300})};
    StrategicPlan plan;
    plan.expansionTarget = {900, 500};
    plan.desiredBases = 2;
    plan.goals = {{GoalKind::expand, UnitKind::nexus, 2, 120, true, "next base"},
                  {GoalKind::train, UnitKind::zealot, 1, 90, false, "reinforcement"}};
    ExpansionCoordinator coordinator;
    coordinator.update(plan, state, {plan.expansionTarget, true, 24});
    expect(!plan.deferExpansion && !coordinator.releaseBuilder(), "travelling expansion retains funding");
    coordinator.update(plan, state, {plan.expansionTarget, true, 8 * 24});
    expect(plan.deferExpansion && coordinator.releaseBuilder(), "stalled expansion cancels worker before bank release");
    ResourceLedger ledger{550, 0};
    const auto actions = MacroPlanner{}.reconcile(state, plan, ledger);
    expect(std::ranges::none_of(actions, [](const MacroAction& action) { return action.target == UnitKind::nexus; }) &&
           std::ranges::any_of(actions, [](const MacroAction& action) { return action.target == UnitKind::zealot && action.reserved; }),
           "expansion recovery frees the bank for a usable reinforcement");
    state.frame += 12 * 24;
    coordinator.update(plan, state, {});
    expect(!plan.deferExpansion, "expansion circuit breaker retries after a bounded recovery window");
    auto nexus = unit(9, UnitKind::nexus, true, plan.expansionTarget);
    nexus.completed = false;
    state.self.units.push_back(nexus);
    coordinator.update(plan, state, {plan.expansionTarget, true, 300});
    expect(!coordinator.releaseBuilder() && !plan.deferExpansion, "an observed warping Nexus cannot be cancelled by stale feedback");
    state.self.units.pop_back();
    auto goon = unit(10, UnitKind::dragoon, true, plan.expansionTarget);
    goon.groundWeapon = {.damage=20, .cooldown=30, .maxRange=192, .targetsGround=true};
    state.self.units.push_back(goon);
    const auto assembly = expansionAssemblyPoint(state, plan.expansionTarget, {100, 100});
    expect(distance(assembly, plan.expansionTarget) > 175, "expansion assembly stays outside the construction footprint");
    auto orders = clearExpansionFootprint(state, plan.expansionTarget, assembly);
    expect(orders.size() == 1 && orders.front().actor == goon.id, "idle army clears Nexus footprint without redirecting workers");
    state.self.units.back().underAttack = true;
    expect(clearExpansionFootprint(state, plan.expansionTarget, assembly).empty(), "footprint clearing never overrides an endangered fighter");

    goon.position = {500, 500};
    goon.weaponCooldown = 20;
    auto neighbor = goon;
    neighbor.id = 11;
    neighbor.position = {500, 532};
    auto reaver = unit(20, UnitKind::reaver, false, {680, 500});
    reaver.ammo = 5;
    reaver.groundWeapon = {.damage=100, .cooldown=60, .maxRange=256, .targetsGround=true};
    CombatEstimate engage;
    engage.decision = FightDecision::engage;
    engage.ratio = 2.0;
    InfluenceMap influence;
    orders = TacticalController{}.control(std::vector{goon, neighbor}, std::vector{reaver}, engage,
        {800, 500}, {200, 500}, influence, goon.position, 3);
    expect(std::ranges::any_of(orders, [](const Command& command) { return command.source == "splash-spacing"; }),
           "ranged cluster uses reload time to spread against observed splash");
    goon.weaponCooldown = 0;
    orders = TacticalController{}.control(std::vector{goon, neighbor}, std::vector{reaver}, engage,
        {800, 500}, {200, 500}, influence, goon.position, 3);
    expect(!orders.empty() && orders.front().actor == goon.id && orders.front().type == CommandType::attackUnit,
           "splash spacing preserves ready ranged volleys");
    goon.attackFrame = true;
    orders = TacticalController{}.control(std::vector{goon}, std::vector{reaver}, engage,
        {800, 500}, {200, 500}, influence, goon.position, 3);
    expect(orders.empty(), "splash spacing never interrupts an attack frame");
    goon.attackFrame = false;
    goon.weaponCooldown = 20;
    reaver.visible = false;
    orders = TacticalController{}.control(std::vector{goon, neighbor}, std::vector{reaver}, engage,
        {800, 500}, {200, 500}, influence, goon.position, 3);
    expect(std::ranges::none_of(orders, [](const Command& command) { return command.source == "splash-spacing"; }),
           "hidden splash memory alone cannot trigger perpetual spreading");
}

int main() {
    testEconomicHarassment();
    testScoutHarassmentAndContainment();
    testDecisionDiagnosticsAndOperations();
    testDefensiveTerrain();
    testOpeningRangedCommitment();
    testContainmentRecovery();
    testLadderSourceImprovements();
    testReportImprovements();
    testBananaBrainMacroRegressions();
    testReserveCounterattack();
    testEconomicTargeting();
    testRangedDefense();
    testCompetitionRegressions();
    testGeometry();
    testSnapshots();
    testNavigation();
    testCatalog();
    testOpponentInferenceAndStrategy();
    testSupplyPlanning();
    testStrategicDirector();
    testOpeningMilestones();
    testStrategicTargeting();
    testEconomicRecovery();
    testMacroReservations();
    testOpponentLearning();
    testInfluenceAndCombat();
    testCommandArbitration();
    testFrameBudget();
    testWorkersAndScouts();
    testLocalSquadsAndDetection();
    testTransportMissions();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All Protodd core tests passed\n";
    return EXIT_SUCCESS;
}
