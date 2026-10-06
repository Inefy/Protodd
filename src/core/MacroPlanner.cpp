#include "protodd/MacroPlanner.hpp"

#include "protodd/Technology.hpp"
#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

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

int queuedUnitsOf(const GameState& state, const UnitKind kind) {
    return static_cast<int>(std::ranges::count(state.self.queuedUnits, kind));
}

int usableProducers(const GameState& state, const UnitKind kind) {
    return static_cast<int>(std::ranges::count_if(state.self.units,
        [kind](const UnitSnapshot& unit) {
            return unit.kind == kind && unit.completed && !unit.disabled &&
                   !unit.loaded && !unit.hallucination &&
                   (!unitStats(kind).requiresPsi || unit.powered);
        }));
}

bool usableProducerUnit(const UnitSnapshot& unit, const UnitKind kind) noexcept {
    return unit.kind == kind && unit.completed && !unit.disabled && !unit.loaded &&
           !unit.hallucination && (!unitStats(kind).requiresPsi || unit.powered);
}

bool activeTechnologyStillExecutable(const GameState& state,
                                     const TechnologyKind technology) {
    if (!technologyInProgress(state.self, technology)) return false;
    const auto producer = technologyStats(technology).producer;
    return producer != UnitKind::unknown && std::ranges::any_of(
        state.self.units, [producer](const UnitSnapshot& unit) {
            return usableProducerUnit(unit, producer);
        });
}

int availableTrainingProducers(const GameState& state, const UnitKind kind) {
    if (state.self.producerSlots.empty()) {
        return std::clamp(usableProducers(state, kind) -
                              queuedForProducer(state, kind),
                          0, usableProducers(state, kind));
    }
    return static_cast<int>(std::ranges::count_if(
        state.self.units, [&state, kind](const UnitSnapshot& unit) {
            if (!usableProducerUnit(unit, kind)) return false;
            const auto slot = std::ranges::find(state.self.producerSlots, unit.id,
                                                &ProducerSlotSnapshot::id);
            return slot != state.self.producerSlots.end() &&
                   !slot->researching && !slot->upgrading &&
                   trainingSlotAvailable(slot->activeTraining,
                                         slot->trainingQueueSize,
                                         slot->remainingTrainFrames,
                                         slot->latencyFrames,
                                         slot->recentTrainCommand);
        }));
}

int availableTechnologyProducers(const GameState& state, const UnitKind kind) {
    if (state.self.producerSlots.empty()) {
        return std::max(0, usableProducers(state, kind) -
                               queuedForProducer(state, kind));
    }
    return static_cast<int>(std::ranges::count_if(
        state.self.units, [&state, kind](const UnitSnapshot& unit) {
            if (!usableProducerUnit(unit, kind)) return false;
            const auto slot = std::ranges::find(state.self.producerSlots, unit.id,
                                                &ProducerSlotSnapshot::id);
            return slot != state.self.producerSlots.end() &&
                   !slot->researching && !slot->upgrading &&
                   trainingSlotAvailable(slot->activeTraining,
                                         slot->trainingQueueSize,
                                         slot->remainingTrainFrames,
                                         slot->latencyFrames,
                                         slot->recentTrainCommand);
        }));
}

// A paid-for prerequisite is a future deadline, not a reason to freeze every
// currently usable producer for its entire construction time.
int prerequisiteWait(const GameState& state, const UnitKind kind) {
    constexpr auto cycleWait = 10 * 60 * 24 + 1;
    std::unordered_set<UnitKind> visiting;
    const auto visit = [&state, &visiting](const auto& self,
                                          const UnitKind current) -> int {
        if (!visiting.insert(current).second) return cycleWait;
        auto wait = 0;
        for (const auto prerequisite : unitPrerequisites(current)) {
            auto earliest = std::numeric_limits<int>::max();
            for (const auto& unit : state.self.units) {
                if (unit.kind != prerequisite) continue;
                const auto remaining = unit.completed ? 0 :
                    unitStats(prerequisite).buildTime *
                        (100 - std::clamp(unit.buildProgress, 0, 100)) / 100;
                earliest = std::min(earliest, remaining);
            }
            if (earliest != std::numeric_limits<int>::max()) {
                wait = std::max(wait, earliest);
            } else {
                wait = std::max(wait, self(self, prerequisite));
            }
        }
        visiting.erase(current);
        return wait;
    };
    return visit(visit, kind);
}

