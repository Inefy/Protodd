#include "astra/Workers.hpp"

#include "astra/UnitCatalog.hpp"

#include <algorithm>
#include <limits>
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

    const auto safeBase = safestOwnedBase(state, influence);
    auto gasRemaining = std::min(plan.desiredGasWorkers, static_cast<int>(workers.size()));

    // Pull only the minimum force required for immediate worker defense. This
    // prevents economy-destroying all-worker chases.
    const auto enemiesInMain = safeBase == nullptr ? 0 : std::ranges::count_if(
        state.enemy.units,
        [safeBase](const UnitSnapshot& enemy) {
            return distanceSquared(enemy.position, safeBase->center) < 420 * 420;
        });
    auto defendersRemaining = plan.posture == Posture::defend
                                  ? std::min(6, static_cast<int>(enemiesInMain) * 2)
                                  : 0;

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
        if (defendersRemaining > 0 && !worker->carryingResources) {
            const auto target = std::ranges::min_element(
                state.enemy.units,
                {},
                [worker](const UnitSnapshot& enemy) {
                    return distanceSquared(worker->position, enemy.position);
                });
            if (target != state.enemy.units.end()) {
                result.push_back({worker->id, WorkerJob::defend, -1, target->id,
                                  target->position, 90});
                --defendersRemaining;
                continue;
            }
        }
        if (gasRemaining > 0) {
            result.push_back({worker->id, WorkerJob::gas, safeBase ? safeBase->id : -1,
                              -1, {-1, -1}, 55});
            --gasRemaining;
        } else {
            result.push_back({worker->id, WorkerJob::minerals, safeBase ? safeBase->id : -1,
                              -1, safeBase ? safeBase->mineralLine : Position{-1, -1}, 50});
        }
    }
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
