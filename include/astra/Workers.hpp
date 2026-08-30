#pragma once

#include "astra/GameState.hpp"
#include "astra/InfluenceMap.hpp"
#include "astra/Strategy.hpp"

#include <vector>

namespace astra {

enum class WorkerJob : std::uint8_t {
    minerals,
    gas,
    build,
    transfer,
    scout,
    defend,
    evacuate,
    idle,
};

struct WorkerAssignment {
    UnitId worker{};
    WorkerJob job{WorkerJob::idle};
    int baseId{-1};
    UnitId targetUnit{-1};
    Position targetPosition{-1, -1};
    int priority{};
};

class WorkerManager {
public:
    [[nodiscard]] std::vector<WorkerAssignment> assign(
        const GameState& state,
        const StrategicPlan& plan,
        const InfluenceMap& influence,
        std::span<const UnitId> reservedBuilders = {}) const;

private:
    [[nodiscard]] static const BaseSnapshot* safestOwnedBase(
        const GameState& state,
        const InfluenceMap& influence);
};

}  // namespace astra

