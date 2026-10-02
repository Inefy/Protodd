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
    // A Lurker Egg is already a confirmed cloak signal at the BWAPI boundary.
    // It must fund the first detector before any Lurker becomes visible.
    {
        GameState pvz; pvz.frame = 7000;
        pvz.self.race = Race::protoss; pvz.enemy.race = Race::zerg;
        pvz.self.units = {unit(1, UnitKind::nexus), unit(2, UnitKind::pylon),
            unit(3, UnitKind::gateway), unit(4, UnitKind::cyberneticsCore)};
        for (int i = 0; i < 12; ++i) pvz.self.units.push_back(unit(20 + i, UnitKind::probe));
        auto egg = unit(100, UnitKind::lurkerEgg, false); egg.lastSeen = pvz.frame;
        pvz.enemy.units = {egg};
        const auto lurkerPlan = StrategyEngine{}.plan(pvz, {});
        check(lurkerPlan.requireMobileDetection && lurkerPlan.desiredGasWorkers >= 3,
              "a confirmed Lurker Egg did not request mobile detection and three gas workers");
        check(std::ranges::any_of(lurkerPlan.goals, [](const ProductionGoal& goal) {
            return goal.goal == GoalKind::train && goal.target == UnitKind::observer &&
                goal.desiredCount == 1 && goal.blocking;
        }), "a confirmed Lurker Egg did not request one blocking first Observer");
        ResourceLedger pvzBank{1000, 1000};
        auto chain = MacroPlanner{}.reconcile(pvz, lurkerPlan, pvzBank);
        check(funded(chain, UnitKind::roboticsFacility) && !funded(chain, UnitKind::gateway),
              "Lurker detection did not reserve the first Robotics prerequisite");
        ResourceLedger emptyBank{100, 0};
        chain = MacroPlanner{}.reconcile(pvz, lurkerPlan, emptyBank);
        check(std::ranges::any_of(chain, [](const MacroAction& action) {
            return action.target == UnitKind::roboticsFacility && action.blocksLowerPriority;
        }) && emptyBank.reservedMinerals > 0,
              "unfunded detection prerequisite failed to protect its future resource bank");

        pvz.self.units.push_back(unit(5, UnitKind::roboticsFacility));
        pvzBank = {1000, 1000};
        chain = MacroPlanner{}.reconcile(pvz, StrategyEngine{}.plan(pvz, {}), pvzBank);
        check(funded(chain, UnitKind::observatory), "Lurker detection skipped the Observatory prerequisite");
        pvz.self.units.push_back(unit(6, UnitKind::observatory));
        pvzBank = {1000, 1000};
        chain = MacroPlanner{}.reconcile(pvz, StrategyEngine{}.plan(pvz, {}), pvzBank);
        check(funded(chain, UnitKind::observer), "Lurker detection did not produce after its prerequisites");

        pvz.self.units.push_back(unit(7, UnitKind::observer));
        const auto completedPlan = StrategyEngine{}.plan(pvz, {});
        pvzBank = {1000, 1000};
        chain = MacroPlanner{}.reconcile(pvz, completedPlan, pvzBank);
        check(completedPlan.requireMobileDetection && !funded(chain, UnitKind::observer),
              "an existing completed Observer caused duplicate detector production");

        auto lurker = unit(101, UnitKind::lurker); lurker.lastSeen = pvz.frame;
        pvz.enemy.units = {lurker};
        const auto visibleLurkerPlan = StrategyEngine{}.plan(pvz, {});
        check(visibleLurkerPlan.requireMobileDetection &&
              std::ranges::any_of(visibleLurkerPlan.goals, [](const ProductionGoal& goal) {
                  return goal.goal == GoalKind::train && goal.target == UnitKind::observer &&
                      goal.desiredCount == 2 && goal.blocking;
              }),
              "a visible Lurker changed the existing mission detector demand");
        lurker.visible = false; lurker.burrowed = true; lurker.lastSeen = pvz.frame;
        pvz.enemy.units = {lurker};
        const auto burrowedLurkerPlan = StrategyEngine{}.plan(pvz, {});
        check(burrowedLurkerPlan.requireMobileDetection &&
              std::ranges::any_of(burrowedLurkerPlan.goals, [](const ProductionGoal& goal) {
                  return goal.goal == GoalKind::train && goal.target == UnitKind::observer &&
                      goal.desiredCount == 2 && goal.blocking;
              }),
              "a recently observed burrowed Lurker changed the mission detector demand");

        pvz.enemy.units = {egg};
        ThreatAssessment independentCloak; independentCloak.cloak = 0.5;
        const auto combinedPlan = StrategyEngine{}.plan(pvz, independentCloak);
        check(std::ranges::any_of(combinedPlan.goals, [](const ProductionGoal& goal) {
            return goal.goal == GoalKind::train && goal.target == UnitKind::observer &&
                goal.desiredCount == 2 && goal.blocking;
        }), "the Egg minimum lowered an independent cloak detector demand");

        // The adapter maps generic BWAPI Egg observations to unknown.
        pvz.enemy.units = {unit(102, UnitKind::unknown)};
        check(!StrategyEngine{}.plan(pvz, {}).requireMobileDetection,
              "a generic Egg incorrectly triggered confirmed-cloak detection");
        pvz.enemy.units = {unit(103, UnitKind::hydralisk)};
        check(!StrategyEngine{}.plan(pvz, {}).requireMobileDetection,
              "a lone Hydra incorrectly triggered confirmed-cloak detection");
        auto unseenEgg = egg; unseenEgg.visible = false; unseenEgg.lastSeen = -1;
        pvz.enemy.units = {unseenEgg};
        check(!StrategyEngine{}.plan(pvz, {}).requireMobileDetection,
              "an Egg with no seen timestamp incorrectly triggered detection");
        pvz.enemy.race = Race::protoss; pvz.enemy.units = {egg};
        check(!StrategyEngine{}.plan(pvz, {}).requireMobileDetection,
              "the Lurker Egg trigger changed PvP behavior");
        pvz.enemy.race = Race::terran;
        check(!StrategyEngine{}.plan(pvz, {}).requireMobileDetection,
              "the Lurker Egg trigger changed PvT behavior");
    }
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
