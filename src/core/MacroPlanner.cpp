#include "protodd/MacroPlanner.hpp"

#include "protodd/Technology.hpp"
#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace protodd {
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
        case UnitKind::carrier:
        case UnitKind::arbiter: return UnitKind::stargate;
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

int usableProducers(const GameState& state, const UnitKind kind) {
    return static_cast<int>(std::ranges::count_if(state.self.units,
        [kind](const UnitSnapshot& unit) {
            return unit.kind == kind && unit.completed && !unit.disabled &&
                   !unit.loaded && !unit.hallucination &&
                   (!unitStats(kind).requiresPsi || unit.powered);
        }));
}

// A paid-for prerequisite is a future deadline, not a reason to freeze every
// currently usable producer for its entire construction time.
int prerequisiteWait(const GameState& state, const UnitKind kind) {
    auto wait = 0;
    for (const auto prerequisite : unitPrerequisites(kind)) {
        auto earliest = std::numeric_limits<int>::max();
        for (const auto& unit : state.self.units) {
            if (unit.kind != prerequisite) continue;
            const auto remaining = unit.completed ? 0 :
                unitStats(prerequisite).buildTime *
                    (100 - std::clamp(unit.buildProgress, 0, 100)) / 100;
            earliest = std::min(earliest, remaining);
        }
        if (earliest != std::numeric_limits<int>::max()) wait = std::max(wait, earliest);
        else wait = std::max(wait, prerequisiteWait(state, prerequisite));
    }
    return wait;
}

}  // namespace

