#pragma once

#include "protodd/GameState.hpp"
#include "protodd/Strategy.hpp"

#include <string>
#include <vector>

namespace protodd {

// The next command arrives after latency: a producer finishing within that
// window can accept one successor, but never a second waiting queue entry.
[[nodiscard]] bool trainingSlotAvailable(bool active, int queueSize,
                                        int remainingFrames, int latencyFrames,
                                        bool recentTrainCommand) noexcept;

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
};

struct ResourceLedger {
    int minerals{};
    int gas{};
    int reservedMinerals{};
    int reservedGas{};

    [[nodiscard]] int freeMinerals() const noexcept { return minerals - reservedMinerals; }
    [[nodiscard]] int freeGas() const noexcept { return gas - reservedGas; }
    [[nodiscard]] bool canReserve(int mineralCost, int gasCost) const noexcept;
    bool reserve(int mineralCost, int gasCost) noexcept;
    void protect(int mineralCost, int gasCost) noexcept;
};

class MacroPlanner {
public:
    [[nodiscard]] std::vector<MacroAction> reconcile(
        const GameState& state,
        const StrategicPlan& plan,
        ResourceLedger& ledger) const;

private:
    struct PendingGoal {
        GoalKind goal{GoalKind::build};
        UnitKind target{UnitKind::unknown};
        int desiredCount{};
        int priority{};
        std::string reason;
        Frame lastRequested{-1};
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
