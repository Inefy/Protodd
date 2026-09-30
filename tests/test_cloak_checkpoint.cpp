#include "protodd/MacroPlanner.hpp"
#include "protodd/Strategy.hpp"

#include <algorithm>
#include <iostream>

int main() {
    using namespace protodd;
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { ++failures; std::cerr << message << '\n'; }
    };
    const auto unit = [](int id, UnitKind kind, bool completed = true) {
        UnitSnapshot result;
        result.id = id; result.kind = kind; result.completed = completed; result.visible = true;
        result.position = {2112, 3824};
        return result;
    };
    const auto funded = [](const auto& actions, UnitKind kind) {
        return std::ranges::any_of(actions, [kind](const MacroAction& action) {
            return action.target == kind && action.reserved && action.executable;
        });
    };
    GameState state; state.frame = 5100;
    state.self.id = 1; state.enemy.id = 2;
    state.self.race = state.enemy.race = Race::protoss;
    state.self.supplyUsed = 44; state.self.supplyTotal = 66;
    state.self.units = {unit(1, UnitKind::nexus), unit(2, UnitKind::pylon),
        unit(3, UnitKind::gateway), unit(4, UnitKind::assimilator),
        unit(5, UnitKind::cyberneticsCore), unit(6, UnitKind::zealot),
        unit(7, UnitKind::dragoon, false)};
    for (int i = 0; i < 18; ++i) state.self.units.push_back(unit(20 + i, UnitKind::probe));
    auto gate = unit(100, UnitKind::gateway), core = unit(101, UnitKind::cyberneticsCore, false);
    gate.position = core.position = {1056, 272};
    gate.lastSeen = core.lastSeen = 3000;
    state.enemy.units = {gate, core};
    ThreatAssessment threat; threat.uncertainty = 0.97;
    const auto plan = StrategyEngine{}.plan(state, threat);
    check(plan.requireMobileDetection, "scouted one-Gateway tech with a paid defender missed DT insurance");
    check(std::ranges::any_of(plan.goals, [](const ProductionGoal& goal) {
        return goal.target == UnitKind::observer && goal.desiredCount == 1 && goal.priority >= 124;
    }), "quiet one-Gateway checkpoint ordered more than the single insurance detector");
    ResourceLedger ledger{200, 200};
    auto actions = MacroPlanner{}.reconcile(state, plan, ledger);
    check(funded(actions, UnitKind::roboticsFacility) && !funded(actions, UnitKind::gateway),
          "optional throughput consumed the insurance Robotics bank");
    state.self.units.push_back(unit(8, UnitKind::roboticsFacility));
    ledger = {100, 100};
    actions = MacroPlanner{}.reconcile(state, StrategyEngine{}.plan(state, threat), ledger);
    check(funded(actions, UnitKind::observatory), "insurance failed to pay for the Observatory");
    state.self.units.push_back(unit(9, UnitKind::observatory));
    ledger = {125, 75};
    actions = MacroPlanner{}.reconcile(state, StrategyEngine{}.plan(state, threat), ledger);
    check(funded(actions, UnitKind::observer) && !funded(actions, UnitKind::dragoon),
          "first insurance Observer lost its gas to a Dragoon cycle");

    auto control = state;
    control.frame = 5039;
    check(!StrategyEngine{}.plan(control, threat).requireMobileDetection,
          "one-Gateway tech insurance preempted the initial opening window");
    control = state; control.enemy.units.pop_back();
    check(!StrategyEngine{}.plan(control, threat).requireMobileDetection,
          "one Gateway without tech became proof of a DT opening");
    control = state; control.enemy.units.push_back(unit(102, UnitKind::zealot));
    check(!StrategyEngine{}.plan(control, threat).requireMobileDetection,
          "visible melee production did not retain the initial screen budget");
    control = state; control.enemy.units.push_back(unit(102, UnitKind::dragoon));
    check(!StrategyEngine{}.plan(control, threat).requireMobileDetection,
          "visible ranged production did not retain the ranged opening budget");
    auto pressure = threat; pressure.combatEnemiesNearMain = 2; pressure.immediateGround = 0.8;
    check(!StrategyEngine{}.plan(state, pressure).requireMobileDetection,
          "soft insurance displaced direct emergency defense");

    state.frame = 3600;
    std::erase_if(state.self.units, [](const UnitSnapshot& own) {
        return own.kind == UnitKind::cyberneticsCore || own.kind == UnitKind::dragoon ||
               own.kind == UnitKind::roboticsFacility || own.kind == UnitKind::observatory;
    });
    ledger = {200, 100};
    actions = MacroPlanner{}.reconcile(state, StrategyEngine{}.plan(state, threat), ledger);
    check(funded(actions, UnitKind::cyberneticsCore) && !funded(actions, UnitKind::gateway),
          "scouted tech Core lost its spend window to another Gateway");
    ledger = {190, 100};
    actions = MacroPlanner{}.reconcile(state, StrategyEngine{}.plan(state, threat), ledger);
    check(!funded(actions, UnitKind::probe) && !funded(actions, UnitKind::zealot),
          "routine cycles repeatedly drained the urgent Core's last mineral deposit");
    return failures ? 1 : 0;
}
