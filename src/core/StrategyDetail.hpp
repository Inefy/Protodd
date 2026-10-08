#pragma once

#include "protodd/Strategy.hpp"
#include "protodd/Technology.hpp"
#include "protodd/Combat.hpp"
#include "protodd/Harassment.hpp"
#include "protodd/ProductionReadiness.hpp"
#include "protodd/UnitCatalog.hpp"
#include "StrategyGoalBuilder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>

namespace protodd::strategy_detail {
namespace {

// Observed counts include current unit snapshots even while they are building;
// completed counts include only finished snapshots. Keep that distinction
// explicit at strategy call sites instead of relying on a boolean flag.
enum class UnitCountBasis : std::uint8_t { observed, completed };

int effectiveUnitCount(
    const GameState& state,
    const UnitKind kind,
    const UnitCountBasis basis) {
    return static_cast<int>(std::ranges::count_if(
        state.self.units,
        [kind, basis](const UnitSnapshot& unit) {
            return unit.kind == kind &&
                   (basis == UnitCountBasis::observed || unit.completed);
        }));
}

int observedRoleCount(const GameState& state, const UnitRole role) {
    return static_cast<int>(std::ranges::count(state.self.units, role, &UnitSnapshot::role));
}

bool gatewayCombatUnit(const UnitKind kind) noexcept {
    return kind == UnitKind::zealot || kind == UnitKind::dragoon ||
           kind == UnitKind::highTemplar || kind == UnitKind::darkTemplar;
}

struct GatewayProductionCost {
    double mineralsPerMinute{};
    double gasPerMinute{};
};

inline constexpr Frame framesPerSecond = 24;
inline constexpr Frame framesPerMinute = 60 * framesPerSecond;

[[nodiscard]] constexpr Frame framesForSeconds(const int seconds) noexcept {
    return static_cast<Frame>(seconds) * framesPerSecond;
}

[[nodiscard]] constexpr Frame framesForMinutes(const int minutes) noexcept {
    return static_cast<Frame>(minutes) * framesPerMinute;
}

GatewayProductionCost gatewayProductionCost(const StrategicPlan& plan) noexcept {
    auto totalWeight = 0.0;
    GatewayProductionCost result;
    for (const auto& target : plan.composition) {
        if (!gatewayCombatUnit(target.kind) || target.weight <= 0.0) continue;
        const auto& stats = unitStats(target.kind);
        if (stats.buildTime <= 0) continue;
        const auto unitsPerMinute = static_cast<double>(framesPerMinute) / stats.buildTime;
        result.mineralsPerMinute += target.weight * stats.minerals * unitsPerMinute;
        result.gasPerMinute += target.weight * stats.gas * unitsPerMinute;
        totalWeight += target.weight;
    }
    if (totalWeight <= 0.0) {
        const auto& stats = unitStats(UnitKind::zealot);
        const auto unitsPerMinute = static_cast<double>(framesPerMinute) /
                                    std::max(1, stats.buildTime);
        return {stats.minerals * unitsPerMinute, stats.gas * unitsPerMinute};
    }
    return {result.mineralsPerMinute / totalWeight,
            result.gasPerMinute / totalWeight};
}

struct DisplayedSupply {
    int points{};
};

int minute(const GameState& state) {
    return state.frame / framesPerMinute;
}

bool supplyAtLeast(const GameState& state, const DisplayedSupply displayedSupply) {
    return state.self.supplyUsed >= displayedSupply.points * 2;
}

bool activeApproach(const GameState& state, const ThreatAssessment& threat) noexcept {
    // Early scouting should react to an army crossing the map. Once the game
    // is established, perimeter movement alone is not enough to keep the
    // economy and army permanently defensive; require a current breach or
    // explicit rush evidence instead.
    return state.frame < framesForMinutes(10) && threat.approachingArmyValue >= 2.0;
}

bool approachingCombatAnchor(
    const UnitSnapshot& unit,
    const Position anchor,
    const PixelRadius radius) noexcept {
    return unit.visible && unit.completed && !unit.flying &&
           isCombatUnit(unit.kind) && unit.position.valid() &&
           unit.lastPosition.valid() && withinPixelRadius(unit.position, anchor, radius) &&
           movedCloserByAtLeast(unit.lastPosition, unit.position, anchor,
                                PixelDistance{2.0});
}

double approachingCombatValueAt(
    const GameState& state,
    const Position anchor,
    const PixelRadius radius,
    const Position alternateSite = {-1, -1}) noexcept {
    if (!anchor.valid()) return 0.0;
    auto value = 0.0;
    for (const auto& enemy : state.enemy.units) {
        if (!approachingCombatAnchor(enemy, anchor, radius)) continue;
        // A natural-bound force is an expansion-site problem first. The same
        // movement can reduce distance to both bases on compact maps, so do
        // not let that proximity alone turn it into a home rush.
        if (alternateSite.valid() &&
            approachingCombatAnchor(enemy, alternateSite, radius) &&
            distanceSquared(enemy.position, alternateSite) <
                distanceSquared(enemy.position, anchor)) {
            continue;
        }
        value += unitStats(enemy.kind).combatValue *
                 std::clamp(enemy.healthFraction(), 0.2, 1.0);
    }
    return value;
}

bool openingPressureExpected(
    const GameState& state,
    const ThreatAssessment& threat) noexcept {
    const auto supported = threat.uncertainty <= 0.75 ||
                           threat.combatEnemiesNearMain > 0 ||
                           activeApproach(state, threat) ||
                           threat.immediateGround > 0.45;
    // A nearly uniform belief distribution still has a numerical winner.
    // Its stale label alone must not hold a ready army at home for 16 minutes.
    return minute(state) < 16 && supported &&
           (threat.mostLikely == EnemyPlan::fastRush ||
            threat.mostLikely == EnemyPlan::heavyPressure);
}

int recentEnemyCount(
    const GameState& state,
    const UnitKind kind,
    const Frame memory = framesForSeconds(90)) {
    return static_cast<int>(std::ranges::count_if(
        state.enemy.units, [kind, memory, &state](const UnitSnapshot& unit) {
            return unit.kind == kind && unit.completed &&
                   (unit.visible || state.frame - unit.lastSeen <= memory);
        }));
}

bool recentEnemyEvidence(
    const GameState& state,
    const UnitKind kind,
    const Frame memory = framesForSeconds(90)) {
    return std::ranges::any_of(state.enemy.units,
        [kind, memory, &state](const UnitSnapshot& unit) {
            return unit.kind == kind && !unit.hallucination &&
                (unit.visible ||
                 (unit.lastSeen >= 0 && state.frame >= unit.lastSeen &&
                  state.frame - unit.lastSeen <= memory));
        });
}

void setCompositionWeight(
    StrategicPlan& plan,
    const UnitKind kind,
    const double minimumWeight) {
    const auto existing = std::ranges::find(plan.composition, kind,
                                             &CompositionTarget::kind);
    if (existing == plan.composition.end()) {
        plan.composition.push_back({kind, minimumWeight});
    } else {
        existing->weight = std::max(existing->weight, minimumWeight);
    }
}

void normalizeComposition(StrategicPlan& plan) {
    const auto total = std::accumulate(
        plan.composition.begin(), plan.composition.end(), 0.0,
        [](const double sum, const CompositionTarget& target) {
            return sum + std::max(0.0, target.weight);
        });
    if (total <= 0.0) return;
    for (auto& target : plan.composition) target.weight /= total;
}

void goal(
    StrategicPlan& plan,
    const GoalKind kind,
    const UnitKind target,
    const int desired,
    const int priority,
    const std::string_view reason,
    const bool blocking = false) {
    static_cast<void>(appendUnitGoal(
        plan, kind, target, desired, priority, reason, blocking));
}

void technologyGoal(
    StrategicPlan& plan,
    const TechnologyKind technology,
    const int desiredLevel,
    const int priority,
    const std::string_view reason,
    const bool blocking = false) {
    static_cast<void>(appendTechnologyGoal(
        plan, technology, desiredLevel, priority, reason, blocking));
}

Frame estimatedStructureReadyFrame(
    const GameState& state,
    const UnitKind kind,
    std::vector<UnitKind>& visiting) {
    auto foundExisting = false;
    auto earliestExistingReady = std::numeric_limits<Frame>::max();
    const auto& stats = unitStats(kind);
    for (const auto& existing : state.self.units) {
        if (existing.kind != kind || existing.hallucination) continue;
        foundExisting = true;
        if (existing.disabled || existing.loaded ||
            (stats.requiresPsi && !existing.powered)) continue;
        if (existing.completed) {
            earliestExistingReady = state.frame;
            break;
        }
        const auto remaining = existing.remainingTrainFrames > 0
            ? existing.remainingTrainFrames
            : std::max(1, stats.buildTime *
                (100 - std::clamp(existing.buildProgress, 0, 100)) / 100);
        earliestExistingReady = std::min(earliestExistingReady, state.frame + remaining);
    }
    if (earliestExistingReady != std::numeric_limits<Frame>::max())
        return earliestExistingReady;
    // An observed but unusable structure does not justify estimating a second
    // copy. Its recovery time is unknown, so deadline feasibility is unknown.
    if (foundExisting) return -1;
    if (std::ranges::find(visiting, kind) != visiting.end()) return -1;
    visiting.push_back(kind);
    auto prerequisiteReady = state.frame;
    for (const auto prerequisite : unitPrerequisites(kind)) {
        const auto ready = estimatedStructureReadyFrame(state, prerequisite, visiting);
        if (ready < 0) {
            visiting.pop_back();
            return -1;
        }
        prerequisiteReady = std::max(prerequisiteReady, ready);
    }
    visiting.pop_back();
    const auto travel = stats.building
        ? std::clamp(state.pylonBuilderTravelFrames, 0, framesForMinutes(2)) : 0;
    return prerequisiteReady + travel + std::max(0, stats.buildTime);
}

Frame estimatedProducerAvailableFrame(const GameState& state, const UnitKind producer) {
    auto earliest = std::numeric_limits<Frame>::max();
    auto hasProducer = false;
    for (const auto& unit : state.self.units) {
        if (unit.kind != producer || !unit.completed) continue;
        hasProducer = true;
        if (unit.disabled || unit.loaded || unit.hallucination ||
            (unitStats(producer).requiresPsi && !unit.powered)) continue;
        auto available = state.frame + std::max(0, unit.remainingTrainFrames);
        const auto slot = std::ranges::find(state.self.producerSlots, unit.id,
                                             &ProducerSlotSnapshot::id);
        if (slot != state.self.producerSlots.end()) {
            if (slot->researching || slot->upgrading) continue;
            if (slot->activeTraining) {
                available = state.frame + std::max(0, slot->remainingTrainFrames);
                for (const auto queued : slot->queuedUnits)
                    available += std::max(1, unitStats(queued).buildTime);
            }
        } else if (std::ranges::find(state.self.busyProducers, producer) !=
                   state.self.busyProducers.end()) {
            // Legacy snapshots know that a producer is occupied but not when
            // its slot will reopen, so do not claim a deadline is feasible.
            continue;
        }
        earliest = std::min(earliest, available);
    }
    if (earliest != std::numeric_limits<Frame>::max()) return earliest;
    if (hasProducer) return -1;
    std::vector<UnitKind> visiting;
    return estimatedStructureReadyFrame(state, producer, visiting);
}

UnitKind trainingProducerFor(const UnitKind target) {
    switch (target) {
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

Frame estimatedGoalReadyFrame(const GameState& state, const ProductionGoal& goal) {
    if (goal.technology != TechnologyKind::none) {
        const auto& technology = technologyStats(goal.technology);
        if (technology.producer == UnitKind::unknown || technology.durationFrames <= 0 ||
            technologyInProgress(state.self, goal.technology)) return -1;
        std::vector<UnitKind> visiting;
        const auto producerReady =
            estimatedProducerAvailableFrame(state, technology.producer);
        auto prerequisiteReady = state.frame;
        const auto prerequisite = technologyPrerequisite(
            goal.technology, technologyLevel(state.self, goal.technology) + 1);
        if (prerequisite != UnitKind::unknown) {
            const auto ready = estimatedStructureReadyFrame(state, prerequisite, visiting);
            if (ready < 0) return -1;
            prerequisiteReady = ready;
        }
        if (producerReady < 0) return -1;
        return std::max(producerReady, prerequisiteReady) + technology.durationFrames;
    }

    if (goal.target == UnitKind::unknown) return -1;
    const auto& stats = unitStats(goal.target);
    std::vector<UnitKind> visiting;
    auto prerequisitesReady = state.frame;
    for (const auto prerequisite : unitPrerequisites(goal.target)) {
        const auto ready = estimatedStructureReadyFrame(state, prerequisite, visiting);
        if (ready < 0) return -1;
        prerequisitesReady = std::max(prerequisitesReady, ready);
    }
    if (goal.goal == GoalKind::train) {
        const auto producer = trainingProducerFor(goal.target);
        if (producer == UnitKind::unknown) return -1;
        const auto producerReady = estimatedProducerAvailableFrame(state, producer);
        if (producerReady < 0) return -1;
        return std::max(prerequisitesReady, producerReady) + stats.buildTime;
    }
    return estimatedStructureReadyFrame(state, goal.target, visiting);
}

bool detectorCheckpoint(const StrategicPlan& plan, const ProductionGoal& goal) {
    if (!plan.requireMobileDetection) return false;
    if (goal.target == UnitKind::observer || goal.target == UnitKind::observatory)
        return true;
    return goal.reason.find("detection") != std::string::npos ||
           goal.reason.find("Observer") != std::string::npos ||
           goal.reason.find("Dark Templar") != std::string::npos;
}

Position ourMain(const GameState& state) {
    const auto nexus = std::ranges::find(state.self.units, UnitKind::nexus, &UnitSnapshot::kind);
    return nexus != state.self.units.end() ? nexus->position : Position{-1, -1};
}

Position enemyMain(const GameState& state) {
    const auto depot = std::ranges::find_if(state.enemy.units, [](const UnitSnapshot& unit) {
        // Liftable resource depots keep their identity after BWAPI confirms
        // the old footprint is empty, but their current position is cleared.
        // Never let that legal stale-memory state suppress the remaining
        // building/base cleanup search.
        return unit.role == UnitRole::resourceDepot && unit.position.valid();
    });
    if (depot != state.enemy.units.end()) {
        return depot->position;
    }

    // Keep attacking known structures after the last depot falls. This avoids
    // the common cleanup failure where an army returns home while a tech
    // building survives elsewhere on the map.
    const auto building = std::ranges::find_if(state.enemy.units, [](const UnitSnapshot& unit) {
        return isBuilding(unit.kind) && unit.position.valid();
    });
    if (building != state.enemy.units.end()) return building->position;

    // Before the enemy start is confirmed, search the stalest plausible start.
    // Explicitly exclude our own start; the previous ownerId != -1 fallback
    // selected Protodd's main as soon as its Nexus was observed.
    const BaseSnapshot* candidate = nullptr;
    auto oldest = std::numeric_limits<Frame>::max();
    for (const auto& base : state.bases) {
        if (!base.startLocation || !base.center.valid() || base.ownerId == state.self.id) continue;
        if (base.ownerId == state.enemy.id) return base.center;
        if (base.ownerId == -1 && base.lastScouted < oldest) {
            oldest = base.lastScouted;
            candidate = &base;
        }
    }
    if (candidate != nullptr) return candidate->center;

    const auto visibleTarget = std::ranges::find_if(
        state.enemy.units, [](const UnitSnapshot& unit) {
            return unit.visible && unit.position.valid();
        });
    if (visibleTarget != state.enemy.units.end()) return visibleTarget->position;

    // Finally sweep stale non-owned expansions to reveal hidden buildings.
    oldest = std::numeric_limits<Frame>::max();
    for (const auto& base : state.bases) {
        if (!base.center.valid() || base.ownerId == state.self.id) continue;
        if (base.lastScouted < oldest) {
            oldest = base.lastScouted;
            candidate = &base;
        }
    }
    return candidate != nullptr ? candidate->center : Position{-1, -1};
}

Position nearestExpansionSite(const GameState& state) {
    const auto home = ourMain(state);
    if (!home.valid()) return {-1, -1};

    // BaseSnapshot centers are the canonical depot centers discovered by the
    // BWAPI bridge.  Pick the closest non-island, unowned resource base so a
    // strategic Nexus target is explicit before macro placement runs.  The
    // previous planners could request a second base without ever naming one;
    // the bridge then used the combat rally point and was free to select a
    // forward or otherwise inappropriate resource cluster.
    const auto firstExpansion = effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed) <= 1;
    const auto gasBaseAvailable = firstExpansion && std::ranges::any_of(
        state.bases, [](const BaseSnapshot& base) {
            return base.ownerId == -1 && !base.island && base.depotFootprintAvailable &&
                   base.center.valid() &&
                   base.mineralsRemaining >= 1000 && base.geysers > 0;
        });
    const BaseSnapshot* best = nullptr;
    auto bestScore = std::numeric_limits<double>::infinity();
    for (const auto& base : state.bases) {
        if (base.ownerId != -1 || base.island || !base.depotFootprintAvailable ||
            !base.center.valid() ||
            base.mineralsRemaining < 1000) continue;
        // The first expansion must be the normal gas natural when one exists.
        // After that, a closer mineral-only base can keep the economy behind
        // the same defended route instead of opening a distant gas flank.
        // Ground distance keeps a cliff-side pocket from looking artificially
        // closer than the reachable natural on maps such as Destination.
        if (gasBaseAvailable && base.geysers == 0) continue;
        const auto route = base.groundDistanceFromMain >= 0
                               ? static_cast<double>(base.groundDistanceFromMain)
                               : distance(home, base.center);
        // Gas remains valuable after the natural, but it should not win over
        // a mineral pocket more than a full screen closer by ground route.
        const auto score = route - (!firstExpansion && base.geysers > 0 ? 384.0 : 0.0);
        if (score < bestScore ||
            (score == bestScore && (best == nullptr || base.id < best->id))) {
            bestScore = score;
            best = &base;
        }
    }
    return best != nullptr ? best->center : Position{-1, -1};
}

Position readyReplacementExpansionSite(const GameState& state) {
    const BaseSnapshot* best = nullptr;
    auto bestRoute = std::numeric_limits<int>::max();
    for (const auto& base : state.bases) {
        if (base.ownerId != -1 || base.island || !base.depotFootprintAvailable ||
            !base.center.valid() ||
            !base.mineralLine.valid() || base.mineralPatches < 4 ||
            base.mineralsRemaining < 4000 || base.groundDistanceFromMain < 0) {
            continue;
        }
        const auto occupiedByOurNexus = std::ranges::any_of(
            state.self.units, [&base](const UnitSnapshot& unit) {
                return unit.kind == UnitKind::nexus && unit.position.valid() &&
                       insidePixelRadius(unit.position, base.center, PixelRadius{320});
            });
        if (occupiedByOurNexus) continue;
        if (base.groundDistanceFromMain < bestRoute ||
            (base.groundDistanceFromMain == bestRoute &&
             (best == nullptr || base.id < best->id))) {
            best = &base;
            bestRoute = base.groundDistanceFromMain;
        }
    }
    return best != nullptr ? best->center : Position{-1, -1};
}

Frame estimateMiningRunwayFrames(const GameState& state,
                                 const int workerCount,
                                 const int ownedMinerals) {
    if (ownedMinerals <= 0) return 0;
    if (workerCount <= 0) return -1;
    const auto gasWorkers = static_cast<int>(std::ranges::count_if(
        state.self.units, [](const UnitSnapshot& unit) {
            return isWorker(unit.kind) && unit.completed && !unit.loaded &&
                   !unit.disabled && unit.gatheringGas;
        }));
    const auto mineralWorkers = std::max(1, workerCount - gasWorkers);
    // Conservative observed-game average. Saturated patches can peak higher,
    // while route time and uneven saturation lower realized income.
    constexpr double mineralsPerWorkerFrame = 0.025;
    const auto frames = static_cast<double>(ownedMinerals) /
                        (mineralWorkers * mineralsPerWorkerFrame);
    return static_cast<Frame>(std::ceil(std::clamp(
        frames, 0.0, static_cast<double>(std::numeric_limits<Frame>::max()))));
}

Position lastNexusRebuildSite(const GameState& state) {
    if (state.self.minerals < unitStats(UnitKind::nexus).minerals ||
        !std::ranges::any_of(state.self.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::probe && unit.completed && !unit.loaded &&
                   !unit.disabled && !unit.hallucination && unit.position.valid();
        })) {
        return {-1, -1};
    }

    const BaseSnapshot* best = nullptr;
    auto bestScore = std::numeric_limits<double>::infinity();
    for (const auto& base : state.bases) {
        if (base.ownerId != -1 || base.island || !base.depotFootprintAvailable ||
            !base.center.valid() ||
            !base.mineralLine.valid() || base.mineralPatches <= 0 ||
            base.mineralsRemaining < 1000 || base.groundDistanceFromMain < 0) {
            continue;
        }
        const auto threatened = std::ranges::any_of(
            state.enemy.units, [&state, &base](const UnitSnapshot& enemy) {
                if (!enemy.completed || enemy.disabled || enemy.loaded ||
                    enemy.hallucination || enemy.invincible ||
                    enemy.groundWeapon.damage <= 0 || !enemy.position.valid() ||
                    (!enemy.visible && (enemy.lastSeen <= 0 ||
                     state.frame - enemy.lastSeen > framesForSeconds(5)))) {
                    return false;
                }
                const auto radius = std::max(480, enemy.groundWeapon.maxRange + 128);
                return withinPixelRadius(base.center, enemy.position, PixelRadius{radius});
            });
        if (threatened) continue;

        auto workerDistance = std::numeric_limits<double>::infinity();
        for (const auto& worker : state.self.units) {
            if (worker.kind != UnitKind::probe || !worker.completed || worker.loaded ||
                worker.disabled || worker.hallucination || !worker.position.valid()) {
                continue;
            }
            workerDistance = std::min(workerDistance, distance(worker.position, base.center));
        }
        const auto score = static_cast<double>(base.groundDistanceFromMain) +
            workerDistance * 0.25 - std::min(base.mineralsRemaining, 8000) * 0.02;
        if (score < bestScore ||
            (score == bestScore && (best == nullptr || base.id < best->id))) {
            bestScore = score;
            best = &base;
        }
    }
    return best != nullptr ? best->center : Position{-1, -1};
}

bool hardBreachAtMain(const GameState& state) noexcept {
    const auto home = ourMain(state);
    if (!home.valid()) return false;
    return std::ranges::any_of(
        state.enemy.units, [&home](const UnitSnapshot& enemy) {
            return enemy.visible && !enemy.flying && isCombatUnit(enemy.kind) &&
                   enemy.position.valid() &&
                   withinPixelRadius(enemy.position, home, PixelRadius{320});
        });
}

bool pvzEarlyPressureEligible(
    const GameState& state,
    const ThreatAssessment& threat) {
    return state.frame >= framesForMinutes(5) && state.frame < framesForMinutes(7) &&
           effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed) == 1 &&
           effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed) >= 6 &&
           effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::observed) > 0 &&
           threat.combatEnemiesNearMain == 0 && !activeApproach(state, threat) &&
           !hardBreachAtMain(state) &&
           recentEnemyCount(state, UnitKind::sunkenColony) == 0;
}