bool trainingSlotAvailable(const bool active, const int queueSize,
                           const int remainingFrames, const int latencyFrames,
                           const bool recentTrainCommand) noexcept {
    if (recentTrainCommand) return false;
    if (!active && queueSize == 0) return true;
    return queueSize == 1 && remainingFrames > 0 &&
           remainingFrames <= std::max(0, latencyFrames);
}

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

    // A non-monotonic frame is a new synthetic game/test context (and the
    // bridge's onStart also resets this in live play). Never carry a stale
    // structure demand across that boundary. Macro reconciliation itself is
    // latency-aware and runs on increasing frames, so an equal frame is safe
    // to treat as a fresh snapshot as well.
    if (lastFrame_ >= state.frame) pendingGoals_.clear();
    lastFrame_ = state.frame;

    // Keep only a bounded, actionable history.  Existing structures clear the
    // demand immediately; otherwise it survives brief scouting/plan churn.
    std::erase_if(pendingGoals_, [&state](const PendingGoal& pending) {
        const auto existing = countExisting(state, pending.target);
        return existing >= pending.desiredCount || pending.lastRequested < 0 ||
               state.frame < pending.lastRequested ||
               state.frame - pending.lastRequested > 30 * 24;
    });
    // An explicit lower/cancelled demand supersedes memory. Expansions must
    // obey today's safety decision immediately, even during the grace period.
    std::erase_if(pendingGoals_, [&plan](const PendingGoal& pending) {
        return (pending.goal == GoalKind::expand &&
                (plan.desiredBases < pending.desiredCount ||
                 std::ranges::none_of(plan.goals, [&pending](const ProductionGoal& goal) {
                     return goal.goal == GoalKind::expand && goal.blocking &&
                            goal.desiredCount >= pending.desiredCount;
                 }))) ||
               std::ranges::any_of(plan.goals, [&pending](const ProductionGoal& goal) {
                   return goal.goal == pending.goal && goal.target == pending.target &&
                          (!goal.blocking || goal.desiredCount < pending.desiredCount);
               });
    });
    for (const auto& pending : pendingGoals_) {
        const auto alreadyRequested = std::ranges::any_of(
            goals, [&pending](const ProductionGoal& candidate) {
                return candidate.goal == pending.goal &&
                       candidate.target == pending.target;
            });
        if (alreadyRequested) continue;
        goals.push_back({pending.goal, pending.target, pending.desiredCount,
                         pending.priority, true, pending.reason});
    }

    // Strategy is assembled from several independent signals (opening
    // recognizer, emergency response, infrastructure, and learned style).
    // Coalesce equivalent structure/technology requests before reservation.
    // Training demand is limited separately by available producer slots.
    std::vector<ProductionGoal> mergedGoals;
    mergedGoals.reserve(goals.size());
    for (const auto& candidate : goals) {
        // Train goals are intentionally not merged: one demand can fill one
        // idle Gateway/Nexus, while a second independent train goal can fill a
        // second producer in the same pass.  Structure/expansion/research
        // requests, on the other hand, have a single strategic checkpoint and
        // must be coalesced to avoid duplicate buildings.
        const auto existing = candidate.goal == GoalKind::train
                                  ? mergedGoals.end()
                                  : std::ranges::find_if(
                                        mergedGoals, [&candidate](const ProductionGoal& prior) {
                                            return prior.goal == candidate.goal &&
                                                   prior.target == candidate.target &&
                                                   prior.technology == candidate.technology;
                                        });
        if (existing == mergedGoals.end()) {
            mergedGoals.push_back(candidate);
            continue;
        }
        if (candidate.priority > existing->priority) {
            existing->reason = candidate.reason;
        }
        existing->desiredCount = std::max(existing->desiredCount,
                                          candidate.desiredCount);
        existing->priority = std::max(existing->priority, candidate.priority);
        existing->blocking = existing->blocking || candidate.blocking;
    }
    goals = std::move(mergedGoals);
    std::unordered_map<UnitKind, int> planned;
    std::unordered_map<UnitKind, int> committedProducers;
    // A blocking Pylon is a supply deadline, not a license to idle the
    // Nexus.  Before the game is within four supply of the cap, let a Probe
    // spend an otherwise-unaffordable Pylon's current mineral shortfall and
    // start the Pylon on the next pass.  This mirrors Stardust's forward
    // resource schedule: protect a future deadline without sacrificing
    // continuous worker production in the present frame.
    const auto protectBlocking = [&ledger, &state](
                                    const UnitKind target,
                                    const int minerals,
                                    const int gas,
                                    const int priority) {
        const auto supplyRoom = state.self.supplyTotal - state.self.supplyUsed;
        if (target == UnitKind::pylon && supplyRoom > 4) return;
        // A high-priority Forge/Cannon in an active emergency is a hard
        // checkpoint, not a long-horizon reservation.  Leaving the worker
        // cycle available here lets a Probe or Battery consume the exact
        // minerals needed to cross the 150-mineral Forge threshold, so the
        // plan can wait forever while the melee wave is already at the main.
        // Reserve the entire current bank for this narrow static anchor;
        // worker production resumes as soon as the structure starts.
        const auto urgentStaticAnchor =
            (target == UnitKind::forge || target == UnitKind::photonCannon) &&
            priority >= 110;
        if (urgentStaticAnchor) {
            ledger.protect(minerals, gas);
            return;
        }
        // A pending expansion or tech chain is a long-horizon reservation.
        // Keeping every currently available mineral behind it can silently
        // stop the worker queue: the worker goal is lower priority, so it
        // never gets a 50-mineral reservation and the income that would
        // complete the transition disappears. Leave one worker cycle
        // available while the bank is below the target cost; once the full
        // cost is actually available the structure can reserve and issue.
        const auto leavesWorkerCycle = target == UnitKind::unknown ||
                                       (target != UnitKind::pylon && isBuilding(target));
        if (leavesWorkerCycle) {
            const auto workerCycle = 50;
            const auto protectable = std::max(0, ledger.freeMinerals() - workerCycle);
            ledger.protect(std::min(minerals, protectable), gas);
            return;
        }
        ledger.protect(minerals, gas);
    };
    auto plannedSupply = state.self.supplyUsed;
    for (const auto& technology : state.self.technologies) {
        if (technology.inProgress) {
            ++committedProducers[technologyStats(technology.kind).producer];
        }
    }

    // Supply is an operational invariant, not merely a strategic preference.
    // If a future strategy accidentally omits its pylon goal, the macro layer
    // still protects the game from a complete production deadlock.
    if (state.self.race == Race::protoss && state.self.supplyTotal > 0 &&
        state.self.supplyTotal < 400) {
        const auto pylons = countExisting(state, UnitKind::pylon);
        const auto pendingPylons = static_cast<int>(std::ranges::count_if(
            state.self.units, [](const UnitSnapshot& unit) {
                return unit.kind == UnitKind::pylon && !unit.completed;
            })) + static_cast<int>(std::ranges::count(state.self.queuedUnits, UnitKind::pylon));
        const auto activeProducers = countCompleted(state, UnitKind::nexus) +
                                     countCompleted(state, UnitKind::gateway) +
                                     countCompleted(state, UnitKind::roboticsFacility) +
                                     countCompleted(state, UnitKind::stargate);
        // Forecast consumption until a new Pylon can finish, including a
        // short builder trip. Normalize composition within each producer type.
        std::unordered_map<UnitKind, double> weights;
        std::unordered_map<UnitKind, double> rates;
        for (const auto& target : plan.composition) {
            const auto producer = producerFor(target.kind);
            const auto& stats = unitStats(target.kind);
            if (producer == UnitKind::unknown || stats.buildTime <= 0 || target.weight <= 0) continue;
            weights[producer] += target.weight;
            rates[producer] += target.weight * stats.supply / stats.buildTime;
        }
        double supplyPerFrame = 0.0;
        for (const auto& [producer, weight] : weights) {
            supplyPerFrame += usableProducers(state, producer) * rates[producer] / weight;
        }
        if (countExisting(state, UnitKind::probe) < plan.desiredWorkers) {
            const auto& probe = unitStats(UnitKind::probe);
            supplyPerFrame += usableProducers(state, UnitKind::nexus) *
                              static_cast<double>(probe.supply) / probe.buildTime;
        }
        const auto forecast = static_cast<int>(std::ceil(
            supplyPerFrame * (unitStats(UnitKind::pylon).buildTime + 96)));
        const auto safetyMargin = std::max(std::clamp(2 + activeProducers * 2, 4, 16), forecast);
        const auto remaining = state.self.supplyTotal + pendingPylons * 16 - state.self.supplyUsed;
        const auto openingDeadline = pylons == 0 && state.self.supplyUsed >= 12;
        if (state.self.supplyTotal + pendingPylons * 16 < 400 &&
            ((openingDeadline && pendingPylons == 0) || remaining <= safetyMargin)) {
            goals.push_back({GoalKind::build, UnitKind::pylon, pylons + 1, 110, true,
                             "operational supply invariant"});
        }
    }
    if (plan.prioritizeReinforcements) {
        // Fund one cycle per usable Gateway before optional structures or
        // research. This remains useful when a matchup rule cleared its
        // composition to save for tech. Busy/unpowered producers reserve none.
        const auto slots = std::clamp(usableProducers(state, UnitKind::gateway) -
            queuedForProducer(state, UnitKind::gateway), 0, 4);
        const auto counterWindow = usableProducers(state, UnitKind::photonCannon) >= 2 &&
            countCompleted(state, UnitKind::zealot) + countCompleted(state, UnitKind::dragoon) >= 1;
        const auto mobileScreen = countCompleted(state, UnitKind::zealot) +
            countCompleted(state, UnitKind::dragoon) + countCompleted(state, UnitKind::darkTemplar);
        const auto protectedSplash = state.enemy.race == Race::protoss &&
            ((mobileScreen >= 4 && usableProducers(state, UnitKind::photonCannon) > 0) ||
             countCompleted(state, UnitKind::dragoon) >= 4);
        auto gasBudget = ledger.freeGas();
        auto dragoons = countExisting(state, UnitKind::dragoon);
        auto zealots = countExisting(state, UnitKind::zealot);
        const auto seenDragoons = std::ranges::count_if(state.enemy.units, [&state](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::dragoon && (unit.visible || state.frame - unit.lastSeen <= 24 * 15);
        });
        const auto seenZealots = std::ranges::count_if(state.enemy.units, [&state](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::zealot && (unit.visible || state.frame - unit.lastSeen <= 24 * 15);
        });
        for (auto cycle = 0; cycle < slots; ++cycle) {
            const auto enoughMelee = state.enemy.race == Race::protoss && seenDragoons >= 3 &&
                seenDragoons >= 2 * seenZealots && zealots >= std::max(2, dragoons / 3);
            const auto ranged = prerequisitesMet(state, UnitKind::dragoon) && (gasBudget >= 50 || enoughMelee);
            const auto kind = ranged ? UnitKind::dragoon : UnitKind::zealot;
            const auto desired = ranged ? ++dragoons : ++zealots;
            if (ranged) gasBudget -= 50;
            goals.push_back({GoalKind::train, kind, desired, 112, true,
                             "protect a defensive reinforcement cycle", TechnologyKind::none, true});
        }
        for (auto& demand : goals) {
            const auto detectorChain = plan.requireMobileDetection &&
                (demand.target == UnitKind::observer || demand.target == UnitKind::observatory ||
                 demand.target == UnitKind::roboticsFacility || demand.target == UnitKind::cyberneticsCore ||
                 demand.target == UnitKind::assimilator);
            if (demand.target == UnitKind::pylon) demand.priority = std::max(130, demand.priority);
            else if (detectorChain) demand.priority = std::max(124, demand.priority);
            else if ((mobileScreen >= 8 || protectedSplash) && demand.goal == GoalKind::train &&
                ((demand.target == UnitKind::highTemplar && countExisting(state, UnitKind::highTemplar) < 2 &&
                  (technologyLevel(state.self, TechnologyKind::psionicStorm) > 0 ||
                   technologyInProgress(state.self, TechnologyKind::psionicStorm))) ||
                 (demand.target == UnitKind::reaver && countExisting(state, UnitKind::reaver) == 0 &&
                  countCompleted(state, UnitKind::roboticsSupportBay) > 0))) {
                demand.priority = std::max(114, demand.priority);
                demand.desiredCount = std::min(demand.desiredCount, demand.target == UnitKind::reaver ? 1 : 2);
            }
            else if (protectedSplash && demand.blocking &&
                (demand.target == UnitKind::roboticsFacility ||
                 demand.target == UnitKind::roboticsSupportBay))
                demand.priority = std::max(113, demand.priority);
            else if (mobileScreen >= 8 && demand.blocking &&
                (demand.technology == TechnologyKind::psionicStorm ||
                 demand.technology == TechnologyKind::singularityCharge ||
                 demand.technology == TechnologyKind::legEnhancements ||
                 demand.target == UnitKind::citadelOfAdun ||
                 demand.target == UnitKind::templarArchives ||
                 demand.target == UnitKind::roboticsSupportBay))
                demand.priority = std::max(113, demand.priority);
            else if (demand.target == UnitKind::probe && countExisting(state, UnitKind::probe) < 8)
                demand.priority = std::max(125, demand.priority);
            else if (counterWindow && demand.target == UnitKind::probe &&
                     countExisting(state, UnitKind::probe) < 16)
                demand.priority = std::max(125, demand.priority);
            else if (counterWindow &&
                     (demand.target == UnitKind::cyberneticsCore ||
                      demand.target == UnitKind::assimilator ||
                      demand.target == UnitKind::shieldBattery) && countExisting(state, demand.target) == 0)
                demand.priority = std::max(120, demand.priority);
            else if ((demand.target == UnitKind::forge || demand.target == UnitKind::photonCannon) &&
                     countExisting(state, UnitKind::photonCannon) == 0 && demand.priority >= 110) {
                demand.priority = std::max(126, demand.priority);
                // Once the first Cannon is paid for, let the mobile screen
                // reinforce while it warps in instead of funding another
                // unfinished Cannon ahead of every available Gateway.
                demand.desiredCount = 1;
            }
            else if (demand.goal != GoalKind::train)
                demand.priority = std::min(111, demand.priority);
        }
    }
    std::ranges::stable_sort(goals, std::greater{}, &ProductionGoal::priority);

    for (auto goal : goals) {
        const auto futureTarget = goal.technology == TechnologyKind::none
            ? goal.target : technologyStats(goal.technology).producer;
        auto wait = prerequisiteWait(state, futureTarget);
        if (goal.technology != TechnologyKind::none) {
            for (const auto& producer : state.self.units) {
                if (producer.kind == futureTarget && !producer.completed)
                    wait = std::max(wait, unitStats(futureTarget).buildTime *
                        (100 - std::clamp(producer.buildProgress, 0, 100)) / 100);
            }
        }
        // Leave the final ten seconds for saving and command latency. This
        // applies to both explicit goals and recursively requested tech, so
        // an Observer cannot indirectly reserve an unusable Observatory.
        if (wait > 10 * 24) continue;
        // Higher-priority detection may have consumed gas since the budget
        // was composed. Recheck the actual ledger before leaving a usable
        // Gateway idle with enough minerals for its fallback unit.
        if (goal.allowMineralFallback && goal.target == UnitKind::dragoon && ledger.freeGas() < 50) {
            goal.target = UnitKind::zealot;
            goal.desiredCount = countExisting(state, UnitKind::zealot) + planned[UnitKind::zealot] + 1;
        }
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
                // Optional research must not invent an opening prerequisite.
                // For example, a low-priority Dragoon range goal used to
                // reserve a Cybernetics Core before the PvP planner had
                // recognized the opponent's two-Gateway melee line.  That
                // consumed the exact mineral window needed by a Forge and
                // left the bot with neither static defense nor a deliberate
                // Core checkpoint.  Blocking tech goals still materialize
                // their prerequisite chain; non-blocking goals wait for the
                // strategic planner to request the structure explicitly.
                if (goal.blocking &&
                    countExisting(state, prerequisite) + planned[prerequisite] == 0) {
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
                        protectBlocking(prerequisite, unit.minerals, unit.gas,
                                        goal.priority);
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
                    if (!actions.back().reserved) {
                        protectBlocking(UnitKind::unknown, minerals, gas,
                                        goal.priority);
                    }
                }
                continue;
            }

            // Research and upgrades share one slot on a building. Saving for
            // another operation there must not starve a usable producer.
            if (usableProducers(state, stats.producer) <=
                committedProducers[stats.producer]) continue;

            MacroAction action{
                stats.research ? MacroActionKind::research : MacroActionKind::upgrade,
                UnitKind::unknown, goal.priority, minerals, gas, false,
                goal.reason, goal.technology, goal.blocking,
            };
            action.reserved = ledger.reserve(minerals, gas);
            if (action.reserved || goal.blocking) ++committedProducers[stats.producer];
            if (action.reserved || goal.blocking) actions.push_back(std::move(action));
            if (goal.blocking && !actions.back().reserved) {
                protectBlocking(UnitKind::unknown, minerals, gas,
                                goal.priority);
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
                queuedForProducer(state, producer) + committedProducers[producer] >=
                    usableProducers(state, producer)) {
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
                    protectBlocking(prerequisite, stats.minerals, stats.gas,
                                    goal.priority);
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
                ++planned[goal.target];
                if (!actions.back().reserved) {
                    protectBlocking(goal.target, target.minerals, target.gas,
                                    goal.priority);
                }
            }
            continue;
        }

        const auto& stats = unitStats(goal.target);
        if (goal.goal == GoalKind::train && state.self.supplyTotal > 0 &&
            plannedSupply + stats.supply > state.self.supplyTotal) {
            // Supply already includes units in production. A blocked train
            // order cannot reserve minerals needed for power, tech, or supply.
            continue;
        }
        MacroAction action{
            actionKind(goal.goal), goal.target, goal.priority,
            stats.minerals, stats.gas, false, goal.reason,
            TechnologyKind::none, goal.blocking,
        };
        action.reserved = ledger.reserve(stats.minerals, stats.gas);
        if (action.reserved || goal.blocking) {
            actions.push_back(std::move(action));
            ++planned[goal.target];
            if (goal.goal == GoalKind::train) {
                ++committedProducers[producerFor(goal.target)];
                if (actions.back().reserved) plannedSupply += stats.supply;
            }
        }
        // Protect each resource independently. A gas-starved upgrade can keep
        // its mineral bank without freezing probes or other mineral-only work
        // paid for from the true surplus.
        if (goal.blocking && !actions.back().reserved) {
            protectBlocking(goal.target, stats.minerals, stats.gas,
                            goal.priority);
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
            0, usableProducers(state, producer) - queuedForProducer(state, producer) -
                   committedProducers[producer]);
    }

    auto armyCount = 0;
    for (const auto& target : plan.composition) {
        armyCount += countExisting(state, target.kind) + planned[target.kind];
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
                target.weight <= 0.0 ||
                openProducerSlots[producer] <= 0 ||
                !prerequisitesMet(state, target.kind) || !supplyAvailable ||
                !ledger.canReserve(stats.minerals, stats.gas)) {
                continue;
            }
            const auto current = countExisting(state, target.kind) + planned[target.kind];
            const auto desired = target.weight * static_cast<double>(armyCount + 1);
            // Affordability is not permission to keep growing an already
            // overrepresented unit type while the desired unit waits for gas.
            if (static_cast<double>(current) >= desired) continue;
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

    // Record blocking structure demands that still need a future pass.  This
    // includes both resource-starved and placement-starved actions; the BWAPI
    // bridge owns retry timing while the planner owns strategic persistence.
    // Do not refresh requests injected from memory or they will never expire.
    for (const auto& candidate : plan.goals) {
        if (!candidate.blocking ||
            (candidate.goal != GoalKind::build && candidate.goal != GoalKind::expand)) {
            continue;
        }
        if (countExisting(state, candidate.target) >= candidate.desiredCount) continue;
        const auto existing = std::ranges::find_if(
            pendingGoals_, [&candidate](const PendingGoal& pending) {
                return pending.goal == candidate.goal &&
                       pending.target == candidate.target;
            });
        if (existing == pendingGoals_.end()) {
            pendingGoals_.push_back({candidate.goal, candidate.target,
                                     candidate.desiredCount, candidate.priority,
                                     candidate.reason, state.frame});
        } else {
            if (existing->lastRequested == state.frame) {
                // Several strategic signals may renew the same checkpoint.
                // Preserve the strongest explicit request from this frame.
                existing->desiredCount = std::max(existing->desiredCount, candidate.desiredCount);
                if (candidate.priority > existing->priority) existing->reason = candidate.reason;
                existing->priority = std::max(existing->priority, candidate.priority);
            } else {
                existing->desiredCount = candidate.desiredCount;
                existing->priority = candidate.priority;
                existing->reason = candidate.reason;
            }
            existing->lastRequested = state.frame;
        }
    }
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

}  // namespace protodd
