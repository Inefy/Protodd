#include "astra/MacroPlanner.hpp"

#include "astra/UnitCatalog.hpp"

#include <algorithm>

namespace astra {

bool ResourceLedger::canReserve(const int mineralCost, const int gasCost) const noexcept {
    return freeMinerals() >= mineralCost && freeGas() >= gasCost;
}

bool ResourceLedger::reserve(const int mineralCost, const int gasCost) noexcept {
    if (!canReserve(mineralCost, gasCost)) {
        return false;
    }
    reservedMinerals += mineralCost;
    reservedGas += gasCost;
    return true;
}

std::vector<MacroAction> MacroPlanner::reconcile(
    const GameState& state,
    const StrategicPlan& plan,
    ResourceLedger& ledger) const {
    std::vector<MacroAction> actions;
    actions.reserve(plan.goals.size());

    for (const auto& goal : plan.goals) {
        const auto existing = countExisting(state, goal.target);
        if (existing >= goal.desiredCount) {
            continue;
        }

        const auto& stats = unitStats(goal.target);
        MacroAction action{
            actionKind(goal.goal), goal.target, goal.priority,
            stats.minerals, stats.gas, false, goal.reason,
        };
        action.reserved = ledger.reserve(stats.minerals, stats.gas);
        if (action.reserved || goal.blocking) {
            actions.push_back(std::move(action));
        }
        // A blocking goal owns the economy until it is affordable. Letting
        // cheaper goals spend around it can delay emergency supply or detection
        // forever under continuous production.
        if (goal.blocking && !actions.back().reserved) {
            break;
        }
    }

    std::ranges::stable_sort(actions, [](const MacroAction& left, const MacroAction& right) {
        if (left.reserved != right.reserved) {
            return left.reserved > right.reserved;
        }
        return left.priority > right.priority;
    });
    return actions;
}

int MacroPlanner::countExisting(const GameState& state, const UnitKind kind) {
    return static_cast<int>(std::ranges::count(state.self.units, kind, &UnitSnapshot::kind));
}

MacroActionKind MacroPlanner::actionKind(const GoalKind goal) noexcept {
    switch (goal) {
        case GoalKind::build: return MacroActionKind::build;
        case GoalKind::train: return MacroActionKind::train;
        case GoalKind::expand: return MacroActionKind::expand;
        case GoalKind::detect: return MacroActionKind::build;
        case GoalKind::research: return MacroActionKind::research;
        case GoalKind::upgrade: return MacroActionKind::upgrade;
    }
    return MacroActionKind::train;
}

}  // namespace astra
