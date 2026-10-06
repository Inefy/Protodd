#include "protodd/Workers.hpp"

#include "protodd/UnitCatalog.hpp"
#include "protodd/Technology.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace protodd {
namespace {

constexpr Frame kLocalDefenseResponseFrames = 4 * 24;
constexpr Frame kWorkerEvacuationEmergencyFrames = 18;
constexpr Frame kWorkerEvacuationReentryThreatFrames = 3 * 24;
constexpr Frame kWorkerEvacuationQuietFrames = 24;

Frame timeToGroundContact(const UnitSnapshot& threat, const UnitSnapshot& worker) {
    if (!threat.position.valid() || !worker.position.valid() ||
        threat.groundWeapon.damage <= 0 || !threat.groundWeapon.targetsGround) {
        return std::numeric_limits<Frame>::max();
    }
    const auto range = std::max(0, threat.groundWeapon.maxRange) + 24;
    const auto approach = std::max(0.0, distance(threat.position, worker.position) - range);
    if (approach <= 0.0 || threat.topSpeed <= 0.0) return 0;
    return static_cast<Frame>(std::ceil(approach / threat.topSpeed));
}

struct EvacuationRoute {
    Position destination{-1, -1};
    Position waypoint{-1, -1};
    double score{std::numeric_limits<double>::infinity()};
};

EvacuationRoute safestEvacuationRoute(
    const UnitSnapshot& worker,
    const UnitSnapshot* threat,
    const Position preferredRefuge,
    const BaseSnapshot* localBase,
    const BaseSnapshot* safeBase,
    const InfluenceMap& influence,
    const NavigationGrid* navigation) {
    std::vector<Position> candidates;
    const auto addCandidate = [&candidates](const Position candidate) {
        if (!candidate.valid() || std::ranges::any_of(candidates, [candidate](const Position prior) {
                return distanceSquared(prior, candidate) <= 32 * 32;
            })) return;
        candidates.push_back(candidate);
    };
    addCandidate(preferredRefuge);
    if (safeBase != nullptr) {
        addCandidate(safeBase->mineralLine);
        addCandidate(safeBase->center);
    }
    if (localBase != nullptr) addCandidate(localBase->mineralLine);
    if (threat != nullptr && threat->position.valid()) {
        const auto dx = static_cast<double>(worker.position.x - threat->position.x);
        const auto dy = static_cast<double>(worker.position.y - threat->position.y);
        const auto magnitude = std::max(1.0, std::sqrt(dx * dx + dy * dy));
        constexpr double escapeDistance = 384.0;
        addCandidate({worker.position.x + static_cast<int>(std::lround(dx / magnitude * escapeDistance)),
                      worker.position.y + static_cast<int>(std::lround(dy / magnitude * escapeDistance))});
        addCandidate({worker.position.x + static_cast<int>(std::lround(-dy / magnitude * escapeDistance)),
                      worker.position.y + static_cast<int>(std::lround(dx / magnitude * escapeDistance))});
    }
    if (candidates.empty()) return {worker.position, worker.position, 0.0};

    EvacuationRoute best;
    for (const auto candidate : candidates) {
        Position waypoint = candidate;
        auto routeLength = distance(worker.position, candidate);
        auto peakThreat = 0.0F;
        auto destinationThreat = 0.0F;
        if (navigation != nullptr && !navigation->empty()) {
            auto start = worker.position;
            if (!navigation->walkable(start)) start = navigation->nearestWalkable(start);
            auto finish = candidate;
            if (!navigation->walkable(finish)) finish = navigation->nearestWalkable(finish);
            if (!start.valid() || !finish.valid()) continue;
            const auto path = navigation->findPath(start, finish, 700);
            if (path.empty()) continue;
            routeLength = 0.0;
            for (std::size_t index = 0; index < path.size(); ++index) {
                if (index > 0) routeLength += distance(path[index - 1], path[index]);
                if (index > 0 || path.size() == 1)
                    peakThreat = std::max(peakThreat, influence.at(path[index]).groundThreat);
            }
            destinationThreat = influence.at(path.back()).groundThreat;
            waypoint = path[std::min<std::size_t>(4, path.size() - 1)];
        } else {
            waypoint = influence.safestStep(worker.position, candidate, false);
            peakThreat = influence.maximumGroundThreat(worker.position, waypoint);
            destinationThreat = influence.at(waypoint).groundThreat;
            routeLength = distance(worker.position, waypoint);
        }
        auto score = static_cast<double>(peakThreat) * 100000.0 +
                     static_cast<double>(destinationThreat) * 10000.0 + routeLength;
        if (distanceSquared(candidate, preferredRefuge) <= 32 * 32) score -= 32.0;
        if (score < best.score) best = {candidate, waypoint, score};
    }
    if (best.waypoint.valid()) return best;

    const Position away = threat != nullptr && threat->position.valid()
        ? Position{2 * worker.position.x - threat->position.x,
                   2 * worker.position.y - threat->position.y}
        : preferredRefuge;
    auto fallback = influence.safestStep(worker.position, away, false);
    if (navigation != nullptr && !navigation->empty() && !navigation->walkable(fallback))
        fallback = navigation->nearestWalkable(fallback);
    if (!fallback.valid()) fallback = worker.position;
    return {fallback, fallback, 0.0};
}

Frame estimatedAttackArrivalFrames(const UnitSnapshot& defender,
                                   const UnitSnapshot& threat,
                                   const NavigationGrid* navigation) {
    if (!defender.position.valid() || !threat.position.valid() ||
        defender.topSpeed <= 0.0) {
        return std::numeric_limits<Frame>::max();
    }
    const auto& weapon = threat.flying ? defender.airWeapon : defender.groundWeapon;
    auto separation = std::sqrt(static_cast<double>(
        distanceSquared(defender.position, threat.position)));
    if (separation > weapon.maxRange && navigation != nullptr &&
        !navigation->empty() &&
        !navigation->lineWalkable(defender.position, threat.position)) {
        // Avoid spending pathfinding work on units that already miss the
        // response deadline by the straight-line lower bound. Bound A* too:
        // an inconclusive route is not credited as local protection.
        const auto lowerBoundFrames = (separation - std::max(0, weapon.maxRange)) /
                                      defender.topSpeed;
        if (lowerBoundFrames > kLocalDefenseResponseFrames)
            return std::numeric_limits<Frame>::max();
        const auto path = navigation->findPath(defender.position, threat.position, 3000);
        if (path.empty()) return std::numeric_limits<Frame>::max();
        separation = distance(defender.position, path.front()) +
            distance(path.back(), threat.position);
        for (auto step = path.begin() + 1; step != path.end(); ++step)
            separation += distance(*(step - 1), *step);
    }
    const auto approachDistance = std::max(
        0.0, separation - static_cast<double>(std::max(0, weapon.maxRange)));
    // This is a straight-line lower bound because WorkerManager has no terrain
    // route. Units whose lower-bound travel time exceeds the emergency window
    // cannot help in time; units inside it still need an actually reachable path.
    return static_cast<Frame>(std::ceil(approachDistance / defender.topSpeed));
}

int militiaDemand(const UnitSnapshot& enemy, const Frame frame) {
    if (!enemy.visible || !enemy.detected || enemy.flying || enemy.hallucination) return 0;
    if (isWorker(enemy.kind)) return 1;
    if (isBuilding(enemy.kind)) {
        if (enemy.completed) return 0;
        if (enemy.kind == UnitKind::photonCannon || enemy.kind == UnitKind::bunker ||
            enemy.kind == UnitKind::sunkenColony) {
            return 4;
        }
        if (enemy.kind == UnitKind::pylon) return 2;
        if (enemy.kind == UnitKind::gateway || enemy.kind == UnitKind::barracks ||
            enemy.kind == UnitKind::forge) {
            return 3;
        }
        return 1;
    }

    // Worker surrounds are an emergency bridge until the first combat units
    // arrive, never a general answer to ranged or high-tier armies.
    if (frame < 7 * 60 * 24 && enemy.kind == UnitKind::zergling) return 2;
    if (frame < 6 * 60 * 24 && enemy.kind == UnitKind::zealot) return 3;
    // A detected Dark Templar is still a short-range melee unit, and it can
    // erase the entire mineral line after the mobile screen has been traded
    // away.  Treat it like a small emergency surround target rather than
    // allowing healthy Probes to keep mining underneath the cloak alarm.
    if (enemy.kind == UnitKind::darkTemplar && frame < 16 * 60 * 24) return 3;
    if (frame < 5 * 60 * 24 && enemy.kind == UnitKind::marine) return 1;
    return 0;
}

int targetPriority(const UnitSnapshot& enemy) {
    if (!enemy.completed && (enemy.kind == UnitKind::photonCannon ||
                             enemy.kind == UnitKind::bunker ||
                             enemy.kind == UnitKind::sunkenColony)) {
        return 4;
    }
    if (isWorker(enemy.kind)) return 3;
    if (!enemy.completed && isBuilding(enemy.kind)) return 2;
    return 1;
}

bool safeGroundRoute(const UnitSnapshot& worker, const Position destination,
                     const InfluenceMap& influence,
                     const NavigationGrid* navigation) {
    if (!worker.position.valid() || !destination.valid()) return false;
    if (navigation == nullptr || navigation->empty())
        return influence.maximumGroundThreat(worker.position, destination) <= 0.25F;

    const auto path = navigation->findPath(worker.position, destination);
    if (path.empty()) return false;
    auto previous = worker.position;
    for (const auto waypoint : path) {
        if (influence.maximumGroundThreat(previous, waypoint) > 0.25F) return false;
        previous = waypoint;
    }
    return influence.maximumGroundThreat(previous, destination) <= 0.25F;
}

}  // namespace

