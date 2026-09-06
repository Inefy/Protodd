#pragma once

#include "astra/GameState.hpp"
#include "astra/InfluenceMap.hpp"
#include "astra/Strategy.hpp"

#include <vector>
#include <unordered_map>

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

struct MineralPatchCandidate {
    UnitId id{-1};
    Position position{-1, -1};
    int assignedWorkers{};
};

[[nodiscard]] UnitId selectMineralPatch(
    std::span<const MineralPatchCandidate> candidates,
    Position mineralLine,
    Position workerPosition,
    UnitId currentTarget = -1) noexcept;

struct MineralWorker {
    UnitId id{-1};
    Position position{-1, -1};
    Position mineralLine{-1, -1};
    UnitId currentTarget{-1};
};

// Resource assignments survive the cargo-return leg, when the engine's
// current order target is the Nexus rather than the mineral patch.
class MineralAllocator {
public:
    void reset() { targets_.clear(); }
    [[nodiscard]] const std::unordered_map<UnitId, UnitId>& assign(
        std::span<const MineralWorker> workers,
        std::span<const MineralPatchCandidate> patches);

private:
    std::unordered_map<UnitId, UnitId> targets_;
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
