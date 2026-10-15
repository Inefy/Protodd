#pragma once

#include "protodd/GameState.hpp"
#include "protodd/Strategy.hpp"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace protodd {

// The next command arrives after latency: a producer finishing within that
// window can accept one successor, but never a second waiting queue entry.
[[nodiscard]] bool trainingSlotAvailable(bool active, int queueSize,
                                        int remainingFrames, int latencyFrames,
                                        bool recentTrainCommand) noexcept;

struct TrainingProducerCandidate {
    UnitId id{-1};
    bool completed{};
    bool powered{};
    bool disabled{};
    bool loaded{};
    bool hallucination{};
    bool legalForTarget{};
    bool activeTraining{};
    int trainingQueueSize{};
    int remainingTrainFrames{};
    int latencyFrames{};
    bool recentTrainCommand{};
    bool researching{};
    bool upgrading{};
};

[[nodiscard]] std::optional<UnitId> selectTrainingProducer(
    std::span<const TrainingProducerCandidate> candidates) noexcept;

enum class MacroActionKind : std::uint8_t { build, train, expand, research, upgrade };

struct MacroAction {
    MacroActionKind action{MacroActionKind::train};
    UnitKind target{UnitKind::unknown};
    int priority{};
    int minerals{};
    int gas{};
    bool reserved{};
    std::string reason;
    TechnologyKind technology{TechnologyKind::none};
    bool blocksLowerPriority{};
    bool executable{true};
    ConstructionTaskSite constructionSite{};
    CriticalGoalTiming timing{};
    Frame preparationFundingFrames{-1};
    Frame pendingCriticalBankHoldFrames{-1};
    int pendingCriticalMineralsProtected{};
};

// Travel preparation is bounded by observed income and carries no spending
// permission. Reserve actual construction separately when its bank is funded.
[[nodiscard]] constexpr Frame expansionPreparationFundingWaitFrames(
    const int freeMinerals, const int mineralIncomePerMinute) noexcept {
    constexpr int nexusCost = 400;
    constexpr Frame maximumPreparationWait = 30 * 24;
    if (freeMinerals >= nexusCost) return 0;
    if (freeMinerals < 0 || mineralIncomePerMinute <= 0) return -1;
    // Leave margin for the builder leaving mining and variation in deliveries.
    const auto conservativeIncome =
        static_cast<std::int64_t>(mineralIncomePerMinute) * 3 / 4;
    if (conservativeIncome <= 0) return -1;
    const auto wait = (static_cast<std::int64_t>(nexusCost - freeMinerals) * 60 * 24 +
                      conservativeIncome - 1) / conservativeIncome;
    return wait <= maximumPreparationWait ? static_cast<Frame>(wait) : -1;
}

enum class BuildBlockerReason : std::uint8_t {
    noBuilder,
    unsafeRoute,
    missingPrerequisite,
    noPlacement,
    noPower,
    rejectedFootprint,
    routePending,
};

struct BuildBlockerFeedback {
    UnitKind target{UnitKind::unknown};
    ConstructionTaskSite constructionSite{};
    BuildBlockerReason reason{BuildBlockerReason::noPlacement};
    Frame retryAt{};
    Frame observedAt{-1};
};

// Retry cadence is a spending/scheduling decision. Keep recent unknown route
// observations visible to strategy and spending admission across that short
// scheduling boundary. Analysis can retry while the bank stays available.
[[nodiscard]] inline bool pendingRouteObservationCurrent(
    const BuildBlockerFeedback& feedback, const Frame frame) noexcept {
    return feedback.reason == BuildBlockerReason::routePending &&
        feedback.observedAt >= 0 && frame >= feedback.observedAt &&
        frame - feedback.observedAt < 5 * 24;
}

struct ResourceLedger {
    int minerals{};
    int gas{};
    int reservedMinerals{};
    int reservedGas{};
    int committedMinerals{};
    int committedGas{};
    int protectedMinerals{};
    int protectedGas{};

