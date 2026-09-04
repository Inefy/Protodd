#include "astra/Combat.hpp"
#include "astra/CommandBus.hpp"
#include "astra/GameState.hpp"
#include "astra/Geometry.hpp"
#include "astra/InfluenceMap.hpp"
#include "astra/Information.hpp"
#include "astra/Learning.hpp"
#include "astra/MacroPlanner.hpp"
#include "astra/Navigation.hpp"
#include "astra/Scouting.hpp"
#include "astra/Runtime.hpp"
#include "astra/Strategy.hpp"
#include "astra/Squads.hpp"
#include "astra/UnitCatalog.hpp"
#include "astra/Technology.hpp"
#include "astra/Transport.hpp"
#include "astra/Workers.hpp"

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
    expect(std::abs(astra::distance({0, 0}, {3, 4}) - 5.0) < 0.001,
           "Euclidean distance");
    expect(astra::moveToward({0, 0}, {100, 0}, 32.0) == astra::Position{32, 0},
           "bounded movement toward a target");
}

void testSnapshots() {
    astra::UnitSnapshot dragoon;
    dragoon.id = 7;
    dragoon.hitPoints = 80;
    dragoon.maxHitPoints = 100;
    dragoon.shields = 40;
    dragoon.maxShields = 80;
    dragoon.groundWeapon = {.damage = 20, .targetsGround = true};

    astra::UnitSnapshot enemy;
    enemy.id = 9;

    expect(std::abs(dragoon.healthFraction() - (120.0 / 180.0)) < 0.001,
           "combined shield and hit point fraction");
    expect(dragoon.canAttack(enemy), "ground target compatibility");

    astra::GameState state;
    state.self.units.push_back(dragoon);
    state.enemy.units.push_back(enemy);
    expect(state.findUnit(9).has_value(), "unit lookup across both players");
    expect(!state.findUnit(42).has_value(), "missing unit lookup");

    auto reaver = dragoon;
    reaver.kind = astra::UnitKind::reaver;
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
    astra::NavigationGrid navigation(width, height, 32, walkable);
    expect(!navigation.lineWalkable({16, 16}, {208, 16}),
           "terrain line test detects a blocking cliff");
    const auto path = navigation.findPath({16, 16}, {208, 16});
    expect(!path.empty() && std::ranges::any_of(path, [](const astra::Position point) {
               return point.y == 80;
           }),
           "A* routes a ground army through the available choke");
    const auto waypoint = navigation.nextWaypoint({16, 16}, {208, 16}, 3);
    expect(waypoint.valid() && waypoint != astra::Position{208, 16},
           "long blocked route yields an intermediate waypoint");

    for (auto y = 0; y < height; ++y) {
        walkable[static_cast<std::size_t>(y * width + 3)] = 0U;
    }
    astra::NavigationGrid disconnected(width, height, 32, walkable);
    expect(disconnected.findPath({16, 16}, {208, 16}).empty(),
           "disconnected terrain fails safely without inventing a route");
}

astra::UnitSnapshot unit(
    const astra::UnitId id,
    const astra::UnitKind kind,
    const bool ours,
    const astra::Position position = {128, 128}) {
    astra::UnitSnapshot result;
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
    expect(astra::unitStats(astra::UnitKind::probe).minerals == 50,
           "Probe catalog cost");
    expect(astra::unitStats(astra::UnitKind::overlord).name == "Overlord",
           "catalog enum and table remain aligned");
    expect(astra::isCombatUnit(astra::UnitKind::dragoon), "Dragoon combat classification");
    expect(!astra::isCombatUnit(astra::UnitKind::pylon), "Pylon combat classification");
    expect(!astra::isCombatUnit(astra::UnitKind::observer),
           "Observer remains support rather than attack army");
    expect(astra::isCombatUnit(astra::UnitKind::valkyrie) &&
               astra::isCombatUnit(astra::UnitKind::guardian) &&
               astra::isCombatUnit(astra::UnitKind::spiderMine),
           "late-game flyers and Spider Mines remain visible to combat evaluation");
    expect(astra::isCombatUnit(astra::UnitKind::zergling) &&
               astra::isCombatUnit(astra::UnitKind::scourge) &&
               astra::isCombatUnit(astra::UnitKind::broodling),
           "low-cost combat units are never filtered by an arbitrary value cutoff");
    expect(astra::isBuilding(astra::UnitKind::scienceFacility) &&
               astra::isBuilding(astra::UnitKind::defilerMound),
           "advanced enemy tech structures remain visible to inference");
    const auto tribunalRequirements = astra::unitPrerequisites(
        astra::UnitKind::arbiterTribunal);
    expect(std::ranges::find(tribunalRequirements, astra::UnitKind::stargate) !=
               tribunalRequirements.end() &&
               std::ranges::find(tribunalRequirements, astra::UnitKind::templarArchives) !=
                   tribunalRequirements.end(),
           "Arbiter Tribunal requires both branches of its tech tree");
    const auto batteryRequirements = astra::unitPrerequisites(
        astra::UnitKind::shieldBattery);
    expect(batteryRequirements.size() == 1 &&
               batteryRequirements.front() == astra::UnitKind::gateway,
           "Shield Battery follows the actual Gateway prerequisite");
    expect(astra::technologyStats(astra::TechnologyKind::protossGroundWeapons)
                       .mineralCost(2) == 150,
           "repeatable upgrades use next-level pricing");
}

