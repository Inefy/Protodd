#include "protodd/MacroPlanner.hpp"

#include <algorithm>
#include <iostream>

int main() {
    using namespace protodd;
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { ++failures; std::cerr << message << '\n'; }
    };
    const auto unit = [](int id, UnitKind kind) {
        UnitSnapshot result;
        result.id = id; result.kind = kind; result.completed = true;
        return result;
    };
    const auto funded = [](const auto& actions, UnitKind kind) {
        return std::ranges::any_of(actions, [kind](const MacroAction& action) {
            return action.target == kind && action.reserved && action.executable;
        });
    };
    const auto run = [](const GameState& snapshot, const StrategicPlan& strategy,
                        ResourceLedger& ledger) {
        return MacroPlanner{}.reconcile(snapshot, strategy, ledger);
    };
    GameState state;
    state.frame = 6000; state.self.race = state.enemy.race = Race::protoss;
    state.self.supplyUsed = 60; state.self.supplyTotal = 100;
    state.self.units = {unit(1, UnitKind::pylon), unit(2, UnitKind::gateway),
        unit(3, UnitKind::gateway), unit(4, UnitKind::assimilator), unit(5, UnitKind::nexus)};
    for (int id = 20; id < 40; ++id) state.self.units.push_back(unit(id, UnitKind::probe));
    StrategicPlan plan;
    plan.prioritizeReinforcements = true;
    plan.desiredWorkers = 22;
    plan.composition = {{UnitKind::dragoon, 1.0}, {UnitKind::reaver, .15}};
    plan.goals = {{GoalKind::build, UnitKind::cyberneticsCore, 1, 123, true, "ranged opening checkpoint"},
                  {GoalKind::train, UnitKind::probe, 22, 105, false, "opening income"}};
    ResourceLedger ledger{200, 320};
    auto actions = run(state, plan, ledger);
    check(funded(actions, UnitKind::cyberneticsCore) && !funded(actions, UnitKind::zealot) &&
              ledger.reservedMinerals == 200,
          "the explicit urgent Core must start before another injected Zealot cycle");
    check(std::ranges::any_of(actions, [](const MacroAction& action) {
        return action.target == UnitKind::cyberneticsCore && action.priority == 123;
    }), "reinforcement arbitration must retain the explicit Core priority");

    ledger = {190, 320};
    actions = run(state, plan, ledger);
    check(!funded(actions, UnitKind::probe) && !funded(actions, UnitKind::zealot) &&
              ledger.reservedMinerals == 190,
          "routine units must not repeatedly consume the urgent Core's last deposit");
    auto thresholdPlan = plan;
    thresholdPlan.goals.front().priority = 119;
    ledger = {200, 320};
    check(funded(run(state, thresholdPlan, ledger), UnitKind::cyberneticsCore),
          "the existing priority-119 hard Core checkpoint must also survive arbitration");

    // Ordinary optional tech keeps the defensive reinforcement order.
    auto optional = plan;
    optional.goals.front().priority = 98;
    ledger = {200, 320};
    actions = run(state, optional, ledger);
    check(!funded(actions, UnitKind::cyberneticsCore) && funded(actions, UnitKind::zealot),
          "ordinary Core infrastructure must not preempt direct defense");
    optional = plan; optional.goals.front().blocking = false;
    ledger = {200, 320};
    actions = run(state, optional, ledger);
    check(!funded(actions, UnitKind::cyberneticsCore) && funded(actions, UnitKind::zealot),
          "a nonblocking Core must not become a hard checkpoint");
    auto late = state; late.frame = 8 * 60 * 24;
    ledger = {200, 320};
    check(funded(run(late, plan, ledger), UnitKind::zealot),
          "the first-Core exception must remain bounded to the opening");
    auto otherRace = state; otherRace.enemy.race = Race::zerg;
    ledger = {200, 320};
    check(funded(run(otherRace, plan, ledger), UnitKind::zealot),
          "the mirror Core exception must not reorder PvZ defense");

    auto paid = state;
    auto core = unit(60, UnitKind::cyberneticsCore);
    core.completed = false; core.buildProgress = 10;
    paid.self.units.push_back(core);
    ledger = {200, 320};
    actions = run(paid, plan, ledger);
    check(!funded(actions, UnitKind::cyberneticsCore) && funded(actions, UnitKind::zealot),
          "a paid Core must immediately release the bank to current reinforcement");
    paid.self.units.back().completed = true;
    ledger = {250, 100};
    actions = run(paid, plan, ledger);
    check(std::ranges::count_if(actions, [](const MacroAction& action) {
        return action.target == UnitKind::dragoon && action.reserved && action.executable;
    }) == 2, "the completed Core must enable both Gateway Dragoon cycles");

    // Existing emergency priorities stay ahead of this prerequisite.
    auto emergency = plan;
    emergency.goals.push_back({GoalKind::build, UnitKind::forge, 1, 118, true, "emergency static anchor"});
    ledger = {200, 320};
    actions = run(state, emergency, ledger);
    check(funded(actions, UnitKind::forge) && !funded(actions, UnitKind::cyberneticsCore),
          "the first emergency static detector anchor must keep its stronger priority");
    auto tight = state; tight.self.supplyUsed = 98;
    ledger = {200, 320};
    actions = run(tight, plan, ledger);
    check(funded(actions, UnitKind::pylon) && !funded(actions, UnitKind::cyberneticsCore),
          "an urgent supply deadline must remain ahead of the Core checkpoint");
    auto recovery = state;
    std::erase_if(recovery.self.units, [](const UnitSnapshot& own) {
        return own.kind == UnitKind::probe && own.id >= 27;
    });
    ledger = {200, 320};
    actions = run(recovery, plan, ledger);
    check(funded(actions, UnitKind::probe),
          "critical worker recovery must remain ahead of the Core checkpoint");

    // Required detection can still own the prerequisite and reserve its full
    // quota after it becomes actionable; this patch never edits that demand.
    auto detection = plan;
    detection.requireMobileDetection = true;
    detection.goals.push_back({GoalKind::train, UnitKind::observer, 3, 128, true, "confirmed cloak"});
    ledger = {200, 320};
    actions = run(state, detection, ledger);
    check(std::ranges::any_of(actions, [](const MacroAction& action) {
        return action.target == UnitKind::cyberneticsCore && action.reserved &&
               action.reason == "unlock Observer" && action.priority == 128;
    }), "confirmed cloak must retain ownership of its stronger Core prerequisite");
    paid.self.units.push_back(unit(61, UnitKind::roboticsFacility));
    paid.self.units.push_back(unit(62, UnitKind::observatory));
    paid.self.units.push_back(unit(63, UnitKind::observer));
    ledger = {25, 75};
    actions = run(paid, detection, ledger);
    check(funded(actions, UnitKind::observer),
          "a stronger detector quota must survive after the first Observer exists");

    if (!failures) std::cout << "Urgent Core checkpoint scenarios passed\n";
    return failures ? 1 : 0;
}
