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
};

enum class BuildBlockerReason : std::uint8_t {
    noBuilder,
    unsafeRoute,
    missingPrerequisite,
    noPlacement,
    noPower,
    rejectedFootprint,
};

struct BuildBlockerFeedback {
    UnitKind target{UnitKind::unknown};
    ConstructionTaskSite constructionSite{};
    BuildBlockerReason reason{BuildBlockerReason::noPlacement};
    Frame retryAt{};
};

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
        std::span<const BuildBlockerFeedback> buildBlockers = {}) const;

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

    [[nodiscard]] static int countExisting(const GameState& state, UnitKind kind);
    [[nodiscard]] static int countCompleted(const GameState& state, UnitKind kind);
    [[nodiscard]] static bool prerequisitesMet(const GameState& state, UnitKind kind);
    [[nodiscard]] static UnitKind nextMissingPrerequisite(
        const GameState& state,
        UnitKind kind);
    [[nodiscard]] static MacroActionKind actionKind(GoalKind goal) noexcept;
};

}  // namespace protodd
