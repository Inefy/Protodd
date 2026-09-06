#pragma once

#include "protodd/GameState.hpp"
#include "protodd/Information.hpp"
#include "protodd/InfluenceMap.hpp"

#include <span>
#include <unordered_map>
#include <vector>

namespace protodd {

enum class ScoutPurpose : std::uint8_t {
    findEnemy,
    checkTech,
    checkExpansion,
    watchArmy,
    patrolDropPath,
};

struct ScoutOrder {
    UnitId scout{};
    Position target{-1, -1};
    ScoutPurpose purpose{ScoutPurpose::findEnemy};
    double score{};
};

[[nodiscard]] UnitId selectOpeningWorkerScout(
    const GameState& state,
    std::span<const UnitId> previousScouts = {},
    std::span<const UnitId> unavailableWorkers = {}) noexcept;

class ScoutManager {
public:
    void reset() noexcept;

    [[nodiscard]] UnitId selectWorkerScout(
        const GameState& state, const ThreatAssessment& threat,
        std::span<const UnitId> previousScouts = {},
        std::span<const UnitId> unavailableWorkers = {});

    [[nodiscard]] std::vector<ScoutOrder> assign(
        const GameState& state,
        std::span<const UnitId> availableScouts,
        const InfluenceMap& influence,
        const ThreatAssessment& threat);

private:
    std::unordered_map<UnitId, ScoutOrder> previousOrders_;
    Frame workerMissionStarted_{-1};
    Frame nextWorkerMission_{};
    UnitId workerScout_{-1};
};

}  // namespace protodd