void testOpponentInferenceAndStrategy() {
    astra::GameState unseen;
    unseen.enemy.race = astra::Race::terran;
    astra::OpponentModel unseenModel;
    unseenModel.update(unseen);
    expect(unseenModel.mostLikelyPlan() == astra::EnemyPlan::unknown,
           "unscouted opponents remain unknown instead of defaulting to worker rush");

    astra::GameState state;
    state.frame = 3 * 60 * 24;
    state.mapWidthPixels = 4096;
    state.mapHeightPixels = 4096;
    state.self.id = 1;
    state.self.race = astra::Race::protoss;
    state.enemy.id = 2;
    state.enemy.race = astra::Race::zerg;
    auto nexus = unit(1, astra::UnitKind::nexus, true, {256, 256});
    nexus.role = astra::UnitRole::resourceDepot;
    state.self.units.push_back(nexus);
    for (int i = 0; i < 8; ++i) {
        auto zergling = unit(100 + i, astra::UnitKind::zergling, false,
                            {300 + i * 4, 300});
        zergling.role = astra::UnitRole::groundArmy;
        zergling.groundWeapon = {.damage = 5, .cooldown = 8, .maxRange = 32,
                                .targetsGround = true};
        state.enemy.units.push_back(zergling);
    }

    astra::OpponentModel model;
    model.update(state);
    expect(model.mostLikelyPlan() == astra::EnemyPlan::fastRush,
           "early zerglings classify as fast rush");
    expect(model.assessment().immediateGround > 0.3,
           "rush produces immediate-ground warning");

    astra::GameState earlyPoolState = state;
    earlyPoolState.enemy.units.clear();
    auto earlyPool = unit(120, astra::UnitKind::spawningPool, false,
                          {2400, 2400});
    earlyPool.firstSeen = 2'700;
    earlyPool.lastSeen = earlyPoolState.frame;
    earlyPoolState.enemy.units.push_back(earlyPool);
    astra::OpponentModel earlyPoolModel;
    earlyPoolModel.update(earlyPoolState);
    expect(earlyPoolModel.mostLikelyPlan() == astra::EnemyPlan::fastRush,
           "an early scouted Spawning Pool warns of a rush before contact");

    astra::StrategyEngine strategy;
    const auto plan = strategy.plan(state, model.assessment());
    expect(plan.posture == astra::Posture::defend, "PvZ rush switches to defense");
    const auto emergencyZealots = std::ranges::find_if(
        plan.goals,
        [](const astra::ProductionGoal& goal) {
            return goal.target == astra::UnitKind::zealot && goal.blocking;
        });
    expect(emergencyZealots != plan.goals.end(), "rush plan contains blocking zealots");
    expect(std::ranges::any_of(plan.goals, [](const astra::ProductionGoal& goal) {
               return goal.target == astra::UnitKind::shieldBattery && goal.blocking;
           }),
           "opening anti-ling response adds a blocking Shield Battery");

    astra::GameState approaching = state;
    approaching.enemy.units.clear();
    for (int i = 0; i < 4; ++i) {
        auto zergling = unit(130 + i, astra::UnitKind::zergling, false,
                            {1180 + i * 8, 256});
        zergling.lastPosition = {1220 + i * 8, 256};
        zergling.role = astra::UnitRole::groundArmy;
        approaching.enemy.units.push_back(zergling);
    }
    for (int i = 0; i < 3; ++i) {
        auto hatchery = unit(140 + i, astra::UnitKind::hatchery, false,
                             {2400 + i * 160, 2400});
        hatchery.role = astra::UnitRole::production;
        approaching.enemy.units.push_back(hatchery);
    }
    astra::OpponentModel approachModel;
    approachModel.update(approaching);
    expect(approachModel.assessment().approachingCombatEnemies == 4 &&
               approachModel.assessment().approachingArmyValue > 0.0,
           "enemy motion toward the main is recognized before base contact");
    expect(approachModel.assessment().enemyProductionCapacity >= 3.0,
           "scouted production is retained as an explicit capacity estimate");

    auto terranPressure = state;
    terranPressure.enemy.race = astra::Race::terran;
    terranPressure.enemy.units.clear();
    terranPressure.bases.push_back(
        {1, {256, 256}, {300, 260}, 8000, 5000, terranPressure.self.id,
         terranPressure.frame, true, false, 8, 1});
    auto marine = unit(150, astra::UnitKind::marine, false, {320, 300});
    marine.role = astra::UnitRole::groundArmy;
    marine.groundWeapon = {.damage = 6, .cooldown = 15, .maxRange = 128,
                           .targetsGround = true};
    terranPressure.enemy.units.push_back(marine);
    for (int id = 160; id < 172; ++id) {
        auto defender = unit(id, astra::UnitKind::probe, true, {260, 260});
        defender.role = astra::UnitRole::worker;
        terranPressure.self.units.push_back(defender);
    }
    astra::ThreatAssessment visiblePressure;
    visiblePressure.combatEnemiesNearMain = 1;
    const auto economicUnderAttack = strategy.plan(
        terranPressure, visiblePressure, astra::OpeningStyle::economic);
    expect(economicUnderAttack.posture == astra::Posture::defend &&
               economicUnderAttack.desiredBases == 1 &&
               economicUnderAttack.desiredWorkers <= 14,
           "learned economic style cannot override visible main-base pressure");

    astra::GameState workerRush;
    workerRush.frame = 2 * 60 * 24;
    workerRush.self.id = 1;
    workerRush.enemy.id = 2;
    workerRush.enemy.race = astra::Race::protoss;
    auto remoteProbe = unit(1, astra::UnitKind::probe, true, {3000, 3000});
    remoteProbe.role = astra::UnitRole::worker;
    auto homeNexus = unit(2, astra::UnitKind::nexus, true, {256, 256});
    homeNexus.role = astra::UnitRole::resourceDepot;
    workerRush.self.units = {remoteProbe, homeNexus};
    for (int i = 0; i < 4; ++i) {
        workerRush.enemy.units.push_back(
            unit(20 + i, astra::UnitKind::probe, false, {300 + i * 12, 280}));
    }
    astra::OpponentModel workerModel;
    workerModel.update(workerRush);
    expect(workerModel.mostLikelyPlan() == astra::EnemyPlan::workerRush &&
               workerModel.assessment().workerRush > 0.3,
           "worker rush inference anchors to the Nexus rather than unit ordering");

    astra::GameState normalScout = workerRush;
    normalScout.enemy.units.resize(1);
    astra::OpponentModel normalScoutModel;
    normalScoutModel.update(normalScout);
    expect(normalScoutModel.mostLikelyPlan() != astra::EnemyPlan::workerRush &&
               normalScoutModel.assessment().workerRush < 0.3,
           "one scouting worker is not misclassified as a worker rush");
    const auto normalScoutPlan = strategy.plan(
        normalScout, normalScoutModel.assessment());
    expect(normalScoutPlan.posture != astra::Posture::defend,
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

    astra::GameState cannonRush = workerRush;
    cannonRush.enemy.units.clear();
    auto cannon = unit(80, astra::UnitKind::photonCannon, false, {480, 300});
    cannon.completed = false;
    cannon.buildProgress = 40;
    cannonRush.enemy.units.push_back(cannon);
    astra::OpponentModel cannonModel;
    cannonModel.update(cannonRush);
    expect(cannonModel.mostLikelyPlan() == astra::EnemyPlan::staticContain,
           "nearby opening cannon classifies as a static contain");
    const auto containPlan = strategy.plan(cannonRush, cannonModel.assessment(),
                                           astra::OpeningStyle::economic);
    expect(containPlan.posture == astra::Posture::defend &&
               containPlan.desiredBases == 1 &&
               std::ranges::none_of(containPlan.goals,
                                    [](const astra::ProductionGoal& candidate) {
                                        return candidate.goal == astra::GoalKind::expand;
                                    }),
           "static contain overrides learned greed and suppresses expansion");

    astra::GameState adaptive;
    adaptive.frame = 11 * 60 * 24;
    adaptive.self.id = 1;
    adaptive.self.race = astra::Race::protoss;
    adaptive.self.supplyUsed = 100;
    adaptive.self.supplyTotal = 150;
    adaptive.enemy.id = 2;
    adaptive.enemy.race = astra::Race::zerg;
    auto adaptiveNexus = unit(300, astra::UnitKind::nexus, true, {256, 256});
    adaptiveNexus.role = astra::UnitRole::resourceDepot;
    adaptive.self.units.push_back(adaptiveNexus);
    for (int i = 0; i < 8; ++i) {
        auto hydralisk = unit(400 + i, astra::UnitKind::hydralisk, false,
                              {1200 + i * 8, 1200});
        hydralisk.role = astra::UnitRole::groundArmy;
        hydralisk.lastSeen = adaptive.frame;
        adaptive.enemy.units.push_back(hydralisk);
    }
    for (int i = 0; i < 6; ++i) {
        auto guardian = unit(500 + i, astra::UnitKind::guardian, false,
                             {1300 + i * 8, 1250});
        guardian.role = astra::UnitRole::airArmy;
        guardian.flying = true;
        guardian.lastSeen = adaptive.frame;
        adaptive.enemy.units.push_back(guardian);
    }
    const auto adaptivePlan = strategy.plan(adaptive, {});
    expect(std::ranges::any_of(adaptivePlan.goals, [](const astra::ProductionGoal& goal) {
               return goal.target == astra::UnitKind::reaver && goal.desiredCount >= 2;
           }),
           "observed hydralisk mass adds a reaver splash counter");
    expect(std::ranges::any_of(adaptivePlan.goals, [](const astra::ProductionGoal& goal) {
               return goal.target == astra::UnitKind::corsair && goal.desiredCount >= 7 &&
                      goal.blocking;
           }),
           "observed Zerg air mass increases blocking air-control production");
}

void testSupplyPlanning() {
    astra::GameState state;
    state.self.id = 1;
    state.self.race = astra::Race::protoss;
    state.enemy.race = astra::Race::terran;
    state.self.supplyUsed = 12;
    state.self.supplyTotal = 18;
    auto nexus = unit(1, astra::UnitKind::nexus, true);
    nexus.role = astra::UnitRole::resourceDepot;
    state.self.units.push_back(nexus);

    astra::StrategyEngine strategy;
    const auto opening = strategy.plan(state, {});
    const auto pylonGoal = std::ranges::find(
        opening.goals, astra::UnitKind::pylon, &astra::ProductionGoal::target);
    expect(pylonGoal != opening.goals.end() && pylonGoal->desiredCount == 1 &&
               pylonGoal->blocking,
           "opening supply logic reserves the first pylon before a supply block");
    state.self.minerals = 100;
    astra::ResourceLedger openingLedger{state.self.minerals, state.self.gas};
    const auto openingActions = astra::MacroPlanner{}.reconcile(
        state, opening, openingLedger);
    expect(!openingActions.empty() &&
               openingActions.front().action == astra::MacroActionKind::build &&
               openingActions.front().target == astra::UnitKind::pylon &&
               openingActions.front().reserved,
           "live opening state turns the blocking pylon goal into the first command");

    auto pendingPylon = unit(2, astra::UnitKind::pylon, true);
    pendingPylon.completed = false;
    pendingPylon.buildProgress = 20;
    state.self.units.push_back(pendingPylon);
    const auto constructing = strategy.plan(state, {});
    const auto constructingGoal = std::ranges::find(
        constructing.goals, astra::UnitKind::pylon, &astra::ProductionGoal::target);
    expect(constructingGoal != constructing.goals.end() &&
               constructingGoal->desiredCount == 1,
           "pending pylon supply prevents a duplicate construction order");

    state.self.supplyUsed = 40;
    state.self.supplyTotal = 60;
    state.self.units.pop_back();
    for (int id = 10; id < 14; ++id) {
        state.self.units.push_back(unit(id, astra::UnitKind::gateway, true));
        state.self.queuedUnits.push_back(astra::UnitKind::dragoon);
    }
    const auto productionForecast = strategy.plan(state, {});
    const auto forecastPylon = std::ranges::find(
        productionForecast.goals, astra::UnitKind::pylon,
        &astra::ProductionGoal::target);
    expect(forecastPylon != productionForecast.goals.end() &&
               forecastPylon->desiredCount == 1,
           "supply forecast accounts for queued units and active production");

    astra::GameState banked;
    banked.frame = 4 * 60 * 24;
    banked.self.race = astra::Race::protoss;
    banked.enemy.race = astra::Race::terran;
    banked.self.minerals = 700;
    banked.self.supplyUsed = 24;
    banked.self.supplyTotal = 34;
    banked.self.units = {
        unit(30, astra::UnitKind::nexus, true),
        unit(31, astra::UnitKind::pylon, true),
        unit(32, astra::UnitKind::gateway, true),
    };
    banked.bases.push_back(
        {1, {128, 128}, {160, 128}, 8000, 5000, banked.self.id,
         banked.frame, true, false, 8, 1});
    const auto spendingPlan = strategy.plan(banked, {});
    expect(std::ranges::any_of(spendingPlan.goals, [](const astra::ProductionGoal& goal) {
               return goal.target == astra::UnitKind::gateway &&
                      goal.desiredCount >= 2 && goal.priority == 62;
           }),
           "sustained mineral surplus adds production instead of banking indefinitely");

    banked.frame = 5 * 60 * 24;
    banked.self.minerals = 0;
    for (int id = 33; id < 45; ++id) {
        auto worker = unit(id, astra::UnitKind::probe, true);
        worker.role = astra::UnitRole::worker;
        banked.self.units.push_back(worker);
    }
    const auto expansionPlan = strategy.plan(banked, {});
    expect(expansionPlan.desiredBases >= 2,
           "PvT observer expansion begins by the five-minute economic phase");
    expect(std::ranges::any_of(
               expansionPlan.goals, [](const astra::ProductionGoal& goal) {
                   return goal.goal == astra::GoalKind::expand && goal.blocking &&
                          goal.priority > 74;
               }),
           "safe due expansion reserves its bank ahead of routine Probe production");

    astra::ThreatAssessment threatenedExpansion;
    threatenedExpansion.combatEnemiesNearMain = 4;
    threatenedExpansion.immediateGround = 0.8;
    const auto defensePlan = strategy.plan(banked, threatenedExpansion);
    expect(std::ranges::none_of(
               defensePlan.goals, [](const astra::ProductionGoal& goal) {
                   return goal.goal == astra::GoalKind::expand && goal.blocking;
               }),
           "immediate pressure cancels expansion banking in favor of defenders");

    auto chainedExpansion = banked;
    chainedExpansion.frame = 11 * 60 * 24;
    auto pendingNexus = unit(45, astra::UnitKind::nexus, true, {640, 640});
    pendingNexus.role = astra::UnitRole::resourceDepot;
    pendingNexus.completed = false;
    chainedExpansion.self.units.push_back(pendingNexus);
    const auto chainedPlan = strategy.plan(chainedExpansion, {});
    expect(std::ranges::none_of(
               chainedPlan.goals, [](const astra::ProductionGoal& goal) {
                   return goal.goal == astra::GoalKind::expand && goal.blocking &&
                          goal.desiredCount >= 3;
               }),
           "an unfinished expansion cannot immediately reserve a third Nexus");

    astra::GameState throughput;
    throughput.frame = 4 * 60 * 24;
    throughput.self.race = astra::Race::protoss;
    throughput.enemy.race = astra::Race::protoss;
    throughput.self.supplyUsed = 40;
    throughput.self.supplyTotal = 50;
    throughput.self.units = {
        unit(40, astra::UnitKind::nexus, true),
        unit(41, astra::UnitKind::pylon, true),
        unit(42, astra::UnitKind::gateway, true),
    };
    throughput.self.units.front().role = astra::UnitRole::resourceDepot;
    for (int id = 50; id < 68; ++id) {
        auto worker = unit(id, astra::UnitKind::probe, true);
        worker.role = astra::UnitRole::worker;
        throughput.self.units.push_back(worker);
    }
    throughput.bases.push_back(
        {1, {128, 128}, {160, 128}, 8000, 5000, throughput.self.id,
         throughput.frame, true, false, 8, 1});
    const auto throughputPlan = strategy.plan(throughput, {});
    expect(std::ranges::any_of(
               throughputPlan.goals, [](const astra::ProductionGoal& goal) {
                   return goal.target == astra::UnitKind::gateway &&
                          goal.desiredCount >= 3 && goal.priority == 73;
               }),
           "saturated one-base economy proactively scales army throughput");

    astra::ThreatAssessment productionThreat;
    productionThreat.enemyProductionCapacity = 3.0;
    throughput.self.units.erase(throughput.self.units.begin() + 3,
                                throughput.self.units.end());
    const auto parityPlan = strategy.plan(throughput, productionThreat);
    expect(std::ranges::any_of(
               parityPlan.goals, [](const astra::ProductionGoal& goal) {
                   return goal.target == astra::UnitKind::gateway &&
                          goal.desiredCount == 3 && goal.priority == 78;
               }),
           "scouted enemy production prevents an underbuilt one-base response");
}

void testStrategicDirector() {
    astra::StrategicDirector director;
    astra::GameState state;
    state.frame = 100;
    astra::StrategicPlan pressure;
    pressure.name = "pressure";
    pressure.posture = astra::Posture::pressure;
    expect(director.stabilize(pressure, state, {}).posture == astra::Posture::pressure,
           "strategic director accepts the initial map-level intent");

    astra::StrategicPlan defense = pressure;
    defense.posture = astra::Posture::defend;
    astra::ThreatAssessment breach;
    breach.combatEnemiesNearMain = 2;
    state.frame += 24;
    expect(director.stabilize(defense, state, breach).posture == astra::Posture::defend,
           "strategic emergencies override an attack immediately");

    state.frame += 4 * 24;
    const auto regrouping = director.stabilize(pressure, state, {});
    expect(regrouping.posture == astra::Posture::defend &&
               regrouping.attackThreshold >= 1.40,
           "one clear observation cannot relaunch an army after base defense");

    state.frame += 5 * 24;
    expect(director.stabilize(pressure, state, {}).posture == astra::Posture::pressure,
           "sustained safety releases the regrouped army");
}

void testOpeningMilestones() {
    astra::GameState state;
    state.self.id = 1;
    state.self.race = astra::Race::protoss;
    state.enemy.id = 2;
    state.enemy.race = astra::Race::terran;
    state.self.supplyTotal = 34;
    auto nexus = unit(1, astra::UnitKind::nexus, true);
    nexus.role = astra::UnitRole::resourceDepot;
    state.self.units.push_back(nexus);
    astra::StrategyEngine strategy;

    state.self.supplyUsed = 16;
    const auto beforeGateway = strategy.plan(state, {});
    expect(std::ranges::none_of(beforeGateway.goals, [](const astra::ProductionGoal& goal) {
               return goal.target == astra::UnitKind::gateway;
           }),
           "opening does not spend on a Gateway before its supply milestone");

    state.self.supplyUsed = 18;
    const auto gatewayTiming = strategy.plan(state, {});
    expect(std::ranges::any_of(gatewayTiming.goals, [](const astra::ProductionGoal& goal) {
               return goal.target == astra::UnitKind::gateway && goal.blocking;
           }),
           "nine-supply Gateway becomes a mandatory opening milestone");

    state.self.supplyUsed = 20;
    const auto terranSafety = strategy.plan(state, {});
    expect(std::ranges::any_of(terranSafety.goals, [](const astra::ProductionGoal& goal) {
               return goal.target == astra::UnitKind::zealot && goal.blocking;
           }),
           "PvT banks one opening bodyguard before committing to dragoon tech");

    state.enemy.race = astra::Race::protoss;
    state.self.supplyUsed = 20;
    const auto zealotTiming = strategy.plan(state, {});
    expect(std::ranges::any_of(zealotTiming.goals, [](const astra::ProductionGoal& goal) {
               return goal.target == astra::UnitKind::zealot && goal.blocking &&
                      goal.priority > 97;
           }),
           "PvP banks the first defenders before spending on technology");

    state.self.supplyUsed = 26;
    state.self.units.push_back(unit(2, astra::UnitKind::gateway, true));
    state.self.units.push_back(unit(3, astra::UnitKind::gateway, true));
    state.self.units.push_back(unit(4, astra::UnitKind::zealot, true));
    const auto coreTiming = strategy.plan(state, {});
    expect(std::ranges::any_of(coreTiming.goals, [](const astra::ProductionGoal& goal) {
               return goal.target == astra::UnitKind::cyberneticsCore && goal.blocking;
           }),
           "thirteen-supply Core becomes a mandatory opening milestone");
    expect(coreTiming.desiredGasWorkers == 3,
           "gas mining starts before gas-dependent Dragoon technology");
    state.self.supplyUsed = 28;
    const auto rangeTiming = strategy.plan(state, {});
    const auto rangeGoal = std::ranges::find_if(
        rangeTiming.goals, [](const astra::ProductionGoal& goal) {
            return goal.technology == astra::TechnologyKind::singularityCharge;
        });
    expect(rangeGoal != rangeTiming.goals.end() && !rangeGoal->blocking,
           "Dragoon range cannot freeze early army and production spending");

    astra::ThreatAssessment baseBreach;
    baseBreach.combatEnemiesNearMain = 3;
    const auto emergencyTiming = strategy.plan(state, baseBreach);
    expect(emergencyTiming.composition.size() == 1 &&
               emergencyTiming.composition.front().kind == astra::UnitKind::zealot &&
               std::ranges::none_of(
                   emergencyTiming.goals, [](const astra::ProductionGoal& goal) {
                       return goal.target == astra::UnitKind::cyberneticsCore ||
                              goal.technology ==
                                  astra::TechnologyKind::singularityCharge;
                   }),
           "PvP base breach cannot reserve tech ahead of continuous defenders");

    state.enemy.race = astra::Race::protoss;
    state.frame = 5 * 60 * 24;
    state.bases.push_back(
        {1, {128, 128}, {160, 128}, 8000, 5000, 1, state.frame,
         true, false, 8, 1});
    const auto oneBasePlan = strategy.plan(state, {});
    expect(oneBasePlan.desiredBases == 1 && oneBasePlan.desiredWorkers <= 22,
           "one-base plans stop Probe production at a useful saturation cap");

    state.enemy.race = astra::Race::zerg;
    state.self.supplyUsed = 20;
    std::erase_if(state.self.units, [](const astra::UnitSnapshot& candidate) {
        return candidate.id == 3;
    });
    const auto safePvZ = strategy.plan(state, {});
    expect(std::ranges::any_of(safePvZ.goals, [](const astra::ProductionGoal& goal) {
               return goal.target == astra::UnitKind::gateway &&
                      goal.desiredCount >= 2 && goal.blocking;
           }),
           "PvZ secures two-gate throughput before exposing the economy");
    expect(std::ranges::any_of(safePvZ.goals, [](const astra::ProductionGoal& goal) {
               return goal.target == astra::UnitKind::forge && goal.blocking;
           }) &&
               std::ranges::any_of(
                   safePvZ.goals, [](const astra::ProductionGoal& goal) {
                       return goal.target == astra::UnitKind::photonCannon &&
                              goal.desiredCount >= 1 && goal.blocking;
                   }),
           "PvZ establishes a fortified anchor before exposing its economy");

    astra::GameState poolFirst;
    poolFirst.self.id = 1;
    poolFirst.self.race = astra::Race::protoss;
    poolFirst.enemy.id = 2;
    poolFirst.enemy.race = astra::Race::zerg;
    poolFirst.self.supplyUsed = 14;
    poolFirst.self.supplyTotal = 18;
    auto poolNexus = unit(80, astra::UnitKind::nexus, true, {128, 128});
    poolNexus.role = astra::UnitRole::resourceDepot;
    poolFirst.self.units.push_back(poolNexus);
    for (int id = 81; id < 88; ++id) {
        auto poolProbe = unit(id, astra::UnitKind::probe, true, {128, 128});
        poolProbe.role = astra::UnitRole::worker;
        poolFirst.self.units.push_back(poolProbe);
    }
    const auto poolFirstPlan = strategy.plan(poolFirst, {});
    expect(poolFirstPlan.desiredWorkers == 8 &&
               std::ranges::any_of(
                   poolFirstPlan.goals, [](const astra::ProductionGoal& goal) {
                       return goal.target == astra::UnitKind::gateway &&
                              goal.blocking;
                   }),
           "pool-first-safe opening banks a Gateway before resuming Probe growth");

    astra::ThreatAssessment earlyZergThreat;
    earlyZergThreat.immediateGround = 0.35;
    earlyZergThreat.combatEnemiesNearMain = 6;
    const auto pressuredPoolFirstPlan = strategy.plan(poolFirst, earlyZergThreat);
    expect(std::ranges::any_of(
               pressuredPoolFirstPlan.goals, [](const astra::ProductionGoal& goal) {
                   return goal.target == astra::UnitKind::photonCannon &&
                          goal.desiredCount >= 2 && goal.priority == 100;
               }),
           "confirmed early Zerg pressure immediately doubles static coverage");

    auto fortifiedPoolFirst = poolFirst;
    fortifiedPoolFirst.self.units.push_back(
        unit(90, astra::UnitKind::forge, true, {160, 160}));
    fortifiedPoolFirst.self.units.push_back(
        unit(91, astra::UnitKind::photonCannon, true, {180, 160}));
    fortifiedPoolFirst.self.units.push_back(
        unit(92, astra::UnitKind::photonCannon, true, {200, 160}));
    const auto recoveryBehindCannons = strategy.plan(
        fortifiedPoolFirst, earlyZergThreat);
    expect(std::ranges::any_of(
               recoveryBehindCannons.goals, [](const astra::ProductionGoal& goal) {
                   return goal.target == astra::UnitKind::probe &&
                          goal.desiredCount >= 10 && goal.priority == 100 &&
                          goal.blocking;
               }),
           "completed anti-rush Cannons immediately restore Probe production");
}

void testStrategicTargeting() {
    astra::GameState state;
    state.self.id = 1;
    state.self.race = astra::Race::protoss;
    state.enemy.id = 2;
    state.enemy.race = astra::Race::terran;
    state.bases = {
        {1, {256, 256}, {280, 256}, 8000, 5000, 1, 100, true, false},
        {2, {1800, 1800}, {1760, 1800}, 8000, 5000, -1, 0, true, false},
    };
    auto nexus = unit(1, astra::UnitKind::nexus, true, {256, 256});
    nexus.role = astra::UnitRole::resourceDepot;
    state.self.units.push_back(nexus);

    astra::StrategyEngine strategy;
    const auto search = strategy.plan(state, {});
    expect(search.attackTarget == astra::Position{1800, 1800},
           "unknown enemy search excludes our owned start location");
    expect(search.rallyPoint != nexus.position &&
               astra::distance(search.rallyPoint, nexus.position) > 128.0 &&
               astra::distance(search.rallyPoint, search.attackTarget) <
                   astra::distance(nexus.position, search.attackTarget),
           "defensive rally screens the mineral line toward the enemy approach");

    auto hiddenTech = unit(20, astra::UnitKind::factory, false, {1500, 1400});
    hiddenTech.visible = false;
    state.enemy.units.push_back(hiddenTech);
    const auto cleanup = strategy.plan(state, {});
    expect(cleanup.attackTarget == hiddenTech.position,
           "cleanup objective retains a remembered enemy structure");
}

void testEconomicRecovery() {
    astra::GameState state;
    state.frame = 10 * 60 * 24;
    state.self.id = 1;
    state.self.race = astra::Race::protoss;
    state.enemy.id = 2;
    state.enemy.race = astra::Race::terran;
    state.self.supplyUsed = 30;
    state.self.supplyTotal = 50;
    state.bases.push_back(
        {1, {256, 256}, {280, 260}, 6000, 5000, -1, 0, true, false, 8, 1});
    for (int id = 1; id <= 5; ++id) {
        auto probe = unit(id, astra::UnitKind::probe, true, {256 + id * 4, 256});
        probe.role = astra::UnitRole::worker;
        state.self.units.push_back(probe);
    }
    astra::StrategyEngine strategy;
    const auto lostMain = strategy.plan(state, {});
    expect(lostMain.posture == astra::Posture::recover &&
               std::ranges::any_of(lostMain.goals, [](const astra::ProductionGoal& goal) {
                   return goal.target == astra::UnitKind::nexus && goal.blocking &&
                          goal.priority == 100;
               }),
           "surviving workers trigger an emergency Nexus rebuild");

    state.self.units.clear();
    state.bases.clear();
    for (int id = 1; id <= 3; ++id) {
        auto nexus = unit(id, astra::UnitKind::nexus, true, {id * 400, 256});
        nexus.role = astra::UnitRole::resourceDepot;
        state.self.units.push_back(nexus);
        state.bases.push_back(
            {id, {id * 400, 256}, {id * 400 + 30, 256}, 0, 0, 1, 0,
             id == 1, false, 0, 1});
    }
    for (int id = 10; id < 30; ++id) {
        auto probe = unit(id, astra::UnitKind::probe, true);
        probe.role = astra::UnitRole::worker;
        state.self.units.push_back(probe);
    }
    const auto depleted = strategy.plan(state, {});
    expect(depleted.desiredBases >= 4 &&
               std::ranges::any_of(depleted.goals, [](const astra::ProductionGoal& goal) {
                   return goal.target == astra::UnitKind::nexus &&
                          goal.desiredCount >= 4 && goal.blocking;
               }),
           "mined-out Nexuses do not prevent replacement expansion");

    auto disabledGateway = unit(50, astra::UnitKind::gateway, true);
    disabledGateway.powered = false;
    state.self.units.push_back(disabledGateway);
    const auto repower = strategy.plan(state, {});
    expect(std::ranges::any_of(repower.goals, [](const astra::ProductionGoal& goal) {
               return goal.target == astra::UnitKind::pylon && goal.priority == 98 &&
                      goal.blocking;
           }),
           "unpowered production triggers mandatory local repowering");
}

void testMacroReservations() {
    astra::GameState state;
    state.self.minerals = 200;
    astra::StrategicPlan plan;
    plan.goals = {
        {astra::GoalKind::build, astra::UnitKind::pylon, 1, 100, true, "supply"},
        {astra::GoalKind::build, astra::UnitKind::gateway, 1, 90, false, "production"},
    };
    astra::ResourceLedger ledger{state.self.minerals, 0};
    astra::MacroPlanner planner;
    const auto actions = planner.reconcile(state, plan, ledger);
    expect(actions.size() == 1, "only affordable macro goal is emitted");
    expect(actions.front().target == astra::UnitKind::pylon && actions.front().reserved,
           "higher-priority pylon reserves first");
    expect(ledger.freeMinerals() == 100, "resource reservation is explicit");

    state.self.units.push_back(unit(1, astra::UnitKind::pylon, true));
    astra::StrategicPlan emergency;
    emergency.goals = {
        {astra::GoalKind::build, astra::UnitKind::gateway, 1, 100, true, "emergency"},
        {astra::GoalKind::train, astra::UnitKind::probe, 1, 50, false, "worker"},
    };
    astra::ResourceLedger poor{100, 0};
    const auto waiting = planner.reconcile(state, emergency, poor);
    expect(waiting.size() == 1 && !waiting.front().reserved,
           "unaffordable blocking goal prevents lower-priority spending");
    expect(poor.freeMinerals() == 0 && poor.reservedMinerals == 100,
           "blocking reservation protects the available partial bank");

    astra::GameState queuedState;
    queuedState.self.minerals = 50;
    queuedState.self.units.push_back(unit(2, astra::UnitKind::nexus, true));
    queuedState.self.queuedUnits.push_back(astra::UnitKind::probe);
    astra::StrategicPlan queuedPlan;
    queuedPlan.goals = {
        {astra::GoalKind::train, astra::UnitKind::probe, 1, 80, false, "worker"},
    };
    astra::ResourceLedger queuedLedger{50, 0};
    expect(planner.reconcile(queuedState, queuedPlan, queuedLedger).empty(),
           "queued production counts toward macro targets");

    astra::GameState duplicateState;
    duplicateState.self.minerals = 800;
    duplicateState.self.units.push_back(unit(3, astra::UnitKind::nexus, true));
    astra::StrategicPlan duplicatePlan;
    duplicatePlan.goals = {
        {astra::GoalKind::expand, astra::UnitKind::nexus, 2, 90, false, "expand"},
        {astra::GoalKind::expand, astra::UnitKind::nexus, 2, 70, false, "economic style"},
    };
    astra::ResourceLedger duplicateLedger{800, 0};
    const auto expansions = planner.reconcile(duplicateState, duplicatePlan, duplicateLedger);
    expect(expansions.size() == 1 && expansions.front().target == astra::UnitKind::nexus,
           "overlapping strategic goals reserve only one missing structure");

    astra::GameState blockedDuplicateState;
    blockedDuplicateState.self.minerals = 100;
    blockedDuplicateState.self.units = {
        unit(30, astra::UnitKind::pylon, true),
        unit(31, astra::UnitKind::gateway, true),
    };
    astra::StrategicPlan blockedDuplicatePlan;
    blockedDuplicatePlan.goals = {
        {astra::GoalKind::build, astra::UnitKind::cyberneticsCore, 1, 95, true,
         "first core goal"},
        {astra::GoalKind::build, astra::UnitKind::cyberneticsCore, 1, 90, true,
         "overlapping core goal"},
    };
    astra::ResourceLedger blockedDuplicateLedger{100, 0};
    const auto blockedCoreActions = planner.reconcile(
        blockedDuplicateState, blockedDuplicatePlan, blockedDuplicateLedger);
    expect(blockedCoreActions.size() == 1 &&
               blockedCoreActions.front().target == astra::UnitKind::cyberneticsCore,
           "unaffordable blocking goals are deduplicated in one macro pass");

    astra::GameState busyProducerState;
    busyProducerState.self.minerals = 250;
    busyProducerState.self.units = {
        unit(35, astra::UnitKind::pylon, true),
        unit(36, astra::UnitKind::gateway, true),
    };
    busyProducerState.self.queuedUnits = {astra::UnitKind::zealot};
    astra::StrategicPlan busyProducerPlan;
    busyProducerPlan.goals = {
        {astra::GoalKind::train, astra::UnitKind::zealot, 3, 98, true,
         "more defenders"},
        {astra::GoalKind::build, astra::UnitKind::gateway, 2, 97, true,
         "increase throughput"},
    };
    astra::ResourceLedger busyProducerLedger{250, 0};
    const auto busyProducerActions = planner.reconcile(
        busyProducerState, busyProducerPlan, busyProducerLedger);
    expect(busyProducerActions.size() == 1 &&
               busyProducerActions.front().target == astra::UnitKind::gateway &&
               busyProducerActions.front().reserved,
           "busy producers do not reserve queued units ahead of new throughput");

    busyProducerState.self.queuedUnits.clear();
    busyProducerState.self.busyProducers = {astra::UnitKind::gateway};
    astra::ResourceLedger latencyBusyLedger{250, 0};
    const auto latencyBusyActions = planner.reconcile(
        busyProducerState, busyProducerPlan, latencyBusyLedger);
    expect(latencyBusyActions.size() == 1 &&
               latencyBusyActions.front().target == astra::UnitKind::gateway,
           "isTraining occupancy closes the BWAPI queue-visibility latency gap");

    astra::GameState techState;
    techState.self.minerals = 100;
    astra::StrategicPlan techPlan;
    techPlan.goals = {
        {astra::GoalKind::train, astra::UnitKind::dragoon, 1, 90, false, "tech unit"},
    };
    astra::ResourceLedger techLedger{100, 0};
    const auto techActions = planner.reconcile(techState, techPlan, techLedger);
    expect(techActions.size() == 1 && techActions.front().target == astra::UnitKind::pylon,
           "unreachable unit goals build the next missing prerequisite first");

    astra::GameState pendingPrerequisite;
    pendingPrerequisite.self.minerals = 150;
    auto unfinishedPylon = unit(38, astra::UnitKind::pylon, true);
    unfinishedPylon.completed = false;
    pendingPrerequisite.self.units.push_back(unfinishedPylon);
    astra::StrategicPlan pendingPrerequisitePlan;
    pendingPrerequisitePlan.goals = {
        {astra::GoalKind::train, astra::UnitKind::zealot, 1, 99, true,
         "opening defender"},
    };
    astra::ResourceLedger pendingPrerequisiteLedger{150, 0};
    const auto chainedActions = planner.reconcile(
        pendingPrerequisite, pendingPrerequisitePlan, pendingPrerequisiteLedger);
    expect(chainedActions.size() == 1 && chainedActions.front().reserved &&
               chainedActions.front().action == astra::MacroActionKind::build &&
               chainedActions.front().target == astra::UnitKind::gateway,
           "an in-progress Pylon advances reservation to the Gateway, not an impossible Zealot");

    astra::GameState finishingGateway;
    finishingGateway.self.minerals = 150;
    auto incompleteGateway = unit(39, astra::UnitKind::gateway, true);
    incompleteGateway.completed = false;
    finishingGateway.self.units.push_back(incompleteGateway);
    finishingGateway.self.units.push_back(
        unit(40, astra::UnitKind::nexus, true));
    astra::StrategicPlan finishingGatewayPlan;
    finishingGatewayPlan.goals = {
        {astra::GoalKind::train, astra::UnitKind::zealot, 1, 99, true,
         "reserve first defender"},
        {astra::GoalKind::train, astra::UnitKind::probe, 1, 74, false,
         "spend safe surplus"},
    };
    astra::ResourceLedger finishingGatewayLedger{150, 0};
    const auto finishingGatewayActions = planner.reconcile(
        finishingGateway, finishingGatewayPlan, finishingGatewayLedger);
    expect(finishingGatewayActions.size() == 2 &&
               finishingGatewayActions.front().target == astra::UnitKind::zealot &&
               finishingGatewayActions.front().reserved &&
               !finishingGatewayActions.front().executable &&
               finishingGatewayActions.back().target == astra::UnitKind::probe &&
               finishingGatewayActions.back().reserved &&
               finishingGatewayActions.back().executable,
           "future unit reservation does not block executable surplus production");

    astra::GameState compositionState;
    compositionState.self.minerals = 125;
    compositionState.self.gas = 50;
    compositionState.self.supplyTotal = 20;
    compositionState.self.units = {
        unit(4, astra::UnitKind::gateway, true),
        unit(5, astra::UnitKind::cyberneticsCore, true),
    };
    astra::StrategicPlan compositionPlan;
    compositionPlan.composition = {{astra::UnitKind::dragoon, 1.0}};
    astra::ResourceLedger compositionLedger{125, 50};
    const auto compositionActions = planner.reconcile(
        compositionState, compositionPlan, compositionLedger);
    expect(compositionActions.size() == 1 &&
               compositionActions.front().target == astra::UnitKind::dragoon,
           "remaining resources continuously reinforce the planned composition");

    compositionState.self.minerals = 100;
    compositionState.self.gas = 0;
    compositionPlan.composition = {
        {astra::UnitKind::dragoon, 0.9}, {astra::UnitKind::zealot, 0.1},
    };
    astra::ResourceLedger fallbackLedger{100, 0};
    const auto fallbackActions = planner.reconcile(
        compositionState, compositionPlan, fallbackLedger);
    expect(fallbackActions.size() == 1 &&
               fallbackActions.front().target == astra::UnitKind::zealot,
           "composition production falls back instead of idling on an unaffordable unit");

    astra::GameState parallelProduction;
    parallelProduction.self.minerals = 300;
    parallelProduction.self.supplyTotal = 40;
    parallelProduction.self.units = {
        unit(20, astra::UnitKind::gateway, true),
        unit(21, astra::UnitKind::gateway, true),
        unit(22, astra::UnitKind::gateway, true),
    };
    astra::StrategicPlan parallelPlan;
    parallelPlan.composition = {{astra::UnitKind::zealot, 1.0}};
    astra::ResourceLedger parallelLedger{300, 0};
    const auto parallelActions = planner.reconcile(
        parallelProduction, parallelPlan, parallelLedger);
    expect(parallelActions.size() == 3 &&
               std::ranges::all_of(parallelActions, [](const astra::MacroAction& action) {
                   return action.action == astra::MacroActionKind::train &&
                          action.target == astra::UnitKind::zealot && action.reserved;
               }),
           "one macro pass fills every affordable idle Gateway");

    parallelProduction.self.queuedUnits.push_back(astra::UnitKind::zealot);
    astra::ResourceLedger partlyBusyLedger{300, 0};
    expect(planner.reconcile(parallelProduction, parallelPlan, partlyBusyLedger).size() == 2,
           "existing queues consume producer slots before parallel pumping");

    parallelProduction.self.queuedUnits.clear();
    parallelProduction.self.supplyUsed = 36;
    astra::ResourceLedger supplyBoundLedger{300, 0};
    expect(planner.reconcile(parallelProduction, parallelPlan, supplyBoundLedger).size() == 1,
           "parallel pumping never overcommits the remaining supply");

    astra::GameState upgradeState;
    upgradeState.self.minerals = 150;
    upgradeState.self.gas = 150;
    upgradeState.self.units.push_back(unit(8, astra::UnitKind::cyberneticsCore, true));
    astra::StrategicPlan upgradePlan;
    upgradePlan.goals = {
        {astra::GoalKind::upgrade, astra::UnitKind::unknown, 1, 95, true,
         "dragoon range", astra::TechnologyKind::singularityCharge},
    };
    astra::ResourceLedger upgradeLedger{150, 150};
    const auto upgradeActions = planner.reconcile(upgradeState, upgradePlan, upgradeLedger);
    expect(upgradeActions.size() == 1 && upgradeActions.front().reserved &&
               upgradeActions.front().action == astra::MacroActionKind::upgrade &&
               upgradeActions.front().technology ==
                   astra::TechnologyKind::singularityCharge,
           "strategic upgrades reserve resources as executable macro actions");

    upgradeState.self.technologies.push_back(
        {astra::TechnologyKind::singularityCharge, 0, true});
    astra::ResourceLedger duplicateUpgradeLedger{150, 150};
    expect(planner.reconcile(upgradeState, upgradePlan, duplicateUpgradeLedger).empty(),
           "in-progress technology is never issued twice");

    upgradeState.self.technologies.clear();
    upgradeState.self.minerals = 100;
    upgradeState.self.gas = 0;
    upgradePlan.goals.push_back(
        {astra::GoalKind::train, astra::UnitKind::probe, 1, 30, false, "worker"});
    astra::ResourceLedger savingLedger{100, 0};
    const auto savingActions = planner.reconcile(upgradeState, upgradePlan, savingLedger);
    expect(savingActions.size() == 1 && !savingActions.front().reserved &&
               savingActions.front().technology ==
                   astra::TechnologyKind::singularityCharge,
           "mandatory technology preserves its bank instead of leaking to cheap production");

    upgradeState.self.units.push_back(unit(9, astra::UnitKind::nexus, true));
    upgradeState.self.minerals = 250;
    astra::ResourceLedger surplusLedger{250, 0};
    const auto surplusActions = planner.reconcile(upgradeState, upgradePlan, surplusLedger);
    expect(std::ranges::any_of(surplusActions, [](const astra::MacroAction& action) {
               return action.target == astra::UnitKind::probe && action.reserved;
           }) && surplusLedger.reservedMinerals == 200,
           "gas-starved technology protects its cost while surplus minerals keep probes flowing");

    astra::GameState supplyInvariant;
    supplyInvariant.self.id = 1;
    supplyInvariant.self.race = astra::Race::protoss;
    supplyInvariant.self.minerals = 100;
    supplyInvariant.self.supplyUsed = 12;
    supplyInvariant.self.supplyTotal = 18;
    supplyInvariant.self.units.push_back(unit(40, astra::UnitKind::nexus, true));
    astra::ResourceLedger supplyLedger{100, 0};
    const auto protectedSupply = planner.reconcile(supplyInvariant, {}, supplyLedger);
    expect(protectedSupply.size() == 1 && protectedSupply.front().reserved &&
               protectedSupply.front().blocksLowerPriority &&
               protectedSupply.front().target == astra::UnitKind::pylon,
           "macro safety layer prevents a supply deadlock even with an empty strategy");

    auto pendingSupply = unit(41, astra::UnitKind::pylon, true);
    pendingSupply.completed = false;
    supplyInvariant.self.units.push_back(pendingSupply);
    astra::ResourceLedger pendingSupplyLedger{100, 0};
    expect(planner.reconcile(supplyInvariant, {}, pendingSupplyLedger).empty(),
           "supply invariant does not duplicate an in-progress pylon");
}

void testOpponentLearning() {
    astra::OpponentHistory history;
    history.parse(
        "Bot,Map,standard,8,2\n"
        "Bot,Map,aggressive,2,8\n"
        "Bot,Map,economic,3,7\n"
        "Bot,Map,deceptive,1,9\n");
    expect(history.choose("Bot", "Map", 7) == astra::OpeningStyle::standard,
           "UCB learning exploits clearly successful opening");
    history.record("Bot", "Map", astra::OpeningStyle::standard, true);
    const auto encoded = history.serialize();
    astra::OpponentHistory restored;
    restored.parse(encoded);
    expect(restored.lookup("Bot", "Map", astra::OpeningStyle::standard).wins == 9,
           "opponent history round-trips through tournament CSV");

    restored.merge("Bot,Map,standard,11,2\nBot,Map,aggressive,1,4\n");
    expect(restored.lookup("Bot", "Map", astra::OpeningStyle::standard).wins == 11 &&
               restored.lookup("Bot", "Map", astra::OpeningStyle::standard).losses == 2 &&
               restored.lookup("Bot", "Map", astra::OpeningStyle::aggressive).losses == 8,
           "read and write learning snapshots merge without losing cumulative results");

    astra::OpponentHistory fresh;
    const auto exploration = fresh.choose("NewBot", "Map", 3);
    expect(exploration != astra::OpeningStyle::count,
           "fresh opponent selects a valid deterministic exploration arm");
}

void testInfluenceAndCombat() {
    astra::GameState state;
    state.mapWidthPixels = 1024;
    state.mapHeightPixels = 1024;
    auto enemy = unit(20, astra::UnitKind::hydralisk, false, {512, 512});
    enemy.role = astra::UnitRole::groundArmy;
    enemy.groundWeapon = {.damage = 10, .cooldown = 15, .maxRange = 128,
                          .targetsGround = true};
    state.enemy.units.push_back(enemy);
    astra::InfluenceMap influence;
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
    cannon.kind = astra::UnitKind::photonCannon;
    cannon.role = astra::UnitRole::staticDefense;
    staleState.enemy.units = {cannon};
    influence.update(staleState);
    expect(influence.at({512, 512}).groundThreat > 0.0F,
           "remembered static defenses persist until their tile is cleared");

    auto detectorState = state;
    auto observer = unit(22, astra::UnitKind::observer, false, {512, 512});
    observer.role = astra::UnitRole::detector;
    observer.flying = true;
    observer.sightRange = 11 * 32;
    detectorState.enemy.units = {observer};
    influence.update(detectorState);
    expect(influence.at({800, 512}).detection > 0.0F,
           "mobile detection field uses the observer's real sight radius");

    std::vector<astra::UnitSnapshot> friendly;
    for (int i = 0; i < 4; ++i) {
        auto dragoon = unit(30 + i, astra::UnitKind::dragoon, true, {400, 400 + i * 8});
        dragoon.role = astra::UnitRole::groundArmy;
        dragoon.shields = 80;
        dragoon.maxShields = 80;
        dragoon.groundWeapon = {.damage = 20, .cooldown = 30, .maxRange = 192,
                                .targetsGround = true};
        friendly.push_back(dragoon);
    }
    astra::CombatEvaluator evaluator;
    const auto estimate = evaluator.evaluate(friendly, state.enemy.units, 1.1, 0.1);
    expect(estimate.decision == astra::FightDecision::engage,
           "overwhelming dragoon force elects to engage");
    expect(estimate.simulatedEnemyRemaining < 0.001,
           "bounded combat simulation predicts lethal focus-fire volleys");
    expect(evaluator.selectTarget(friendly.front(), state.enemy.units) != nullptr,
           "combat target selection finds compatible target");

    auto hallucination = enemy;
    hallucination.hallucination = true;
    const std::vector<astra::UnitSnapshot> hallucinations{hallucination};
    const auto hallucinationEstimate = evaluator.evaluate(
        friendly, hallucinations, 1.1, 0.0);
    expect(hallucinationEstimate.enemyPower == 0.0 &&
               evaluator.selectTarget(friendly.front(), hallucinations) == nullptr,
           "known hallucinations neither deter the army nor consume volleys");

    auto wounded = enemy;
    wounded.id = 21;
    wounded.hitPoints = 10;
    const std::vector<astra::UnitSnapshot> targetChoices{wounded, enemy};
    const astra::TargetAllocation lethalVolley[]{
        {wounded.id, 20},
    };
    expect(evaluator.selectTarget(friendly.front(), targetChoices, lethalVolley)->id == enemy.id,
           "focus fire redirects once a target has lethal committed damage");

    auto firstCorsair = unit(70, astra::UnitKind::corsair, true, {400, 400});
    firstCorsair.role = astra::UnitRole::airArmy;
    firstCorsair.flying = true;
    firstCorsair.airWeapon = {.damage = 5, .cooldown = 8, .maxRange = 160,
                              .targetsAir = true, .hits = 2};
    auto secondCorsair = firstCorsair;
    secondCorsair.id = 71;
    auto firstScourge = unit(72, astra::UnitKind::scourge, false, {450, 400});
    firstScourge.role = astra::UnitRole::airArmy;
    firstScourge.flying = true;
    firstScourge.hitPoints = 8;
    firstScourge.maxHitPoints = 25;
    auto secondScourge = firstScourge;
    secondScourge.id = 73;
    secondScourge.position = {455, 405};
    const std::vector<astra::UnitSnapshot> corsairs{firstCorsair, secondCorsair};
    const std::vector<astra::UnitSnapshot> scourge{firstScourge, secondScourge};
    astra::CombatEstimate volleyEstimate;
    volleyEstimate.decision = astra::FightDecision::engage;
    astra::InfluenceMap volleyInfluence;
    astra::TacticalController volleyTactics;
    const auto volleyOrders = volleyTactics.control(
        corsairs, scourge, volleyEstimate, {900, 900}, {100, 100}, volleyInfluence);
    expect(volleyOrders.size() == 2 &&
               volleyOrders[0].targetUnit != volleyOrders[1].targetUnit,
           "multi-hit volleys reserve exact lethal damage and avoid overkill");

    auto explosiveAttacker = friendly.front();
    explosiveAttacker.groundWeapon = {
        .damage = 100, .cooldown = 1000, .maxRange = 192,
        .damageType = astra::DamageType::explosive, .targetsGround = true,
    };
    auto smallTarget = enemy;
    smallTarget.hitPoints = 100;
    smallTarget.maxHitPoints = 100;
    smallTarget.size = astra::UnitSize::small;
    auto largeTarget = smallTarget;
    largeTarget.size = astra::UnitSize::large;
    const std::vector<astra::UnitSnapshot> oneAttacker{explosiveAttacker};
    const std::vector<astra::UnitSnapshot> smallForce{smallTarget};
    const std::vector<astra::UnitSnapshot> largeForce{largeTarget};
    const auto versusSmall = evaluator.evaluate(oneAttacker, smallForce, 1.0, 0.0);
    const auto versusLarge = evaluator.evaluate(oneAttacker, largeForce, 1.0, 0.0);
    expect(versusSmall.simulatedEnemyRemaining > versusLarge.simulatedEnemyRemaining,
           "simulation applies Brood War damage-type modifiers by unit size");

    auto singleHit = friendly.front();
    singleHit.groundWeapon = {.damage = 6, .cooldown = 30, .maxRange = 192,
                              .targetsGround = true, .hits = 1};
    auto multiHit = singleHit;
    multiHit.groundWeapon.hits = 4;
    const std::vector<astra::UnitSnapshot> singleHitForce{singleHit};
    const std::vector<astra::UnitSnapshot> multiHitForce{multiHit};
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
    const std::vector<astra::UnitSnapshot> armoredForce{armoredTarget};
    const std::vector<astra::UnitSnapshot> smallHitsForce{fourSmallHits};
    const std::vector<astra::UnitSnapshot> largeHitForce{oneLargeHit};
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
    auto melee = unit(50, astra::UnitKind::zergling, false, {250, 200});
    melee.role = astra::UnitRole::groundArmy;
    melee.groundWeapon = {.damage = 5, .cooldown = 8, .maxRange = 32,
                          .targetsGround = true};
    astra::CombatEstimate kiteEstimate;
    kiteEstimate.decision = astra::FightDecision::kite;
    astra::InfluenceMap emptyInfluence;
    astra::TacticalController tactics;
    const std::vector<astra::UnitSnapshot> kitingForce{kiter};
    const std::vector<astra::UnitSnapshot> meleeForce{melee};
    const auto kiteOrders = tactics.control(
        kitingForce, meleeForce, kiteEstimate, {900, 900}, {100, 200}, emptyInfluence);
    expect(kiteOrders.size() == 1 && kiteOrders.front().type == astra::CommandType::move &&
               kiteOrders.front().targetPosition.x < kiter.position.x,
           "ranged cooldown micro steps directly away from a nearby melee threat");

    auto firing = kiter;
    firing.attackFrame = true;
    const std::vector<astra::UnitSnapshot> firingForce{firing};
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
    const std::vector<astra::UnitSnapshot> equalTargets{equalFirst, equalLocked};
    expect(evaluator.selectTarget(lockedAttacker, equalTargets)->id == equalLocked.id,
           "equal-value focus fire retains the current target instead of oscillating");

    auto zealotDefender = unit(82, astra::UnitKind::zealot, true, {200, 200});
    zealotDefender.groundWeapon = {.damage = 8, .cooldown = 22, .maxRange = 32,
                                   .targetsGround = true, .hits = 2};
    auto nearbyLing = melee;
    nearbyLing.id = 83;
    nearbyLing.position = {280, 200};
    auto distantLing = nearbyLing;
    distantLing.id = 84;
    distantLing.position = {700, 200};
    distantLing.hitPoints = 1;
    const std::vector<astra::UnitSnapshot> splitRush{distantLing, nearbyLing};
    expect(evaluator.selectTarget(zealotDefender, splitRush)->id == nearbyLing.id,
           "melee defenders do not chase a tempting distant target out of the base");

    auto templar = unit(60, astra::UnitKind::highTemplar, true, {600, 500});
    templar.role = astra::UnitRole::spellcaster;
    const std::vector<astra::UnitSnapshot> casters{templar};
    const auto casterOrders = tactics.control(
        casters, meleeForce, estimate, {900, 900}, {100, 100}, emptyInfluence, {400, 400});
    expect(casterOrders.size() == 1 && casterOrders.front().source == "spellcaster-screen",
           "high-value spellcasters stay behind the formation screen");

    auto stormTemplar = templar;
    stormTemplar.energy = 100;
    stormTemplar.position = {500, 500};
    std::vector<astra::UnitSnapshot> stormTargets;
    for (int i = 0; i < 4; ++i) {
        auto marine = unit(90 + i, astra::UnitKind::marine, false,
                           {650 + i * 12, 500 + (i % 2) * 12});
        marine.role = astra::UnitRole::groundArmy;
        stormTargets.push_back(marine);
    }
    const std::vector<astra::UnitSnapshot> stormCasters{stormTemplar};
    const auto stormOrders = tactics.control(
        stormCasters, stormTargets, estimate, {900, 900}, {100, 100},
        emptyInfluence, {500, 500}, 2, true);
    expect(stormOrders.size() == 1 &&
               stormOrders.front().type == astra::CommandType::useTech &&
               stormOrders.front().technology == astra::TechnologyKind::psionicStorm,
           "researched High Templar cast safe high-value Psionic Storms tactically");

    auto friendlyDragoon = unit(95, astra::UnitKind::dragoon, true,
                                stormTargets.front().position);
    const std::vector<astra::UnitSnapshot> unsafeCasters{stormTemplar, friendlyDragoon};
    const auto unsafeStorm = tactics.control(
        unsafeCasters, stormTargets, estimate, {900, 900}, {100, 100},
        emptyInfluence, {500, 500}, 2, true);
    expect(std::ranges::none_of(unsafeStorm, [](const astra::Command& command) {
               return command.type == astra::CommandType::useTech;
           }),
           "storm targeting rejects clusters with excessive friendly fire");

    astra::EngagementTracker engagement;
    expect(engagement.stabilize(77, astra::FightDecision::engage, 1.3, 1.2, 100) ==
               astra::FightDecision::engage,
           "first local combat estimate establishes a squad decision");
    expect(engagement.stabilize(77, astra::FightDecision::kite, 1.0, 1.2, 102) ==
               astra::FightDecision::engage &&
               engagement.stabilize(77, astra::FightDecision::kite, 1.0, 1.2, 104) ==
                   astra::FightDecision::engage &&
               engagement.stabilize(77, astra::FightDecision::kite, 1.0, 1.2, 106) ==
                   astra::FightDecision::kite,
           "borderline simulation noise cannot reverse a squad on one frame");
    expect(engagement.stabilize(77, astra::FightDecision::retreat, 0.4, 1.2, 108) ==
               astra::FightDecision::retreat,
           "catastrophic local odds bypass combat hysteresis immediately");
}

void testCommandArbitration() {
    astra::CommandBus bus;
    bus.beginFrame(100, 2);
    bus.submit({7, astra::CommandType::move, -1, {400, 400},
                astra::UnitKind::unknown, 20, 0, "patrol"});
    bus.submit({7, astra::CommandType::attackUnit, 9, {-1, -1},
                astra::UnitKind::unknown, 80, 0, "combat"});
    bus.submit({7, astra::CommandType::recharge, 10, {-1, -1},
                astra::UnitKind::shieldBattery, 95, 0, "recharge"});
    auto selected = bus.finalize();
    expect(selected.size() == 1 && selected.front().type == astra::CommandType::recharge,
           "shield preservation can override a routine attack per actor");

    bus.clear();
    bus.beginFrame(100, 2);
    bus.submit({7, astra::CommandType::attackUnit, 9, {-1, -1},
                astra::UnitKind::unknown, 80, 0, "combat"});
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
            bus.submit({actor, astra::CommandType::move, -1, {actor * 32, 100},
                        astra::UnitKind::unknown, 50, 0, "budget"});
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
    astra::FrameBudget budget;
    expect(budget.load(100) == astra::RuntimeLoad::normal,
           "frame budget begins at full quality");
    budget.record(100, 30000);
    expect(budget.load(101) == astra::RuntimeLoad::reduced &&
               !budget.allowSimulation(101) &&
               budget.expensiveCadenceMultiplier(101) == 2,
           "slow frame temporarily sheds expensive optional work");
    expect(budget.load(400) == astra::RuntimeLoad::normal,
           "quality automatically recovers after the cooldown window");
    budget.record(500, 56000);
    expect(budget.load(501) == astra::RuntimeLoad::emergency &&
               budget.combatCommandLimit(501) == 40U &&
               budget.navigationInterval(501) == 96,
           "dangerous frame time enters the emergency budget");
    expect(budget.stats().over42ms == 1U && budget.stats().over55ms == 1U,
           "AIIDE frame-time thresholds are counted explicitly");
}

void testWorkersAndScouts() {
    const std::vector<astra::MineralPatchCandidate> unbalancedPatches{
        {10, {300, 256}, 2}, {11, {340, 256}, 0}, {12, {380, 256}, 1},
    };
    expect(astra::selectMineralPatch(unbalancedPatches, {340, 256}, {256, 256}, 10) == 11,
           "mineral assignment fills the least-saturated patch first");
    const std::vector<astra::MineralPatchCandidate> balancedPatches{
        {10, {300, 256}, 1}, {11, {340, 256}, 1}, {12, {380, 256}, 1},
    };
    expect(astra::selectMineralPatch(balancedPatches, {340, 256}, {256, 256}, 12) == 12,
           "balanced mineral assignment retains its current patch");

    astra::GameState state;
    state.frame = 5000;
    state.mapWidthPixels = 2048;
    state.mapHeightPixels = 2048;
    state.self.id = 1;
    state.enemy.id = 2;
    state.bases.push_back({1, {256, 256}, {300, 260}, 8000, 5000, 1, 4000,
                           true, false, 8, 1});
    state.bases.push_back({2, {1700, 1700}, {1680, 1700}, 8000, 5000, -1, 0, true, false});
    auto probe = unit(5, astra::UnitKind::probe, true, {260, 260});
    probe.role = astra::UnitRole::worker;
    state.self.units.push_back(probe);
    auto observer = unit(6, astra::UnitKind::observer, true, {300, 300});
    observer.flying = true;
    observer.role = astra::UnitRole::detector;
    state.self.units.push_back(observer);
    state.self.units.push_back(unit(7, astra::UnitKind::assimilator, true, {320, 256}));

    astra::InfluenceMap influence;
    influence.update(state);
    astra::StrategicPlan plan;
    plan.desiredGasWorkers = 1;
    astra::WorkerManager workers;
    const auto assignments = workers.assign(state, plan, influence);
    expect(assignments.size() == 1 && assignments.front().job == astra::WorkerJob::gas,
           "gas policy assigns requested worker count");

    auto gasState = state;
    auto existingGasProbe = probe;
    existingGasProbe.id = 8;
    existingGasProbe.position = {420, 256};
    existingGasProbe.orderTargetId = 7;
    gasState.self.units.push_back(existingGasProbe);
    const auto stableGas = workers.assign(gasState, plan, influence);
    expect(std::ranges::any_of(stableGas, [](const astra::WorkerAssignment& assignment) {
               return assignment.worker == 8 && assignment.job == astra::WorkerJob::gas;
           }) && std::ranges::none_of(stableGas, [](const astra::WorkerAssignment& assignment) {
               return assignment.worker == 5 && assignment.job == astra::WorkerJob::gas;
           }),
           "gas rebalance preserves an existing refinery worker instead of oscillating jobs");

    gasState.self.minerals = 50;
    gasState.self.gas = 400;
    plan.posture = astra::Posture::defend;
    const auto mineralRecovery = workers.assign(gasState, plan, influence);
    expect(std::ranges::none_of(
               mineralRecovery, [](const astra::WorkerAssignment& assignment) {
                   return assignment.job == astra::WorkerJob::gas;
               }),
           "mineral-starved defense releases gas workers after a sufficient gas bank");

    astra::GameState militiaState;
    militiaState.frame = 4 * 60 * 24;
    militiaState.self.id = 1;
    militiaState.enemy.id = 2;
    militiaState.mapWidthPixels = 2048;
    militiaState.mapHeightPixels = 2048;
    militiaState.bases.push_back(
        {1, {256, 256}, {300, 260}, 8000, 5000, 1, 0, true, false, 8, 1});
    for (int i = 0; i < 10; ++i) {
        auto defender = unit(100 + i, astra::UnitKind::probe, true,
                             {240 + i * 8, 260});
        defender.role = astra::UnitRole::worker;
        militiaState.self.units.push_back(defender);
    }
    auto tank = unit(200, astra::UnitKind::siegeTank, false, {400, 260});
    tank.role = astra::UnitRole::groundArmy;
    tank.groundWeapon = {.damage = 70, .cooldown = 75, .maxRange = 384,
                         .targetsGround = true};
    militiaState.enemy.units.push_back(tank);
    astra::InfluenceMap militiaInfluence;
    militiaInfluence.update(militiaState);
    const auto tankResponse = workers.assign(militiaState, {}, militiaInfluence);
    expect(std::ranges::none_of(tankResponse, [](const astra::WorkerAssignment& assignment) {
               return assignment.job == astra::WorkerJob::defend;
           }),
           "worker militia never charges a siege tank");

    auto loneScout = unit(202, astra::UnitKind::probe, false, {350, 260});
    loneScout.role = astra::UnitRole::worker;
    militiaState.enemy.units = {loneScout};
    militiaInfluence.update(militiaState);
    const auto scoutResponse = workers.assign(militiaState, {}, militiaInfluence);
    expect(std::ranges::none_of(
               scoutResponse, [](const astra::WorkerAssignment& assignment) {
                   return assignment.job == astra::WorkerJob::defend;
               }),
           "one enemy scout does not pull a Probe away from mining");

    auto zealotThreat = unit(203, astra::UnitKind::zealot, false, {300, 260});
    zealotThreat.role = astra::UnitRole::groundArmy;
    zealotThreat.groundWeapon = {.damage = 16, .cooldown = 22, .maxRange = 32,
                                 .targetsGround = true, .hits = 2};
    militiaState.enemy.units = {zealotThreat};
    militiaState.self.units.front().underAttack = true;
    militiaState.self.units.front().hitPoints = 60;
    militiaInfluence.update(militiaState);
    const auto woundedResponse = workers.assign(militiaState, {}, militiaInfluence);
    expect(std::ranges::any_of(
               woundedResponse, [zealotThreat](const astra::WorkerAssignment& assignment) {
                   return assignment.worker == 100 &&
                          assignment.job == astra::WorkerJob::evacuate &&
                          assignment.targetUnit == zealotThreat.id;
               }),
           "a Probe wounded by melee pressure disengages after the first hit");
    militiaState.self.units.front().underAttack = false;
    militiaState.self.units.front().hitPoints = 100;

    for (auto& worker : militiaState.self.units) {
        if (worker.kind == astra::UnitKind::probe) worker.carryingResources = true;
    }
    auto firstLing = unit(204, astra::UnitKind::zergling, false, {300, 252});
    firstLing.role = astra::UnitRole::groundArmy;
    firstLing.groundWeapon = {.damage = 5, .cooldown = 8, .maxRange = 32,
                              .targetsGround = true};
    auto secondLing = firstLing;
    secondLing.id = 205;
    secondLing.position = {304, 268};
    militiaState.enemy.units = {firstLing, secondLing};
    militiaInfluence.update(militiaState);
    const auto cargoMilitia = workers.assign(militiaState, {}, militiaInfluence);
    expect(std::ranges::count(cargoMilitia, astra::WorkerJob::defend,
                              &astra::WorkerAssignment::job) == 4,
           "mineral-carrying Probes still join an emergency anti-ling surround");
    for (auto& worker : militiaState.self.units) {
        if (worker.kind == astra::UnitKind::probe) worker.carryingResources = false;
    }

    auto proxyCannon = unit(201, astra::UnitKind::photonCannon, false, {420, 280});
    proxyCannon.completed = false;
    proxyCannon.buildProgress = 35;
    militiaState.enemy.units = {loneScout};
    militiaState.enemy.units.push_back(proxyCannon);
    militiaInfluence.update(militiaState);
    const auto cannonResponse = workers.assign(militiaState, {}, militiaInfluence);
    expect(std::ranges::count(cannonResponse, astra::WorkerJob::defend,
                              &astra::WorkerAssignment::job) == 4 &&
               std::ranges::all_of(cannonResponse, [](const astra::WorkerAssignment& assignment) {
                   return assignment.job != astra::WorkerJob::defend ||
                          assignment.targetUnit == 201;
               }),
           "four healthy Probes focus an unfinished proxy cannon");

    const astra::UnitId reservedProbe[]{probe.id};
    const auto leased = workers.assign(state, plan, influence, reservedProbe);
    expect(leased.size() == 1 && leased.front().job == astra::WorkerJob::build,
           "leased scout or builder probe cannot be reclaimed by mining");

    astra::GameState openingScoutState;
    openingScoutState.frame = 120;
    openingScoutState.self.units.push_back(probe);
    expect(astra::selectOpeningWorkerScout(openingScoutState) == -1,
           "worker scouting waits until the opening pylon has started");
    openingScoutState.self.units.push_back(
        unit(8, astra::UnitKind::pylon, true, {300, 300}));
    expect(astra::selectOpeningWorkerScout(openingScoutState, {}, reservedProbe) == -1,
           "worker scouting never overwrites a reserved builder order");
    auto alternateProbe = probe;
    alternateProbe.id = 9;
    openingScoutState.self.units.push_back(alternateProbe);
    expect(astra::selectOpeningWorkerScout(openingScoutState, {}, reservedProbe) == 9,
           "worker scouting selects a non-builder after pylon construction begins");

    astra::GameState noNexus;
    noNexus.self.id = 1;
    noNexus.mapWidthPixels = 2048;
    noNexus.mapHeightPixels = 2048;
    noNexus.bases.push_back(
        {1, {256, 256}, {300, 260}, 6000, 5000, -1, 0, true, false, 8, 1});
    auto survivor = unit(70, astra::UnitKind::probe, true, {260, 260});
    survivor.role = astra::UnitRole::worker;
    noNexus.self.units.push_back(survivor);
    astra::InfluenceMap recoveryInfluence;
    recoveryInfluence.update(noNexus);
    const auto recoveryMining = workers.assign(noNexus, {}, recoveryInfluence);
    expect(recoveryMining.size() == 1 &&
               recoveryMining.front().job == astra::WorkerJob::minerals,
           "surviving Probes keep mining while a replacement Nexus is built");

    const auto scoutState = state;
    state.bases[1].ownerId = 1;
    state.bases[1].mineralPatches = 8;
    for (int i = 0; i < 16; ++i) {
        auto extra = unit(20 + i, astra::UnitKind::probe, true, {260 + i, 270});
        extra.role = astra::UnitRole::worker;
        state.self.units.push_back(extra);
    }
    plan.desiredGasWorkers = 0;
    const auto balanced = workers.assign(state, plan, influence);
    expect(std::ranges::any_of(balanced, [](const astra::WorkerAssignment& assignment) {
               return assignment.job == astra::WorkerJob::transfer && assignment.baseId == 2;
           }),
           "oversaturated mineral lines transfer workers to an owned expansion");

    const astra::UnitId scouts[]{6};
    astra::ScoutManager scouting;
    const auto orders = scouting.assign(scoutState, scouts, influence, {});
    expect(orders.size() == 1 && orders.front().target == astra::Position{1700, 1700},
           "scout prioritizes stale unexplored start location");

    astra::GameState riskState;
    riskState.mapWidthPixels = 2048;
    riskState.mapHeightPixels = 2048;
    riskState.self.id = 1;
    riskState.enemy.id = 2;
    auto riskProbe = unit(300, astra::UnitKind::probe, true, {128, 128});
    riskProbe.role = astra::UnitRole::worker;
    riskState.self.units.push_back(riskProbe);
    riskState.bases.push_back(
        {3, {900, 128}, {900, 128}, 8000, 0, -1, 0, false, false, 8, 0});
    riskState.bases.push_back(
        {4, {128, 1800}, {128, 1800}, 8000, 0, -1, 0, false, false, 8, 0});
    auto corridorTank = unit(301, astra::UnitKind::siegeTank, false, {520, 128});
    corridorTank.role = astra::UnitRole::groundArmy;
    corridorTank.groundWeapon = {.damage = 70, .cooldown = 75, .maxRange = 384,
                                 .targetsGround = true};
    riskState.enemy.units.push_back(corridorTank);
    astra::InfluenceMap riskInfluence;
    riskInfluence.update(riskState);
    astra::ScoutManager riskScouting;
    const astra::UnitId riskScoutIds[]{300};
    const auto safeProbeOrder = riskScouting.assign(riskState, riskScoutIds,
                                                    riskInfluence, {});
    expect(safeProbeOrder.size() == 1 &&
               safeProbeOrder.front().target == astra::Position{128, 1800},
           "ground scout rejects a shorter route through siege-tank influence");

    riskState.self.units.front().kind = astra::UnitKind::observer;
    riskState.self.units.front().role = astra::UnitRole::detector;
    riskState.self.units.front().flying = true;
    riskScouting.reset();
    const auto flyingOrder = riskScouting.assign(riskState, riskScoutIds,
                                                 riskInfluence, {});
    expect(flyingOrder.size() == 1 &&
               flyingOrder.front().target == astra::Position{520, 128} &&
               flyingOrder.front().purpose == astra::ScoutPurpose::watchArmy,
           "flying scout ignores ground-only danger and shadows the army");
}

void testLocalSquadsAndDetection() {
    astra::GameState state;
    state.mapWidthPixels = 2048;
    state.mapHeightPixels = 2048;
    state.self.id = 1;
    state.enemy.id = 2;
    state.bases.push_back({1, {256, 256}, {300, 260}, 8000, 5000, 1, 0, true, false});

    std::vector<astra::UnitSnapshot> friendly;
    for (int i = 0; i < 6; ++i) {
        auto dragoon = unit(10 + i, astra::UnitKind::dragoon, true,
                            i < 3 ? astra::Position{300 + i * 24, 300}
                                  : astra::Position{1500 + i * 24, 1500});
        dragoon.role = astra::UnitRole::groundArmy;
        dragoon.groundWeapon = {.damage = 20, .cooldown = 30, .maxRange = 192,
                                .targetsGround = true};
        friendly.push_back(dragoon);
    }
    auto lurker = unit(90, astra::UnitKind::lurker, false, {380, 320});
    lurker.role = astra::UnitRole::groundArmy;
    lurker.burrowed = true;
    lurker.detected = false;
    lurker.groundWeapon = {.damage = 20, .cooldown = 37, .maxRange = 192,
                           .targetsGround = true};
    const std::vector<astra::UnitSnapshot> enemy{lurker};

    astra::StrategicPlan plan;
    plan.rallyPoint = {256, 256};
    plan.attackTarget = {1800, 1800};
    astra::SquadPlanner planner;
    auto squads = planner.form(state, friendly, enemy, plan, {256, 256});
    const auto defense = std::ranges::find_if(squads, [](const astra::Squad& squad) {
        return squad.role == astra::SquadRole::baseDefense;
    });
    expect(defense != squads.end() && defense->needsDetection,
           "cloaked base threat creates detection-aware defense squad");
    expect(defense != squads.end() &&
               astra::distance(defense->retreat, state.bases.front().mineralLine) >
                   astra::distance(state.bases.front().center,
                                   state.bases.front().mineralLine) &&
               defense->requiredRatio < 0.6,
           "base defense screens on the safe side of the economy instead of retreating through workers");

    auto breachedDefense = *defense;
    auto visibleLing = unit(91, astra::UnitKind::zergling, false,
                            breachedDefense.retreat);
    visibleLing.role = astra::UnitRole::groundArmy;
    visibleLing.groundWeapon = {.damage = 5, .cooldown = 8, .maxRange = 32,
                                .targetsGround = true};
    breachedDefense.enemies = {visibleLing};
    expect(astra::SquadPlanner::mustHoldDefensiveScreen(breachedDefense),
           "base defenders stop retreating once melee attackers breach the economy screen");
    visibleLing.position = {1800, 1800};
    breachedDefense.enemies = {visibleLing};
    expect(!astra::SquadPlanner::mustHoldDefensiveScreen(breachedDefense),
           "base defenders can still disengage before a distant threat reaches the economy");

    std::vector<astra::UnitSnapshot> heavyThreats;
    for (int i = 0; i < 3; ++i) {
        auto tank = unit(110 + i, astra::UnitKind::siegeTank, false,
                         {400 + i * 24, 320});
        tank.role = astra::UnitRole::groundArmy;
        tank.groundWeapon = {.damage = 70, .cooldown = 75, .maxRange = 384,
                             .targetsGround = true};
        heavyThreats.push_back(tank);
    }
    const auto heavyDefense = planner.form(
        state, friendly, heavyThreats, plan, {256, 256});
    const auto committed = std::ranges::find_if(
        heavyDefense, [](const astra::Squad& squad) {
            return squad.role == astra::SquadRole::baseDefense;
        });
    expect(committed != heavyDefense.end() && committed->units.size() == friendly.size(),
           "base defense commits enough army value to answer heavy units, not a fixed headcount");

    auto observer = unit(100, astra::UnitKind::observer, true, {200, 200});
    observer.flying = true;
    observer.role = astra::UnitRole::detector;
    state.self.units.push_back(observer);
    astra::InfluenceMap influence;
    influence.update(state);
    const auto escorts = planner.detectorEscorts(state, squads, influence);
    expect(!escorts.empty() && escorts.front().actor == observer.id,
           "observer is assigned to highest-priority detection squad");

    auto localCannon = unit(101, astra::UnitKind::photonCannon, true, {280, 280});
    localCannon.role = astra::UnitRole::staticDefense;
    localCannon.groundWeapon = {.damage = 20, .cooldown = 22, .maxRange = 224,
                                .targetsGround = true};
    friendly.push_back(localCannon);
    squads = planner.form(state, friendly, {}, plan, {256, 256});
    const auto mainGroups = std::ranges::count_if(squads, [](const astra::Squad& squad) {
        return squad.role == astra::SquadRole::mainArmy;
    });
    expect(mainGroups == 2 && std::ranges::none_of(
               squads, [](const astra::Squad& squad) {
                   return std::ranges::any_of(squad.units, [](const astra::UnitSnapshot& member) {
                       return astra::isStaticDefense(member.kind);
                   });
               }),
           "static defenses cannot glue disconnected mobile armies into one squad");

    const auto* vanguard = astra::SquadPlanner::selectVanguard(squads, plan.attackTarget);
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
    astra::GameState state;
    state.mapWidthPixels = 2048;
    state.mapHeightPixels = 2048;
    auto shuttle = unit(200, astra::UnitKind::shuttle, true, {100, 100});
    shuttle.flying = true;
    shuttle.role = astra::UnitRole::transport;
    shuttle.cargoSpace = 8;
    auto reaver = unit(201, astra::UnitKind::reaver, true, {300, 100});
    reaver.role = astra::UnitRole::groundArmy;
    state.self.units = {shuttle, reaver};
    astra::InfluenceMap influence;
    influence.update(state);
    astra::TransportController transports;

    auto orders = transports.control(state, {1000, 100}, {100, 100}, influence);
    expect(std::ranges::any_of(orders, [](const astra::Command& command) {
               return command.actor == 200 && command.source == "shuttle-rendezvous";
           }) && std::ranges::any_of(orders, [](const astra::Command& command) {
               return command.actor == 201 && command.source == "reaver-rendezvous";
           }),
           "shuttle and reaver rendezvous under persistent mission ownership");

    state.frame = 1;
    state.self.units[1].position = {150, 100};
    orders = transports.control(state, {1000, 100}, {100, 100}, influence);
    expect(std::ranges::any_of(orders, [](const astra::Command& command) {
               return command.type == astra::CommandType::load && command.targetUnit == 201;
           }),
           "nearby reaver receives a transport load command");

    state.frame = 2;
    state.self.units[1].loaded = true;
    state.self.units[1].transportId = 200;
    state.self.units[1].position = state.self.units[0].position;
    orders = transports.control(state, {1000, 100}, {100, 100}, influence);
    expect(std::ranges::any_of(orders, [](const astra::Command& command) {
               return command.source == "shuttle-attack-route";
           }),
           "loaded shuttle begins its threat-aware attack transit");

    state.frame = 3;
    state.self.units[0].position = {900, 100};
    state.self.units[1].position = {900, 100};
    orders = transports.control(state, {1000, 100}, {100, 100}, influence);
    expect(std::ranges::any_of(orders, [](const astra::Command& command) {
               return command.type == astra::CommandType::unload &&
                      command.source == "reaver-drop";
           }),
           "shuttle unloads the reaver at the mission objective");

    state.frame = 4;
    state.self.units[1].loaded = false;
    state.self.units[1].transportId = -1;
    static_cast<void>(transports.control(state, {1000, 100}, {100, 100}, influence));
    state.frame = 4 + 7 * 24;
    orders = transports.control(state, {1000, 100}, {100, 100}, influence);
    expect(std::ranges::any_of(orders, [](const astra::Command& command) {
               return command.type == astra::CommandType::load &&
                      command.source == "reaver-extract";
           }),
           "drop mission extracts its reaver after the bounded firing window");
}

}  // namespace

int main() {
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
    std::cout << "All Astra core tests passed\n";
    return EXIT_SUCCESS;
}
