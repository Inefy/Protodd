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

    astra::StrategyEngine strategy;
    const auto plan = strategy.plan(state, model.assessment());
    expect(plan.posture == astra::Posture::defend, "PvZ rush switches to defense");
    const auto emergencyZealots = std::ranges::find_if(
        plan.goals,
        [](const astra::ProductionGoal& goal) {
            return goal.target == astra::UnitKind::zealot && goal.blocking;
        });
    expect(emergencyZealots != plan.goals.end(), "rush plan contains blocking zealots");
}

void testSupplyPlanning() {
    astra::GameState state;
    state.self.id = 1;
    state.self.race = astra::Race::protoss;
    state.enemy.race = astra::Race::terran;
    state.self.supplyUsed = 16;
    state.self.supplyTotal = 18;
    auto nexus = unit(1, astra::UnitKind::nexus, true);
    nexus.role = astra::UnitRole::resourceDepot;
    state.self.units.push_back(nexus);

    astra::StrategyEngine strategy;
    const auto opening = strategy.plan(state, {});
    const auto pylonGoal = std::ranges::find(
        opening.goals, astra::UnitKind::pylon, &astra::ProductionGoal::target);
    expect(pylonGoal != opening.goals.end() && pylonGoal->desiredCount == 1,
           "opening supply logic requests one pylon rather than overbuilding two");

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

    state.self.supplyUsed = 18;
    const auto beforeGateway = strategy.plan(state, {});
    expect(std::ranges::none_of(beforeGateway.goals, [](const astra::ProductionGoal& goal) {
               return goal.target == astra::UnitKind::gateway;
           }),
           "opening does not spend on a Gateway before its supply milestone");

    state.self.supplyUsed = 20;
    const auto gatewayTiming = strategy.plan(state, {});
    expect(std::ranges::any_of(gatewayTiming.goals, [](const astra::ProductionGoal& goal) {
               return goal.target == astra::UnitKind::gateway && goal.blocking;
           }),
           "ten-supply Gateway becomes a mandatory opening milestone");

    state.self.supplyUsed = 26;
    const auto coreTiming = strategy.plan(state, {});
    expect(std::ranges::any_of(coreTiming.goals, [](const astra::ProductionGoal& goal) {
               return goal.target == astra::UnitKind::cyberneticsCore && goal.blocking;
           }),
           "thirteen-supply Core becomes a mandatory opening milestone");
    expect(coreTiming.desiredGasWorkers == 3,
           "gas mining starts before gas-dependent Dragoon technology");

    state.enemy.race = astra::Race::zerg;
    state.self.supplyUsed = 24;
    const auto safePvZ = strategy.plan(state, {});
    expect(std::ranges::none_of(safePvZ.goals, [](const astra::ProductionGoal& goal) {
               return goal.target == astra::UnitKind::photonCannon;
           }),
           "peaceful one-base PvZ does not sink its opening into arbitrary Cannons");
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
    expect(poor.freeMinerals() == 100, "blocking reservation preserves current bank");

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

    auto wounded = enemy;
    wounded.id = 21;
    wounded.hitPoints = 10;
    const std::vector<astra::UnitSnapshot> targetChoices{wounded, enemy};
    const astra::TargetAllocation lethalVolley[]{
        {wounded.id, 20},
    };
    expect(evaluator.selectTarget(friendly.front(), targetChoices, lethalVolley)->id == enemy.id,
           "focus fire redirects once a target has lethal committed damage");

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

    auto templar = unit(60, astra::UnitKind::highTemplar, true, {600, 500});
    templar.role = astra::UnitRole::spellcaster;
    const std::vector<astra::UnitSnapshot> casters{templar};
    const auto casterOrders = tactics.control(
        casters, meleeForce, estimate, {900, 900}, {100, 100}, emptyInfluence, {400, 400});
    expect(casterOrders.size() == 1 && casterOrders.front().source == "spellcaster-screen",
           "high-value spellcasters stay behind the formation screen");
}

void testCommandArbitration() {
    astra::CommandBus bus;
    bus.beginFrame(100, 2);
    bus.submit({7, astra::CommandType::move, -1, {400, 400},
                astra::UnitKind::unknown, 20, 0, "patrol"});
    bus.submit({7, astra::CommandType::attackUnit, 9, {-1, -1},
                astra::UnitKind::unknown, 80, 0, "combat"});
    auto selected = bus.finalize();
    expect(selected.size() == 1 && selected.front().type == astra::CommandType::attackUnit,
           "higher-priority command wins per-unit arbitration");
    bus.markIssued(selected.front());

    bus.beginFrame(101, 2);
    bus.submit(selected.front());
    expect(bus.finalize().empty(), "latency-window duplicate is suppressed");

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

    const astra::UnitId reservedProbe[]{probe.id};
    const auto leased = workers.assign(state, plan, influence, reservedProbe);
    expect(leased.size() == 1 && leased.front().job == astra::WorkerJob::build,
           "leased scout or builder probe cannot be reclaimed by mining");

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
    const auto orders = scouting.assign(scoutState, scouts, influence);
    expect(orders.size() == 1 && orders.front().target == astra::Position{1700, 1700},
           "scout prioritizes stale unexplored start location");
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

    auto observer = unit(100, astra::UnitKind::observer, true, {200, 200});
    observer.flying = true;
    observer.role = astra::UnitRole::detector;
    state.self.units.push_back(observer);
    astra::InfluenceMap influence;
    influence.update(state);
    const auto escorts = planner.detectorEscorts(state, squads, influence);
    expect(!escorts.empty() && escorts.front().actor == observer.id,
           "observer is assigned to highest-priority detection squad");

    squads = planner.form(state, friendly, {}, plan, {256, 256});
    const auto mainGroups = std::ranges::count_if(squads, [](const astra::Squad& squad) {
        return squad.role == astra::SquadRole::mainArmy;
    });
    expect(mainGroups == 2, "disconnected armies receive independent local decisions");
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
