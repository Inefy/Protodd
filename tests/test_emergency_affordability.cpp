#include "protodd/MacroPlanner.hpp"

#include <algorithm>
#include <iostream>
#include <ranges>

namespace {
using namespace protodd;

UnitSnapshot unit(const int id, const UnitKind kind, const Position position) {
    UnitSnapshot result;
    result.id = id;
    result.kind = kind;
    result.position = position;
    result.completed = result.powered = true;
    result.hitPoints = result.maxHitPoints = 100;
    return result;
}

GameState stalledDefense() {
    GameState state;
    state.frame = 7367;
    state.self.id = 0;
    state.self.race = state.enemy.race = Race::protoss;
    state.self.minerals = 118;
    state.self.gas = 228;
    state.self.gatheredMinerals = 2618;
    state.self.gatheredGas = 328;
    // The long rolling estimate still reports income after deliveries stop.
    state.estimatedMineralIncomePerMinute = 256;
    state.self.supplyUsed = 36;
    state.self.supplyTotal = 66;
    state.self.units = {
        unit(1, UnitKind::nexus, {3808, 464}),
        unit(2, UnitKind::pylon, {3744, 576}),
        unit(3, UnitKind::pylon, {3872, 608}),
        unit(4, UnitKind::pylon, {4032, 672}),
        unit(5, UnitKind::gateway, {3648, 464}),
        unit(6, UnitKind::gateway, {3552, 592}),
        unit(7, UnitKind::forge, {3824, 672}),
        unit(8, UnitKind::cyberneticsCore, {3696, 640}),
    };
    for (int id = 20; id < 38; ++id)
        state.self.units.push_back(unit(id, UnitKind::probe, {3956, 436}));
    auto enemy = unit(100, UnitKind::zealot, {3911, 387});
    enemy.visible = true;
    state.enemy.units.push_back(enemy);
    return state;
}

StrategicPlan emergencyPlan() {
    StrategicPlan plan;
    plan.posture = Posture::defend;
    plan.prioritizeReinforcements = true;
    plan.desiredWorkers = 18;
    plan.goals = {
        {GoalKind::build, UnitKind::photonCannon, 1, 120, true,
         "replace the lost emergency anchor"},
        {GoalKind::train, UnitKind::zealot, 2, 105, true, "restore the melee screen"},
    };
    return plan;
}

bool funded(const std::vector<MacroAction>& actions, const UnitKind kind) {
    return std::ranges::any_of(actions, [kind](const MacroAction& action) {
        return action.target == kind && action.reserved && action.executable;
    });
}

std::vector<MacroAction> replay(GameState state, const StrategicPlan& plan,
                                ResourceLedger& bank) {
    MacroPlanner planner;
    auto prior = state;
    prior.frame = 6887;
    prior.self.gatheredMinerals = 2538;
    auto cannon = unit(9, UnitKind::photonCannon, {3520, 480});
    cannon.completed = false;
    prior.self.units.push_back(cannon);
    ResourceLedger earlierBank{38, 180};
    (void)planner.reconcile(prior, plan, earlierBank);
    // The Cannon is now gone; one last mineral delivery reaches the bank.
    prior = state;
    prior.frame = 7247;
    prior.self.gatheredMinerals = 2618;
    ResourceLedger lastDelivery{118, 228};
    (void)planner.reconcile(prior, plan, lastDelivery);
    return planner.reconcile(state, plan, bank);
}
}  // namespace

