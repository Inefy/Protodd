#include "astra/MacroPlanner.hpp"

#include "astra/Technology.hpp"
#include "astra/UnitCatalog.hpp"

#include <algorithm>
#include <limits>
#include <unordered_map>

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
    std::unordered_map<UnitKind, int> planned;

    for (const auto& goal : plan.goals) {
        if (goal.technology != TechnologyKind::none) {
            const auto& stats = technologyStats(goal.technology);
            const auto currentLevel = technologyLevel(state.self, goal.technology);
            const auto desiredLevel = std::clamp(goal.desiredCount, 1, stats.maximumLevel);
            if (currentLevel >= desiredLevel ||
                technologyInProgress(state.self, goal.technology)) {
                continue;
            }

            const auto nextLevel = currentLevel + 1;
            const auto minerals = stats.mineralCost(nextLevel);
            const auto gas = stats.gasCost(nextLevel);
            if (countCompleted(state, stats.producer) == 0) {
                const auto nested = nextMissingPrerequisite(state, stats.producer);
                const auto prerequisite = nested != UnitKind::unknown ? nested : stats.producer;
                if (countExisting(state, prerequisite) + planned[prerequisite] == 0) {
                    const auto& unit = unitStats(prerequisite);
                    MacroAction action{
                        MacroActionKind::build, prerequisite, goal.priority,
                        unit.minerals, unit.gas, false,
                        "unlock " + std::string(stats.name), TechnologyKind::none,
                        goal.blocking,
                    };
                    action.reserved = ledger.reserve(unit.minerals, unit.gas);
                    actions.push_back(std::move(action));
                    if (actions.back().reserved) ++planned[prerequisite];
                    if (goal.blocking && !actions.back().reserved) break;
                } else if (goal.blocking) {
                    MacroAction waiting{
                        stats.research ? MacroActionKind::research
                                       : MacroActionKind::upgrade,
                        UnitKind::unknown, goal.priority, minerals, gas, false,
                        goal.reason, goal.technology, true,
                    };
                    waiting.reserved = ledger.reserve(minerals, gas);
                    actions.push_back(std::move(waiting));
                    break;
                }
                continue;
            }

            MacroAction action{
                stats.research ? MacroActionKind::research : MacroActionKind::upgrade,
                UnitKind::unknown, goal.priority, minerals, gas, false,
                goal.reason, goal.technology, goal.blocking,
            };
            action.reserved = ledger.reserve(minerals, gas);
            if (action.reserved || goal.blocking) actions.push_back(std::move(action));
            if (goal.blocking && !actions.back().reserved) break;
            continue;
        }

        const auto existing = countExisting(state, goal.target) + planned[goal.target];
        if (existing >= goal.desiredCount) {
            continue;
        }
        if (!prerequisitesMet(state, goal.target)) {
            const auto prerequisite = nextMissingPrerequisite(state, goal.target);
            if (prerequisite != UnitKind::unknown &&
                countExisting(state, prerequisite) + planned[prerequisite] == 0) {
                const auto& stats = unitStats(prerequisite);
                MacroAction action{
                    MacroActionKind::build, prerequisite, goal.priority,
                    stats.minerals, stats.gas, false,
                    "unlock " + std::string(unitStats(goal.target).name),
                    TechnologyKind::none, goal.blocking,
                };
                action.reserved = ledger.reserve(stats.minerals, stats.gas);
                if (action.reserved || goal.blocking) {
                    actions.push_back(std::move(action));
                    if (actions.back().reserved) ++planned[prerequisite];
                }
                if (goal.blocking && !actions.back().reserved) break;
            } else if (goal.blocking) {
                // The prerequisite already exists but is incomplete. Reserve
                // the target now so routine production cannot drain its bank
                // before the structure finishes.
                const auto& target = unitStats(goal.target);
                MacroAction waiting{
                    actionKind(goal.goal), goal.target, goal.priority,
                    target.minerals, target.gas, false, goal.reason,
                    TechnologyKind::none, true,
                };
                waiting.reserved = ledger.reserve(target.minerals, target.gas);
                actions.push_back(std::move(waiting));
                break;
            }
            continue;
        }

        const auto& stats = unitStats(goal.target);
        MacroAction action{
            actionKind(goal.goal), goal.target, goal.priority,
            stats.minerals, stats.gas, false, goal.reason,
            TechnologyKind::none, goal.blocking,
        };
        action.reserved = ledger.reserve(stats.minerals, stats.gas);
        if (action.reserved || goal.blocking) {
            actions.push_back(std::move(action));
            if (actions.back().reserved) ++planned[goal.target];
        }
        // A blocking goal owns the economy until it is affordable. Letting
        // cheaper goals spend around it can delay emergency supply or detection
        // forever under continuous production.
        if (goal.blocking && !actions.back().reserved) {
            break;
        }
    }

    // Spend remaining resources toward the strategic composition rather than
    // stopping at the opening's fixed unit counts. Select the most
    // underrepresented currently-producible unit for one production cycle.
    UnitKind compositionChoice = UnitKind::unknown;
    auto largestDeficit = -std::numeric_limits<double>::infinity();
    auto armyCount = 0;
    for (const auto& target : plan.composition) {
        armyCount += countExisting(state, target.kind) + planned[target.kind];
    }
    for (const auto& target : plan.composition) {
        const auto& stats = unitStats(target.kind);
        const auto directlyProducible = !stats.building && stats.minerals + stats.gas > 0;
        if (!directlyProducible || !prerequisitesMet(state, target.kind) ||
            std::ranges::any_of(actions, [&target](const MacroAction& action) {
                return action.target == target.kind;
            })) {
            continue;
        }
        const auto count = countExisting(state, target.kind) + planned[target.kind];
        const auto desired = target.weight * static_cast<double>(armyCount + 1);
        const auto deficit = desired - static_cast<double>(count);
        if (deficit > largestDeficit) {
            largestDeficit = deficit;
            compositionChoice = target.kind;
        }
    }
    if (compositionChoice != UnitKind::unknown) {
        const auto& stats = unitStats(compositionChoice);
        const auto supplyAvailable = state.self.supplyUsed + stats.supply <= state.self.supplyTotal;
        if (supplyAvailable && ledger.reserve(stats.minerals, stats.gas)) {
            actions.push_back({MacroActionKind::train, compositionChoice, 58,
                               stats.minerals, stats.gas, true,
                               "maintain strategic army composition"});
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
    const auto units = std::ranges::count(state.self.units, kind, &UnitSnapshot::kind);
    const auto queued = std::ranges::count(state.self.queuedUnits, kind);
    return static_cast<int>(units) + static_cast<int>(queued);
}

int MacroPlanner::countCompleted(const GameState& state, const UnitKind kind) {
    return static_cast<int>(std::ranges::count_if(
        state.self.units,
        [kind](const UnitSnapshot& unit) { return unit.kind == kind && unit.completed; }));
}

bool MacroPlanner::prerequisitesMet(const GameState& state, const UnitKind kind) {
    return std::ranges::all_of(unitPrerequisites(kind), [&state](const UnitKind prerequisite) {
        return countCompleted(state, prerequisite) > 0;
    });
}

UnitKind MacroPlanner::nextMissingPrerequisite(
    const GameState& state,
    const UnitKind kind) {
    for (const auto prerequisite : unitPrerequisites(kind)) {
        if (countCompleted(state, prerequisite) > 0) continue;
        const auto nested = nextMissingPrerequisite(state, prerequisite);
        return nested != UnitKind::unknown ? nested : prerequisite;
    }
    return UnitKind::unknown;
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
