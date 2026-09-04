#include "astra/MacroPlanner.hpp"

#include "astra/Technology.hpp"
#include "astra/UnitCatalog.hpp"

#include <algorithm>
#include <unordered_map>

namespace astra {
namespace {

UnitKind producerFor(const UnitKind kind) noexcept {
    switch (kind) {
        case UnitKind::probe: return UnitKind::nexus;
        case UnitKind::zealot:
        case UnitKind::dragoon:
        case UnitKind::highTemplar:
        case UnitKind::darkTemplar: return UnitKind::gateway;
        case UnitKind::reaver:
        case UnitKind::observer:
        case UnitKind::shuttle: return UnitKind::roboticsFacility;
        case UnitKind::scout:
        case UnitKind::corsair:
        case UnitKind::carrier: return UnitKind::stargate;
        default: return UnitKind::unknown;
    }
}

int queuedForProducer(const GameState& state, const UnitKind producer) {
    const auto queued = static_cast<int>(std::ranges::count_if(
        state.self.queuedUnits, [producer](const UnitKind queued) {
            return producerFor(queued) == producer;
        }));
    const auto busyWithoutQueue = static_cast<int>(std::ranges::count(
        state.self.busyProducers, producer));
    return queued + busyWithoutQueue;
}

}  // namespace

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

void ResourceLedger::protect(const int mineralCost, const int gasCost) noexcept {
    reservedMinerals += std::min(std::max(0, mineralCost),
                                 std::max(0, freeMinerals()));
    reservedGas += std::min(std::max(0, gasCost), std::max(0, freeGas()));
}

std::vector<MacroAction> MacroPlanner::reconcile(
    const GameState& state,
    const StrategicPlan& plan,
    ResourceLedger& ledger) const {
    std::vector<MacroAction> actions;
    std::vector<ProductionGoal> goals = plan.goals;
    actions.reserve(goals.size() + 1U);
    std::unordered_map<UnitKind, int> planned;

    // Supply is an operational invariant, not merely a strategic preference.
    // If a future strategy accidentally omits its pylon goal, the macro layer
    // still protects the game from a complete production deadlock.
    if (state.self.race == Race::protoss && state.self.supplyTotal > 0 &&
        state.self.supplyTotal < 400) {
        const auto pylons = countExisting(state, UnitKind::pylon);
        const auto pylonInProgress = std::ranges::any_of(
            state.self.units, [](const UnitSnapshot& unit) {
                return unit.kind == UnitKind::pylon && !unit.completed;
            });
        const auto activeProducers = countCompleted(state, UnitKind::nexus) +
                                     countCompleted(state, UnitKind::gateway) +
                                     countCompleted(state, UnitKind::roboticsFacility) +
                                     countCompleted(state, UnitKind::stargate);
        const auto safetyMargin = std::clamp(2 + activeProducers * 2, 4, 16);
        const auto remaining = state.self.supplyTotal - state.self.supplyUsed;
        const auto openingDeadline = pylons == 0 && state.self.supplyUsed >= 12;
        if (!pylonInProgress && (openingDeadline || remaining <= safetyMargin)) {
            goals.push_back({GoalKind::build, UnitKind::pylon, pylons + 1, 110, true,
                             "operational supply invariant"});
        }
    }
    std::ranges::stable_sort(goals, std::greater{}, &ProductionGoal::priority);

    for (const auto& goal : goals) {
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
                    ++planned[prerequisite];
                    if (goal.blocking && !actions.back().reserved) {
                        ledger.protect(unit.minerals, unit.gas);
                    }
                } else if (goal.blocking) {
                    MacroAction waiting{
                        stats.research ? MacroActionKind::research
                                       : MacroActionKind::upgrade,
                        UnitKind::unknown, goal.priority, minerals, gas, false,
                        goal.reason, goal.technology, true,
                    };
                    waiting.reserved = ledger.reserve(minerals, gas);
                    waiting.executable = false;
                    actions.push_back(std::move(waiting));
                    if (!actions.back().reserved) ledger.protect(minerals, gas);
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
            if (goal.blocking && !actions.back().reserved) {
                ledger.protect(minerals, gas);
            }
            continue;
        }

        const auto existing = countExisting(state, goal.target) + planned[goal.target];
        if (existing >= goal.desiredCount) {
            continue;
        }
        if (goal.goal == GoalKind::train) {
            const auto producer = producerFor(goal.target);
            const auto producerCount = countCompleted(state, producer);
            if (producer != UnitKind::unknown && producerCount > 0 &&
                queuedForProducer(state, producer) >= producerCount) {
                // A queued train action is already keeping every producer
                // occupied. Do not reserve a second layer of queue entries and
                // starve structures that increase actual throughput.
                continue;
            }
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
                    ++planned[prerequisite];
                }
                if (goal.blocking && !actions.back().reserved) {
                    ledger.protect(stats.minerals, stats.gas);
                }
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
                waiting.executable = false;
                actions.push_back(std::move(waiting));
                if (!actions.back().reserved) {
                    ledger.protect(target.minerals, target.gas);
                }
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
            ++planned[goal.target];
        }
        // Protect each resource independently. A gas-starved upgrade can keep
        // its mineral bank without freezing probes or other mineral-only work
        // paid for from the true surplus.
        if (goal.blocking && !actions.back().reserved) {
            ledger.protect(stats.minerals, stats.gas);
        }
    }

    // Spend remaining resources toward the strategic composition rather than
    // stopping at the opening's fixed unit counts. Allocate every genuinely
    // idle producer once; issuing only one composition action per planning
    // pass left large gateway economies needlessly idle with a full bank.
    struct CompositionCandidate {
        UnitKind kind{UnitKind::unknown};
        UnitKind producer{UnitKind::unknown};
        double deficit{};
    };
    std::unordered_map<UnitKind, int> openProducerSlots;
    for (const auto& target : plan.composition) {
        const auto producer = producerFor(target.kind);
        if (producer == UnitKind::unknown || openProducerSlots.contains(producer)) continue;
        openProducerSlots[producer] = std::max(
            0, countCompleted(state, producer) - queuedForProducer(state, producer));
    }
    for (const auto& action : actions) {
        if (!action.reserved || action.action != MacroActionKind::train) continue;
        const auto producer = producerFor(action.target);
        if (const auto slot = openProducerSlots.find(producer);
            slot != openProducerSlots.end()) {
            slot->second = std::max(0, slot->second - 1);
        }
    }

    auto armyCount = 0;
    for (const auto& target : plan.composition) {
        armyCount += countExisting(state, target.kind) + planned[target.kind];
    }
    auto plannedSupply = state.self.supplyUsed;
    for (const auto& action : actions) {
        if (action.reserved && action.action == MacroActionKind::train) {
            plannedSupply += unitStats(action.target).supply;
        }
    }
    constexpr auto maximumCompositionActions = 8;
    for (auto cycle = 0; cycle < maximumCompositionActions; ++cycle) {
        std::vector<CompositionCandidate> candidates;
        for (const auto& target : plan.composition) {
            const auto& stats = unitStats(target.kind);
            const auto producer = producerFor(target.kind);
            const auto directlyProducible = !stats.building &&
                                            stats.minerals + stats.gas > 0;
            const auto supplyAvailable = state.self.supplyTotal <= 0 ||
                                         plannedSupply + stats.supply <=
                                             state.self.supplyTotal;
            if (!directlyProducible || producer == UnitKind::unknown ||
                openProducerSlots[producer] <= 0 ||
                !prerequisitesMet(state, target.kind) || !supplyAvailable ||
                !ledger.canReserve(stats.minerals, stats.gas)) {
                continue;
            }
            const auto current = countExisting(state, target.kind) + planned[target.kind];
            const auto desired = target.weight * static_cast<double>(armyCount + 1);
            candidates.push_back({target.kind, producer,
                                  desired - static_cast<double>(current)});
        }
        if (candidates.empty()) break;
        const auto best = std::ranges::max_element(
            candidates, {}, &CompositionCandidate::deficit);
        const auto& stats = unitStats(best->kind);
        if (!ledger.reserve(stats.minerals, stats.gas)) break;
        actions.push_back({MacroActionKind::train, best->kind, 58,
                           stats.minerals, stats.gas, true,
                           "fill idle production with strategic composition"});
        ++planned[best->kind];
        ++armyCount;
        plannedSupply += stats.supply;
        --openProducerSlots[best->producer];
        if (std::ranges::none_of(openProducerSlots, [](const auto& entry) {
                return entry.second > 0;
            })) {
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
        // Construction has already been paid for. Continue walking the direct
        // requirements so the ledger can fund the next structure in the chain
        // rather than reserving an impossible end-unit behind this one.
        if (countExisting(state, prerequisite) > 0) continue;
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
