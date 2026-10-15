#include "protodd/MacroPlanner.hpp"

#include <algorithm>
#include <iostream>
#include <ranges>

namespace {
using namespace protodd;

UnitSnapshot unit(const int id, const UnitKind kind) {
    UnitSnapshot result;
    result.id = id;
    result.kind = kind;
    result.position = {320, 320};
    result.completed = result.powered = true;
    result.hitPoints = result.maxHitPoints = 100;
    return result;
}

GameState queuedScreen() {
    GameState state;
    state.frame = 10000;
    state.self.race = state.enemy.race = Race::protoss;
    state.self.supplyUsed = 24;
    state.self.supplyTotal = 100;
    state.self.units = {unit(1, UnitKind::nexus), unit(2, UnitKind::pylon),
                        unit(3, UnitKind::gateway), unit(4, UnitKind::gateway)};
    auto active = unit(10, UnitKind::zealot);
    active.completed = false;
    state.self.units.push_back(active);
    state.self.queuedUnits = {UnitKind::zealot};
    ProducerSlotSnapshot busy;
    busy.id = 3;
    busy.kind = UnitKind::gateway;
    busy.activeTraining = true;
    busy.trainingQueueSize = 2;
    busy.remainingTrainFrames = 200;
    busy.queuedUnits = {UnitKind::zealot};
    ProducerSlotSnapshot idle;
    idle.id = 4;
    idle.kind = UnitKind::gateway;
    state.self.producerSlots = {busy, idle};
    return state;
}

int funded(const std::vector<MacroAction>& actions, const UnitKind kind) {
    return static_cast<int>(std::ranges::count_if(actions, [kind](const MacroAction& action) {
        return action.target == kind && action.reserved && action.executable;
    }));
}
}  // namespace

int main() {
    using namespace protodd;
    int failures = 0;
    const auto check = [&failures](const bool ok, const char* message) {
        if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };
    auto state = queuedScreen();
    StrategicPlan plan;
    plan.goals = {{GoalKind::train, UnitKind::zealot, 3, 100, true, "three paid defenders"}};
    ResourceLedger bank{100, 0};
    auto actions = MacroPlanner{}.reconcile(state, plan, bank);
    check(funded(actions, UnitKind::zealot) == 1,
          "one active and one waiting unit leave the third-unit goal unfulfilled");
    plan.goals.front().desiredCount = 2;
    bank = {100, 0};
    check(funded(MacroPlanner{}.reconcile(state, plan, bank), UnitKind::zealot) == 0,
          "paid waiting production still fulfills its share of the goal");

    state.frame = 3500;
    plan = {};
    plan.openingScreenTarget = 3;
    bank = {100, 0};
    check(funded(MacroPlanner{}.reconcile(state, plan, bank), UnitKind::zealot) == 1,
          "the opening screen counts the waiting defender once");

    state = queuedScreen();
    state.self.units.push_back(unit(5, UnitKind::cyberneticsCore));
    for (int id = 11; id <= 13; ++id)
        state.self.units.push_back(unit(id, UnitKind::dragoon));
    plan = {};
    plan.composition = {{UnitKind::dragoon, 0.5}, {UnitKind::zealot, 0.5}};
    bank = {125, 50};
    actions = MacroPlanner{}.reconcile(state, plan, bank);
    check(funded(actions, UnitKind::zealot) == 1 && funded(actions, UnitKind::dragoon) == 0,
          "composition sees two paid Zealots and three Dragoons instead of a phantom third Zealot");
    return failures == 0 ? 0 : 1;
}
