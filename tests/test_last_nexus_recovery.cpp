#include "protodd/Workers.hpp"

#include "protodd/MacroPlanner.hpp"
#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <iostream>

namespace {

protodd::UnitSnapshot makeUnit(const int id, const protodd::UnitKind kind,
                               const protodd::Position position) {
    protodd::UnitSnapshot unit;
    unit.id = id;
    unit.kind = kind;
    unit.position = position;
    unit.completed = true;
    unit.visible = true;
    unit.role = kind == protodd::UnitKind::probe ? protodd::UnitRole::worker :
        kind == protodd::UnitKind::nexus ? protodd::UnitRole::resourceDepot :
        protodd::UnitRole::groundArmy;
    return unit;
}

protodd::GameState recoveryState(const int minerals) {
    using namespace protodd;
    GameState state;
    state.frame = 12000;
    state.mapWidthPixels = 2048;
    state.mapHeightPixels = 1024;
    state.self.id = 1;
    state.self.minerals = minerals;
    state.enemy.id = 2;
    state.enemy.race = Race::protoss;
    state.bases = {
        {1, {256, 512}, {300, 560}, 0, 0, -1, 0, true, false, 0, 0, -1, {}, 0},
        {2, {900, 512}, {940, 560}, 6000, 0, -1, 0, false, false, 8, 1, -1, {}, 500},
        {3, {1500, 512}, {1540, 560}, 7000, 0, -1, 0, false, false, 8, 1, -1, {}, 1400},
    };
    state.self.units.push_back(makeUnit(10, UnitKind::probe, {320, 512}));
    state.self.units.push_back(makeUnit(11, UnitKind::probe, {360, 512}));
    return state;
}

}  // namespace

int main() {
    using namespace protodd;
    int errors = 0;
    const auto check = [&](const bool value, const char* message) {
        if (!value) { ++errors; std::cerr << message << '\n'; }
    };

    auto state = recoveryState(400);
    auto tank = makeUnit(90, UnitKind::siegeTank, state.bases[1].center);
    tank.groundWeapon = {.damage = 70, .cooldown = 75, .maxRange = 384,
                         .targetsGround = true};
    tank.lastSeen = state.frame;
    state.enemy.units.push_back(tank);

    const auto plan = StrategyEngine{}.plan(state, {});
    check(plan.expansionTarget == state.bases[2].center &&
              plan.desiredBases == 1 && plan.posture == Posture::hold &&
              std::ranges::any_of(plan.goals, [](const ProductionGoal& goal) {
                  return goal.goal == GoalKind::expand && goal.target == UnitKind::nexus &&
                      goal.blocking && goal.priority >= 127;
              }),
          "last-Nexus recovery funds the reachable, safe neutral base ahead of other spending");
    auto lowSupply = state;
    lowSupply.self.supplyTotal = 18;
    lowSupply.self.supplyUsed = 18;
    auto lowSupplyPlan = StrategyEngine{}.plan(lowSupply, {});
    ResourceLedger recoveryLedger{400, 0};
    const auto recoveryActions = MacroPlanner{}.reconcile(
        lowSupply, lowSupplyPlan, recoveryLedger);
    check(recoveryActions.size() == 1 && recoveryActions.front().target == UnitKind::nexus &&
              recoveryActions.front().reserved,
          "last-Nexus recovery reserves the bank instead of diverting it to an automatic Pylon");

    InfluenceMap influence;
    auto stagingState = recoveryState(400);
    const auto stagingPlan = StrategyEngine{}.plan(stagingState, {});
    influence.update(stagingState);
    NavigationGrid openMap(64, 32, 32, std::vector<std::uint8_t>(64 * 32, 1));
    const auto staged = WorkerManager{}.assign(stagingState, stagingPlan, influence, {},
                                               false, false, &openMap);
    check(staged.size() == 2 && std::ranges::all_of(staged, [&](const WorkerAssignment& job) {
              return job.job == WorkerJob::rebuild && job.baseId == stagingState.bases[1].id &&
                     job.targetPosition == stagingState.bases[1].mineralLine;
          }),
          "survivors stage only at the selected site and receive no fictitious mineral income");

    auto unaffordable = recoveryState(399);
    const auto brokePlan = StrategyEngine{}.plan(unaffordable, {});
    InfluenceMap brokeInfluence;
    brokeInfluence.update(unaffordable);
    const auto waiting = WorkerManager{}.assign(unaffordable, brokePlan, brokeInfluence);
    check(!brokePlan.expansionTarget.valid() && brokePlan.desiredBases == 0 &&
              waiting.size() == 2 && std::ranges::all_of(waiting, [](const WorkerAssignment& job) {
                  return job.job == WorkerJob::rebuild && !job.targetPosition.valid();
              }),
          "below Nexus cost the bot cancels the rebuild request and stops stale mineral orders");

    auto unreachable = recoveryState(500);
    unreachable.bases[1].groundDistanceFromMain = -1;
    unreachable.bases[2].groundDistanceFromMain = -1;
    const auto unreachablePlan = StrategyEngine{}.plan(unreachable, {});
    check(!unreachablePlan.expansionTarget.valid(),
          "a resource site without a known ground route is not selected for recovery");

    auto enemyOwned = recoveryState(500);
    enemyOwned.bases[1].ownerId = enemyOwned.enemy.id;
    enemyOwned.bases[2].ownerId = enemyOwned.enemy.id;
    const auto enemyOwnedPlan = StrategyEngine{}.plan(enemyOwned, {});
    check(!enemyOwnedPlan.expansionTarget.valid(),
          "last-Nexus recovery never selects an enemy-owned resource base");

    auto blockedRoute = recoveryState(500);
    InfluenceMap blockedInfluence;
    blockedInfluence.update(blockedRoute);
    std::vector<std::uint8_t> terrain(64 * 32, 1);
    for (int y = 0; y < 32; ++y)
        terrain[static_cast<std::size_t>(y * 64 + 25)] = 0;
    NavigationGrid divided(64, 32, 32, std::move(terrain));
    auto reachablePlan = StrategyEngine{}.plan(blockedRoute, {});
    const auto stranded = WorkerManager{}.assign(
        blockedRoute, reachablePlan, blockedInfluence, {}, false, false, &divided);
    check(std::ranges::all_of(stranded, [](const WorkerAssignment& job) {
              return job.job == WorkerJob::rebuild && !job.targetPosition.valid();
          }),
          "workers do not stage at a site their current terrain route cannot reach");

    if (!errors) std::cout << "Last-Nexus recovery scenarios passed\n";
    return errors ? 1 : 0;
}