int countExistingAtSite(const GameState& state, const UnitKind kind,
                        const ConstructionTaskSite& site) {
    if (!site.valid()) return 0;
    const auto radius = kind == UnitKind::pylon ? 128 : 416;
    return static_cast<int>(std::ranges::count_if(
        state.self.units, [kind, &site, radius](const UnitSnapshot& unit) {
            return unit.kind == kind && unit.position.valid() &&
                   distanceSquared(unit.position, site.anchor) <= radius * radius;
        }));
}

bool sameConstructionTask(const ConstructionTaskSite& left,
                          const ConstructionTaskSite& right) noexcept {
    return left.valid() == right.valid() &&
           (!left.valid() || left.id == right.id);
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

std::optional<UnitId> selectTrainingProducer(
    const std::span<const TrainingProducerCandidate> candidates) noexcept {
    std::optional<UnitId> selected;
    for (const auto& candidate : candidates) {
        if (candidate.id < 0 || !candidate.completed || !candidate.powered ||
            candidate.disabled || candidate.loaded || candidate.hallucination ||
            !candidate.legalForTarget || candidate.researching || candidate.upgrading ||
            !trainingSlotAvailable(candidate.activeTraining,
                                   candidate.trainingQueueSize,
                                   candidate.remainingTrainFrames,
                                   candidate.latencyFrames,
                                   candidate.recentTrainCommand)) {
            continue;
        }
        if (!selected || candidate.id < *selected) selected = candidate.id;
    }
    return selected;
}

void ResourceLedger::beginFrame(const int observedMinerals, const int observedGas) noexcept {
    minerals = std::max(0, observedMinerals);
    gas = std::max(0, observedGas);
    reservedMinerals = std::min(std::max(0, reservedMinerals), minerals);
    reservedGas = std::min(std::max(0, reservedGas), gas);
    protectedMinerals = std::min(std::max(0, protectedMinerals), reservedMinerals);
    protectedGas = std::min(std::max(0, protectedGas), reservedGas);
    committedMinerals = std::min(std::max(0, committedMinerals),
                                 reservedMinerals - protectedMinerals);
    committedGas = std::min(std::max(0, committedGas), reservedGas - protectedGas);
}

bool ResourceLedger::canReserve(const int mineralCost, const int gasCost) const noexcept {
    return mineralCost >= 0 && gasCost >= 0 &&
           freeMinerals() >= mineralCost && freeGas() >= gasCost;
}

bool ResourceLedger::reserve(const int mineralCost, const int gasCost) noexcept {
    if (!canReserve(mineralCost, gasCost)) {
        return false;
    }
    reservedMinerals += mineralCost;
    reservedGas += gasCost;
    committedMinerals += mineralCost;
    committedGas += gasCost;
    return true;
}

void ResourceLedger::protect(const int mineralCost, const int gasCost) noexcept {
    const auto protectedMineralsNow = std::min(std::max(0, mineralCost), freeMinerals());
    const auto protectedGasNow = std::min(std::max(0, gasCost), freeGas());
    reservedMinerals += protectedMineralsNow;
    reservedGas += protectedGasNow;
    protectedMinerals += protectedMineralsNow;
    protectedGas += protectedGasNow;
}

bool ResourceLedger::canSpendCommitted(const int mineralCost, const int gasCost) const noexcept {
    return mineralCost >= 0 && gasCost >= 0 && minerals >= mineralCost && gas >= gasCost &&
           committedMinerals >= mineralCost && committedGas >= gasCost &&
           reservedMinerals >= mineralCost && reservedGas >= gasCost;
}

bool ResourceLedger::spendCommitted(const int mineralCost, const int gasCost) noexcept {
    if (!canSpendCommitted(mineralCost, gasCost)) return false;
    minerals -= mineralCost;
    gas -= gasCost;
    reservedMinerals -= mineralCost;
    reservedGas -= gasCost;
    committedMinerals -= mineralCost;
    committedGas -= gasCost;
    return true;
}

bool ResourceLedger::releaseCommitted(const int mineralCost, const int gasCost) noexcept {
    if (mineralCost < 0 || gasCost < 0 ||
        committedMinerals < mineralCost || committedGas < gasCost ||
        reservedMinerals < mineralCost || reservedGas < gasCost) return false;
    reservedMinerals -= mineralCost;
    reservedGas -= gasCost;
    committedMinerals -= mineralCost;
    committedGas -= gasCost;
    return true;
}

bool ResourceLedger::spendAvailable(const int mineralCost, const int gasCost) noexcept {
    if (!canReserve(mineralCost, gasCost)) return false;
    minerals -= mineralCost;
    gas -= gasCost;
    return true;
}

std::vector<MacroAction> MacroPlanner::reconcile(
    const GameState& state,
    const StrategicPlan& plan,
    ResourceLedger& ledger,
    const std::span<const BuildBlockerFeedback> buildBlockers) const {
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
        const auto existing = pending.constructionSite.valid()
            ? countExistingAtSite(state, pending.target, pending.constructionSite)
            : countExisting(state, pending.target);
        return existing >= pending.desiredCount || pending.lastRequested < 0 ||
               state.frame < pending.lastRequested ||
               state.frame - pending.lastRequested > 30 * 24;
    });
    if (plan.recoveringLastNexus) {
        std::erase_if(pendingGoals_, [](const PendingGoal& pending) {
            return pending.target != UnitKind::nexus;
        });
    }
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
                          sameConstructionTask(goal.constructionSite,
                                               pending.constructionSite) &&
                          (!goal.blocking || goal.desiredCount < pending.desiredCount);
               });
    });
    for (const auto& pending : pendingGoals_) {
        const auto alreadyRequested = std::ranges::any_of(
            goals, [&pending](const ProductionGoal& candidate) {
                return candidate.goal == pending.goal &&
                       candidate.target == pending.target &&
                       sameConstructionTask(candidate.constructionSite,
                                            pending.constructionSite);
            });
        if (alreadyRequested) continue;
        goals.push_back({pending.goal, pending.target, pending.desiredCount,
                         pending.priority, true, pending.reason,
                         TechnologyKind::none, false, false,
                         pending.constructionSite});
    }

    // Strategy is assembled from several independent signals (opening
    // recognizer, emergency response, infrastructure, and learned style).
    // Coalesce equivalent structure/technology requests before reservation.
    // Training demand is limited separately by available producer slots.
    std::vector<ProductionGoal> mergedGoals;
    mergedGoals.reserve(goals.size());
    for (const auto& candidate : goals) {
        // A fulfilled opening checkpoint cannot lend its priority to a larger,
        // optional quota. Otherwise "two Gateways before tech" plus "four
        // Gateways eventually" becomes four Gateways before tech forever.
        if (candidate.goal != GoalKind::train) {
            const auto fulfilled = candidate.technology != TechnologyKind::none
                ? technologyLevel(state.self, candidate.technology) >= candidate.desiredCount
                : (candidate.constructionSite.valid()
                       ? countExistingAtSite(state, candidate.target,
                                             candidate.constructionSite)
                       : countExisting(state, candidate.target)) >= candidate.desiredCount;
            if (fulfilled) continue;
        }
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
                                                   prior.technology == candidate.technology &&
                                                   sameConstructionTask(
                                                       prior.constructionSite,
                                                       candidate.constructionSite);
                                        });
        if (existing == mergedGoals.end()) {
            mergedGoals.push_back(candidate);
            continue;
        }
        if (candidate.priority > existing->priority) {
            *existing = candidate;
        } else if (candidate.priority == existing->priority) {
            existing->desiredCount = std::max(existing->desiredCount, candidate.desiredCount);
            existing->blocking = existing->blocking || candidate.blocking;
        }
    }
    goals = std::move(mergedGoals);
    std::unordered_map<UnitKind, int> planned;
    std::unordered_map<UnitKind, int> committedProducers;
    std::unordered_set<UnitKind> gasStarvedProducers;
    const auto blockedBuild = [&state, buildBlockers](
                                  const UnitKind target,
                                  const ConstructionTaskSite& constructionSite) {
        return std::ranges::find_if(buildBlockers,
            [&state, target, &constructionSite](const BuildBlockerFeedback& feedback) {
                return feedback.target == target && feedback.retryAt > state.frame &&
                       sameConstructionTask(feedback.constructionSite, constructionSite);
            });
    };
    // A blocking Pylon is a supply deadline, not a license to idle the
    // Nexus.  Before the game is within four supply of the cap, let a Probe
    // spend an otherwise-unaffordable Pylon's current mineral shortfall and
    // start the Pylon on the next pass.  This mirrors Stardust's forward
    // resource schedule: protect a future deadline without sacrificing
    // continuous worker production in the present frame.
    const auto protectBlocking = [&ledger, &state, &blockedBuild, buildBlockers](
                                    const UnitKind target,
                                    const int minerals,
                                    const int gas,
                                    const int priority,
                                    const ConstructionTaskSite& constructionSite) {
        if (blockedBuild(target, constructionSite) != buildBlockers.end()) return;
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
        const auto urgentMirrorCore = target == UnitKind::cyberneticsCore && priority >= 119 &&
            state.enemy.race == Race::protoss && state.frame < 8 * 60 * 24;
        if (urgentStaticAnchor || urgentMirrorCore || (target == UnitKind::nexus && priority >= 120)) {
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
    if (state.self.producerSlots.empty()) {
        for (const auto& technology : state.self.technologies) {
            if (activeTechnologyStillExecutable(state, technology.kind)) {
                ++committedProducers[technologyStats(technology.kind).producer];
            }
        }
    }

    // Supply is an operational invariant, not merely a strategic preference.
    // If a future strategy accidentally omits its pylon goal, the macro layer
    // still protects the game from a complete production deadlock.
    if (state.self.race == Race::protoss && state.self.supplyTotal > 0 &&
        state.self.supplyTotal < 400) {
        const auto pylons = countExisting(state, UnitKind::pylon);
        const auto pylonBuildFrames = unitStats(UnitKind::pylon).buildTime;
        const auto horizon = pylonBuildFrames + std::clamp(
            state.pylonBuilderTravelFrames, 0, 2 * 60 * 24);
        const auto timelyPendingPylons = static_cast<int>(std::ranges::count_if(
            state.self.units, [horizon](const UnitSnapshot& unit) {
                if (unit.kind != UnitKind::pylon || unit.completed) return false;
                const auto remaining = unitStats(UnitKind::pylon).buildTime *
                    (100 - std::clamp(unit.buildProgress, 0, 100)) / 100;
                return remaining <= horizon;
            }));
        const auto projectedSupplyTotal = std::min(
            400, state.self.supplyTotal + timelyPendingPylons * 16);
        const auto activeProducers = countCompleted(state, UnitKind::nexus) +
                                     countCompleted(state, UnitKind::gateway) +
                                     countCompleted(state, UnitKind::roboticsFacility) +
                                     countCompleted(state, UnitKind::stargate);
        // Forecast consumption until a new Pylon can finish, including a
        // short builder trip. Normalize composition within each producer type.
        std::unordered_map<UnitKind, double> weights;
        std::unordered_map<UnitKind, double> rates;
        std::unordered_map<UnitKind, int> firstCycle;
        for (const auto& target : plan.composition) {
            const auto producer = producerFor(target.kind);
            const auto& stats = unitStats(target.kind);
            if (producer == UnitKind::unknown || stats.buildTime <= 0 || target.weight <= 0) continue;
            weights[producer] += target.weight;
            rates[producer] += target.weight * stats.supply / stats.buildTime;
            firstCycle[producer] = std::max(firstCycle[producer], stats.supply);
        }
        for (const auto& demand : plan.goals) {
            if (demand.goal != GoalKind::train ||
                countExisting(state, demand.target) >= demand.desiredCount) continue;
            const auto producer = producerFor(demand.target);
            if (producer != UnitKind::unknown)
                firstCycle[producer] = std::max(firstCycle[producer], unitStats(demand.target).supply);
        }
        if (countExisting(state, UnitKind::probe) < plan.desiredWorkers) {
            const auto& probe = unitStats(UnitKind::probe);
            weights[UnitKind::nexus] = 1.0;
            rates[UnitKind::nexus] = static_cast<double>(probe.supply) / probe.buildTime;
            firstCycle[UnitKind::nexus] = probe.supply;
        }
        // Include waiting items on each producer and the cycles that can start
        // before a new Pylon can finish. BWAPI supplyUsed includes the active
        // item, but not items queued behind it. A new Pylon also needs its
        // builder to reach the home anchor before construction begins.
        double forecastSupply = 0.0;
        for (const auto& producer : state.self.units) {
            if (!firstCycle.contains(producer.kind) || producer.disabled || producer.loaded ||
                producer.hallucination || (producer.completed && !producer.powered)) continue;
            auto availableIn = producer.completed ? std::max(0, producer.remainingTrainFrames)
                : unitStats(producer.kind).buildTime *
                    (100 - std::clamp(producer.buildProgress, 0, 100)) / 100;
            const auto slot = std::ranges::find(state.self.producerSlots, producer.id,
                                                &ProducerSlotSnapshot::id);
            if (slot != state.self.producerSlots.end()) {
                if (slot->researching || slot->upgrading) continue;
                if (slot->activeTraining) {
                    availableIn = std::max(availableIn, slot->remainingTrainFrames);
                    if (slot->recentTrainCommand && availableIn == 0)
                        availableIn = std::max(1, slot->latencyFrames);
                }
                auto queuedDuration = 0;
                auto queuedIndex = std::size_t{};
                const auto queueTailCount = std::max(
                    0, slot->trainingQueueSize - (slot->activeTraining ? 1 : 0));
                for (; queuedIndex < slot->queuedUnits.size() &&
                       queuedIndex < static_cast<std::size_t>(queueTailCount); ++queuedIndex) {
                    const auto queued = slot->queuedUnits[queuedIndex];
                    const auto queuedFrames = std::max(1, unitStats(queued).buildTime);
                    if (availableIn + queuedDuration <= horizon)
                        forecastSupply += unitStats(queued).supply;
                    queuedDuration += queuedFrames;
                }
                // A transitional BWAPI queue entry may not yet have a usable
                // UnitType. Reserve the average planned cycle for its supply
                // and duration instead of pretending that the producer is idle.
                while (queuedIndex < static_cast<std::size_t>(queueTailCount)) {
                    const auto averageFrames = weights[producer.kind] > 0.0 &&
                        rates[producer.kind] > 0.0
                            ? std::max(1, static_cast<int>(std::ceil(
                                  weights[producer.kind] / rates[producer.kind])))
                            : 1;
                    if (availableIn + queuedDuration <= horizon)
                        forecastSupply += firstCycle[producer.kind];
                    queuedDuration += averageFrames;
                    ++queuedIndex;
                }
                availableIn += queuedDuration;
            }
            if (availableIn > horizon) continue;
            const auto cycleSupply = firstCycle[producer.kind];
            forecastSupply += cycleSupply;
            if (weights[producer.kind] > 0.0 && rates[producer.kind] > 0.0 &&
                cycleSupply > 0) {
                const auto cycleFrames = std::max(1, static_cast<int>(std::ceil(
                    weights[producer.kind] * cycleSupply / rates[producer.kind])));
                const auto cycles = (horizon - availableIn) / cycleFrames;
                forecastSupply += cycles * cycleSupply;
            }
        }
        if (state.self.producerSlots.empty()) {
            // Legacy and pure-planner snapshots lack queue ownership by
            // building. Retain their global commitments conservatively.
            for (const auto queued : state.self.queuedUnits)
                if (producerFor(queued) != UnitKind::unknown)
                    forecastSupply += unitStats(queued).supply;
        }
        const auto forecast = static_cast<int>(std::ceil(forecastSupply));
        // With a single Nexus, the old four-supply minimum waited until two
        // Probes were the only remaining buffer. If minerals still had to be
        // gathered, the Pylon could not finish before the Nexus consumed that
        // headroom. Keep six supply in reserve even in a one-producer opening.
        const auto safetyMargin = std::max(
            std::clamp(2 + activeProducers * 2, 6, 16), forecast);
        const auto remaining = projectedSupplyTotal - state.self.supplyUsed;
        const auto openingDeadline = pylons == 0 && state.self.supplyUsed >= 12;
        if (!plan.recoveringLastNexus && projectedSupplyTotal < 400 &&
            (openingDeadline || remaining <= safetyMargin)) {
            // At four supply or less, an unstarted Pylon is already on the
            // critical path. A planned Nexus may have priority 120, but
            // reserving its 400 minerals first can leave every Gateway idle
            // while both goals wait. Fund the supply deadline first.
            const auto priority = remaining <= 4 ? 130 : 110;
            goals.push_back({GoalKind::build, UnitKind::pylon, pylons + 1, priority, true,
                             "operational supply invariant"});
        }
    }
    if (plan.prioritizeReinforcements) {
        // Fund one cycle per usable Gateway before optional structures or
        // research. This remains useful when a matchup rule cleared its
        // composition to save for tech. Busy/unpowered producers reserve none.
        const auto slots = std::clamp(
            availableTrainingProducers(state, UnitKind::gateway), 0, 4);
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
                             "protect a defensive reinforcement cycle", TechnologyKind::none, !enoughMelee});
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
                     demand.desiredCount > countExisting(state, demand.target) && demand.blocking &&
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
    // A ready first Gateway must deliver bodies before optional opening
    // infrastructure consumes its bank. Count paid-for queues, not just
    // completed units, so this never reserves a second layer of production.
    const auto openingScreen = countExisting(state, UnitKind::zealot) +
        countExisting(state, UnitKind::dragoon) +
        static_cast<int>(std::ranges::count(state.self.queuedUnits, UnitKind::zealot)) +
        static_cast<int>(std::ranges::count(state.self.queuedUnits, UnitKind::dragoon));
    if (!plan.requireMobileDetection && state.enemy.race != Race::zerg &&
        state.frame >= 2 * 60 * 24 &&
        state.frame < 4 * 60 * 24 &&
        openingScreen < 2 && countExisting(state, UnitKind::cyberneticsCore) == 0 &&
        availableTrainingProducers(state, UnitKind::gateway) > 0) {
        goals.push_back({GoalKind::train, UnitKind::zealot,
            countExisting(state, UnitKind::zealot) + 1, openingScreen == 0 ? 127 : 119, true,
            "deliver the first mobile screen before opening infrastructure"});
    }
    std::ranges::stable_sort(goals, std::greater{}, &ProductionGoal::priority);

    for (auto goal : goals) {
        if (plan.deferExpansion && goal.target == UnitKind::nexus &&
            (goal.goal == GoalKind::expand || goal.goal == GoalKind::build)) continue;
        const auto futureTarget = goal.technology == TechnologyKind::none
            ? goal.target : technologyStats(goal.technology).producer;
        auto wait = prerequisiteWait(state, futureTarget);
        if (goal.technology != TechnologyKind::none) {
            const auto requirement = technologyPrerequisite(goal.technology,
                technologyLevel(state.self, goal.technology) + 1);
            for (const auto building : {futureTarget, requirement}) {
                if (building == UnitKind::unknown || countCompleted(state, building) > 0) continue;
                wait = std::max(wait, prerequisiteWait(state, building));
                auto earliest = std::numeric_limits<int>::max();
                for (const auto& unit : state.self.units) {
                    if (unit.kind == building)
                        earliest = std::min(earliest, unitStats(building).buildTime *
                            (100 - std::clamp(unit.buildProgress, 0, 100)) / 100);
                }
                if (earliest != std::numeric_limits<int>::max()) wait = std::max(wait, earliest);
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
                activeTechnologyStillExecutable(state, goal.technology)) {
                continue;
            }

            const auto nextLevel = currentLevel + 1;
            const auto minerals = stats.mineralCost(nextLevel);
            const auto gas = stats.gasCost(nextLevel);
            const auto levelRequirement = technologyPrerequisite(goal.technology, nextLevel);
            const auto missing = countCompleted(state, stats.producer) == 0 ? stats.producer :
                (levelRequirement != UnitKind::unknown && countCompleted(state, levelRequirement) == 0
                    ? levelRequirement : UnitKind::unknown);
            if (missing != UnitKind::unknown) {
                const auto nested = nextMissingPrerequisite(state, missing);
                const auto prerequisite = nested != UnitKind::unknown ? nested : missing;
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
                    const auto blockerActive =
                        blockedBuild(prerequisite, ConstructionTaskSite{}) != buildBlockers.end();
                    MacroAction action{
                        MacroActionKind::build, prerequisite, goal.priority,
                        unit.minerals, unit.gas, false,
                        "unlock " + std::string(stats.name), TechnologyKind::none,
                        goal.blocking,
                    };
                    action.reserved = !blockerActive && ledger.reserve(unit.minerals, unit.gas);
                    action.executable = !blockerActive;
                    actions.push_back(std::move(action));
                    ++planned[prerequisite];
                    if (goal.blocking && !actions.back().reserved) {
                        protectBlocking(prerequisite, unit.minerals, unit.gas,
                                        goal.priority, ConstructionTaskSite{});
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
                                        goal.priority, ConstructionTaskSite{});
                    }
                }
                continue;
            }

            // Research and upgrades share one slot on a building. Saving for
            // another operation there must not starve a usable producer.
            if (availableTechnologyProducers(state, stats.producer) <=
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
                                goal.priority, ConstructionTaskSite{});
            }
            continue;
        }

        const auto existing = goal.constructionSite.valid()
            ? countExistingAtSite(state, goal.target, goal.constructionSite)
            : countExisting(state, goal.target) + planned[goal.target] +
                  (goal.goal == GoalKind::train
                       ? queuedUnitsOf(state, goal.target) : 0);
        if (existing >= goal.desiredCount) {
            continue;
        }
        if (goal.goal == GoalKind::train) {
            const auto producer = producerFor(goal.target);
            if (unitStats(goal.target).gas > ledger.freeGas() &&
                gasStarvedProducers.contains(producer)) continue;
            if (producer != UnitKind::unknown &&
                countCompleted(state, producer) > 0 &&
                availableTrainingProducers(state, producer) <=
                    committedProducers[producer]) {
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
                const auto blockerActive =
                    blockedBuild(prerequisite, ConstructionTaskSite{}) != buildBlockers.end();
                MacroAction action{
                    MacroActionKind::build, prerequisite, goal.priority,
                    stats.minerals, stats.gas, false,
                    "unlock " + std::string(unitStats(goal.target).name),
                    TechnologyKind::none, goal.blocking,
                };
                action.reserved = !blockerActive && ledger.reserve(stats.minerals, stats.gas);
                action.executable = !blockerActive;
                if (action.reserved || goal.blocking) {
                    actions.push_back(std::move(action));
                    ++planned[prerequisite];
                }
                if (goal.blocking && !actions.back().reserved) {
                    protectBlocking(prerequisite, stats.minerals, stats.gas,
                                    goal.priority, ConstructionTaskSite{});
                }
            } else if (goal.blocking) {
                // The prerequisite already exists but is incomplete. Reserve
                // the target now so routine production cannot drain its bank
                // before the structure finishes.
                const auto& target = unitStats(goal.target);
                const auto blockerActive =
                    blockedBuild(goal.target, goal.constructionSite) != buildBlockers.end();
                MacroAction waiting{
                    actionKind(goal.goal), goal.target, goal.priority,
                    target.minerals, target.gas, false, goal.reason,
                    TechnologyKind::none, true,
                };
                waiting.constructionSite = goal.constructionSite;
                waiting.reserved = !blockerActive &&
                                   ledger.reserve(target.minerals, target.gas);
                waiting.executable = false;
                actions.push_back(std::move(waiting));
                ++planned[goal.target];
                if (!actions.back().reserved) {
                    protectBlocking(goal.target, target.minerals, target.gas,
                                    goal.priority, goal.constructionSite);
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
        action.constructionSite = goal.constructionSite;
        const auto isConstructionGoal = goal.goal == GoalKind::build ||
                                        goal.goal == GoalKind::expand;
        const auto blockerActive = isConstructionGoal &&
            blockedBuild(goal.target, goal.constructionSite) != buildBlockers.end();
        action.reserved = !blockerActive && ledger.reserve(stats.minerals, stats.gas);
        action.executable = !blockerActive;
        if (action.reserved || goal.blocking) {
            actions.push_back(std::move(action));
            if (!goal.constructionSite.valid()) ++planned[goal.target];
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
                            goal.priority, goal.constructionSite);
            // One future unit can reserve its mineral cost while gas arrives.
            // Repeating that reservation for every idle Gateway locks the
            // entire bank behind an income stream that may have been raided.
            if (goal.goal == GoalKind::train && stats.gas > ledger.freeGas())
                gasStarvedProducers.insert(producerFor(goal.target));
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
    auto availableCompositionWeight = 0.0;
    for (const auto& target : plan.composition) {
        const auto producer = producerFor(target.kind);
        // Shares for technology we have not started cannot be filled. Keeping
        // them in the denominator can cap every available unit and idle the
        // entire army economy. Preserve paid-for prerequisites, including
        // unfinished ones, so a tech transition still has its intended share.
        // Neither affordability nor a temporarily busy/unpowered producer
        // changes that share: a gas shortage must not flood us with Zealots.
        const auto& stats = unitStats(target.kind);
        const auto techCommitted = std::ranges::all_of(
            unitPrerequisites(target.kind), [&state](const UnitKind prerequisite) {
                return countExisting(state, prerequisite) > 0;
            });
        if (producer != UnitKind::unknown && !stats.building &&
            stats.minerals + stats.gas > 0 && target.weight > 0.0 && techCommitted) {
            availableCompositionWeight += target.weight;
        }
        if (producer == UnitKind::unknown || openProducerSlots.contains(producer)) continue;
        openProducerSlots[producer] = std::max(
            0, availableTrainingProducers(state, producer) -
                   committedProducers[producer]);
    }

    auto armyCount = 0;
    for (const auto& target : plan.composition) {
        armyCount += countExisting(state, target.kind) +
                     queuedUnitsOf(state, target.kind) + planned[target.kind];
    }
    constexpr auto maximumCompositionActions = 8;
    for (auto cycle = 0; cycle < maximumCompositionActions &&
                         availableCompositionWeight > 0.0; ++cycle) {
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
            const auto current = countExisting(state, target.kind) +
                                 queuedUnitsOf(state, target.kind) + planned[target.kind];
            const auto desired = target.weight / availableCompositionWeight *
                                 static_cast<double>(armyCount + 1);
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
                           "fill idle production with available tech composition"});
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

    const auto earlyFallbackReady = !earlyPvzMineralFallback_ ||
        (countCompleted(state, UnitKind::nexus) >= 2 &&
         countCompleted(state, UnitKind::cyberneticsCore) >= 1 &&
         countCompleted(state, UnitKind::photonCannon) >= 2 &&
         countCompleted(state, UnitKind::probe) >= 26);
    if (pvzMineralFallback_ && state.enemy.race == Race::zerg &&
        state.frame >= (earlyPvzMineralFallback_ ? 12000 : 12 * 60 * 24) &&
        earlyFallbackReady && state.self.minerals >= 800 &&
        (earlyPvzMineralFallback_ || ledger.freeGas() < 125) &&
        ledger.freeMinerals() >= 600 &&
        countCompleted(state, UnitKind::nexus) > 0 &&
        countCompleted(state, UnitKind::gateway) >= 4 &&
        std::ranges::any_of(plan.composition, [](const CompositionTarget& target) {
            return target.kind == UnitKind::zealot && target.weight > 0.0;
        })) {
        // The PvZ replay-opening game held more than 5,000 minerals while
        // seven powered Gateways sat idle. Preserve every higher-priority
        // reservation, then turn only the remaining mineral surplus into a
        // small Zealot cycle instead of waiting indefinitely for gas tech.
        const auto& zealot = unitStats(UnitKind::zealot);
        auto& slots = openProducerSlots[UnitKind::gateway];
        for (auto cycle = 0; cycle < 4 && slots > 0 &&
             ledger.freeMinerals() >= zealot.minerals + 400; ++cycle) {
            if (state.self.supplyTotal > 0 &&
                plannedSupply + zealot.supply > state.self.supplyTotal) break;
            if (!ledger.reserve(zealot.minerals, 0)) break;
            actions.push_back({MacroActionKind::train, UnitKind::zealot, 57,
                zealot.minerals, 0, true,
                "spend mineral surplus in idle PvZ Gateways"});
            plannedSupply += zealot.supply;
            --slots;
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
    // Optional harassment spending must disappear immediately on an emergency.
    for (const auto& candidate : plan.goals) {
        if (!candidate.blocking || candidate.harassmentOnly ||
            (candidate.goal != GoalKind::build && candidate.goal != GoalKind::expand)) {
            continue;
        }
        const auto existingAtDemand = candidate.constructionSite.valid()
            ? countExistingAtSite(state, candidate.target, candidate.constructionSite)
            : countExisting(state, candidate.target);
        if (existingAtDemand >= candidate.desiredCount) continue;
        const auto existing = std::ranges::find_if(
            pendingGoals_, [&candidate](const PendingGoal& pending) {
                return pending.goal == candidate.goal &&
                       pending.target == candidate.target &&
                       sameConstructionTask(pending.constructionSite,
                                            candidate.constructionSite);
            });
        if (existing == pendingGoals_.end()) {
            pendingGoals_.push_back({candidate.goal, candidate.target,
                                     candidate.desiredCount, candidate.priority,
                                     candidate.reason, state.frame,
                                     candidate.constructionSite});
        } else {
            if (existing->lastRequested == state.frame) {
                // Several strategic signals may renew the same checkpoint.
                // Preserve the strongest explicit request from this frame.
                if (candidate.priority > existing->priority) {
                    existing->desiredCount = candidate.desiredCount;
                    existing->reason = candidate.reason;
                    existing->priority = candidate.priority;
                } else if (candidate.priority == existing->priority) {
                    existing->desiredCount = std::max(existing->desiredCount, candidate.desiredCount);
                }
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
    std::unordered_set<UnitKind> visiting;
    const auto visit = [&state, &visiting](const auto& self,
                                           const UnitKind current) -> UnitKind {
        if (!visiting.insert(current).second) return UnitKind::unknown;
        for (const auto prerequisite : unitPrerequisites(current)) {
            if (countCompleted(state, prerequisite) > 0) continue;
            // Construction has already been paid for. Continue walking the
            // direct requirements so the ledger can fund the next structure
            // in the chain rather than reserving an impossible end-unit.
            if (countExisting(state, prerequisite) > 0) continue;
            const auto nested = self(self, prerequisite);
            visiting.erase(current);
            return nested != UnitKind::unknown ? nested : prerequisite;
        }
        visiting.erase(current);
        return UnitKind::unknown;
    };
    return visit(visit, kind);
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
