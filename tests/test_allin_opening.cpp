#include "protodd/AllInOpening.hpp"
#include "protodd/MacroPlanner.hpp"
#include "protodd/Squads.hpp"

#include <algorithm>
#include <iostream>
#include <tuple>

using namespace protodd;
namespace {
int failures{};
void check(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
void add(GameState& state, UnitKind kind, int amount, bool complete = true) {
    for (int i = 0; i < amount; ++i) {
        UnitSnapshot unit; unit.id = static_cast<int>(state.self.units.size()) + 1;
        unit.kind = kind; unit.ours = true; unit.completed = complete;
        unit.position = {128, 128}; unit.hitPoints = 100;
        state.self.units.push_back(unit);
    }
}
StrategicPlan baseline() {
    StrategicPlan plan;
    plan.name = "native"; plan.expansionTarget = {1024, 1024};
    plan.attackTarget = {3000, 3000}; plan.desiredBases = 3; plan.desiredWorkers = 50;
    plan.goals = {{GoalKind::build, UnitKind::photonCannon, 4, 120, true, "optional"},
                  {GoalKind::build, UnitKind::roboticsFacility, 1, 118, true, "optional"}};
    return plan;
}
int requested(const StrategicPlan& plan, UnitKind kind) {
    int result = 0;
    for (const auto& goal : plan.goals) if (goal.target == kind) result = std::max(result, goal.desiredCount);
    return result;
}
}
int main() {
    {
        const auto makePriorityState = [] {
            GameState scenario;
            scenario.frame = 10 * 60 * 24;
            scenario.self.race = scenario.enemy.race = Race::protoss;
            scenario.self.supplyUsed = 40;
            scenario.self.supplyTotal = 80;
            add(scenario, UnitKind::nexus, 1);
            add(scenario, UnitKind::pylon, 1);
            add(scenario, UnitKind::gateway, 1);
            add(scenario, UnitKind::cyberneticsCore, 1);
            add(scenario, UnitKind::assimilator, 1);
            scenario.self.minerals = 200;
            scenario.self.gas = 200;
            return scenario;
        };
        const auto makePriorityPlan = [](const GoalKind observerKind, const bool observerBlocking,
                                         const bool requireDetection) {
            StrategicPlan candidate;
            candidate.name = "production priority synthetic";
            candidate.requireMobileDetection = requireDetection;
            candidate.prioritizeReinforcements = true;
            candidate.goals = {
                {GoalKind::train, UnitKind::dragoon, 4, 123, true, "fund four Dragoons"},
                {GoalKind::build, UnitKind::gateway, 2, 122, true, "Gateway throughput"},
                {observerKind, UnitKind::observer, 1, 85, observerBlocking, "optional Observer"},
            };
            return candidate;
        };

        // With no mobile-detection requirement or cloak evidence, optional
        // train/detect Observer goals must leave the first bank to the explicit
        // blocking Dragoon checkpoint and Gateway throughput.
        for (const auto observerKind : {GoalKind::train, GoalKind::detect}) {
            auto scenario = makePriorityState();
            const auto candidate = makePriorityPlan(observerKind, false, false);
            ResourceLedger bank{scenario.self.minerals, scenario.self.gas};
            MacroPlanner macro;
            const auto actions = macro.reconcile(scenario, candidate, bank);
            check(std::ranges::any_of(actions, [](const MacroAction& action) {
                      return action.target == UnitKind::dragoon &&
                             action.action == MacroActionKind::train && action.reserved &&
                             action.blocksLowerPriority;
                  }), "optional Observer without detection requirement does not preempt funded four-Dragoon goal");
            check(std::ranges::none_of(actions, [](const MacroAction& action) {
                      return action.target == UnitKind::roboticsFacility && action.reserved;
                  }), "optional Observer without cloak evidence does not reserve Robotics Facility bank");
            check(bank.reservedGas == 50,
                  "Dragoon bank is reserved before optional Observer without detection requirement");
        }

        // The detector floor itself is independent of goal.blocking. If a plan
        // carries requireMobileDetection, even an optional Observer goal is
        // lifted to 124 and can reserve the entire Robotics Facility bank.
        {
            auto scenario = makePriorityState();
            const auto candidate = makePriorityPlan(GoalKind::train, false, true);
            ResourceLedger bank{scenario.self.minerals, scenario.self.gas};
            MacroPlanner macro;
            const auto actions = macro.reconcile(scenario, candidate, bank);
            check(std::ranges::any_of(actions, [](const MacroAction& action) {
                      return action.target == UnitKind::roboticsFacility && action.reserved &&
                             !action.blocksLowerPriority && action.priority == 124;
                  }), "nonblocking Observer inherits detector priority floor and reserves the bank");
            check(std::ranges::none_of(actions, [](const MacroAction& action) {
                      return action.target == UnitKind::dragoon && action.reserved;
                  }), "priority-floored optional Observer spends ahead of the four-Dragoon goal");
            check(std::ranges::none_of(actions, [](const MacroAction& action) {
                      return action.target == UnitKind::gateway && action.reserved;
                  }), "priority-floored optional Observer spends ahead of Gateway throughput");
        }

        // Blocking urgent Observer is the positive control for the same
        // detector-chain priority/reservation path.
        {
            auto scenario = makePriorityState();
            const auto candidate = makePriorityPlan(GoalKind::train, true, true);
            ResourceLedger bank{scenario.self.minerals, scenario.self.gas};
            MacroPlanner macro;
            const auto actions = macro.reconcile(scenario, candidate, bank);
            check(std::ranges::any_of(actions, [](const MacroAction& action) {
                      return action.target == UnitKind::roboticsFacility && action.reserved &&
                             action.blocksLowerPriority && action.priority == 124;
                  }), "blocking urgent Observer preempts the army through its funded Robotics prerequisite");
            check(std::ranges::none_of(actions, [](const MacroAction& action) {
                      return action.target == UnitKind::dragoon && action.reserved;
                  }), "urgent Observer positive control reserves before the four-Dragoon goal");
        }
    }

    ThreatAssessment threat;
    GameState state; state.self.race = Race::protoss; state.enemy.race = Race::protoss;
    add(state, UnitKind::probe, 12); add(state, UnitKind::nexus, 1); add(state, UnitKind::gateway, 1);
    AllInOpeningPlanner opening; opening.reset(AllInBuild::twoGateZealot);
    auto plan = baseline(); opening.apply(plan, state, threat);
    check(plan.maximumBases == 1 && plan.desiredWorkers == 14 && plan.desiredGasWorkers == 0,
          "gasless commitment caps workers, expansion, and gas");
    check(requested(plan, UnitKind::gateway) == 2 && requested(plan, UnitKind::photonCannon) == 0,
          "fund second Gateway instead of speculative static defense");
    check(requested(plan, UnitKind::roboticsFacility) == 0, "no speculative detection tax");
    check(!SquadPlanner::shouldCoverExpansion(state, plan), "one-base army never guards an unrequested natural");

    {
        auto scenarioState = state;
        AllInOpeningPlanner scenarioOpening;
        scenarioOpening.reset(AllInBuild::twoGateZealot);
        const auto nativeObserver = ProductionGoal{GoalKind::train, UnitKind::observer, 1, 120, true,
                                                    "native cloak assessment"};
        auto scenarioPlan = baseline(); scenarioPlan.goals.push_back(nativeObserver);
        scenarioOpening.apply(scenarioPlan, scenarioState, threat);
        check(scenarioPlan.requireMobileDetection && scenarioPlan.desiredGasWorkers >= 3 &&
              requested(scenarioPlan, UnitKind::observer) == 1,
              "urgent native Observer commitment survives assemble before visible cloak");
        check(std::ranges::any_of(scenarioPlan.goals, [](const ProductionGoal& demand) {
                  return demand.goal == GoalKind::detect && demand.target == UnitKind::observer &&
                         demand.blocking && demand.priority == 128;
              }), "native commitment restores the high-priority detection goal");
        MacroPlanner macro;
        scenarioState.self.minerals = 1000; scenarioState.self.gas = 500;
        scenarioState.self.supplyTotal = 200;
        add(scenarioState, UnitKind::pylon, 1);
        ResourceLedger ledger{scenarioState.self.minerals, scenarioState.self.gas};
        const auto macroActions = macro.reconcile(scenarioState, scenarioPlan, ledger);
        check(std::ranges::any_of(macroActions, [](const MacroAction& action) {
                  return action.target == UnitKind::cyberneticsCore ||
                         action.target == UnitKind::roboticsFacility ||
                         action.target == UnitKind::observatory || action.target == UnitKind::observer;
              }), "detection prerequisite chain receives a macro production goal");

        auto nonUrgent = baseline();
        nonUrgent.goals.push_back({GoalKind::train, UnitKind::observer, 1, 120, false, "optional"});
        scenarioOpening.reset(AllInBuild::twoGateZealot);
        scenarioOpening.apply(nonUrgent, scenarioState, threat);
        check(!nonUrgent.requireMobileDetection && requested(nonUrgent, UnitKind::observer) == 0,
              "optional native Observer goal does not interrupt ordinary opening");
        nonUrgent = baseline();
        nonUrgent.goals.push_back({GoalKind::detect, UnitKind::observer, 1, 120, false, "optional"});
        scenarioOpening.reset(AllInBuild::twoGateZealot);
        scenarioOpening.apply(nonUrgent, scenarioState, threat);
        check(!nonUrgent.requireMobileDetection && requested(nonUrgent, UnitKind::observer) == 0,
              "optional native detection goal does not interrupt ordinary opening");
        nonUrgent = baseline();
        nonUrgent.goals.push_back({GoalKind::detect, UnitKind::observer, 0, 120, true, "cancel"});
        scenarioOpening.reset(AllInBuild::twoGateZealot);
        scenarioOpening.apply(nonUrgent, scenarioState, threat);
        check(!nonUrgent.requireMobileDetection && requested(nonUrgent, UnitKind::observer) == 0,
              "zero-count native Observer cancellation does not interrupt ordinary opening");

        add(scenarioState, UnitKind::zealot, 4);
        scenarioOpening.reset(AllInBuild::twoGateZealot);
        scenarioPlan = baseline(); scenarioPlan.goals.push_back(nativeObserver);
        scenarioOpening.apply(scenarioPlan, scenarioState, threat);
        check(scenarioOpening.phase() == AllInPhase::pressure && scenarioPlan.requireMobileDetection &&
              requested(scenarioPlan, UnitKind::observer) == 1,
              "urgent native Observer commitment survives pressure before visible cloak");
        scenarioState.self.units.erase(std::remove_if(scenarioState.self.units.begin(),
            scenarioState.self.units.end(), [](const UnitSnapshot& unit) {
                return unit.kind == UnitKind::zealot;
            }), scenarioState.self.units.end());
        add(scenarioState, UnitKind::observer, 1);
        scenarioPlan = baseline(); scenarioPlan.goals.push_back(nativeObserver);
        scenarioOpening.reset(AllInBuild::twoGateZealot);
        scenarioOpening.apply(scenarioPlan, scenarioState, threat);
        ledger = {scenarioState.self.minerals, scenarioState.self.gas};
        const auto completedObserverActions = macro.reconcile(scenarioState, scenarioPlan, ledger);
        check(std::ranges::none_of(completedObserverActions, [](const MacroAction& action) {
                  return action.action == MacroActionKind::train && action.target == UnitKind::observer;
              }), "completed Observer count prevents duplicate production");

        scenarioOpening.reset(AllInBuild::twoGateZealot); scenarioState.frame = 9000;
        scenarioPlan = baseline(); scenarioPlan.goals.push_back(nativeObserver);
        scenarioOpening.apply(scenarioPlan, scenarioState, threat);
        check(scenarioOpening.phase() == AllInPhase::transition &&
              std::ranges::any_of(scenarioPlan.goals, [](const ProductionGoal& demand) {
                  return demand.target == UnitKind::observer && demand.goal == GoalKind::train;
              }) && !scenarioPlan.requireMobileDetection,
              "transition preserves native goals and bypasses opening detection override");
    }
    add(state, UnitKind::zealot, 4, false);
    state.frame = 4000; plan = baseline(); opening.apply(plan, state, threat);
    check(opening.phase() == AllInPhase::assemble, "unfinished army cannot launch");
    for (auto& unit : state.self.units) unit.completed = true;
    state.frame = 4656; plan = baseline(); opening.apply(plan, state, threat);
    check(opening.launchFrame() == 4656 && plan.posture == Posture::attack, "four Zealots launch sticky attack");
    threat.combatEnemiesNearMain = 3;
    state.frame += 24; plan = baseline(); opening.apply(plan, state, threat);
    check(plan.posture == Posture::defend && opening.launchFrame() == 4656, "real breach interrupts without reroll");
    threat = {}; state.frame = 4656 + 120 * 24;
    plan = baseline(); opening.apply(plan, state, threat);
    check(opening.phase() == AllInPhase::transition && opening.transitionReason() == "pressure-window",
          "bounded pressure window ends commitment");
    check(plan.desiredBases >= 2 && plan.desiredWorkers >= 32 && plan.sustainEconomy,
          "transition funds economic recovery");
    check(plan.posture == Posture::attack && plan.minimumAttackSize == 4,
          "expanding does not recall surviving pressure army");
    state.bases = {{}};
    state.bases[0].center = {1000, 1000}; state.bases[0].groundDistanceFromMain = 900;
    state.bases[0].mineralsRemaining = 10000;
    plan = baseline(); plan.expansionTarget = {-1, -1}; threat.proxy = 1;
    opening.apply(plan, state, threat);
    check(plan.expansionTarget == Position{1000, 1000} && requested(plan, UnitKind::nexus) >= 2,
          "perimeter proxy belief alone cannot leave economic recovery without a site");
    threat = {};
    state.frame += 24; plan = baseline(); opening.apply(plan, state, threat);
    check(opening.phase() == AllInPhase::transition, "transition cannot revert to a new all-in");
    opening.reset(AllInBuild::twoGateZealot);
    state.frame = 9000; plan = baseline(); opening.apply(plan, state, threat);
    check(opening.transitionReason() == "deadline", "hard deadline aborts stalled opening even without launch");
    for (const auto build : {AllInBuild::threeGateDragoon, AllInBuild::fourGateDragoon, AllInBuild::darkTemplar}) {
        opening.reset(build); state = {}; state.frame = 7200;
        add(state, UnitKind::probe, 20); add(state, UnitKind::gateway, 2);
        add(state, UnitKind::cyberneticsCore, 1); add(state, UnitKind::assimilator, 1);
        add(state, UnitKind::dragoon, 6);
        if (build == AllInBuild::darkTemplar) add(state, UnitKind::darkTemplar, 2);
        plan = baseline(); opening.apply(plan, state, threat);
        check(opening.phase() == AllInPhase::pressure && plan.posture == Posture::attack,
              "each alternative launches on its completed army milestone");
        check(plan.desiredGasWorkers == 3 && requested(plan, UnitKind::gateway) >= 3,
              "alternative funds gas and explicit Gateway ceiling");
        std::erase_if(state.self.units, [](const UnitSnapshot& u) {
            return u.kind == UnitKind::dragoon || u.kind == UnitKind::darkTemplar;
        });
        state.frame += 12 * 24; plan = baseline(); opening.apply(plan, state, threat);
        check(opening.transitionReason() == "army-loss", "failed push resumes economy instead of endless recommitment");
    }

    {
        const auto makePvpState = [](const bool scoutedTwoGateways) {
            GameState scenario;
            scenario.frame = 4 * 60 * 24 + 12 * 24;
            scenario.self.id = 1; scenario.enemy.id = 2;
            scenario.self.race = scenario.enemy.race = Race::protoss;
            scenario.self.supplyUsed = 48; scenario.self.supplyTotal = 100;
            add(scenario, UnitKind::nexus, 1);
            add(scenario, UnitKind::pylon, 1);
            add(scenario, UnitKind::gateway, 4);
            add(scenario, UnitKind::assimilator, 1);
            add(scenario, UnitKind::cyberneticsCore, 1);
            add(scenario, UnitKind::dragoon, 6);
            add(scenario, UnitKind::probe, 20);
            if (scoutedTwoGateways) {
                for (int i = 0; i < 2; ++i) {
                    UnitSnapshot enemyGateway;
                    enemyGateway.id = 100 + i;
                    enemyGateway.kind = UnitKind::gateway;
                    enemyGateway.position = {2200 + i * 128, 2200};
                    enemyGateway.visible = true;
                    enemyGateway.completed = true;
                    enemyGateway.lastSeen = scenario.frame;
                    scenario.enemy.units.push_back(enemyGateway);
                }
            }
            return scenario;
        };
        const auto observerCommitment = [](const StrategicPlan& candidate) {
            return std::ranges::any_of(candidate.goals, [](const ProductionGoal& demand) {
                return demand.target == UnitKind::observer && demand.blocking &&
                       demand.desiredCount > 0 &&
                       (demand.goal == GoalKind::train || demand.goal == GoalKind::detect);
            });
        };

        auto covertState = makePvpState(true);
        ThreatAssessment noBreach;
        check(noBreach.mostLikely != EnemyPlan::fastRush &&
              std::ranges::none_of(covertState.enemy.units, [](const UnitSnapshot& enemy) {
                  return enemy.kind == UnitKind::zealot || enemy.kind == UnitKind::darkTemplar;
              }), "synthetic covert fixture has no FastRush label or known melee attacker");
        auto nativePlan = StrategyEngine{}.plan(covertState, noBreach);
        check(observerCommitment(nativePlan),
              "native PvP covert-tech assessment emits a blocking Observer commitment");

        AllInOpeningPlanner fourGate;
        fourGate.reset(AllInBuild::fourGateDragoon);
        fourGate.apply(nativePlan, covertState, noBreach);
        check(nativePlan.requireMobileDetection && nativePlan.desiredGasWorkers >= 3 &&
              std::ranges::any_of(nativePlan.goals, [](const ProductionGoal& demand) {
                  return demand.goal == GoalKind::detect && demand.target == UnitKind::observer &&
                         demand.blocking && demand.desiredCount > 0;
              }), "four-Gate opening retains the native covert Observer commitment");

        StrategicDirector director;
        auto stabilized = director.stabilize(nativePlan, covertState, noBreach);
        ResourceLedger detectionBank{1200, 1000};
        MacroPlanner production;
        const auto detectionActions = production.reconcile(covertState, stabilized, detectionBank);
        check(std::ranges::any_of(detectionActions, [](const MacroAction& action) {
                  return action.blocksLowerPriority && action.executable && action.reserved &&
                         (action.target == UnitKind::roboticsFacility ||
                          action.target == UnitKind::observatory ||
                          action.target == UnitKind::observer);
              }), "stabilized four-Gate production requests a funded detection prerequisite");

        // Exercise the completed tech chain under the ladder's gas bottleneck.
        // Army goals remain present, but the urgent Observer must own the gas
        // bank until it reaches the 75-gas train cost.
        auto observerBankState = makePvpState(true);
        observerBankState.self.minerals = 78;
        observerBankState.self.gas = 26;
        observerBankState.self.units.clear();
        add(observerBankState, UnitKind::nexus, 1);
        add(observerBankState, UnitKind::probe, 20);
        add(observerBankState, UnitKind::gateway, 4);
        add(observerBankState, UnitKind::assimilator, 1);
        add(observerBankState, UnitKind::cyberneticsCore, 1);
        add(observerBankState, UnitKind::roboticsFacility, 1);
        add(observerBankState, UnitKind::observatory, 1);
        add(observerBankState, UnitKind::dragoon, 6);
        observerBankState.self.supplyUsed = 48;
        observerBankState.self.supplyTotal = 100;
        UnitSnapshot visibleDt;
        visibleDt.id = 300; visibleDt.kind = UnitKind::darkTemplar;
        visibleDt.visible = true; visibleDt.completed = true;
        visibleDt.position = {2200, 2200}; visibleDt.lastSeen = observerBankState.frame;
        observerBankState.enemy.units.push_back(visibleDt);

        const auto reconcileObserverBank = [&](GameState& bankState) {
            auto bankPlan = StrategyEngine{}.plan(bankState, noBreach);
            fourGate.reset(AllInBuild::fourGateDragoon);
            fourGate.apply(bankPlan, bankState, noBreach);
            const auto phase = fourGate.phase();
            bankPlan = director.stabilize(bankPlan, bankState, noBreach);
            ResourceLedger bank{bankState.self.minerals, bankState.self.gas};
            auto actions = production.reconcile(bankState, bankPlan, bank);
            return std::tuple{std::move(bankPlan), phase, std::move(bank), std::move(actions)};
        };

        auto [lowGasPlan, lowGasPhase, lowGasLedger, lowGasActions] = reconcileObserverBank(observerBankState);
        check(lowGasPhase == AllInPhase::pressure && lowGasPlan.requireMobileDetection &&
              std::ranges::any_of(lowGasPlan.goals, [](const ProductionGoal& demand) {
                  return demand.goal == GoalKind::train && demand.target == UnitKind::dragoon;
              }), "low-gas covert pressure fixture retains army production alongside detection");
        const auto lowGasObserver = std::ranges::find_if(lowGasActions, [](const MacroAction& action) {
            return action.target == UnitKind::observer && action.blocksLowerPriority;
        });
        check(lowGasObserver != lowGasActions.end() && !lowGasObserver->reserved,
              "26 gas emits an unaffordable blocking Observer action");
        check(lowGasObserver != lowGasActions.end() &&
              lowGasObserver->action == MacroActionKind::train,
              "26 gas Observer commitment remains a train action");
        check(lowGasLedger.reservedGas == 26 && lowGasLedger.freeGas() == 0,
              "unaffordable Observer protects all 26 gas from lower-priority spends");
        check(std::ranges::none_of(lowGasActions, [](const MacroAction& action) {
                  return action.target == UnitKind::dragoon && action.reserved && action.gas > 0;
              }), "low-gas army production cannot spend the protected Observer gas");

        observerBankState.self.minerals = 308;
        observerBankState.self.gas = 70;
        auto [nearGasPlan, nearGasPhase, nearGasLedger, nearGasActions] = reconcileObserverBank(observerBankState);
        (void)nearGasPlan; (void)nearGasPhase;
        const auto nearGasObserver = std::ranges::find_if(nearGasActions, [](const MacroAction& action) {
            return action.target == UnitKind::observer && action.blocksLowerPriority;
        });
        check(nearGasObserver != nearGasActions.end() && !nearGasObserver->reserved &&
              nearGasLedger.reservedGas == 70 && nearGasLedger.freeGas() == 0,
              "70 gas remains protected for the blocking Observer train");
        check(nearGasObserver != nearGasActions.end() &&
              nearGasObserver->action == MacroActionKind::train,
              "70 gas Observer commitment remains a train action");

        observerBankState.self.gas = 75;
        auto [readyGasPlan, readyGasPhase, readyGasLedger, readyGasActions] = reconcileObserverBank(observerBankState);
        (void)readyGasPlan; (void)readyGasPhase;
        const auto readyObserver = std::ranges::find_if(readyGasActions, [](const MacroAction& action) {
            return action.target == UnitKind::observer && action.blocksLowerPriority;
        });
        check(readyObserver != readyGasActions.end() && readyObserver->blocksLowerPriority &&
              readyObserver->reserved && readyObserver->executable && readyObserver->gas == 75 &&
              readyGasLedger.reservedGas >= 75,
              "75 gas funds executable blocking Observer train from completed Robotics Facility");
        check(readyObserver != readyGasActions.end() &&
              readyObserver->action == MacroActionKind::train,
              "75 gas Observer commitment becomes an Observer train action");

        // Keep detect goals that name buildings on the construction path.
        auto detectionBuildState = makePvpState(true);
        detectionBuildState.self.units.clear();
        add(detectionBuildState, UnitKind::nexus, 1);
        add(detectionBuildState, UnitKind::probe, 20);
        add(detectionBuildState, UnitKind::pylon, 1);
        add(detectionBuildState, UnitKind::gateway, 1);
        add(detectionBuildState, UnitKind::cyberneticsCore, 1);
        add(detectionBuildState, UnitKind::forge, 1);
        detectionBuildState.self.supplyTotal = 100;
        StrategicPlan cannonDetection;
        cannonDetection.goals.push_back({GoalKind::detect, UnitKind::photonCannon, 1, 120, true,
                                         "building detector regression"});
        ResourceLedger cannonBank{500, 100};
        MacroPlanner cannonMacro;
        const auto cannonActions = cannonMacro.reconcile(detectionBuildState, cannonDetection, cannonBank);
        check(std::ranges::any_of(cannonActions, [](const MacroAction& action) {
                  return action.target == UnitKind::photonCannon &&
                         action.action == MacroActionKind::build && action.blocksLowerPriority;
              }), "building detection target remains a blocking build action");

        auto missingRoboState = makePvpState(true);
        missingRoboState.self.units.clear();
        add(missingRoboState, UnitKind::nexus, 1);
        add(missingRoboState, UnitKind::probe, 20);
        add(missingRoboState, UnitKind::pylon, 1);
        add(missingRoboState, UnitKind::gateway, 1);
        add(missingRoboState, UnitKind::cyberneticsCore, 1);
        missingRoboState.self.supplyTotal = 100;
        StrategicPlan missingRoboPlan;
        missingRoboPlan.goals.push_back({GoalKind::detect, UnitKind::observer, 1, 128, true,
                                         "missing Robotics Facility regression"});
        ResourceLedger missingRoboBank{500, 100};
        MacroPlanner missingRoboMacro;
        const auto missingRoboActions = missingRoboMacro.reconcile(
            missingRoboState, missingRoboPlan, missingRoboBank);
        check(std::ranges::any_of(missingRoboActions, [](const MacroAction& action) {
                  return action.target == UnitKind::roboticsFacility &&
                         action.action == MacroActionKind::build && action.blocksLowerPriority;
              }), "Observer detect goal keeps unmet Robotics Facility prerequisite as build");

        auto observerAlreadyPresent = detectionBuildState;
        add(observerAlreadyPresent, UnitKind::roboticsFacility, 1);
        add(observerAlreadyPresent, UnitKind::observatory, 1);
        add(observerAlreadyPresent, UnitKind::observer, 1);
        StrategicPlan satisfiedDetection;
        satisfiedDetection.goals.push_back({GoalKind::detect, UnitKind::observer, 1, 128, true,
                                            "already satisfied Observer regression"});
        ResourceLedger satisfiedBank{500, 100};
        MacroPlanner satisfiedMacro;
        const auto satisfiedActions = satisfiedMacro.reconcile(
            observerAlreadyPresent, satisfiedDetection, satisfiedBank);
        check(std::ranges::none_of(satisfiedActions, [](const MacroAction& action) {
                  return action.target == UnitKind::observer;
              }), "sufficient completed Observer count suppresses duplicate detection production");

        StrategicPlan ordinaryKinds;
        ordinaryKinds.goals = {{GoalKind::train, UnitKind::dragoon, 1, 120, true, "ordinary train"},
                               {GoalKind::build, UnitKind::photonCannon, 1, 119, true, "ordinary build"}};
        ResourceLedger ordinaryBank{500, 100};
        MacroPlanner ordinaryMacro;
        const auto ordinaryActions = ordinaryMacro.reconcile(
            detectionBuildState, ordinaryKinds, ordinaryBank);
        check(std::ranges::any_of(ordinaryActions, [](const MacroAction& action) {
                  return action.target == UnitKind::dragoon && action.action == MacroActionKind::train;
              }) && std::ranges::any_of(ordinaryActions, [](const MacroAction& action) {
                  return action.target == UnitKind::photonCannon && action.action == MacroActionKind::build;
              }), "ordinary train and build goals retain their action kinds");

        auto ordinaryState = makePvpState(false);
        std::erase_if(ordinaryState.self.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::dragoon;
        });
        const auto ordinaryPlan = StrategyEngine{}.plan(ordinaryState, noBreach);
        check(!observerCommitment(ordinaryPlan),
              "ordinary PvP opening without covert evidence emits no blocking Observer goal");
    }

