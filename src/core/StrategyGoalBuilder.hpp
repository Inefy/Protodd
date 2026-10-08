#pragma once

#include "protodd/Strategy.hpp"

#include <optional>
#include <string_view>

namespace protodd::strategy_detail {

[[nodiscard]] std::optional<ProductionGoal> makeUnitGoal(
    GoalKind kind,
    UnitKind target,
    int desiredCount,
    int priority,
    std::string_view reason,
    bool blocking = false);

[[nodiscard]] bool appendUnitGoal(
    StrategicPlan& plan,
    GoalKind kind,
    UnitKind target,
    int desiredCount,
    int priority,
    std::string_view reason,
    bool blocking = false);

[[nodiscard]] bool appendTechnologyGoal(
    StrategicPlan& plan,
    TechnologyKind technology,
    int desiredLevel,
    int priority,
    std::string_view reason,
    bool blocking = false);

}  // namespace protodd::strategy_detail
