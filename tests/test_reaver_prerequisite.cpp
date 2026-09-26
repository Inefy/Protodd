#include "protodd/MacroPlanner.hpp"

#include <algorithm>
#include <iostream>

int main() {
    using namespace protodd;
    int errors = 0;
    const auto check = [&](bool value, const char* message) {
        if (!value) { ++errors; std::cerr << message << '\n'; }
    };
    const auto building = [](int id, UnitKind kind) {
        UnitSnapshot unit;
        unit.id = id;
        unit.kind = kind;
        unit.completed = true;
        unit.powered = true;
        return unit;
    };
    GameState state;
    state.self.minerals = 200;
    state.self.gas = 100;
    state.self.supplyTotal = 40;
    state.self.units = {building(1, UnitKind::roboticsFacility)};
    StrategicPlan plan;
    // This goal must fund its actual missing prerequisite, not reserve all
    // the money for a Reaver that BWAPI will refuse to train.
    plan.goals = {{GoalKind::train, UnitKind::reaver, 1, 122, true, "splash screen"},
                  {GoalKind::build, UnitKind::roboticsSupportBay, 1, 102, true, "splash tech"}};
    const auto run = [](const GameState& snapshot, const StrategicPlan& strategy,
                        ResourceLedger& ledger) {
        return MacroPlanner{}.reconcile(snapshot, strategy, ledger);
    };
    ResourceLedger ledger{200, 100};
    auto actions = run(state, plan, ledger);
    check(actions.size() == 1 && actions[0].target == UnitKind::roboticsSupportBay &&
              actions[0].action == MacroActionKind::build && actions[0].reserved &&
              ledger.freeMinerals() == 50 && ledger.freeGas() == 0,
          "Reaver demand must unlock its Support Bay before consuming the bank");

    plan.goals.clear();
    plan.composition = {{UnitKind::reaver, 1.0}};
    ledger = {200, 100};
    check(run(state, plan, ledger).empty() && ledger.freeMinerals() == 200,
          "composition cannot issue or reserve an impossible Reaver");

    auto bay = building(2, UnitKind::roboticsSupportBay);
    bay.completed = false;
    bay.buildProgress = 0;
    state.self.units.push_back(bay);
    plan.composition.clear();
    plan.goals = {{GoalKind::train, UnitKind::reaver, 1, 122, true, "splash screen"}};
    ledger = {200, 100};
    check(run(state, plan, ledger).empty() && ledger.freeMinerals() == 200,
          "an early Support Bay warp-in leaves money free for current production");
    state.self.units.back().buildProgress = 90;
    ledger = {200, 100};
    actions = run(state, plan, ledger);
    check(actions.size() == 1 && actions[0].target == UnitKind::reaver &&
              actions[0].reserved && !actions[0].executable,
          "near completion can reserve a Reaver but cannot issue it early");
    state.self.units.back().completed = true;
    ledger = {200, 100};
    actions = run(state, plan, ledger);
    check(actions.size() == 1 && actions[0].target == UnitKind::reaver &&
              actions[0].reserved && actions[0].executable,
          "completed Support Bay permits the funded Reaver");

    state.self.units.erase(state.self.units.begin());
    ledger = {200, 200};
    actions = run(state, plan, ledger);
    check(std::ranges::none_of(actions, [](const MacroAction& action) {
              return action.action == MacroActionKind::train && action.executable;
          }), "a Support Bay alone is not a Reaver producer");
    if (!errors) std::cout << "Reaver prerequisite scenarios passed\n";
    return errors ? 1 : 0;
}
