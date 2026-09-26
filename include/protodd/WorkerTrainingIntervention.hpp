#pragma once

#include "protodd/Strategy.hpp"
#include <algorithm>

namespace protodd {

enum class WorkerTrainingProfile { baseline, plusOne, plusTwo };

struct WorkerTrainingDecision {
    bool applied{};
    int committed{};
    int beforeGoal{};
    int afterGoal{};
    int beforePriority{};
    int afterPriority{};
};

// Bounded experimental treatment. No model weights, replay targets, or hidden
// enemy state enter this rule. The normal plan handles all emergency decisions.
inline WorkerTrainingDecision applyWorkerTrainingIntervention(
    StrategicPlan& plan, const GameState& state, WorkerTrainingProfile profile) {
    WorkerTrainingDecision result;
    if (profile == WorkerTrainingProfile::baseline || state.self.race != Race::protoss ||
        state.frame < 2400 || state.frame >= 7200 || plan.prioritizeReinforcements ||
        plan.posture == Posture::defend || plan.posture == Posture::recover ||
        plan.requireMobileDetection) return result;

    int bases{};
    for (const auto& unit : state.self.units) {
        if (unit.kind == UnitKind::nexus) ++bases;
        if (unit.kind == UnitKind::probe) ++result.committed;
    }
    // A low worker target can be a deliberate rush-defense constraint.
    if (bases < 1 || plan.desiredWorkers < 16) return result;
    for (const auto kind : state.self.queuedUnits)
        if (kind == UnitKind::probe) ++result.committed;

    const int extra = profile == WorkerTrainingProfile::plusOne ? 1 : 2;
    const int cap = std::min(32, 24 + (bases - 1) * 8);
    if (result.committed >= cap) return result;
    const int desired = std::min(cap, std::max(plan.desiredWorkers + extra, result.committed + extra));
    const int priority = profile == WorkerTrainingProfile::plusOne ? 100 : 108;
    result.beforeGoal = plan.desiredWorkers;
    result.afterGoal = std::max(plan.desiredWorkers, desired);
    plan.desiredWorkers = result.afterGoal;
    for (auto& goal : plan.goals) {
        if (goal.goal != GoalKind::train || goal.target != UnitKind::probe) continue;
        result.beforePriority = goal.priority;
        goal.desiredCount = std::max(goal.desiredCount, result.afterGoal);
        goal.priority = std::max(goal.priority, priority);
        result.afterPriority = goal.priority;
        result.applied = result.afterGoal > result.beforeGoal ||
                         result.afterPriority > result.beforePriority;
        return result;
    }
    result.afterPriority = priority;
    plan.goals.push_back({GoalKind::train, UnitKind::probe, result.afterGoal,
                          priority, false, "bounded worker training intervention"});
    result.applied = true;
    return result;
}

}  // namespace protodd
