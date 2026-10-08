#pragma once

#include "protodd/GameState.hpp"
#include "protodd/InfluenceMap.hpp"
#include "protodd/Navigation.hpp"
#include "protodd/Strategy.hpp"

#include <vector>
#include <unordered_map>

namespace protodd {

enum class WorkerJob : std::uint8_t {
    minerals,
    gas,
    build,
    transfer,
    scout,
    defend,
    evacuate,
    idle,
    rebuild,
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

struct MineralWorker {
    UnitId id{-1};
    Position position{-1, -1};
    Position mineralLine{-1, -1};
    UnitId currentTarget{-1};
    double topSpeed{4.0};
    bool carryingResources{};
};

struct MineralServiceModel {
    // A deterministic service-time prior in frames, not a claim about a
    // specific Brood War map or engine cycle. Live traces can calibrate it.
    int miningFrames{96};
    int additionalWorkerFrames{48};
    int switchingFrames{48};
};

struct MineralServiceEstimate {
    int cargoReturnFrames{};
    int travelToPatchFrames{};
    int travelToDepotFrames{};
    int miningFrames{};
    int congestionFrames{};
    int switchingFrames{};
    int totalFrames{};
};

[[nodiscard]] MineralServiceEstimate estimateMineralService(
    const MineralWorker& worker, const MineralPatchCandidate& patch,
    int assignedWorkers, UnitId previousTarget,
    MineralServiceModel model = {}) noexcept;

[[nodiscard]] UnitId selectMineralPatch(
    std::span<const MineralPatchCandidate> candidates,
    Position mineralLine,
    Position workerPosition,
    UnitId currentTarget = -1) noexcept;

// Resource assignments survive the cargo-return leg, when the engine's
// current order target is the Nexus rather than the mineral patch.
class MineralAllocator {
public:
    explicit MineralAllocator(MineralServiceModel model = {}) : model_(model) {}
    void reset() { targets_.clear(); }
    [[nodiscard]] const std::unordered_map<UnitId, UnitId>& assign(
        std::span<const MineralWorker> workers,
        std::span<const MineralPatchCandidate> patches);

private:
    MineralServiceModel model_;
    std::unordered_map<UnitId, UnitId> targets_;
};

class GasBankController {
public:
    [[nodiscard]] int target(const GameState& state, const StrategicPlan& plan);
private:
    bool paused_{};
    Frame lastFrame_{-1};
};

class WorkerManager {
public:
    static constexpr int maximumEconomicRouteSearches = 8;
    struct RoutingStats {
        int economicPathSearches{};
        int deferredChecks{};
    };
    [[nodiscard]] const RoutingStats& routingStats() const noexcept { return routingStats_; }
    [[nodiscard]] std::vector<WorkerAssignment> assign(
        const GameState& state,
        const StrategicPlan& plan,
        const InfluenceMap& influence,
        std::span<const UnitId> reservedBuilders = {},
        bool evacuateAbandonedBase = false,
        bool stageExpansionWorkers = false,
        const NavigationGrid* navigation = nullptr) const;

private:
    struct EvacuationMemory {
        Frame lastDangerFrame{-1};
        Frame firstSeen{-1};
        int baseId{-1};
        UnitId threatId{-1};
        Position refuge{-1, -1};
    };

    mutable GasBankController gasBank_;
    mutable RoutingStats routingStats_;
    mutable Frame lastAssignedFrame_{-1};
    mutable std::unordered_map<UnitId, EvacuationMemory> evacuationMemory_;
    [[nodiscard]] static const BaseSnapshot* safestOwnedBase(
        const GameState& state,
        const InfluenceMap& influence);
};

}  // namespace protodd
