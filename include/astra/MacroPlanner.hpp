#pragma once

#include "astra/GameState.hpp"
#include "astra/Strategy.hpp"

#include <string>
#include <vector>

namespace astra {

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
};

class MacroPlanner {
public:
    [[nodiscard]] std::vector<MacroAction> reconcile(
        const GameState& state,
        const StrategicPlan& plan,
        ResourceLedger& ledger) const;

private:
    [[nodiscard]] static int countExisting(const GameState& state, UnitKind kind);
    [[nodiscard]] static int countCompleted(const GameState& state, UnitKind kind);
    [[nodiscard]] static bool prerequisitesMet(const GameState& state, UnitKind kind);
    [[nodiscard]] static UnitKind nextMissingPrerequisite(
        const GameState& state,
        UnitKind kind);
    [[nodiscard]] static MacroActionKind actionKind(GoalKind goal) noexcept;
};

}  // namespace astra
