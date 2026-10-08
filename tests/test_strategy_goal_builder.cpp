#include "StrategyGoalBuilder.hpp"

#include <iostream>
#include <string_view>

namespace {

using namespace protodd;
using namespace protodd::strategy_detail;
int failures{};

void check(const bool condition, const std::string_view message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

}  // namespace

int main() {
    const auto gateway = makeUnitGoal(
        GoalKind::build, UnitKind::gateway, 2, 90, "production", true);
    check(gateway && gateway->goal == GoalKind::build &&
              gateway->target == UnitKind::gateway && gateway->desiredCount == 2 &&
              gateway->priority == 90 && gateway->blocking &&
              gateway->reason == "production",
          "unit builder preserves the requested fields");
    check(!makeUnitGoal(GoalKind::build, UnitKind::zealot, 1, 90, "invalid"),
          "build goals reject unit targets");
    check(!makeUnitGoal(GoalKind::train, UnitKind::gateway, 1, 90, "invalid"),
          "train goals reject building targets");
    check(!makeUnitGoal(GoalKind::expand, UnitKind::gateway, 1, 90, "invalid"),
          "expansion goals require a Nexus target");
    check(!makeUnitGoal(GoalKind::build, UnitKind::unknown, 1, 90, "invalid"),
          "unit goals reject unknown targets");
    check(!makeUnitGoal(GoalKind::train, static_cast<UnitKind>(65535), 1, 90, "invalid"),
          "unit goals reject out-of-range target values");
    check(!makeUnitGoal(static_cast<GoalKind>(255), UnitKind::zealot, 1, 90, "invalid"),
          "unit goals reject out-of-range goal values");
    check(!makeUnitGoal(GoalKind::train, UnitKind::zealot, -1, 90, "invalid"),
          "unit goals reject negative desired counts");

    StrategicPlan plan;
    check(appendTechnologyGoal(plan, TechnologyKind::psionicStorm, 1, 80,
                               "storm", false),
          "technology builder accepts a valid research goal");
    check(plan.goals.size() == 1 && plan.goals.front().goal == GoalKind::research,
          "research technology maps to the research goal kind");
    check(appendTechnologyGoal(plan, TechnologyKind::psionicStorm, 2, 90,
                               "urgent storm", true),
          "technology builder merges repeated requests");
    check(plan.goals.size() == 1 && plan.goals.front().desiredCount == 2 &&
              plan.goals.front().priority == 90 && plan.goals.front().blocking &&
              plan.goals.front().reason == "urgent storm",
          "technology merge retains established priority, reason, and blocking rules");
    check(appendTechnologyGoal(plan, TechnologyKind::protossGroundWeapons, 1, 70,
                               "weapons"),
          "technology builder accepts a valid upgrade goal");
    check(plan.goals.size() == 2 && plan.goals.back().goal == GoalKind::upgrade,
          "upgrade technology maps to the upgrade goal kind");
    check(!appendTechnologyGoal(plan, TechnologyKind::none, 1, 70, "invalid") &&
              !appendTechnologyGoal(plan, static_cast<TechnologyKind>(255), 1, 70, "invalid") &&
              !appendTechnologyGoal(plan, TechnologyKind::psionicStorm, 0, 70, "invalid"),
          "technology builder rejects invalid technology and nonpositive levels");

    if (failures == 0) {
        std::cout << "Strategy goal builder checks passed\n";
        return 0;
    }
    std::cerr << failures << " strategy goal builder check(s) failed\n";
    return 1;
}
