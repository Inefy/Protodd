#pragma once

#include "protodd/GameState.hpp"
#include "protodd/CommandBus.hpp"
#include "protodd/Information.hpp"
#include "protodd/InfluenceMap.hpp"
#include "protodd/Navigation.hpp"

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

// One opening Probe alternates harassment and escape. Withdrawal is latched:
// a fighter disappearing into fog never invites the Probe back into danger.
class ProbeHarasser {
public:
    void reset() noexcept;
    void finish() noexcept { finished_ = true; }
    [[nodiscard]] bool finished() const noexcept { return finished_; }
    [[nodiscard]] std::optional<Command> control(
        const GameState& state, UnitId scout, Position scoutGoal,
        const InfluenceMap& influence, const NavigationGrid* terrain = nullptr);
private:
    bool withdrawing_{};
    bool finished_{};
    Frame evadeUntil_{};
    Frame routeFrame_{-1};
    Position returnWaypoint_{-1, -1};
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
    [[nodiscard]] UnitId openingScout() const noexcept {
        return openingMission_ ? workerScout_ : -1;
    }
    [[nodiscard]] std::optional<Command> controlWorkerScout(
        const GameState& state, const InfluenceMap& influence, const NavigationGrid* terrain = nullptr);

private:
    std::unordered_map<UnitId, ScoutOrder> previousOrders_;
    Frame workerMissionStarted_{-1};
    Frame nextWorkerMission_{};
    UnitId workerScout_{-1};
    bool openingMission_{};
    ProbeHarasser harasser_;
};

}  // namespace protodd
