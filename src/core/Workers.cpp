#include "astra/Workers.hpp"

#include "astra/UnitCatalog.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace astra {
namespace {

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

std::vector<WorkerAssignment> WorkerManager::assign(
    const GameState& state,
    const StrategicPlan& plan,
    const InfluenceMap& influence,
    const std::span<const UnitId> reservedBuilders) const {
    std::vector<const UnitSnapshot*> workers;
    for (const auto& unit : state.self.units) {
        if (isWorker(unit.kind) && unit.completed) {
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
    if (ownedBases.empty() && !workers.empty()) {
        // After the last Nexus is destroyed, surviving Probes must keep mining
        // while the recovery Nexus is constructed. Use the nearest safe-ish
        // resource cluster as a temporary economy anchor.
        const BaseSnapshot* fallback = nullptr;
        auto bestDistance = std::numeric_limits<int>::max();
        for (const auto& base : state.bases) {
            if (!base.center.valid() || base.mineralsRemaining <= 0) continue;
            const auto candidate = distanceSquared(workers.front()->position, base.center);
            if (candidate < bestDistance) {
                bestDistance = candidate;
                fallback = &base;
            }
        }
        if (fallback != nullptr) ownedBases.push_back(fallback);
    }
    std::ranges::sort(ownedBases, {}, [](const BaseSnapshot* base) { return base->id; });
    const auto safeBase = safestOwnedBase(state, influence);
    std::vector<const UnitSnapshot*> available;
    available.reserve(workers.size());

    for (const auto* worker : workers) {
        if (builders.contains(worker->id)) {
            result.push_back({worker->id, WorkerJob::build, -1, -1, {-1, -1}, 100});
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
        available.push_back(worker);
    }

    // A worker militia only answers threats for which worker contact is useful:
    // workers, unfinished proxies, and tiny opening-unit groups. In particular,
    // never feed Probes into tanks or completed static defenses.
    struct MilitiaTarget {
        const UnitSnapshot* unit{};
        int demand{};
    };
    std::vector<MilitiaTarget> baseThreats;
    const auto localEnemyWorkers = std::ranges::count_if(
        state.enemy.units, [&ownedBases](const UnitSnapshot& enemy) {
            return enemy.visible && isWorker(enemy.kind) && enemy.position.valid() &&
                   std::ranges::any_of(ownedBases, [&enemy](const BaseSnapshot* base) {
                       return distanceSquared(enemy.position, base->center) < 512 * 512;
                   });
        });
    for (const auto& enemy : state.enemy.units) {
        const auto demand = militiaDemand(enemy, state.frame);
        if (demand == 0 || !enemy.position.valid()) continue;
        if (isWorker(enemy.kind) && localEnemyWorkers < 3) continue;
        if (std::ranges::any_of(ownedBases, [&enemy](const BaseSnapshot* base) {
                return distanceSquared(enemy.position, base->center) < 512 * 512;
            })) {
            baseThreats.push_back({&enemy, demand});
        }
    }
    const auto requestedDefenders = std::accumulate(
        baseThreats.begin(), baseThreats.end(), 0,
        [](const int total, const MilitiaTarget& target) { return total + target.demand; });
    const auto localArmy = std::ranges::count_if(
        state.self.units, [&baseThreats](const UnitSnapshot& unit) {
            return unit.completed && isCombatUnit(unit.kind) && !unit.flying &&
                   std::ranges::any_of(baseThreats, [&unit](const MilitiaTarget& target) {
                       return distanceSquared(unit.position, target.unit->position) < 576 * 576;
                   });
        });
    const auto economyCap = workers.size() >= 10U
                                ? static_cast<int>(available.size() / 2U)
                                : std::min(6, std::max(
                                      4, static_cast<int>(available.size()) - 2));
    auto defendersRemaining = std::clamp(
        requestedDefenders - static_cast<int>(localArmy) * 2, 0,
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
                if (!isBuilding(target.unit->kind)) {
                    const auto contactRange = target.unit->groundWeapon.maxRange + 128;
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
        if (assignedTarget != baseThreats.end()) --assignedTarget->demand;
        --defendersRemaining;
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
        if (base != ownedBases.end()) {
            for (auto slot = 0; slot < 3; ++slot) {
                gasSlots.push_back({*base, &building});
            }
        }
    }
    auto effectiveDesiredGas = plan.desiredGasWorkers;
    const auto mineralStarved = state.self.minerals < 150;
    if (mineralStarved && state.self.gas >= 300 &&
        (plan.posture == Posture::defend || plan.posture == Posture::recover)) {
        // A large existing gas bank already funds several Dragoon/tech cycles.
        // During a base defense, the binding resource is almost always the
        // mineral cost of units, pylons, batteries, and replacement workers.
        effectiveDesiredGas = 0;
    } else if (mineralStarved && state.self.gas >= 600) {
        effectiveDesiredGas = std::min(effectiveDesiredGas, 1);
    }
    const auto desiredGas = std::min(
        {effectiveDesiredGas, static_cast<int>(gasSlots.size()),
         static_cast<int>(available.size())});
    auto gasAssigned = 0;

    // Keep workers that are already on the requested refinery. Re-selecting
    // the probes nearest the Nexus every worker tick used to rotate mineral
    // workers onto gas and gas workers back to minerals, losing mining time.
    for (auto worker = available.begin(); worker != available.end() &&
                                    gasAssigned < desiredGas;) {
        const auto slot = std::ranges::find_if(
            gasSlots, [worker](const GasSlot& candidate) {
                return candidate.refinery->id == (*worker)->orderTargetId;
            });
        if (slot == gasSlots.end()) {
            ++worker;
            continue;
        }
        result.push_back({(*worker)->id, WorkerJob::gas, slot->base->id, -1,
                          slot->refinery->position, 55});
        worker = available.erase(worker);
        gasSlots.erase(slot);
        ++gasAssigned;
    }

    while (gasAssigned < desiredGas && !gasSlots.empty()) {
        const auto slot = gasSlots.begin();
        const auto worker = std::ranges::min_element(
            available, {}, [slot](const UnitSnapshot* candidate) {
                return distanceSquared(candidate->position, slot->refinery->position);
            });
        if (worker == available.end()) break;
        result.push_back({(*worker)->id, WorkerJob::gas, slot->base->id, -1,
                          slot->refinery->position, 55});
        available.erase(worker);
        gasSlots.erase(slot);
        ++gasAssigned;
    }

    // Greedily equalize mineral saturation while retaining a small distance
    // bias. A transfer order is explicit so the adapter retargets workers that
    // are already gathering at an oversaturated base.
    std::unordered_map<int, int> assignedPerBase;
    for (const auto* worker : available) {
        const BaseSnapshot* bestBase = nullptr;
        auto bestScore = std::numeric_limits<double>::infinity();
        for (const auto* base : ownedBases) {
            const auto patches = base->mineralPatches > 0 ? base->mineralPatches : 8;
            const auto capacity = std::clamp(patches * 2, 4, 16);
            const auto saturation = static_cast<double>(assignedPerBase[base->id] + 1) /
                                    static_cast<double>(capacity);
            const auto travel = distance(worker->position, base->center) / 2048.0;
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
            result.push_back({worker->id, WorkerJob::idle, -1, -1, {-1, -1}, 0});
            continue;
        }
        ++assignedPerBase[bestBase->id];
        const auto transfer = distanceSquared(worker->position, bestBase->center) > 640 * 640;
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

}  // namespace astra
