#pragma once

#include "protodd/GameState.hpp"
#include "protodd/CommandBus.hpp"
#include "protodd/Information.hpp"
#include "protodd/InfluenceMap.hpp"
#include "protodd/Navigation.hpp"

#include <span>
#include <string_view>
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

enum class ScoutGapReason : std::uint8_t {
    none,
    noAvailableScout,
    unsafeRoute,
    lowerPriority,
};

struct ScoutInformationGap {
    bool unresolved{};
    Position target{-1, -1};
    ScoutPurpose purpose{ScoutPurpose::checkExpansion};
    Frame deadlineFrame{-1};
    // A 0..1 information-gap urgency score, not an attack probability.
    double risk{};
    ScoutGapReason reason{ScoutGapReason::none};
};

[[nodiscard]] std::string_view scoutPurposeName(ScoutPurpose purpose) noexcept;
[[nodiscard]] std::string_view scoutGapReasonName(ScoutGapReason reason) noexcept;

struct ScoutOrder {
    UnitId scout{};
    Position target{-1, -1};
    ScoutPurpose purpose{ScoutPurpose::findEnemy};
    double score{};
    Position informationTarget{-1, -1};
    double routeRisk{};
    Frame deadlineFrame{-1};
    Position routeWaypoint{-1, -1};
    std::uint64_t leaseGeneration{};
};

enum class ScoutCommandStatus : std::uint8_t {
    accepted,
    alreadyActive,
    deferred,
    rejected,
};

struct ScoutCommandFeedback {
    UnitId scout{-1};
    ScoutCommandStatus status{ScoutCommandStatus::rejected};
    std::uint64_t leaseGeneration{};
};

struct ScoutMissionMetrics {
    std::uint64_t newInformation{};
    std::uint64_t completed{};
    std::uint64_t cancelled{};
    std::uint64_t lost{};
    std::uint64_t rejected{};
    std::uint64_t workerScoutFrames{};
    std::uint64_t detectorScoutFrames{};
    double routeRiskFrames{};
};

// One opening Probe alternates harassment and escape. Withdrawal is latched:
// a fighter disappearing into fog never invites the Probe back into danger.
class ProbeHarasser {
public:
    void reset() noexcept;
    void withdraw() noexcept { withdrawing_ = true; }
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
    void forgetUnit(UnitId id) noexcept;
    [[nodiscard]] std::uint64_t releaseLease(UnitId id, Frame frame);

    [[nodiscard]] UnitId selectWorkerScout(
        const GameState& state, const ThreatAssessment& threat,
        std::span<const UnitId> previousScouts = {},
        std::span<const UnitId> unavailableWorkers = {});

    [[nodiscard]] std::vector<ScoutOrder> assign(
        const GameState& state,
        std::span<const UnitId> availableScouts,
        const InfluenceMap& influence,
        const ThreatAssessment& threat,
        const NavigationGrid* terrain = nullptr);
    [[nodiscard]] UnitId openingScout() const noexcept {
        return openingMission_ ? workerScout_ : -1;
    }
    [[nodiscard]] UnitId returningScout() const noexcept {
        return returningMission_ ? workerScout_ : -1;
    }
    [[nodiscard]] const ScoutInformationGap& informationGap() const noexcept {
        return informationGap_;
    }
    [[nodiscard]] const ScoutMissionMetrics& missionMetrics() const noexcept {
        return missionMetrics_;
    }
    void recordCommandFeedback(const ScoutCommandFeedback& feedback, Frame frame);
    [[nodiscard]] std::optional<Command> controlWorkerScout(
        const GameState& state, const InfluenceMap& influence, const NavigationGrid* terrain = nullptr);
    [[nodiscard]] std::vector<Command> protectObservers(
        const GameState& state, const InfluenceMap& influence,
        std::span<const UnitId> escorts = {});
    [[nodiscard]] bool observerEvading(UnitId observer, Frame frame) const noexcept {
        const auto lease = observerEvadeUntil_.find(observer);
        return lease != observerEvadeUntil_.end() && lease->second > frame;
    }
    [[nodiscard]] static bool observerInDanger(
        const GameState& state, const UnitSnapshot& observer,
        const InfluenceMap& influence) noexcept;
    [[nodiscard]] static double observerExposure(
        const GameState& state, const UnitSnapshot& observer,
        const InfluenceMap& influence, Position position) noexcept;
    [[nodiscard]] static bool observerRouteSafe(
        const GameState& state, const UnitSnapshot& observer,
        const InfluenceMap& influence, Position target) noexcept;

private:
    struct ScoutMission {
        ScoutOrder order;
        Frame started{};
        Frame lastAccounted{};
        Frame lastProgress{};
        Frame observedAtStart{-1};
        int bestDistance{};
        bool informationGained{};
    };
    std::unordered_map<UnitId, ScoutMission> previousOrders_;
    std::unordered_map<UnitId, std::uint64_t> leaseGenerations_;
    std::unordered_map<std::uint64_t, Frame> revisitAfter_;
    std::unordered_map<std::uint64_t, Frame> retryAfter_;
    ScoutMissionMetrics missionMetrics_{};
    Frame workerMissionStarted_{-1};
    Frame nextWorkerMission_{};
    UnitId workerScout_{-1};
    bool openingMission_{};
    bool returningMission_{};
    ProbeHarasser harasser_;
    ProbeHarasser returnHarasser_;
    std::unordered_map<UnitId, Frame> observerEvadeUntil_;
    std::unordered_map<UnitId, Position> observerEscapeWaypoint_;
    ScoutInformationGap informationGap_{};
};

}  // namespace protodd
