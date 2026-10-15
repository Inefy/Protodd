#pragma once

#include "protodd/GameState.hpp"
#include "protodd/InfluenceMap.hpp"
#include "protodd/Navigation.hpp"
#include "protodd/Runtime.hpp"
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
    // A deferred route query can retain an already running safe mining cycle.
    bool preserveNativeOrder{};
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
    bool preserveCurrentTarget{};
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

struct WorkerRoutingLimits {
    int totalQueries{16};
    int totalExpansions{32000};
    int economicQueries{8};
    int defensiveQueries{8};
    int economicExpansions{12000};
    int defensiveExpansions{3000};
    int evacuationExpansions{3000};
    int economicReserve{8000};
};

[[nodiscard]] constexpr WorkerRoutingLimits workerRoutingLimits(const RuntimeLoad load) noexcept {
    switch (load) {
        case RuntimeLoad::normal: return {};
        case RuntimeLoad::reduced: return {8, 16000, 4, 4, 8192, 2000, 2000, 4096};
        case RuntimeLoad::emergency: return {4, 8192, 1, 2, 2048, 1500, 1500, 2048};
    }
    return {4, 8192, 1, 2, 2048, 1500, 1500, 2048};
}

class WorkerManager {
public:
    static constexpr int maximumEconomicRouteSearches = 8;
    static constexpr int maximumRouteQueries = 16;
    static constexpr int maximumRouteExpansions = 32000;
    struct RoutingStats {
        int economicPathSearches{};
        int deferredChecks{};
        int defensivePathSearches{};
        int evacuationPathSearches{};
        int totalPathQueries{};
        // Sum of per-query allowances; cheap queries return unused work to
        // the shared budget, so this can exceed its actual expansion limit.
        int reservedExpansions{};
        int expandedNodes{};
        int economicDeferredChecks{};
        int defensiveDeferredChecks{};
        int evacuationDeferredChecks{};
    };
    [[nodiscard]] const RoutingStats& routingStats() const noexcept { return routingStats_; }
    // Recent emergency assignments exclude a worker from new construction,
    // including the interval after its old Build order is cancelled. These are
    // eligibility decisions, not claims that a proposed worker command ran.
    [[nodiscard]] std::vector<UnitId> constructionExclusions(const GameState& state) const;
    [[nodiscard]] std::vector<WorkerAssignment> assign(
        const GameState& state,
        const StrategicPlan& plan,
        const InfluenceMap& influence,
        std::span<const UnitId> reservedBuilders = {},
        bool evacuateAbandonedBase = false,
        bool stageExpansionWorkers = false,
        const NavigationGrid* navigation = nullptr,
        WorkerRoutingLimits routingLimits = {}) const;

private:
    struct EvacuationMemory {
        Frame lastDangerFrame{-1};
        Frame firstSeen{-1};
        int baseId{-1};
        UnitId threatId{-1};
        Position refuge{-1, -1};
    };

    struct BootstrapMemory {
        Frame firstSeen{-1};
        int baseId{-1};
    };

    mutable std::unordered_map<UnitId, BootstrapMemory> bootstrapMemory_;
    mutable GasBankController gasBank_;
    mutable RoutingStats routingStats_;
    mutable Frame lastAssignedFrame_{-1};
    mutable std::unordered_map<UnitId, EvacuationMemory> evacuationMemory_;
    mutable std::unordered_map<UnitId, Frame> emergencyConstructionExclusions_;
    void rememberConstructionExclusions(
        const GameState& state, std::span<const WorkerAssignment> assignments) const;
    [[nodiscard]] static const BaseSnapshot* safestOwnedBase(
        const GameState& state,
        const InfluenceMap& influence);
};

}  // namespace protodd