UnitId selectMineralPatch(
    const std::span<const MineralPatchCandidate> candidates,
    const Position mineralLine,
    const Position workerPosition,
    const UnitId currentTarget) noexcept {
    auto selected = UnitId{-1};
    auto bestScore = std::numeric_limits<long long>::max();
    for (const auto& patch : candidates) {
        if (patch.id < 0 || !patch.position.valid()) continue;
        const auto load = std::max(0, patch.assignedWorkers);
        const auto anchorDistance = distanceSquared(patch.position, mineralLine);
        const auto workerDistance = distanceSquared(patch.position, workerPosition);
        const auto stabilityBonus = patch.id == currentTarget ? 5'000'000LL : 0LL;
        const auto score = static_cast<long long>(load) * 1'000'000'000LL +
                           static_cast<long long>(anchorDistance) * 4LL +
                           static_cast<long long>(workerDistance) - stabilityBonus;
        if (score < bestScore || (score == bestScore &&
                                  (selected < 0 || patch.id < selected))) {
            bestScore = score;
            selected = patch.id;
        }
    }
    return selected;
}

const std::unordered_map<UnitId, UnitId>& MineralAllocator::assign(
    const std::span<const MineralWorker> workers,
    const std::span<const MineralPatchCandidate> patches) {
    std::unordered_map<UnitId, UnitId> retained;
    std::unordered_map<UnitId, int> load;
    for (const auto& worker : workers) {
        const auto previous = targets_.find(worker.id);
        const auto target = previous != targets_.end() ? previous->second : worker.currentTarget;
        const auto patch = std::ranges::find(patches, target, &MineralPatchCandidate::id);
        if (patch != patches.end() &&
            distanceSquared(patch->position, worker.mineralLine) <= 480 * 480) {
            retained[worker.id] = target;
            ++load[target];
        }
    }
    for (const auto& worker : workers) {
        const auto previous = retained.find(worker.id);
        const auto current = previous != retained.end() ? previous->second : -1;
        if (current >= 0) --load[current];
        std::vector<MineralPatchCandidate> candidates;
        for (const auto& patch : patches) {
            if (distanceSquared(patch.position, worker.mineralLine) <= 480 * 480) {
                candidates.push_back({patch.id, patch.position, load[patch.id]});
            }
        }
        const auto selected = selectMineralPatch(candidates, worker.mineralLine,
                                                 worker.position, current);
        if (selected >= 0) {
            retained[worker.id] = selected;
            ++load[selected];
        }
    }
    targets_ = std::move(retained);
    return targets_;
}

int GasBankController::target(const GameState& state, const StrategicPlan& plan) {
    if (state.frame < lastFrame_) paused_ = false;
    lastFrame_ = state.frame;
    struct Demand {
        UnitKind kind;
        int desiredCount;
    };
    std::vector<Demand> unitDemands;
    std::vector<std::pair<TechnologyKind, int>> technologyDemands;
    for (const auto& goal : plan.goals) {
        if (!goal.blocking) continue;
        if ((goal.goal == GoalKind::research || goal.goal == GoalKind::upgrade) &&
            goal.technology != TechnologyKind::none) {
            const auto found = std::ranges::find(technologyDemands, goal.technology,
                &std::pair<TechnologyKind, int>::first);
            if (found == technologyDemands.end())
                technologyDemands.emplace_back(goal.technology, goal.desiredCount);
            else
                found->second = std::max(found->second, goal.desiredCount);
            continue;
        }
        if (goal.target == UnitKind::unknown) continue;
        const auto found = std::ranges::find(unitDemands, goal.target, &Demand::kind);
        if (found == unitDemands.end())
            unitDemands.push_back({goal.target, goal.desiredCount});
        else
            found->desiredCount = std::max(found->desiredCount, goal.desiredCount);
    }

    auto obligation = 0;
    for (const auto& [technology, desiredLevel] : technologyDemands) {
        const auto level = technologyLevel(state.self, technology);
        if (level < desiredLevel && !technologyInProgress(state.self, technology))
            obligation += technologyStats(technology).gasCost(level + 1);
    }
    for (const auto& demand : unitDemands) {
        const auto existing = std::ranges::count(state.self.units, demand.kind,
                                                   &UnitSnapshot::kind) +
                              std::ranges::count(state.self.queuedUnits, demand.kind);
        // One next cycle per distinct target is useful to protect against a
        // mineral-heavy field army consuming the bank. Units already alive or
        // queued have paid their gas, so only an unmet target adds a reserve.
        if (existing < demand.desiredCount) obligation += unitStats(demand.kind).gas;
    }
    const auto highWater = std::max(300, obligation + 100);
    // Separate stop/resume thresholds prevent every 50-mineral spend or
    // strategic plan flip from shuffling workers between gas and minerals.
    if (paused_ && (state.self.gas < highWater / 2 || state.self.minerals >= 300)) paused_ = false;
    if (!paused_ && state.self.minerals < 150 && state.self.gas >= highWater) paused_ = true;
    return paused_ ? 0 : std::max(0, plan.desiredGasWorkers);
}