int main() {
    using namespace protodd;
    int failures = 0;
    const auto check = [&failures](const bool ok, const char* message) {
        if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };
    auto state = stalledDefense();
    auto plan = emergencyPlan();
    ResourceLedger bank{118, 228};
    auto actions = replay(state, plan, bank);
    check(funded(actions, UnitKind::zealot),
          "idle powered Gateways field affordable defense after mineral deliveries stop");
    check(std::ranges::count_if(actions, [](const MacroAction& action) {
              return action.target == UnitKind::zealot && action.reserved;
          }) == 1 && bank.committedMinerals == 100 && bank.committedGas == 0,
          "the interrupted-income fallback funds exactly one affordable cycle");

    state.frame = 7366;
    bank = {118, 228};
    check(!funded(replay(state, plan, bank), UnitKind::zealot),
          "a short delivery gap does not abandon the intended ranged reinforcement");
    state = stalledDefense();
    state.self.gatheredMinerals += 8;
    bank = {118, 228};
    check(!funded(replay(state, plan, bank), UnitKind::zealot),
          "continued mineral deliveries preserve ranged saving");
    state = stalledDefense();
    bank = {125, 228};
    actions = replay(state, plan, bank);
    check(funded(actions, UnitKind::dragoon) && !funded(actions, UnitKind::zealot),
          "an affordable Dragoon retains its normal priority");
    bank = {225, 228};
    actions = replay(state, plan, bank);
    check(funded(actions, UnitKind::dragoon) && funded(actions, UnitKind::zealot) &&
          bank.committedMinerals == 225 && bank.committedGas == 50,
          "each idle producer uses the remaining bank without replacing an affordable Dragoon");
    state.estimatedMineralIncomePerMinute = -1;
    bank = {118, 228};
    check(!funded(replay(state, plan, bank), UnitKind::zealot),
          "unknown gathering observations do not invent a mining interruption");
    state = stalledDefense();
    state.self.gatheredMinerals = 100;
    bank = {118, 228};
    check(!funded(replay(state, plan, bank), UnitKind::zealot),
          "a reset resource counter starts a fresh observation window");
    state = stalledDefense();
    state.frame = 100;
    bank = {118, 228};
    check(!funded(replay(state, plan, bank), UnitKind::zealot),
          "a frame rewind cannot carry an earlier mining interruption forward");

    for (int scenario = 0; scenario < 8; ++scenario) {
        state = stalledDefense();
        auto& enemy = state.enemy.units.front();
        if (scenario == 0) enemy.position = {320, 320};
        if (scenario == 1) enemy.visible = false;
        if (scenario == 2) enemy.flying = true;
        if (scenario == 3) enemy.detected = false;
        if (scenario == 4) enemy.disabled = true;
        if (scenario == 5) enemy.loaded = true;
        if (scenario == 6) enemy.hallucination = true;
        if (scenario == 7) enemy.invincible = true;
        bank = {118, 228};
        check(!funded(replay(state, plan, bank), UnitKind::zealot),
              "remote or untargetable enemies do not divert ranged reinforcement funds");
    }
    state = stalledDefense();
    state.self.units.front().completed = false;
    bank = {118, 228};
    check(!funded(replay(state, plan, bank), UnitKind::zealot),
          "an unfinished depot does not establish a mining base under attack");
    state = stalledDefense();
    state.self.busyProducers = {UnitKind::gateway, UnitKind::gateway};
    bank = {118, 228};
    check(!funded(replay(state, plan, bank), UnitKind::zealot),
          "busy Gateways receive no duplicate fallback cycle");
    state.self.busyProducers.clear();
    for (auto& own : state.self.units)
        if (own.kind == UnitKind::gateway) own.powered = false;
    bank = {118, 228};
    check(!funded(replay(state, plan, bank), UnitKind::zealot),
          "unpowered Gateways cannot spend fallback resources");

    state = stalledDefense();
    bank = {99, 228};
    check(!funded(replay(state, plan, bank), UnitKind::zealot),
          "the fallback must itself be affordable");
    bank = {118, 228};
    bank.reserve(75, 0);
    check(!funded(replay(state, plan, bank), UnitKind::zealot) && bank.committedMinerals >= 75,
          "existing spending commitments are not released for the fallback");
    state.self.supplyUsed = 64;
    bank = {118, 228};
    check(!funded(replay(state, plan, bank), UnitKind::zealot),
          "urgent supply keeps its reservation ahead of the fallback");
    state = stalledDefense();
    plan.requireMobileDetection = true;
    plan.goals.push_back({GoalKind::build, UnitKind::roboticsFacility, 1, 124, true,
                          "urgent mobile detection"});
    bank = {118, 228};
    check(!funded(replay(state, plan, bank), UnitKind::zealot),
          "urgent detection retains its higher-priority funding");
    plan = emergencyPlan();
    plan.goals.push_back({GoalKind::train, UnitKind::dragoon, 1, 118, true,
                          "required ranged counter", TechnologyKind::none, false});
    bank = {118, 228};
    check(!funded(replay(state, plan, bank), UnitKind::zealot),
          "an explicit higher-priority no-fallback counter is respected");
    return failures == 0 ? 0 : 1;
}