    [[nodiscard]] int freeMinerals() const noexcept {
        return std::max(0, minerals - reservedMinerals);
    }
    [[nodiscard]] int freeGas() const noexcept { return std::max(0, gas - reservedGas); }
    void beginFrame(int observedMinerals, int observedGas) noexcept;
    [[nodiscard]] bool canReserve(int mineralCost, int gasCost) const noexcept;
    bool reserve(int mineralCost, int gasCost) noexcept;
    void protect(int mineralCost, int gasCost) noexcept;
    [[nodiscard]] bool canSpendCommitted(int mineralCost, int gasCost) const noexcept;
    bool spendCommitted(int mineralCost, int gasCost) noexcept;
    bool releaseCommitted(int mineralCost, int gasCost) noexcept;
    bool spendAvailable(int mineralCost, int gasCost) noexcept;
};

class MacroPlanner {
public:
    explicit MacroPlanner(bool pvzMineralFallback = false,
                          bool earlyPvzMineralFallback = false) noexcept
        : pvzMineralFallback_(pvzMineralFallback),
          earlyPvzMineralFallback_(earlyPvzMineralFallback) {}
    [[nodiscard]] std::vector<MacroAction> reconcile(
        const GameState& state,
        const StrategicPlan& plan,
        ResourceLedger& ledger,
        std::span<const BuildBlockerFeedback> buildBlockers = {},
        std::span<const ConstructionTaskSite> leasedExpansionSites = {}) const;

private:
    bool pvzMineralFallback_{};
    bool earlyPvzMineralFallback_{};
    struct PendingGoal {
        GoalKind goal{GoalKind::build};
        UnitKind target{UnitKind::unknown};
        int desiredCount{};
        int priority{};
        std::string reason;
        Frame lastRequested{-1};
        ConstructionTaskSite constructionSite{};
    };

    // A blocking structure is a strategic checkpoint, not a one-frame scout
    // reaction.  Keep a short-lived demand memory so a hidden building or a
    // transiently stale enemy snapshot cannot erase a Forge/Core/Nexus that
    // was already selected but was still waiting on resources or placement.
    mutable std::vector<PendingGoal> pendingGoals_;
    mutable Frame lastFrame_{-1};
    // The rolling income estimate can stay positive long after a mineral-line
    // evacuation. Actual delivery counters bound how long emergency production
    // may wait for the last few minerals of its preferred unit.
    mutable int lastGatheredMinerals_{-1};
    mutable Frame lastMineralDeliveryFrame_{-1};
    // Observed construction consumes the first emergency anchor boost. A
    // destroyed Cannon must not restart that boost ahead of every Gateway.
    mutable bool cannonCommitmentObserved_{};
    struct PendingCriticalBankHold {
        ConstructionTaskSite site{};
        Frame firstPendingFrame{-1};
    };
    // Fresh route observations and site rotation cannot renew the same
    // critical-depletion waiting episode. This is allocation, never a
    // construction command or route-readiness permission.
    mutable std::optional<PendingCriticalBankHold> pendingCriticalBankHold_;
    struct PendingStaticBankHold {
        UnitKind target{UnitKind::unknown};
        ConstructionTaskSite site{};
        Frame firstPendingFrame{-1};
    };
    mutable std::optional<PendingStaticBankHold> pendingStaticBankHold_;

    [[nodiscard]] static int countExisting(const GameState& state, UnitKind kind);
    [[nodiscard]] static bool constructionDemandFulfilled(
        const GameState& state, GoalKind goal, UnitKind target, int desiredCount,
        const ConstructionTaskSite& site, int plannedNexuses = 0);
    [[nodiscard]] static int countCompleted(const GameState& state, UnitKind kind);
    [[nodiscard]] static bool prerequisitesMet(const GameState& state, UnitKind kind);
    [[nodiscard]] static UnitKind nextMissingPrerequisite(
        const GameState& state,
        UnitKind kind);
    [[nodiscard]] static MacroActionKind actionKind(GoalKind goal) noexcept;
};

}  // namespace protodd
