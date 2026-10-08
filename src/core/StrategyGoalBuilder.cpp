#include "StrategyGoalBuilder.hpp"

#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace protodd::strategy_detail {

std::optional<ProductionGoal> makeUnitGoal(
    const GoalKind kind,
    const UnitKind target,
    const int desiredCount,
    const int priority,
    const std::string_view reason,
    const bool blocking) {
    if (target == UnitKind::unknown || target >= UnitKind::count ||
        desiredCount < 0) return std::nullopt;

    switch (kind) {
        case GoalKind::build:
            if (!isBuilding(target)) return std::nullopt;
            break;
        case GoalKind::train:
            if (isBuilding(target)) return std::nullopt;
            break;
        case GoalKind::expand:
            if (target != UnitKind::nexus) return std::nullopt;
            break;
        case GoalKind::detect:
        case GoalKind::research:
        case GoalKind::upgrade:
            return std::nullopt;
        default:
            return std::nullopt;
    }

    return ProductionGoal{kind, target, desiredCount, priority, blocking,
                          std::string(reason)};
}

bool appendUnitGoal(
    StrategicPlan& plan,
    const GoalKind kind,
    const UnitKind target,
    const int desiredCount,
    const int priority,
    const std::string_view reason,
    const bool blocking) {
    auto candidate = makeUnitGoal(kind, target, desiredCount, priority, reason, blocking);
    if (!candidate) return false;
    plan.goals.push_back(std::move(*candidate));
    return true;
}

bool appendTechnologyGoal(
    StrategicPlan& plan,
    const TechnologyKind technology,
    const int desiredLevel,
    const int priority,
    const std::string_view reason,
    const bool blocking) {
    if (technology == TechnologyKind::none || technology >= TechnologyKind::count ||
        desiredLevel < 1) return false;

    const auto kind = technology == TechnologyKind::psionicStorm ||
                              technology == TechnologyKind::stasisField ||
                              technology == TechnologyKind::recall
                          ? GoalKind::research
                          : GoalKind::upgrade;
    const auto existing = std::ranges::find(
        plan.goals, technology, &ProductionGoal::technology);
    if (existing != plan.goals.end()) {
        existing->desiredCount = std::max(existing->desiredCount, desiredLevel);
        if (priority > existing->priority) existing->reason = reason;
        existing->priority = std::max(existing->priority, priority);
        existing->blocking = existing->blocking || blocking;
        return true;
    }

    plan.goals.push_back({kind, UnitKind::unknown, desiredLevel, priority, blocking,
                          std::string(reason), technology});
    return true;
}

}  // namespace protodd::strategy_detail
