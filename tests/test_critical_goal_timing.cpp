#include "protodd/MacroPlanner.hpp"
#include "protodd/Strategy.hpp"
#include "protodd/Technology.hpp"
#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <iostream>
#include <ranges>

namespace {

using namespace protodd;

int failures = 0;

void check(const bool condition, const char* message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

UnitSnapshot unit(const UnitId id, const UnitKind kind, const bool completed = true,
                  const bool powered = true) {
    UnitSnapshot result;
    result.id = id;
    result.kind = kind;
    result.ours = true;
    result.completed = completed;
    result.powered = powered;
    result.buildProgress = completed ? 100 : 0;
    result.hitPoints = result.maxHitPoints = 100;
    return result;
}

GameState detectionState() {
    GameState state;
    state.frame = 10'000;
    state.self.id = 1;
    state.self.race = Race::protoss;
    state.self.supplyUsed = 20;
    state.self.supplyTotal = 34;
    state.self.units = {
        unit(1, UnitKind::nexus),
        unit(2, UnitKind::pylon),
        unit(3, UnitKind::gateway),
        unit(4, UnitKind::probe),
        unit(5, UnitKind::roboticsFacility),
        unit(6, UnitKind::observatory),
    };
    state.self.producerSlots = {
        {1, UnitKind::nexus, false, 0, 0, 6, false, false, false, {}},
        {5, UnitKind::roboticsFacility, false, 0, 0, 6, false, false, false, {}},
    };
    return state;
}

ProductionGoal observerGoal(const int priority = 124) {
    ProductionGoal goal;
    goal.goal = GoalKind::train;
    goal.target = UnitKind::observer;
    goal.desiredCount = 1;
    goal.priority = priority;
    goal.blocking = true;
    goal.reason = "deadline-aware mobile detection";
    return goal;
}

void testDetectionDeadlineFeasibilityAndSlack() {
    const auto state = detectionState();
    const auto goal = observerGoal();
    const auto requiredBy = state.frame + unitStats(UnitKind::observer).buildTime + 120;
    const auto timing = assessCriticalGoalTiming(
        state, goal, requiredBy, CriticalReservationReason::detection);

    check(timing.active() && timing.feasible,
          "powered prerequisites and an idle Robotics Facility make detection feasible");
    check(timing.expectedReadyFrame == state.frame + unitStats(UnitKind::observer).buildTime &&
              timing.slackFrames == 120,
          "detection timing reports the estimated arrival frame and exact slack");
    check(criticalDeadlinePriorityAdjustment(timing, state.frame) > 0 &&
              criticalDeadlinePriorityAdjustment(timing, requiredBy + 1) == 0,
          "deadline urgency is temporary and disappears after the checkpoint");
}

void testMissedDeadlineDoesNotFreezeWorkerContinuity() {
    auto state = detectionState();
    auto& slot = state.self.producerSlots.back();
    slot.activeTraining = true;
    slot.trainingQueueSize = 1;
    slot.remainingTrainFrames = 600;

    auto observer = observerGoal();
    observer.timing = assessCriticalGoalTiming(
        state, observer, state.frame + 100,
        CriticalReservationReason::detection);
    check(observer.timing.active() && !observer.timing.feasible &&
              observer.timing.slackFrames < 0,
          "a busy detector producer is explicitly marked as missing its checkpoint");

    StrategicPlan plan;
    plan.desiredWorkers = 2;
    plan.goals.push_back(observer);
    ProductionGoal worker;
    worker.goal = GoalKind::train;
    worker.target = UnitKind::probe;
    worker.desiredCount = 2;
    worker.priority = 100;
    worker.blocking = true;
    worker.reason = "maintain worker continuity";
    plan.goals.push_back(worker);
    ResourceLedger bank{50, 0};
    const auto actions = MacroPlanner{}.reconcile(state, plan, bank);

    check(std::ranges::any_of(actions, [](const MacroAction& action) {
              return action.target == UnitKind::probe && action.reserved;
          }),
          "an infeasible detection reservation leaves enough minerals for the next Probe");
    check(observer.priority == 124 && observer.blocking,
          "deadline downgrade is local to the current planning pass");
}

void testUnusableProducerIsNotEstimatedAsReady() {
    auto state = detectionState();
    state.self.units[4].powered = false;
    const auto timing = assessCriticalGoalTiming(
        state, observerGoal(), state.frame + 2'000,
        CriticalReservationReason::detection);
    check(timing.active() && !timing.feasible && timing.expectedReadyFrame < 0,
          "an unpowered Robotics Facility is not treated as a feasible detector producer");
}

void testRangeUpgradeHasMeasuredDuration() {
    GameState state;
    state.frame = 4'000;
    state.self.race = Race::protoss;
    state.self.units = {
        unit(1, UnitKind::cyberneticsCore),
        unit(2, UnitKind::probe),
    };
    state.self.producerSlots = {
        {1, UnitKind::cyberneticsCore, false, 0, 0, 6, false, false, false, {}},
    };
    ProductionGoal range;
    range.goal = GoalKind::upgrade;
    range.technology = TechnologyKind::singularityCharge;
    range.desiredCount = 1;
    range.priority = 120;
    range.blocking = true;
    const auto duration = technologyStats(range.technology).durationFrames;
    const auto timing = assessCriticalGoalTiming(
        state, range, state.frame + duration + 90,
        CriticalReservationReason::range);

    check(duration > 0 && timing.feasible && timing.slackFrames == 90,
          "Singularity Charge range timing includes its observed research duration");
}

void testStrategyAnnotatesSelectedDetectionAndRangeGoals() {
    auto detection = detectionState();
    detection.enemy.race = Race::protoss;
    ThreatAssessment cloakThreat;
    cloakThreat.cloak = 0.6;
    cloakThreat.earliestApproachArrivalFrame = detection.frame + 5'000;
    const auto detectionPlan = StrategyEngine{}.plan(detection, cloakThreat);
    const auto observer = std::ranges::find_if(detectionPlan.goals,
        [](const ProductionGoal& goal) { return goal.target == UnitKind::observer; });
    check(observer != detectionPlan.goals.end() && observer->timing.active() &&
              observer->timing.reservationReason == CriticalReservationReason::detection,
          "the strategic cloak response carries a required-by detection checkpoint");

    auto range = detectionState();
    range.enemy.race = Race::protoss;
    range.self.units.push_back(unit(7, UnitKind::cyberneticsCore));
    range.self.units.push_back(unit(8, UnitKind::dragoon));
    range.self.producerSlots.push_back(
        {7, UnitKind::cyberneticsCore, false, 0, 0, 6, false, false, false, {}});
    ThreatAssessment containment;
    containment.staticContain = 0.5;
    containment.earliestApproachArrivalFrame = range.frame + 4'000;
    const auto rangePlan = StrategyEngine{}.plan(range, containment);
    const auto charge = std::ranges::find_if(rangePlan.goals,
        [](const ProductionGoal& goal) {
            return goal.technology == TechnologyKind::singularityCharge;
        });
    check(charge != rangePlan.goals.end() && charge->timing.active() &&
              charge->timing.reservationReason == CriticalReservationReason::range,
          "the strategic ranged-containment response carries a range checkpoint");
}

void testSupplyFallbackCarriesFeasibleTiming() {
    GameState state;
    state.frame = 5'000;
    state.self.race = Race::protoss;
    state.self.supplyUsed = 26;
    state.self.supplyTotal = 34;
    state.self.minerals = 1'000;
    state.self.units = {
        unit(1, UnitKind::nexus),
        unit(2, UnitKind::pylon),
        unit(3, UnitKind::gateway),
        unit(4, UnitKind::gateway),
        unit(5, UnitKind::probe),
    };
    state.self.producerSlots = {
        {1, UnitKind::nexus, false, 0, 0, 6, false, false, false, {}},
        {3, UnitKind::gateway, false, 0, 0, 6, false, false, false, {}},
        {4, UnitKind::gateway, false, 0, 0, 6, false, false, false, {}},
    };
    StrategicPlan plan;
    plan.desiredWorkers = 0;
    plan.composition = {{UnitKind::zealot, 1.0}};
    ResourceLedger bank{1'000, 0};
    const auto actions = MacroPlanner{}.reconcile(state, plan, bank);
    const auto pylon = std::ranges::find_if(actions, [](const MacroAction& action) {
        return action.target == UnitKind::pylon && action.action == MacroActionKind::build;
    });

    check(pylon != actions.end() && pylon->timing.active() && pylon->timing.feasible &&
              pylon->timing.reservationReason == CriticalReservationReason::supply &&
              pylon->timing.slackFrames >= 0,
          "the supply fallback carries a feasible required-by checkpoint to its action");
    check(pylon != actions.end() && pylon->priority > 110,
          "a feasible supply deadline receives only a temporary priority adjustment");
}

void testMissedSupplyDeadlineDoesNotReserveAWorkerCycle() {
    GameState state;
    state.frame = 5'000;
    state.self.race = Race::protoss;
    state.self.supplyUsed = 32;
    state.self.supplyTotal = 34;
    state.self.minerals = 50;
    state.self.units = {
        unit(1, UnitKind::nexus),
        unit(2, UnitKind::pylon),
        unit(3, UnitKind::gateway),
        unit(4, UnitKind::gateway),
        unit(5, UnitKind::probe),
    };
    state.self.producerSlots = {
        {1, UnitKind::nexus, false, 0, 0, 6, false, false, false, {}},
        {3, UnitKind::gateway, false, 0, 0, 6, false, false, false, {}},
        {4, UnitKind::gateway, false, 0, 0, 6, false, false, false, {}},
    };
    StrategicPlan plan;
    plan.desiredWorkers = 2;
    plan.composition = {{UnitKind::zealot, 1.0}};
    ProductionGoal worker;
    worker.goal = GoalKind::train;
    worker.target = UnitKind::probe;
    worker.desiredCount = 2;
    worker.priority = 100;
    worker.blocking = true;
    worker.reason = "maintain worker continuity";
    plan.goals.push_back(worker);
    ResourceLedger bank{50, 0};
    const auto actions = MacroPlanner{}.reconcile(state, plan, bank);
    const auto pylon = std::ranges::find_if(actions, [](const MacroAction& action) {
        return action.target == UnitKind::pylon && action.action == MacroActionKind::build;
    });

    check(pylon != actions.end() && pylon->timing.active() && !pylon->timing.feasible &&
              !pylon->reserved && pylon->blocksLowerPriority,
          "a missed supply checkpoint remains actionable without reserving an unaffordable Pylon");
    check(std::ranges::any_of(actions, [](const MacroAction& action) {
              return action.target == UnitKind::probe && action.reserved;
          }),
          "a missed supply checkpoint leaves the available worker cycle funded");
}

}  // namespace

int main() {
    testDetectionDeadlineFeasibilityAndSlack();
    testMissedDeadlineDoesNotFreezeWorkerContinuity();
    testUnusableProducerIsNotEstimatedAsReady();
    testRangeUpgradeHasMeasuredDuration();
    testStrategyAnnotatesSelectedDetectionAndRangeGoals();
    testSupplyFallbackCarriesFeasibleTiming();
    testMissedSupplyDeadlineDoesNotReserveAWorkerCycle();
    return failures == 0 ? 0 : 1;
}