bool defensiveExpansionWindow(const GameState& state, const ThreatAssessment& threat) {
    // Holding an army at home and growing the economy are separate decisions.
    // Keep the rush/contain/detection vetoes, but don't require an attack order
    // before spending a saturated mineral line's income on another base.
    if (state.frame < framesForMinutes(6) || hardBreachAtMain(state) ||
        threat.combatEnemiesNearMain > 0 || activeApproach(state, threat) ||
        threat.immediateGround > 0.45 || threat.workerRush > 0.30 ||
        threat.proxy + threat.staticContain > 0.34 ||
        (threat.cloak > 0.28 && effectiveUnitCount(state, UnitKind::observer, UnitCountBasis::completed) == 0)) return false;
    const auto mobileArmy = std::ranges::count_if(state.self.units, [](const UnitSnapshot& unit) {
        return unit.completed && !unit.hallucination && !isBuilding(unit.kind) &&
               isCombatUnit(unit.kind) && !isWorker(unit.kind);
    });
    return mobileArmy >= 6;
}

bool coveredRangedNatural(const GameState& state, const ThreatAssessment& threat,
                          const double requiredScreenAdvantage = 1.50) {
    if (state.enemy.race != Race::protoss || state.frame < framesForMinutes(5) ||
        state.frame >= framesForMinutes(12) || effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed) != 1 ||
        effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::completed) != 1 || effectiveUnitCount(state, UnitKind::probe, UnitCountBasis::completed) < 18 ||
        effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::completed) == 0 ||
        effectiveUnitCount(state, UnitKind::roboticsFacility, UnitCountBasis::observed) == 0 || hardBreachAtMain(state) ||
        threat.workerRush > 0.30 || threat.proxy + threat.staticContain > 0.34 ||
        threat.air > 0.45) return false;
    const auto home = ourMain(state);
    const auto natural = nearestExpansionSite(state);
    if (!home.valid() || !natural.valid()) return false;
    const auto rangedTech = std::ranges::any_of(state.enemy.units, [](const UnitSnapshot& enemy) {
        return enemy.kind == UnitKind::cyberneticsCore || enemy.kind == UnitKind::roboticsFacility;
    }) || recentEnemyCount(state, UnitKind::dragoon) > 0;
    if (!rangedTech) return false;
    const auto detector = effectiveUnitCount(state, UnitKind::observer, UnitCountBasis::completed) > 0;
    if (!detector && (threat.cloak > 0.20 ||
        recentEnemyCount(state, UnitKind::darkTemplar) > 0 ||
        std::ranges::any_of(state.enemy.units, [](const UnitSnapshot& enemy) {
            return enemy.kind == UnitKind::citadelOfAdun || enemy.kind == UnitKind::templarArchives;
        }))) return false;
    if (std::ranges::any_of(state.self.units, [](const UnitSnapshot& unit) {
        return isWorker(unit.kind) && unit.underAttack;
    })) return false;

    auto screenPower = 0.0;
    auto healthyDragoons = 0;
    for (const auto& ally : state.self.units) {
        if (!ally.completed || ally.disabled || ally.loaded || ally.hallucination ||
            !ally.position.valid() || isBuilding(ally.kind) || !isCombatUnit(ally.kind) ||
            outsidePixelRadius(ally.position, home, PixelRadius{1400})) continue;
        screenPower += unitStats(ally.kind).combatValue * ally.healthFraction();
        if (ally.kind == UnitKind::dragoon && ally.healthFraction() >= 0.60) ++healthyDragoons;
    }
    if (healthyDragoons < 6) return false;
    auto pressurePower = 0.0;
    for (const auto& enemy : state.enemy.units) {
        if (!enemy.completed || enemy.disabled || enemy.hallucination || !enemy.position.valid() ||
            (!enemy.visible && (state.frame < enemy.lastSeen || state.frame - enemy.lastSeen > framesForSeconds(8))) ||
            isWorker(enemy.kind) || (!isCombatUnit(enemy.kind) && !isStaticDefense(enemy.kind))) continue;
        if (outsidePixelRadius(enemy.position, home, PixelRadius{1400}) &&
            outsidePixelRadius(enemy.position, natural, PixelRadius{960})) continue;
        if (!enemy.detected && (enemy.cloaked || enemy.burrowed || enemy.kind == UnitKind::darkTemplar))
            return false;
        // Equal Dragoon counts do not cover a splash-supported crossing.
        if (enemy.kind == UnitKind::reaver && effectiveUnitCount(state, UnitKind::reaver, UnitCountBasis::completed) == 0)
            return false;
        pressurePower += unitStats(enemy.kind).combatValue * enemy.healthFraction();
    }
    return screenPower >= pressurePower * requiredScreenAdvantage;
}

}
}  // namespace protodd::strategy_detail