std::vector<WorkerAssignment> WorkerManager::assign(
    const GameState& state,
    const StrategicPlan& plan,
    const InfluenceMap& influence,
    const std::span<const UnitId> reservedBuilders,
    const bool evacuateAbandonedBase,
    const bool stageExpansionWorkers,
    const NavigationGrid* navigation) const {
    std::vector<const UnitSnapshot*> workers;
    for (const auto& unit : state.self.units) {
        if (isWorker(unit.kind) && unit.completed && !unit.loaded &&
            !unit.disabled && !unit.hallucination) {
            workers.push_back(&unit);
        }
    }
    std::ranges::sort(workers, {}, [](const UnitSnapshot* worker) { return worker->id; });

    std::unordered_set<UnitId> builders(reservedBuilders.begin(), reservedBuilders.end());
    std::vector<WorkerAssignment> result;
    result.reserve(workers.size());

    std::vector<const BaseSnapshot*> ownedBases;
    for (const auto& base : state.bases) {
        if (base.ownerId == state.self.id && base.center.valid()) {
            ownedBases.push_back(&base);
        }
    }
    std::ranges::sort(ownedBases, {}, [](const BaseSnapshot* base) { return base->id; });
    const auto baseForWorker = [&ownedBases](const UnitSnapshot& worker) {
        const BaseSnapshot* nearest = nullptr;
        auto nearestDistance = std::numeric_limits<int>::max();
        for (const auto* base : ownedBases) {
            const auto separation = distanceSquared(worker.position, base->center);
            if (separation < nearestDistance) {
                nearestDistance = separation;
                nearest = base;
            }
        }
        return nearest;
    };
    std::unordered_map<std::uint64_t, bool> routeSafetyCache;
    const auto safeRoute = [&routeSafetyCache, &influence, navigation](
        const UnitSnapshot* worker, const int destinationKey, const Position destination) {
        const auto key = (static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(worker->id)) << 32U) |
            static_cast<std::uint32_t>(destinationKey);
        if (const auto known = routeSafetyCache.find(key); known != routeSafetyCache.end())
            return known->second;
        const auto safe = safeGroundRoute(*worker, destination, influence, navigation);
        routeSafetyCache.emplace(key, safe);
        return safe;
    };
    const auto safeBase = safestOwnedBase(state, influence);
    if (lastAssignedFrame_ >= 0 && state.frame < lastAssignedFrame_)
        evacuationMemory_.clear();
    lastAssignedFrame_ = state.frame;
    for (auto memory = evacuationMemory_.begin(); memory != evacuationMemory_.end();) {
        const auto worker = std::ranges::find(workers, memory->first,
            [](const UnitSnapshot* candidate) { return candidate->id; });
        if (worker == workers.end() || (*worker)->firstSeen != memory->second.firstSeen) {
            memory = evacuationMemory_.erase(memory);
        } else {
            ++memory;
        }
    }
    const auto hasCompletedStaticScreen = std::ranges::any_of(
        state.self.units, [](const UnitSnapshot& unit) {
            return unit.completed && (unit.kind == UnitKind::photonCannon ||
                                      unit.kind == UnitKind::shieldBattery);
        });
    std::unordered_map<std::uint64_t, int> localScreenCountByThreat;
    const auto localScreenCount = [&](const UnitSnapshot& threat,
                                      const BaseSnapshot* base) {
        if (base == nullptr) return 0;
        const auto cacheKey = (static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(base->id)) << 32U) |
            static_cast<std::uint32_t>(threat.id);
        if (const auto known = localScreenCountByThreat.find(cacheKey);
            known != localScreenCountByThreat.end()) return known->second;
        auto count = 0;
        const auto detected = threat.kind != UnitKind::darkTemplar || threat.detected ||
            std::ranges::any_of(state.self.units, [&threat](const UnitSnapshot& detector) {
                return detector.completed && detector.position.valid() &&
                    ((detector.kind == UnitKind::observer &&
                      distanceSquared(detector.position, threat.position) <= 320 * 320) ||
                     ((detector.kind == UnitKind::photonCannon ||
                       detector.kind == UnitKind::missileTurret ||
                       detector.kind == UnitKind::sporeColony) && detector.powered &&
                      distanceSquared(detector.position, threat.position) <= 320 * 320));
            });
        if (detected) {
            for (const auto& defender : state.self.units) {
                if (!defender.completed || !isCombatUnit(defender.kind) || defender.flying ||
                    defender.disabled || defender.loaded || defender.hallucination ||
                    defender.invincible || defender.healthFraction() < 0.35 ||
                    baseForWorker(defender) != base || !defender.canAttack(threat)) continue;
                if (estimatedAttackArrivalFrames(defender, threat, navigation) <=
                    kLocalDefenseResponseFrames) ++count;
            }
        }
        localScreenCountByThreat[cacheKey] = count;
        return count;
    };
    std::vector<const UnitSnapshot*> available;
    available.reserve(workers.size());

    for (const auto* worker : workers) {
        const auto builderLease = builders.contains(worker->id);
        const auto* localBase = baseForWorker(*worker);
        auto memory = evacuationMemory_.find(worker->id);
        if (memory != evacuationMemory_.end() &&
            memory->second.firstSeen != worker->firstSeen) {
            evacuationMemory_.erase(memory);
            memory = evacuationMemory_.end();
        }
        const UnitSnapshot* nearestDanger = nullptr;
        auto nearestDangerFrames = std::numeric_limits<Frame>::max();
        const UnitSnapshot* nearestRetainedThreat = nullptr;
        auto nearestRetainedFrames = std::numeric_limits<Frame>::max();
        for (const auto& enemy : state.enemy.units) {
            if (!enemy.completed || enemy.flying || enemy.disabled || enemy.loaded ||
                enemy.hallucination || enemy.invincible ||
                enemy.groundWeapon.damage <= 0 || !enemy.groundWeapon.targetsGround ||
                !enemy.position.valid() ||
                (!enemy.visible && (enemy.lastSeen <= 0 ||
                    state.frame - enemy.lastSeen > kWorkerEvacuationReentryThreatFrames))) {
                continue;
            }
            const auto targetsWorker = enemy.orderTargetId == worker->id;
            const auto workerThreat = isWorker(enemy.kind);
            if (workerThreat && !targetsWorker && !worker->underAttack) continue;
            if (!targetsWorker && localBase != nullptr &&
                distanceSquared(enemy.position, localBase->center) > 800 * 800) continue;
            const auto arrival = timeToGroundContact(enemy, *worker);
            if (arrival < nearestRetainedFrames) {
                nearestRetainedFrames = arrival;
                nearestRetainedThreat = &enemy;
            }
            const auto militiaCanRespond = militiaDemand(enemy, state.frame) > 0 &&
                !(enemy.kind == UnitKind::marine && hasCompletedStaticScreen);
            if ((targetsWorker ||
                 (arrival <= kWorkerEvacuationEmergencyFrames && !militiaCanRespond)) &&
                (arrival < nearestDangerFrames || targetsWorker)) {
                nearestDangerFrames = arrival;
                nearestDanger = &enemy;
            }
        }
        const auto protectedByScreen = [&](const UnitSnapshot* threat) {
            return threat != nullptr && !worker->underAttack &&
                threat->orderTargetId != worker->id &&
                localScreenCount(*threat, localBase) >= 2;
        };
        if (protectedByScreen(nearestDanger)) {
            nearestDanger = nullptr;
        }
        const auto immediateDanger = worker->underAttack || nearestDanger != nullptr;
        const auto sameRetainedThreat = memory != evacuationMemory_.end() &&
            nearestRetainedThreat != nullptr &&
            (nearestRetainedThreat->id == memory->second.threatId ||
             nearestRetainedThreat->orderTargetId == worker->id);
        const auto localThreatPresent = std::ranges::any_of(
            state.enemy.units, [&](const UnitSnapshot& enemy) {
                return enemy.completed && !enemy.flying && !enemy.disabled &&
                    !enemy.loaded && !enemy.hallucination && !enemy.invincible &&
                    enemy.groundWeapon.damage > 0 && enemy.groundWeapon.targetsGround &&
                    enemy.position.valid() &&
                    (enemy.visible || (enemy.lastSeen > 0 &&
                     state.frame - enemy.lastSeen <= kWorkerEvacuationReentryThreatFrames)) &&
                    (localBase == nullptr ||
                     distanceSquared(enemy.position, localBase->center) <= 800 * 800);
            });
        const auto retainedDanger = memory != evacuationMemory_.end() &&
            ((sameRetainedThreat &&
              nearestRetainedFrames <= kWorkerEvacuationReentryThreatFrames &&
              !protectedByScreen(nearestRetainedThreat)) ||
             worker->underAttack);
        const auto holdAfterClear = memory != evacuationMemory_.end() &&
            nearestRetainedThreat == nullptr && !localThreatPresent &&
            state.frame >= memory->second.lastDangerFrame &&
            state.frame - memory->second.lastDangerFrame <= kWorkerEvacuationQuietFrames;
        if (immediateDanger || retainedDanger || holdAfterClear) {
            auto threat = nearestDanger != nullptr ? nearestDanger : nearestRetainedThreat;
            if (threat == nullptr && memory != evacuationMemory_.end()) {
                const auto rememberedThreat = std::ranges::find(
                    state.enemy.units, memory->second.threatId, &UnitSnapshot::id);
                if (rememberedThreat != state.enemy.units.end())
                    threat = &*rememberedThreat;
            }
            auto refuge = memory != evacuationMemory_.end()
                ? memory->second.refuge
                : safeBase != nullptr && safeBase->mineralLine.valid()
                    ? safeBase->mineralLine
                    : safeBase != nullptr ? safeBase->center : Position{-1, -1};
            auto threatId = threat != nullptr ? threat->id
                : memory != evacuationMemory_.end() ? memory->second.threatId : -1;
            if (immediateDanger || retainedDanger) {
                const auto route = safestEvacuationRoute(
                    *worker, threat, refuge, localBase, safeBase, influence, navigation);
                refuge = route.destination;
                const auto lastDanger = state.frame;
                evacuationMemory_[worker->id] = {
                    lastDanger, worker->firstSeen, localBase != nullptr ? localBase->id : -1,
                    threatId, refuge};
                result.push_back({worker->id, WorkerJob::evacuate,
                    safeBase != nullptr ? safeBase->id : -1, threatId,
                    route.waypoint, 99});
            } else {
                const auto route = safestEvacuationRoute(
                    *worker, nullptr, refuge, localBase, safeBase, influence, navigation);
                if (distanceSquared(worker->position, refuge) <= 128 * 128) {
                    result.push_back({worker->id, WorkerJob::idle,
                        localBase != nullptr ? localBase->id : -1, threatId,
                        refuge, 99});
                } else {
                    result.push_back({worker->id, WorkerJob::evacuate,
                        safeBase != nullptr ? safeBase->id : -1, threatId,
                        route.waypoint, 99});
                }
            }
            continue;
        }
        if (memory != evacuationMemory_.end()) evacuationMemory_.erase(memory);

        // A live construction lease blocks ordinary work reassignment, but it
        // must not make a Probe immune to the same contact-time escape logic
        // as other workers. The BWAPI bridge cancels an endangered en-route
        // build and retains the lease until the old order is observed gone.
        // Once the structure exists, the bridge drops the lease and the Probe
        // can evacuate without cancelling a construction that has started.
        if (builderLease) {
            result.push_back({worker->id, WorkerJob::build, -1, -1, {-1, -1}, 100});
            continue;
        }

        if (evacuateAbandonedBase && safeBase != nullptr) {
            // After a Nexus falls, its surviving Probes can keep receiving
            // ordinary mining/transfer orders through a hostile mineral line.
            // Give the still-exposed workers a high-priority safe route before
            // regular resource balancing attempts to recover production.
            const BaseSnapshot* abandonedBase = nullptr;
            auto localDistance = 640 * 640 + 1;
            for (const auto& base : state.bases) {
                if (!base.center.valid()) continue;
                const auto separation = distanceSquared(worker->position, base.center);
                if (separation < localDistance) {
                    localDistance = separation;
                    abandonedBase = &base;
                }
            }
            if (abandonedBase != nullptr && abandonedBase->ownerId != state.self.id &&
                abandonedBase->id != safeBase->id &&
                distanceSquared(abandonedBase->center, safeBase->center) > 640 * 640) {
                const auto threatening = [&state](const BaseSnapshot& base) {
                    return std::ranges::any_of(state.enemy.units, [&state, &base](
                        const UnitSnapshot& enemy) {
                        return enemy.position.valid() && enemy.completed &&
                            !enemy.disabled && !enemy.hallucination && !enemy.loaded &&
                            enemy.groundWeapon.damage > 0 &&
                            (enemy.visible || (enemy.lastSeen > 0 &&
                                state.frame - enemy.lastSeen <= 3 * 24)) &&
                            distanceSquared(enemy.position, base.center) <= 800 * 800;
                    });
                };
                if (threatening(*abandonedBase) && !threatening(*safeBase)) {
                    const auto destination = safeBase->mineralLine.valid()
                        ? safeBase->mineralLine : safeBase->center;
                    result.push_back({worker->id, WorkerJob::evacuate, safeBase->id,
                        -1, influence.safestStep(worker->position,
                                                 destination, false), 98});
                    continue;
                }
            }
        }

        const auto rangedBio = std::ranges::min_element(
            state.enemy.units, {}, [worker, &ownedBases](const UnitSnapshot& enemy) {
                const auto relevant = enemy.visible && enemy.detected && enemy.completed &&
                                      enemy.kind == UnitKind::marine &&
                                      enemy.position.valid() &&
                                      std::ranges::any_of(
                                          ownedBases, [&enemy](const BaseSnapshot* base) {
                                              return distanceSquared(enemy.position,
                                                                     base->center) <
                                                     512 * 512;
                                          });
                return relevant
                           ? distanceSquared(worker->position, enemy.position)
                           : std::numeric_limits<int>::max();
            });
        if (hasCompletedStaticScreen && rangedBio != state.enemy.units.end() &&
            rangedBio->visible && rangedBio->kind == UnitKind::marine &&
            distanceSquared(worker->position, rangedBio->position) < 288 * 288) {
            const Position away{
                worker->position.x + worker->position.x - rangedBio->position.x,
                worker->position.y + worker->position.y - rangedBio->position.y,
            };
            result.push_back({worker->id, WorkerJob::evacuate,
                              safeBase != nullptr ? safeBase->id : -1,
                              rangedBio->id,
                              influence.safestStep(worker->position, away, false), 99});
            continue;
        }

        const auto local = influence.at(worker->position);
        const auto woundedNearThreat = worker->healthFraction() < 0.75;
        if ((worker->healthFraction() < 0.35 || woundedNearThreat) &&
            local.groundThreat > 0.25F && safeBase != nullptr) {
            const auto threat = std::ranges::min_element(
                state.enemy.units, {}, [worker](const UnitSnapshot& enemy) {
                    return enemy.visible && enemy.position.valid() &&
                                   enemy.groundWeapon.damage > 0
                               ? distanceSquared(worker->position, enemy.position)
                               : std::numeric_limits<int>::max();
                });
            const auto threatId = threat != state.enemy.units.end() &&
                                          threat->visible &&
                                          threat->groundWeapon.damage > 0
                                      ? threat->id
                                      : -1;
            const auto escape = influence.safestStep(
                worker->position, safeBase->mineralLine, false);
            result.push_back({worker->id, WorkerJob::evacuate, safeBase->id,
                              threatId, escape, 98});
            continue;
        }

        // An undetected DT cannot be a militia target. Only workers within
        // its approach radius escape; safe workers keep the detection funded.
        const auto cloakedNearBase = std::ranges::min_element(
            state.enemy.units, {}, [worker](const UnitSnapshot& enemy) {
                return enemy.visible && enemy.position.valid() &&
                               enemy.kind == UnitKind::darkTemplar &&
                               !enemy.detected
                           ? distanceSquared(enemy.position, worker->position)
                           : std::numeric_limits<int>::max();
            });
        if (safeBase != nullptr && cloakedNearBase != state.enemy.units.end() &&
            cloakedNearBase->visible && cloakedNearBase->position.valid() &&
            cloakedNearBase->kind == UnitKind::darkTemplar &&
            !cloakedNearBase->detected &&
            distanceSquared(worker->position, cloakedNearBase->position) <= 160 * 160) {
            const Position away{2 * worker->position.x - cloakedNearBase->position.x,
                                2 * worker->position.y - cloakedNearBase->position.y};
            const auto escape = influence.safestStep(
                worker->position, away, false);
            result.push_back({worker->id, WorkerJob::evacuate, safeBase->id,
                              cloakedNearBase->id, escape, 99});
            continue;
        }
        available.push_back(worker);
    }

    // A worker militia only answers threats for which worker contact is useful:
    // workers, unfinished proxies, and tiny opening-unit groups. In particular,
    // never feed Probes into tanks or completed static defenses.
    struct MilitiaTarget {
        const UnitSnapshot* unit{};
        int demand{};
        const BaseSnapshot* base{};
    };
    std::vector<MilitiaTarget> baseThreats;
    const auto localEnemyWorkers = std::ranges::count_if(
        state.enemy.units, [&ownedBases](const UnitSnapshot& enemy) {
            return enemy.visible && isWorker(enemy.kind) && enemy.position.valid() &&
                   std::ranges::any_of(ownedBases, [&enemy](const BaseSnapshot* base) {
            return distanceSquared(enemy.position, base->center) < 640 * 640;
                   });
        });
    for (const auto& enemy : state.enemy.units) {
        auto demand = militiaDemand(enemy, state.frame);
        // A militia time limit must not also switch off worker protection.
        const auto meleeDanger = enemy.visible && enemy.completed && !enemy.hallucination &&
                                 (enemy.kind == UnitKind::zealot || enemy.kind == UnitKind::zergling ||
                                  enemy.kind == UnitKind::darkTemplar);
        if ((demand == 0 && !meleeDanger) || !enemy.position.valid()) continue;
        // Probes cannot close on ranged bio efficiently. Once a Cannon/Battery
        // screen exists, charging Marines only donates the economy and blocks
        // the combat units that should be using that screen.
        if (enemy.kind == UnitKind::marine && hasCompletedStaticScreen) continue;
        if (isWorker(enemy.kind) && localEnemyWorkers < 3) {
            const auto attackingProbe = enemy.orderTargetId >= 0 &&
                std::ranges::any_of(workers, [&enemy](const UnitSnapshot* worker) {
                    return worker->id == enemy.orderTargetId;
                });
            const auto hurtingMineralLine = std::ranges::any_of(
                workers, [&enemy](const UnitSnapshot* worker) {
                    return worker->underAttack &&
                           distanceSquared(worker->position, enemy.position) <= 96 * 96;
                });
            // Ignore a harmless scouting worker, but immediately surround one
            // that has started attacking the mineral line. One defending Probe
            // merely trades hits with an SCV; two can prevent the repeat kills
            // seen while the rest of the line continued mining.
            if (!attackingProbe && !hurtingMineralLine) continue;
            demand = std::max(demand, 2);
        }
        const BaseSnapshot* nearestThreatenedBase = nullptr;
        auto nearestThreatDistance = 640 * 640;
        for (const auto* base : ownedBases) {
            const auto separation = distanceSquared(enemy.position, base->center);
            if (separation < nearestThreatDistance) {
                nearestThreatDistance = separation;
                nearestThreatenedBase = base;
            }
        }
        if (nearestThreatenedBase != nullptr)
            baseThreats.push_back({&enemy, demand, nearestThreatenedBase});
    }
    std::unordered_map<const BaseSnapshot*, int> localArmyByBase;
    for (const auto& target : baseThreats) localArmyByBase.try_emplace(target.base, 0);
    for (const auto& unit : state.self.units) {
        if (!unit.completed || !isCombatUnit(unit.kind) || unit.flying || unit.disabled ||
            unit.loaded || unit.hallucination || unit.healthFraction() < 0.35 ||
            unit.invincible) continue;
        const MilitiaTarget* nearestCoveredThreat = nullptr;
        auto nearestArrivalFrames = kLocalDefenseResponseFrames + 1;
        for (const auto& target : baseThreats) {
            if (target.unit->kind == UnitKind::darkTemplar && !target.unit->detected &&
                !std::ranges::any_of(state.self.units, [&target](const UnitSnapshot& detector) {
                    return detector.completed && detector.position.valid() &&
                        ((detector.kind == UnitKind::observer &&
                          distanceSquared(detector.position, target.unit->position) <= 320 * 320) ||
                         ((detector.kind == UnitKind::photonCannon ||
                           detector.kind == UnitKind::missileTurret ||
                           detector.kind == UnitKind::sporeColony) && detector.powered &&
                          distanceSquared(detector.position, target.unit->position) <= 320 * 320));
                })) continue;
            if (!unit.canAttack(*target.unit)) continue;
            const auto arrivalFrames = estimatedAttackArrivalFrames(
                unit, *target.unit, navigation);
            if (arrivalFrames <= kLocalDefenseResponseFrames &&
                arrivalFrames < nearestArrivalFrames) {
                nearestArrivalFrames = arrivalFrames;
                nearestCoveredThreat = &target;
            }
        }
        if (nearestCoveredThreat != nullptr) ++localArmyByBase[nearestCoveredThreat->base];
    }
    // Do not turn a defended mineral line into a second melee squad.  The
    // previous demand calculation sent up to eight Probes into every visible
    // Zealot wave even when four-to-six Zealots/Dragoons were already in
    // contact.  Those workers were then lost, the mineral income collapsed,
    // and the next reinforcement cycle never arrived.  Keep militia as a
    // true last resort: it becomes eligible again if the mobile screen has
    // been wiped down to one or fewer nearby combat units.
    std::unordered_map<const BaseSnapshot*, int> demandByBase;
    std::unordered_map<const BaseSnapshot*, int> defendersNeededByBase;
    for (const auto& target : baseThreats) {
        const auto melee = target.unit->kind == UnitKind::zealot ||
                           target.unit->kind == UnitKind::zergling ||
                           target.unit->kind == UnitKind::darkTemplar;
        if (!(melee && localArmyByBase[target.base] >= 2))
            demandByBase[target.base] += target.demand;
    }
    auto requestedDefenders = 0;
    for (const auto& [base, demand] : demandByBase) {
        const auto covered = localArmyByBase[base];
        const auto needed = std::max(0, demand - covered * 2);
        defendersNeededByBase[base] = needed;
        requestedDefenders += needed;
    }
    // Evacuation is local to each exposed mineral line. A capable screen at
    // another base cannot trigger worker movement here, and a healthy local
    // screen keeps that line mining instead of needlessly pulling Probes.
    if (safeBase != nullptr && state.frame >= 5 * 60 * 24) {
        std::unordered_map<const BaseSnapshot*, int> evacuationBudgetByBase;
        std::unordered_set<const BaseSnapshot*> initializedEvacuationBudgets;
        for (auto worker = available.begin(); worker != available.end(); ++worker) {
            const auto* base = baseForWorker(**worker);
            if (base != nullptr) ++evacuationBudgetByBase[base];
        }
        for (const auto& target : baseThreats) {
            const auto melee = target.unit->kind == UnitKind::zealot ||
                               target.unit->kind == UnitKind::zergling ||
                               target.unit->kind == UnitKind::darkTemplar;
            if (!melee || localArmyByBase[target.base] >= 2) continue;
            auto budget = evacuationBudgetByBase.find(target.base);
            if (budget == evacuationBudgetByBase.end()) continue;
            if (initializedEvacuationBudgets.insert(target.base).second)
                budget->second = std::max(0, budget->second - 4);
            while (budget->second > 0) {
                auto closestWorker = available.end();
                auto closestDistance = 160 * 160 + 1;
                for (auto worker = available.begin(); worker != available.end(); ++worker) {
                    if (baseForWorker(**worker) != target.base) continue;
                    const auto separation = distanceSquared(
                        (*worker)->position, target.unit->position);
                    if (separation < closestDistance) {
                        closestDistance = separation;
                        closestWorker = worker;
                    }
                }
                if (closestWorker == available.end()) break;
                // Move directly away from the local attacker. The adapter
                // resumes mining once the worker has regained separation.
                const auto* localBase = baseForWorker(**closestWorker);
                const auto refuge = safeBase->mineralLine.valid()
                    ? safeBase->mineralLine : safeBase->center;
                const auto route = safestEvacuationRoute(
                    **closestWorker, target.unit, refuge, localBase, safeBase,
                    influence, navigation);
                evacuationMemory_[(*closestWorker)->id] = {
                    state.frame, (*closestWorker)->firstSeen,
                    localBase != nullptr ? localBase->id : -1,
                    target.unit->id, route.destination};
                result.push_back({(*closestWorker)->id, WorkerJob::evacuate, safeBase->id,
                                  target.unit->id, route.waypoint, 97});
                available.erase(closestWorker);
                --budget->second;
            }
        }
    }
    const auto meleeBreach = std::ranges::any_of(
        baseThreats, [&ownedBases](const MilitiaTarget& target) {
            if (target.unit->kind != UnitKind::zealot &&
                target.unit->kind != UnitKind::zergling &&
                target.unit->kind != UnitKind::darkTemplar) {
                return false;
            }
            return std::ranges::any_of(ownedBases, [&target](const BaseSnapshot* base) {
                return distanceSquared(target.unit->position, base->center) < 320 * 320;
            });
        });
    const auto economyCap = meleeBreach
                                ? std::min(10, std::max(
                                      4, static_cast<int>(available.size()) - 2))
                                : (workers.size() >= 10U
                                       ? static_cast<int>(available.size() / 2U)
                                       : std::min(6, std::max(
                                             4, static_cast<int>(available.size()) - 2)));
    auto defendersRemaining = std::clamp(
        requestedDefenders, 0,
        std::min(8, economyCap));
    while (defendersRemaining > 0 && !available.empty()) {
        auto bestWorker = available.end();
        const UnitSnapshot* bestTarget = nullptr;
        auto bestScore = std::numeric_limits<long long>::max();
        for (auto worker = available.begin(); worker != available.end(); ++worker) {
            // When an actual rush is already inside the base, carrying a
            // mineral must not exempt a healthy Probe from the emergency
            // surround. Cargo is disposable; the Nexus and worker line are
            // not. Wounded Probes are still handled by the evacuation pass.
            if ((*worker)->healthFraction() < 0.5) continue;
            for (const auto& target : baseThreats) {
                if (target.demand <= 0) continue;
                if (baseForWorker(**worker) != target.base) continue;
                const auto baseNeed = defendersNeededByBase.find(target.base);
                if (baseNeed == defendersNeededByBase.end() || baseNeed->second <= 0) continue;
                if (!isBuilding(target.unit->kind)) {
                    const auto isMelee = target.unit->groundWeapon.maxRange <= 32;
                    // Once a melee threat is inside the base perimeter, a
                    // Probe that is still mining can reach it before the
                    // mineral line is erased.  The old 320px gate left the
                    // militia idle while a Zealot pack fought the last
                    // standing Zealots just outside the Nexus; use a wider
                    // 640px contact window for short-range attackers while
                    // keeping ranged units on their weapon-range leash.
                    const auto contactRange = isMelee
                                                  ? 640
                                                  : target.unit->groundWeapon.maxRange + 128;
                    if (distanceSquared((*worker)->position, target.unit->position) >
                        contactRange * contactRange) {
                        continue;
                    }
                }
                const auto priorityBias = 5 - targetPriority(*target.unit);
                const auto score = static_cast<long long>(priorityBias) * 1'000'000LL +
                                   distanceSquared((*worker)->position,
                                                   target.unit->position);
                if (score < bestScore) {
                    bestScore = score;
                    bestWorker = worker;
                    bestTarget = target.unit;
                }
            }
        }
        if (bestWorker == available.end() || bestTarget == nullptr) break;
        result.push_back({(*bestWorker)->id, WorkerJob::defend, -1, bestTarget->id,
                          bestTarget->position, 90});
        available.erase(bestWorker);
        const auto assignedTarget = std::ranges::find(
            baseThreats, bestTarget, &MilitiaTarget::unit);
        if (assignedTarget != baseThreats.end()) {
            --assignedTarget->demand;
            auto baseNeed = defendersNeededByBase.find(assignedTarget->base);
            if (baseNeed != defendersNeededByBase.end()) --baseNeed->second;
        }
        --defendersRemaining;
    }

    // Let a small militia answer first, then remove the remaining Probes whose
    // estimated contact time is already critical. This keeps a four-Probe
    // mining floor from trapping the rest of a line after its defenders have
    // been selected.
    for (auto worker = available.begin(); worker != available.end();) {
        const auto* localBase = baseForWorker(**worker);
        const UnitSnapshot* imminentThreat = nullptr;
        auto nearestArrival = std::numeric_limits<Frame>::max();
        auto directlyTargeted = false;
        for (const auto& enemy : state.enemy.units) {
            if (!enemy.completed || enemy.flying || enemy.disabled || enemy.loaded ||
                enemy.hallucination || enemy.invincible ||
                enemy.groundWeapon.damage <= 0 || !enemy.groundWeapon.targetsGround ||
                !enemy.position.valid() ||
                (!enemy.visible && (enemy.lastSeen <= 0 ||
                    state.frame - enemy.lastSeen > kWorkerEvacuationReentryThreatFrames))) {
                continue;
            }
            const auto targetsWorker = enemy.orderTargetId == (*worker)->id;
            if (isWorker(enemy.kind) && !targetsWorker && !(*worker)->underAttack) continue;
            if (!targetsWorker && localBase != nullptr &&
                distanceSquared(enemy.position, localBase->center) > 800 * 800) continue;
            const auto arrival = timeToGroundContact(enemy, **worker);
            if (targetsWorker || arrival <= kWorkerEvacuationEmergencyFrames) {
                if (arrival < nearestArrival || targetsWorker) {
                    nearestArrival = arrival;
                    imminentThreat = &enemy;
                    directlyTargeted = targetsWorker;
                }
            }
        }
        if (imminentThreat == nullptr && !(*worker)->underAttack) {
            ++worker;
            continue;
        }
        if (!(*worker)->underAttack && !directlyTargeted &&
            localScreenCount(*imminentThreat, localBase) >= 2) {
            ++worker;
            continue;
        }
        const auto memory = evacuationMemory_.find((*worker)->id);
        const auto refuge = memory != evacuationMemory_.end()
            ? memory->second.refuge
            : safeBase != nullptr && safeBase->mineralLine.valid()
                ? safeBase->mineralLine
                : safeBase != nullptr ? safeBase->center : Position{-1, -1};
        const auto route = safestEvacuationRoute(
            **worker, imminentThreat, refuge, localBase, safeBase, influence, navigation);
        const auto threatId = imminentThreat != nullptr ? imminentThreat->id : -1;
        evacuationMemory_[(*worker)->id] = {
            state.frame, (*worker)->firstSeen, localBase != nullptr ? localBase->id : -1,
            threatId, route.destination};
        result.push_back({(*worker)->id, WorkerJob::evacuate,
            safeBase != nullptr ? safeBase->id : -1, threatId, route.waypoint, 99});
        worker = available.erase(worker);
    }

    if (ownedBases.empty()) {
        const auto pendingNexus = std::ranges::find_if(
            state.self.units, [](const UnitSnapshot& unit) {
                return unit.kind == UnitKind::nexus && !unit.completed &&
                    !unit.disabled && !unit.loaded && unit.position.valid();
            });
        const auto rebuilding = pendingNexus != state.self.units.end();
        const auto siteAnchor = rebuilding ? pendingNexus->position : plan.expansionTarget;
        const auto funded = rebuilding ||
            state.self.minerals >= unitStats(UnitKind::nexus).minerals;
        const BaseSnapshot* rebuildSite = nullptr;
        auto bestSiteScore = std::numeric_limits<double>::infinity();
        if (funded && siteAnchor.valid()) {
            for (const auto& base : state.bases) {
                if (!base.center.valid() || !base.mineralLine.valid() || base.island ||
                    (!rebuilding && (base.ownerId != -1 || base.mineralPatches <= 0 ||
                                     base.mineralsRemaining < 1000)) ||
                    (rebuilding && base.ownerId >= 0 && base.ownerId != state.self.id) ||
                    distanceSquared(base.center, siteAnchor) > 320 * 320 ||
                    influence.at(base.center).groundThreat > 0.25F) {
                    continue;
                }
                auto nearestWorker = std::numeric_limits<double>::infinity();
                for (const auto* worker : available) {
                    if (safeRoute(worker, base.id, base.mineralLine)) {
                        nearestWorker = std::min(nearestWorker,
                            distance(worker->position, base.mineralLine));
                    }
                }
                if (!std::isfinite(nearestWorker)) continue;
                const auto score = nearestWorker +
                    static_cast<double>(influence.at(base.center).groundThreat) * 4096.0;
                if (score < bestSiteScore ||
                    (score == bestSiteScore &&
                     (rebuildSite == nullptr || base.id < rebuildSite->id))) {
                    bestSiteScore = score;
                    rebuildSite = &base;
                }
            }
        }

        // A mineral field is not an income source after the last legal depot
        // disappears. Keep workers at a viable, affordable rebuild site only
        // when they can reach it safely; otherwise stop their stale gather
        // orders so they do not keep walking into an exposed mineral line.
        for (const auto* worker : available) {
            result.push_back({worker->id, WorkerJob::rebuild,
                rebuildSite != nullptr ? rebuildSite->id : -1, -1,
                rebuildSite != nullptr ? rebuildSite->mineralLine : Position{-1, -1}, 90});
        }
        std::ranges::sort(result, {}, &WorkerAssignment::worker);
        return result;
    }

    // Each completed assimilator has exactly three efficient worker slots.
    // Pair gas workers with a concrete base so multi-base economies do not
    // repeatedly drag probes across the map.
    struct GasSlot {
        const BaseSnapshot* base{};
        const UnitSnapshot* refinery{};
    };
    std::vector<GasSlot> gasSlots;
    for (const auto& building : state.self.units) {
        if (building.kind != UnitKind::assimilator || !building.completed) continue;
        const auto base = std::ranges::min_element(
            ownedBases, {}, [&building](const BaseSnapshot* candidate) {
                return distanceSquared(building.position, candidate->center);
        });
        // A refinery can survive after its Nexus falls. It no longer belongs
        // to the nearest surviving economy across the map: doing that sends
        // replacement gas workers back through the army that destroyed it.
        const auto exposed = std::ranges::any_of(state.enemy.units,
            [&state, &building](const UnitSnapshot& enemy) {
                return enemy.position.valid() && enemy.completed && !isWorker(enemy.kind) &&
                    enemy.groundWeapon.damage > 0 && !enemy.disabled &&
                    (enemy.visible || state.frame - enemy.lastSeen <= 8 * 24) &&
                    distanceSquared(enemy.position, building.position) <=
                        (enemy.groundWeapon.maxRange + 128) * (enemy.groundWeapon.maxRange + 128);
            });
        if (base != ownedBases.end() &&
            distanceSquared(building.position, (*base)->center) <= 384 * 384 && !exposed) {
            for (auto slot = 0; slot < 3; ++slot) {
                gasSlots.push_back({*base, &building});
            }
        }
    }
    const auto effectiveDesiredGas = gasBank_.target(state, plan);
    const auto desiredGas = std::min(
        {effectiveDesiredGas, static_cast<int>(gasSlots.size()),
         // Gas cannot replace lost Probes. Preserve enough unleased workers
         // on minerals even when the strategic gas request predates a raid.
         std::max(0, static_cast<int>(available.size()) - 6)});
    auto gasAssigned = 0;
    const auto safeGasRoute = [&safeRoute](const UnitSnapshot* worker, const GasSlot& slot) {
        return safeRoute(worker, slot.refinery->id, slot.refinery->position);
    };

    // Keep workers that are already on the requested refinery. Re-selecting
    // the probes nearest the Nexus every worker tick used to rotate mineral
    // workers onto gas and gas workers back to minerals, losing mining time.
    for (auto worker = available.begin(); worker != available.end() &&
                                    gasAssigned < desiredGas;) {
        auto slot = std::ranges::find_if(
            gasSlots, [worker, &safeGasRoute](const GasSlot& candidate) {
                return candidate.refinery->id == (*worker)->orderTargetId && safeGasRoute(*worker, candidate);
            });
        if (slot == gasSlots.end() && (*worker)->gatheringGas) {
            // ReturnGas targets the Nexus, and workers inside a refinery can
            // temporarily have no target. Preserve their mining cycle too.
            slot = std::ranges::min_element(gasSlots, {}, [worker](const GasSlot& candidate) {
                return distanceSquared(candidate.refinery->position, (*worker)->position);
            });
            if (slot != gasSlots.end() &&
                (distanceSquared(slot->refinery->position, (*worker)->position) > 480 * 480 ||
                 !safeGasRoute(*worker, *slot))) {
                slot = gasSlots.end();
            }
        }
        if (slot == gasSlots.end()) {
            ++worker;
            continue;
        }
        result.push_back({(*worker)->id, WorkerJob::gas, slot->base->id, slot->refinery->id,
                          slot->refinery->position, 55});
        worker = available.erase(worker);
        gasSlots.erase(slot);
        ++gasAssigned;
    }

    while (gasAssigned < desiredGas && !gasSlots.empty()) {
        const auto slot = gasSlots.begin();
        const auto worker = std::ranges::min_element(
            available, {}, [slot, &safeGasRoute](const UnitSnapshot* candidate) {
                return safeGasRoute(candidate, *slot)
                    ? distanceSquared(candidate->position, slot->refinery->position)
                    : std::numeric_limits<int>::max();
            });
        if (worker == available.end()) break;
        if (!safeGasRoute(*worker, *slot)) {
            gasSlots.erase(slot);
            continue;
        }
        result.push_back({(*worker)->id, WorkerJob::gas, slot->base->id, slot->refinery->id,
                          slot->refinery->position, 55});
        available.erase(worker);
        gasSlots.erase(slot);
        ++gasAssigned;
    }

    std::unordered_map<int, int> assignedPerBase;
    std::unordered_map<int, int> stagedTransfersPerBase;
    constexpr auto maximumStagedWorkersPerBase = 8;
    if (stageExpansionWorkers && plan.expansionTarget.valid()) {
        const auto remote = std::ranges::find_if(ownedBases, [&plan](const BaseSnapshot* base) {
            return base->mineralLine.valid() && base->mineralsRemaining >= 1000 &&
                distanceSquared(base->center, plan.expansionTarget) <= 384 * 384;
        });
        if (remote != ownedBases.end() &&
            influence.at((*remote)->center).groundThreat <= 0.25F) {
            std::ranges::sort(available, [remote](const UnitSnapshot* left,
                                                  const UnitSnapshot* right) {
                const auto leftDistance = distanceSquared(left->position, (*remote)->center);
                const auto rightDistance = distanceSquared(right->position, (*remote)->center);
                return leftDistance != rightDistance ? leftDistance < rightDistance :
                       left->id < right->id;
            });
            auto& sent = stagedTransfersPerBase[(*remote)->id];
            for (auto worker = available.begin(); worker != available.end() &&
                 sent < maximumStagedWorkersPerBase;) {
                if (baseForWorker(**worker) == *remote) {
                    ++worker;
                    continue;
                }
                if (!safeRoute(*worker, -(*remote)->id - 1, (*remote)->mineralLine)) {
                    ++worker;
                    continue;
                }
                result.push_back({(*worker)->id, WorkerJob::transfer, (*remote)->id,
                                  -1, (*remote)->mineralLine, 60});
                worker = available.erase(worker);
                ++sent;
                ++assignedPerBase[(*remote)->id];
            }
        }
    }

    // Greedily equalize mineral saturation while retaining a small distance
    // bias. A remote destination receives at most one staged group per update.
    for (const auto* worker : available) {
        const BaseSnapshot* bestBase = nullptr;
        auto bestScore = std::numeric_limits<double>::infinity();
        for (const auto* base : ownedBases) {
            if (base->mineralsRemaining <= 0) continue;
            const auto patches = base->mineralPatches > 0 ? base->mineralPatches : 8;
            const auto capacity = std::clamp(patches * 2, 4, 16);
            const auto saturation = static_cast<double>(assignedPerBase[base->id] + 1) /
                                    static_cast<double>(capacity);
            const auto travel = distance(worker->position, base->center) / 2048.0;
            const auto* sourceBase = baseForWorker(*worker);
            const auto transfer = sourceBase == nullptr
                ? distanceSquared(worker->position, base->center) > 640 * 640
                : sourceBase->id != base->id;
            if (transfer && stagedTransfersPerBase[base->id] >= maximumStagedWorkersPerBase)
                continue;
            if (influence.at(base->center).groundThreat > 0.25F ||
                !safeRoute(worker, -base->id - 1, base->mineralLine)) continue;
            const auto local = influence.at(base->center);
            const auto depletion = base->mineralsRemaining > 0 ? 0.0 : 100.0;
            const auto score = saturation + travel * 0.18 +
                               static_cast<double>(local.groundThreat) * 1.5 + depletion;
            if (score < bestScore) {
                bestScore = score;
                bestBase = base;
            }
        }
        if (bestBase == nullptr) {
            // An exhausted Nexus is still an owned base, but it has no local
            // patch to mine. Return stranded workers to safety instead of
            // leaving their previous remote gather order running.
            const auto refuge = safeBase != nullptr && safeBase->mineralLine.valid()
                ? safeBase->mineralLine :
                safeBase != nullptr ? safeBase->center : Position{-1, -1};
            if (refuge.valid() &&
                distanceSquared(worker->position, refuge) > 256 * 256) {
                result.push_back({worker->id, WorkerJob::evacuate, safeBase->id,
                    -1, influence.safestStep(worker->position,
                                             refuge, false), 80});
            } else {
                result.push_back({worker->id, WorkerJob::idle, -1, -1, {-1, -1}, 0});
            }
            continue;
        }
        ++assignedPerBase[bestBase->id];
        const auto* sourceBase = baseForWorker(*worker);
        const auto transfer = sourceBase == nullptr
            ? distanceSquared(worker->position, bestBase->center) > 640 * 640
            : sourceBase->id != bestBase->id;
        if (transfer) ++stagedTransfersPerBase[bestBase->id];
        result.push_back({worker->id,
                          transfer ? WorkerJob::transfer : WorkerJob::minerals,
                          bestBase->id, -1, bestBase->mineralLine, 50});
    }

    std::ranges::sort(result, {}, &WorkerAssignment::worker);
    return result;
}

const BaseSnapshot* WorkerManager::safestOwnedBase(
    const GameState& state,
    const InfluenceMap& influence) {
    const BaseSnapshot* best = nullptr;
    auto bestScore = std::numeric_limits<double>::infinity();
    for (const auto& base : state.bases) {
        if (base.ownerId != state.self.id || !base.center.valid()) {
            continue;
        }
        const auto cell = influence.at(base.center);
        const auto score = static_cast<double>(cell.groundThreat + cell.airThreat) -
                           static_cast<double>(base.mineralsRemaining) / 100000.0;
        if (score < bestScore) {
            bestScore = score;
            best = &base;
        }
    }
    return best;
}

}  // namespace protodd
