#include "astra/Workers.hpp"

#include "astra/UnitCatalog.hpp"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace astra {

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
        if (worker->healthFraction() < 0.35 && local.groundThreat > 0.25F && safeBase != nullptr) {
            result.push_back({worker->id, WorkerJob::evacuate, safeBase->id, -1,
                              safeBase->mineralLine, 98});
            continue;
        }
        available.push_back(worker);
    }

    // Pull a bounded local militia whenever a visible enemy reaches an owned
    // mineral line. Hidden memory must never send probes chasing ghosts.
    std::vector<const UnitSnapshot*> baseThreats;
    for (const auto& enemy : state.enemy.units) {
        if (!enemy.visible || enemy.flying || !enemy.position.valid()) continue;
        if (std::ranges::any_of(ownedBases, [&enemy](const BaseSnapshot* base) {
                return distanceSquared(enemy.position, base->center) < 420 * 420;
            })) {
            baseThreats.push_back(&enemy);
        }
    }
    auto defendersRemaining = std::min(
        {6, static_cast<int>(baseThreats.size()) * 2,
         static_cast<int>(available.size())});
    while (defendersRemaining > 0 && !available.empty()) {
        auto bestWorker = available.end();
        const UnitSnapshot* bestTarget = nullptr;
        auto bestDistance = std::numeric_limits<int>::max();
        for (auto worker = available.begin(); worker != available.end(); ++worker) {
            if ((*worker)->carryingResources) continue;
            for (const auto* enemy : baseThreats) {
                const auto candidate = distanceSquared((*worker)->position, enemy->position);
                if (candidate < bestDistance) {
                    bestDistance = candidate;
                    bestWorker = worker;
                    bestTarget = enemy;
                }
            }
        }
        if (bestWorker == available.end() || bestTarget == nullptr) break;
        result.push_back({(*bestWorker)->id, WorkerJob::defend, -1, bestTarget->id,
                          bestTarget->position, 90});
        available.erase(bestWorker);
        --defendersRemaining;
    }

    // Each completed assimilator has exactly three efficient worker slots.
    // Pair gas workers with a concrete base so multi-base economies do not
    // repeatedly drag probes across the map.
    std::vector<const BaseSnapshot*> gasSlots;
    for (const auto& building : state.self.units) {
        if (building.kind != UnitKind::assimilator || !building.completed) continue;
        const auto base = std::ranges::min_element(
            ownedBases, {}, [&building](const BaseSnapshot* candidate) {
                return distanceSquared(building.position, candidate->center);
            });
        if (base != ownedBases.end()) {
            for (auto slot = 0; slot < 3; ++slot) gasSlots.push_back(*base);
        }
    }
    const auto desiredGas = std::min(
        {plan.desiredGasWorkers, static_cast<int>(gasSlots.size()),
         static_cast<int>(available.size())});
    for (auto slot = 0; slot < desiredGas; ++slot) {
        const auto* base = gasSlots[static_cast<std::size_t>(slot)];
        const auto worker = std::ranges::min_element(
            available, {}, [base](const UnitSnapshot* candidate) {
                return distanceSquared(candidate->position, base->center);
            });
        if (worker == available.end()) break;
        result.push_back({(*worker)->id, WorkerJob::gas, base->id, -1, base->center, 55});
        available.erase(worker);
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
