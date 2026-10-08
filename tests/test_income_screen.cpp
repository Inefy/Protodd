#include "protodd/MacroPlanner.hpp"
#include "protodd/Strategy.hpp"
#include "protodd/UnitCatalog.hpp"
#include <algorithm>
#include <iostream>

int main() {
    using namespace protodd;
    int errors = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { ++errors; std::cerr << message << '\n'; }
    };
    const auto unit = [](int id, UnitKind kind) {
        UnitSnapshot u;
        u.id = id; u.kind = kind; u.completed = true; u.position = {320, 320};
        u.role = kind == UnitKind::probe ? UnitRole::worker :
            (kind == UnitKind::nexus ? UnitRole::resourceDepot : UnitRole::groundArmy);
        return u;
    };
    GameState state;
    state.frame = 3600; state.self.id = 1; state.enemy.id = 2;
    state.self.supplyUsed = 28; state.self.supplyTotal = 50;
    state.self.units = {unit(1, UnitKind::nexus), unit(2, UnitKind::pylon),
                        unit(3, UnitKind::gateway)};
    StrategicPlan opening;
    opening.goals = {{GoalKind::build, UnitKind::gateway, 2, 115, true, "second gate"},
                     {GoalKind::build, UnitKind::cyberneticsCore, 1, 118, true, "tech"}};
    const auto run = [&](const StrategicPlan& plan, int minerals) {
        ResourceLedger ledger{minerals, 0};
        MacroPlanner planner;
        return planner.reconcile(state, plan, ledger);
    };
    const auto paid = [](const auto& actions, UnitKind kind) {
        return std::ranges::any_of(actions, [=](const MacroAction& action) {
            return action.target == kind && action.reserved && action.executable;
        });
    };
    for (auto race : {Race::protoss, Race::terran}) {
        state.enemy.race = race;
        auto actions = run(opening, 244);
        check(paid(actions, UnitKind::zealot), "first Gateway budget lost to opening infrastructure");
        check(!paid(actions, UnitKind::cyberneticsCore), "Core spent the first defender's bank");
        state.self.units.push_back(unit(4, UnitKind::zealot));
        check(paid(run(opening, 100), UnitKind::zealot), "second screen cycle was not protected");
        state.self.units.pop_back();
    }
    state.self.queuedUnits = {UnitKind::zealot};
    check(!paid(run(opening, 244), UnitKind::zealot), "busy Gateway received a duplicate cycle");
    state.self.queuedUnits.clear();
    state.self.units[2].powered = false;
    check(!paid(run(opening, 244), UnitKind::zealot), "unpowered Gateway reserved screen minerals");
    state.self.units[2].powered = true;
    auto detection = opening;
    detection.requireMobileDetection = true;
    detection.goals = {{GoalKind::build, UnitKind::cyberneticsCore, 1, 124, true,
                       "unlock urgent mobile detection"}};
    check(paid(run(detection, 200), UnitKind::cyberneticsCore) &&
          !paid(run(detection, 200), UnitKind::zealot),
          "a melee bodyguard consumed the urgent cloak-detection bank");
    state.enemy.race = Race::zerg;
    check(!paid(run(opening, 244), UnitKind::zealot), "opening guarantee displaced the PvZ static opening");

    state.self.race = Race::protoss;
    state.self.units.push_back(unit(4, UnitKind::zealot));
    state.self.units.push_back(unit(5, UnitKind::zealot));
    state.self.supplyUsed = 36;
    state.frame = 4200;
    auto depot = unit(90, UnitKind::commandCenter); depot.position = {2800, 2800};
    depot.role = UnitRole::resourceDepot; depot.visible = true;
    auto production = unit(91, UnitKind::barracks); production.position = {2700, 2800};
    production.visible = true; production.lastSeen = state.frame;
    auto second = production; second.id = 92;
    state.enemy.race = Race::terran;
    state.enemy.units = {depot, production, second};
    auto warning = StrategyEngine{}.plan(state, {});
    check(paid(run(warning, 150), UnitKind::forge),
          "scouted one-base double Barracks waited for Marines at home before fortifying");
    auto factory = unit(93, UnitKind::factory);
    factory.visible = true; factory.lastSeen = state.frame;
    state.enemy.units.push_back(factory);
    warning = StrategyEngine{}.plan(state, {});
    check(std::ranges::none_of(warning.goals, [](const ProductionGoal& goal) {
        return goal.target == UnitKind::forge && goal.priority >= 110;
    }), "Factory tech was misclassified as an all-in bio opening");
    state.enemy.units = {depot, production};
    auto earlyMarine = unit(95, UnitKind::marine);
    earlyMarine.position = depot.position;
    earlyMarine.visible = true; earlyMarine.lastSeen = state.frame;
    earlyMarine.firstSeen = 2220;
    state.enemy.units.push_back(earlyMarine);
    warning = StrategyEngine{}.plan(state, {});
    check(paid(run(warning, 150), UnitKind::forge),
          "early Marine timing required scouting a second Barracks before fortifying");
    state.enemy.units.back().firstSeen = 4400;
    warning = StrategyEngine{}.plan(state, {});
    check(std::ranges::none_of(warning.goals, [](const ProductionGoal& goal) {
        return goal.target == UnitKind::forge && goal.priority >= 110;
    }), "ordinary later Marine timing was treated as an early bio rush");
    state.enemy.units.back().firstSeen = 2220;
    state.enemy.units.push_back(factory);
    warning = StrategyEngine{}.plan(state, {});
    check(std::ranges::none_of(warning.goals, [](const ProductionGoal& goal) {
        return goal.target == UnitKind::forge && goal.priority >= 110;
    }), "early Marine timing overrode observed Factory tech");
    state.enemy.race = Race::protoss;
    production.kind = second.kind = UnitKind::gateway;
    state.enemy.units = {production, second};
    warning = StrategyEngine{}.plan(state, {});
    check(paid(run(warning, 150), UnitKind::forge),
          "two escorts still waited for four before funding the melee anchor");
    const auto completedMirror = state;
    state.frame = 3000;
    state.self.units.pop_back(); state.self.units.back().completed = false;
    for (auto& enemy : state.enemy.units) enemy.completed = false;
    warning = StrategyEngine{}.plan(state, {});
    check(paid(run(warning, 150), UnitKind::forge),
          "remembered unfinished second Gateway hid the melee flood from the default planner");
    auto enemyCore = unit(94, UnitKind::cyberneticsCore); enemyCore.completed = false;
    enemyCore.lastSeen = state.frame; enemyCore.visible = true;
    state.enemy.units.push_back(enemyCore);
    warning = StrategyEngine{}.plan(state, {});
    check(warning.name.find("ranged economy") != std::string::npos,
          "an observed unfinished Core was ignored when classifying construction");
    state = completedMirror;
    state.self.units.pop_back();
    state.self.units.back().completed = false;
    warning = StrategyEngine{}.plan(state, {});
    check(paid(run(warning, 150), UnitKind::forge),
          "the paid first escort waited for completion before starting the static chain");
    state.self.units.back().completed = true;
    state.self.units.push_back(unit(5, UnitKind::zealot));
    state.self.units.pop_back(); state.self.units.pop_back();
    state.enemy.units.clear();

    state.frame = 18000; state.enemy.race = Race::terran;
    state.self.minerals = 390; state.self.supplyUsed = 80; state.self.supplyTotal = 140;
    for (auto kind : {UnitKind::cyberneticsCore, UnitKind::assimilator,
                      UnitKind::roboticsFacility, UnitKind::observatory, UnitKind::observer})
        state.self.units.push_back(unit(200 + static_cast<int>(kind), kind));
    state.self.units.push_back(unit(299, UnitKind::observer));
    for (int i = 0; i < 16; ++i) state.self.units.push_back(unit(10 + i, UnitKind::probe));
    for (int i = 0; i < 8; ++i) state.self.units.push_back(unit(30 + i, UnitKind::zealot));
    BaseSnapshot main; main.id = 0; main.ownerId = 1; main.center = {320, 320};
    main.mineralLine = {360, 320}; main.mineralPatches = 8;
    main.mineralsRemaining = 3500;
    BaseSnapshot natural; natural.id = 1; natural.ownerId = -1;
    natural.center = {960, 320}; natural.mineralLine = {1000, 320};
    natural.mineralsRemaining = 12000; natural.mineralPatches = 8;
    natural.geysers = 1; natural.groundDistanceFromMain = 640;
    state.bases = {main, natural};
    StrategyEngine strategy;
    ThreatAssessment threat;
    auto plan = strategy.plan(state, threat);
    check(plan.desiredBases == 2 && plan.expansionTarget == natural.center && plan.sustainEconomy,
          "one-base pre-depletion recovery was erased by a matchup cap");
    auto actions = run(plan, 390);
    check(!paid(actions, UnitKind::zealot) && !paid(actions, UnitKind::probe),
          "routine production consumed the replacement Nexus bank");
    check(paid(run(plan, 400), UnitKind::nexus), "replacement mining never reached the spend threshold");
    check(plan.estimatedMiningRunwayFrames > 0 &&
          plan.estimatedMiningRunwayFrames <= 10 * 60 * 24,
          "mining runway was not estimated from the owned mineral bank and worker count");

    auto simultaneous = state;
    simultaneous.bases[0].mineralsRemaining = 2200;
    simultaneous.bases[1].ownerId = state.self.id;
    simultaneous.bases[1].mineralsRemaining = 2600;
    auto third = natural;
    third.id = 2; third.ownerId = -1; third.center = {1600, 320};
    third.mineralLine = {1640, 320}; third.mineralsRemaining = 14000;
    third.groundDistanceFromMain = 1280;
    simultaneous.bases.push_back(third);
    auto secondNexus = unit(480, UnitKind::nexus);
    secondNexus.position = simultaneous.bases[1].center;
    simultaneous.self.units.push_back(secondNexus);
    const auto multiBasePlan = strategy.plan(simultaneous, threat);
    check(multiBasePlan.estimatedMiningRunwayFrames > 0 &&
          multiBasePlan.estimatedMiningRunwayFrames <= 10 * 60 * 24 &&
          multiBasePlan.desiredBases >= 3 &&
          multiBasePlan.expansionTarget == third.center &&
          std::ranges::any_of(multiBasePlan.goals, [](const ProductionGoal& goal) {
              return goal.goal == GoalKind::expand && goal.target == UnitKind::nexus &&
                     goal.priority == 114 && goal.blocking;
          }), "simultaneous depleted bases did not start a ready third-base replacement");

    auto noNextBase = state;
    noNextBase.bases.resize(1);
    noNextBase.bases[0].mineralsRemaining = 2600;
    const auto survivalPlan = strategy.plan(noNextBase, threat);
    check(survivalPlan.posture == Posture::recover && survivalPlan.deferExpansion &&
          !survivalPlan.expansionTarget.valid() && survivalPlan.desiredBases == 1 &&
          std::ranges::any_of(survivalPlan.goals, [](const ProductionGoal& goal) {
              return goal.target == UnitKind::probe && goal.priority == 118 && goal.blocking;
          }), "no ready next base did not produce an explicit worker-preservation plan");

    auto closing = state;
    closing.self.supplyTotal = 400; closing.self.supplyUsed = 392;
    const auto closeout = strategy.plan(closing, threat);
    check(closeout.posture == Posture::attack && !closeout.expansionTarget.valid() &&
          closeout.desiredBases == 1,
          "pre-depletion growth recalled a maxed army from its closeout");
    state.self.units.back().underAttack = true; // Army damage alone is not a worker breach.
    state.self.units[9].underAttack = true;
    plan = strategy.plan(state, threat);
    check(!plan.sustainEconomy, "a worker breach retained protected economic spending");
    state.self.units[9].underAttack = false;
    auto marine = unit(100, UnitKind::marine); marine.visible = true;
    marine.groundWeapon.damage = 6; state.enemy.units.push_back(marine);
    threat.combatEnemiesNearMain = 1; threat.immediateGround = 0.8;
    plan = strategy.plan(state, threat);
    check(!plan.sustainEconomy && plan.desiredBases <= 1,
          "a real main breach failed to cancel expansion funding");

    GameState unpoweredEconomy;
    unpoweredEconomy.frame = 8 * 60 * 24;
    unpoweredEconomy.self.id = 1;
    unpoweredEconomy.self.race = Race::protoss;
    unpoweredEconomy.self.minerals = 900;
    unpoweredEconomy.self.supplyUsed = 60;
    unpoweredEconomy.self.supplyTotal = 100;
    unpoweredEconomy.enemy.id = 2;
    unpoweredEconomy.enemy.race = Race::terran;
    auto unpoweredGateway = unit(3, UnitKind::gateway);
    unpoweredGateway.powered = false;
    auto remotePylon = unit(2, UnitKind::pylon);
    remotePylon.position = {600, 320};
    unpoweredEconomy.self.units = {
        unit(1, UnitKind::nexus), remotePylon, unpoweredGateway};
    for (int id = 10; id < 32; ++id)
        unpoweredEconomy.self.units.push_back(unit(id, UnitKind::probe));
    BaseSnapshot incomeBase;
    incomeBase.id = 1;
    incomeBase.center = {320, 320};
    incomeBase.ownerId = unpoweredEconomy.self.id;
    incomeBase.mineralsRemaining = 8'000;
    incomeBase.mineralPatches = 8;
    incomeBase.geysers = 1;
    unpoweredEconomy.bases.push_back(incomeBase);
    ThreatAssessment calmEconomy;
    calmEconomy.uncertainty = 0.2;
    const auto recoveryPlan = StrategyEngine{}.plan(unpoweredEconomy, calmEconomy);
    check(std::ranges::any_of(recoveryPlan.goals, [](const ProductionGoal& goal) {
              return goal.target == UnitKind::pylon &&
                     goal.reason == "restore power to disabled production";
          }), "a high-bank unpowered producer opens its local Pylon recovery task");
    check(std::ranges::none_of(recoveryPlan.goals, [](const ProductionGoal& goal) {
              return goal.target == UnitKind::gateway &&
                     goal.goal == GoalKind::build && goal.desiredCount > 1;
          }), "an unusable existing Gateway does not trigger another Gateway investment");
    ResourceLedger recoveryBank{900, 0};
    const auto recoveryActions = MacroPlanner{}.reconcile(
        unpoweredEconomy, recoveryPlan, recoveryBank);
    for (const auto& action : recoveryActions)
        std::cerr << "T030 target=" << static_cast<int>(action.target)
                  << " reserved=" << action.reserved << " priority=" << action.priority
                  << " reason=" << action.reason << '\n';
    check(std::ranges::any_of(recoveryActions, [](const MacroAction& action) {
              return action.action == MacroActionKind::build &&
                     action.target == UnitKind::pylon && action.reserved &&
                     action.reason == "restore power to disabled production";
          }) &&
          std::ranges::none_of(recoveryActions, [](const MacroAction& action) {
              return action.action == MacroActionKind::build &&
                     action.target == UnitKind::gateway && action.reserved &&
                     action.reason != "restore power to disabled production";
          }), "the high bank funds local power recovery before another Gateway");

    GameState capacityEconomy;
    capacityEconomy.frame = 4 * 60 * 24;
    capacityEconomy.self.id = 1;
    capacityEconomy.self.race = Race::protoss;
    capacityEconomy.self.minerals = 700;
    capacityEconomy.self.supplyUsed = 24;
    capacityEconomy.self.supplyTotal = 34;
    capacityEconomy.enemy.id = 2;
    capacityEconomy.enemy.race = Race::terran;
    capacityEconomy.self.units = {
        unit(30, UnitKind::nexus), unit(31, UnitKind::pylon),
        unit(32, UnitKind::gateway)};
    capacityEconomy.bases.push_back(
        {1, {128, 128}, {160, 128}, 8000, 5000, capacityEconomy.self.id,
         capacityEconomy.frame, true, false, 8, 1});
    capacityEconomy.estimatedMineralIncomePerMinute = 2000;
    capacityEconomy.estimatedGasIncomePerMinute = 2000;
    capacityEconomy.self.producerSlots = {
        {32, UnitKind::gateway, false, 0, 0, 0, false, false, false}};
    const auto idleCapacityPlan = StrategyEngine{}.plan(capacityEconomy, {});
    const auto hasExtraGateway = [](const StrategicPlan& plan) {
        return std::ranges::any_of(plan.goals, [](const ProductionGoal& goal) {
            return goal.goal == GoalKind::build && goal.target == UnitKind::gateway &&
                   goal.desiredCount > 1 && !goal.blocking;
        });
    };
    check(!hasExtraGateway(idleCapacityPlan),
          "an observed idle Gateway is filled before adding production capacity");

    capacityEconomy.self.producerSlots[0].activeTraining = true;
    const auto busyCapacityPlan = StrategyEngine{}.plan(capacityEconomy, {});
    check(hasExtraGateway(busyCapacityPlan),
          "busy capacity with an army readiness gap can expand when income sustains it");

    capacityEconomy.estimatedGasIncomePerMinute = 50;
    const auto gasLimitedPlan = StrategyEngine{}.plan(capacityEconomy, {});
    check(!hasExtraGateway(gasLimitedPlan),
          "a large bank cannot add a Gateway beyond measured sustainable gas income");

    capacityEconomy.estimatedGasIncomePerMinute = 2000;
    capacityEconomy.estimatedMineralIncomePerMinute = 50;
    const auto mineralLimitedPlan = StrategyEngine{}.plan(capacityEconomy, {});
    check(!hasExtraGateway(mineralLimitedPlan),
          "a large bank cannot add a Gateway beyond measured sustainable mineral income");

    capacityEconomy.estimatedMineralIncomePerMinute = 2000;
    capacityEconomy.self.supplyUsed = 100;
    capacityEconomy.self.supplyTotal = 120;
    for (int id = 40; id < 64; ++id)
        capacityEconomy.self.units.push_back(unit(id, UnitKind::zealot));
    const auto readyArmyPlan = StrategyEngine{}.plan(capacityEconomy, {});
    check(!hasExtraGateway(readyArmyPlan),
          "a ready army does not trigger a bank-only Gateway beyond its throughput target");
    return errors ? 1 : 0;
}
