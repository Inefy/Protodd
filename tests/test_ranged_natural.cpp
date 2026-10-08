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
    const auto unit = [](int id, UnitKind kind, Position position = Position{320, 320}) {
        UnitSnapshot u;
        u.id = id; u.kind = kind; u.position = position; u.completed = true;
        u.hitPoints = u.maxHitPoints = 100;
        u.role = kind == UnitKind::probe ? UnitRole::worker :
            (kind == UnitKind::nexus ? UnitRole::resourceDepot : UnitRole::groundArmy);
        if (kind == UnitKind::dragoon) u.groundWeapon = {20, 30, 0, 192};
        u.lastSeen = 9000;
        return u;
    };
    GameState baseline;
    baseline.frame = 9000; baseline.self.id = 1; baseline.enemy.id = 2;
    baseline.self.race = baseline.enemy.race = Race::protoss;
    baseline.self.supplyUsed = 90; baseline.self.supplyTotal = 146;
    for (auto kind : {UnitKind::nexus, UnitKind::pylon, UnitKind::gateway,
                      UnitKind::gateway, UnitKind::cyberneticsCore, UnitKind::assimilator,
                      UnitKind::roboticsFacility, UnitKind::observatory, UnitKind::observer})
        baseline.self.units.push_back(unit(static_cast<int>(baseline.self.units.size()), kind));
    for (int i = 0; i < 22; ++i) baseline.self.units.push_back(unit(100 + i, UnitKind::probe));
    for (int i = 0; i < 6; ++i) baseline.self.units.push_back(unit(200 + i, UnitKind::dragoon));
    BaseSnapshot main;
    main.id = 0; main.center = {320, 320}; main.ownerId = 1; main.mineralsRemaining = 9000;
    BaseSnapshot natural;
    natural.id = 1; natural.center = {960, 320}; natural.ownerId = -1;
    natural.mineralsRemaining = 12000; natural.geysers = 1;
    baseline.bases = {main, natural};
    baseline.enemy.units = {unit(300, UnitKind::cyberneticsCore, {3000, 3000}),
                            unit(301, UnitKind::nexus, {3000, 3000})};
    ThreatAssessment pressure;
    pressure.uncertainty = 0.95; pressure.mostLikely = EnemyPlan::heavyPressure;
    StrategyEngine strategy{false, false, false, false, false, false, false,
                            false, false, false, true};
    const auto covered = [&](const GameState& state, const ThreatAssessment& threat) {
        const auto plan = strategy.plan(state, threat);
        return plan.name.find("covered ranged natural") != std::string::npos;
    };
    auto state = baseline;
    check(StrategyEngine{}.plan(state, pressure).name.find("covered ranged natural") == std::string::npos,
          "an unvalidated economic experiment was enabled in the default strategy");
    auto plan = strategy.plan(state, pressure);
    check(covered(state, pressure) && plan.desiredBases == 2 && plan.sustainEconomy &&
          plan.expansionTarget == natural.center && plan.rallyPoint == natural.center,
          "a stable ranged screen still waited for a completed Reaver before its natural");
    check(plan.posture == Posture::hold && !plan.prioritizeReinforcements,
          "the expansion window sent the covering army on an attack or starved its bank");
    for (auto kind : {UnitKind::forge, UnitKind::photonCannon, UnitKind::shieldBattery})
        check(std::ranges::none_of(plan.goals, [kind](const ProductionGoal& demand) {
            return demand.target == kind && demand.desiredCount > 0;
        }), "optional static defense survived the covered economic transition");
    const auto paid = [&](int minerals, UnitKind kind) {
        ResourceLedger ledger{minerals, 300};
        MacroPlanner planner;
        const auto actions = planner.reconcile(state, plan, ledger);
        return std::ranges::any_of(actions, [kind](const MacroAction& action) {
            return action.target == kind && action.executable && action.reserved;
        });
    };
    check(!paid(390, UnitKind::dragoon) && !paid(390, UnitKind::probe),
          "optional production spent the covered natural's bank");
    check(paid(400, UnitKind::nexus), "covered natural never became an executable purchase");

    for (int i = 0; i < 3; ++i) {
        auto enemy = unit(310 + i, UnitKind::dragoon, {1120, 320 + i * 32});
        enemy.visible = true;
        state.enemy.units.push_back(enemy);
    }
    pressure.combatEnemiesNearMain = 3; pressure.immediateGround = 0.8;
    pressure.approachingArmyValue = 5.0;
    check(covered(state, pressure), "a covered perimeter wave was treated as an economic all-in");
    auto marginalPressure = state;
    for (int i = 0; i < 2; ++i) {
        auto enemy = unit(315 + i, UnitKind::dragoon, {1120, 500 + i * 32});
        enemy.visible = true;
        marginalPressure.enemy.units.push_back(enemy);
    }
    plan = strategy.plan(marginalPressure, pressure);
    check(plan.desiredBases == 2 && plan.expansionTarget == natural.center &&
          plan.rallyPoint == natural.center,
          "a small increase in observed perimeter pressure reversed a covered natural commitment");
    auto unsafe = state;
    for (int i = 0; i < 4; ++i) {
        auto enemy = unit(320 + i, UnitKind::dragoon, {1120, 400 + i * 32});
        enemy.visible = true;
        unsafe.enemy.units.push_back(enemy);
    }
    check(!covered(unsafe, pressure), "an overwhelming crossing allowed protected Nexus spending");
    unsafe = state;
    unsafe.enemy.units.back().position = main.center;
    check(!covered(unsafe, pressure), "a main breach kept the covered economic window open");
    unsafe = state; unsafe.self.units[9].underAttack = true;
    check(!covered(unsafe, pressure), "workers under attack retained the expansion bank");
    unsafe = state;
    auto reaver = unit(330, UnitKind::reaver, {1120, 320}); reaver.visible = true;
    unsafe.enemy.units.push_back(reaver);
    check(!covered(unsafe, pressure), "a splash-supported crossing bypassed our splash checkpoint");
    unsafe = state;
    auto dt = unit(331, UnitKind::darkTemplar, {1120, 320});
    dt.visible = true; dt.detected = false; dt.cloaked = true;
    unsafe.enemy.units.push_back(dt);
    check(!covered(unsafe, pressure), "an Observer elsewhere excused undetected local DT pressure");
    unsafe = baseline;
    std::erase_if(unsafe.self.units, [](const UnitSnapshot& u) { return u.kind == UnitKind::observer; });
    auto cloakRisk = pressure; cloakRisk.cloak = 0.5;
    check(!covered(unsafe, cloakRisk), "missing cloak coverage allowed expansion spending");
    unsafe = baseline; unsafe.self.units.back().hitPoints = 30;
    check(!covered(unsafe, {}), "a wounded sixth Dragoon counted as a healthy expansion screen");
    unsafe = baseline; unsafe.self.units.back().loaded = true;
    check(!covered(unsafe, {}), "a loaded sixth Dragoon counted as home coverage");
    unsafe = baseline; unsafe.enemy.race = Race::terran;
    check(!covered(unsafe, {}), "the ranged mirror rule changed another matchup");
    unsafe = baseline; unsafe.enemy.units.clear();
    check(!covered(unsafe, {}), "unscouted melee tech was classified as ordinary ranged pressure");
    unsafe = baseline;
    unsafe.self.units.push_back(unit(400, UnitKind::photonCannon));
    unsafe.self.units.push_back(unit(401, UnitKind::roboticsSupportBay));
    plan = strategy.plan(unsafe, {});
    check(std::ranges::any_of(plan.goals, [](const ProductionGoal& demand) {
        return demand.target == UnitKind::photonCannon && demand.desiredCount == 1;
    }), "the economic transition forgot already paid-for static defense");
    check(std::ranges::any_of(plan.goals, [](const ProductionGoal& demand) {
        return demand.target == UnitKind::reaver && demand.desiredCount >= 1 && demand.priority < 120;
    }), "splash tech was deleted instead of overlapping the natural at lower priority");
    return errors ? 1 : 0;
}
