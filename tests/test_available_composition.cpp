#include "protodd/MacroPlanner.hpp"

#include <algorithm>
#include <iostream>

int main() {
    using namespace protodd;
    int errors = 0;
    const auto check = [&](bool value, const char* message) {
        if (!value) { ++errors; std::cerr << message << '\n'; }
    };
    const auto makeUnit = [](int id, UnitKind kind) {
        UnitSnapshot result;
        result.id = id;
        result.kind = kind;
        result.completed = true;
        result.powered = true;
        return result;
    };
    // Archived reference-a/game-2 frame 8160: five Zealots, two idle
    // Gateways, no Core, enough unreserved minerals and supply for a Zealot.
    GameState state;
    state.self.minerals = 112;
    state.self.supplyUsed = 40;
    state.self.supplyTotal = 64;
    state.self.units = {makeUnit(1, UnitKind::gateway), makeUnit(2, UnitKind::gateway)};
    for (int id = 3; id < 8; ++id) state.self.units.push_back(makeUnit(id, UnitKind::zealot));
    StrategicPlan plan;
    plan.composition = {{UnitKind::zealot, .35}, {UnitKind::dragoon, .12},
                        {UnitKind::highTemplar, .25}, {UnitKind::corsair, .18},
                        {UnitKind::archon, .10}};
    const auto run = [](const GameState& snapshot, const StrategicPlan& strategy,
                        ResourceLedger& ledger) {
        MacroPlanner planner;
        return planner.reconcile(snapshot, strategy, ledger);
    };
    const auto trains = [](const auto& actions, UnitKind kind) {
        return std::ranges::count_if(actions, [=](const MacroAction& action) {
            return action.action == MacroActionKind::train && action.target == kind &&
                   action.reserved && action.executable;
        });
    };
    ResourceLedger ledger{112, 0};
    auto actions = run(state, plan, ledger);
    check(trains(actions, UnitKind::zealot) == 1 && ledger.freeMinerals() == 12,
          "absent tech shares must not suppress an affordable surplus defender");

    ledger = {300, 0};
    actions = run(state, plan, ledger);
    check(trains(actions, UnitKind::zealot) == 2,
          "available composition must fill both idle Gateways, not overqueue");
    state.self.queuedUnits = {UnitKind::zealot};
    ledger = {300, 0};
    check(trains(run(state, plan, ledger), UnitKind::zealot) == 1,
          "queued units consume production capacity");
    state.self.queuedUnits.clear();
    state.self.units[0].powered = false;
    state.self.units[1].disabled = true;
    ledger = {300, 0};
    check(run(state, plan, ledger).empty(), "unusable Gateways cannot receive units");
    state.self.units[0].powered = true;
    state.self.units[1].disabled = false;
    state.self.supplyTotal = state.self.supplyUsed;
    ledger = {300, 0};
    check(run(state, plan, ledger).empty(), "available composition cannot exceed supply");
    state.self.supplyTotal = 64;

    // Keep the mix when Dragoon tech is paid for, even before completion.
    auto core = makeUnit(8, UnitKind::cyberneticsCore);
    core.completed = false;
    core.buildProgress = 95;
    state.self.units.push_back(core);
    ledger = {300, 100};
    check(run(state, plan, ledger).empty(),
          "an almost finished Core preserves Dragoon share and an open Gateway");
    state.self.units.back().completed = true;
    ledger = {300, 0};
    check(run(state, plan, ledger).empty(),
          "gas shortage must not remove unlocked Dragoon share");
    ledger = {300, 100};
    actions = run(state, plan, ledger);
    check(trains(actions, UnitKind::dragoon) == 2 && trains(actions, UnitKind::zealot) == 0,
          "completed Core with gas reinforces the missing Dragoon share");
    state.self.units.pop_back();

    plan.goals = {{GoalKind::build, UnitKind::cyberneticsCore, 1, 82, true, "tech"}};
    ledger = {200, 0};
    actions = run(state, plan, ledger);
    check(trains(actions, UnitKind::zealot) == 0 && ledger.reservedMinerals == 200,
          "composition must preserve the Core reservation");
    state.self.units.push_back(makeUnit(9, UnitKind::nexus));
    plan.goals.push_back({GoalKind::train, UnitKind::probe, 1, 74, false, "worker"});
    ledger = {350, 0};
    actions = run(state, plan, ledger);
    check(trains(actions, UnitKind::probe) == 1 && trains(actions, UnitKind::zealot) == 1 &&
              ledger.freeMinerals() == 0,
          "surplus reinforcement follows both tech and worker spending");
    plan.goals.clear();
    plan.composition = {{UnitKind::highTemplar, 1.0}};
    ledger = {500, 500};
    check(run(state, plan, ledger).empty(), "a wholly unavailable mix creates no orders");
    if (!errors) std::cout << "available composition scenarios passed\n";
    return errors ? 1 : 0;
}