    opening.reset(AllInBuild::twoGateZealot); state = {}; add(state, UnitKind::probe, 12);
    add(state, UnitKind::zealot, 3); add(state, UnitKind::gateway, 2);
    plan = baseline(); opening.apply(plan, state, threat);
    check(requested(plan, UnitKind::assimilator) == 1, "gas begins behind the first three completed Zealots");
    add(state, UnitKind::assimilator, 1);
    plan = baseline(); opening.apply(plan, state, threat);
    check(plan.desiredGasWorkers == 3 && requested(plan, UnitKind::cyberneticsCore) == 1,
          "two-Gate pressure develops ranged access before the expansion transition");
    add(state, UnitKind::cyberneticsCore, 1);
    plan = baseline(); opening.apply(plan, state, threat);
    check(requested(plan, UnitKind::dragoon) == 2, "completed Core shifts reinforcement priority to Dragoons");
    UnitSnapshot enemy; enemy.kind = UnitKind::darkTemplar; state.enemy.units.push_back(enemy);
    plan = baseline(); opening.apply(plan, state, threat);
    check(plan.requireMobileDetection && requested(plan, UnitKind::observer) == 1,
          "observed cloak threat retains legal detector production");
    opening.reset(AllInBuild::standard); plan = baseline(); opening.apply(plan, state, threat);
    check(plan.desiredBases == 3 && plan.goals.size() == 2, "standard profile unchanged");
    return failures ? 1 : 0;
}
