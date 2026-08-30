#pragma once

#include "astra/CommandBus.hpp"
#include "astra/GameState.hpp"
#include "astra/InfluenceMap.hpp"

#include <span>
#include <vector>

namespace astra {

enum class FightDecision : std::uint8_t { engage, kite, retreat };

struct CombatEstimate {
    double friendlyPower{};
    double enemyPower{};
    double ratio{};
    double confidence{};
    FightDecision decision{FightDecision::retreat};
};

struct TargetAllocation {
    UnitId target{-1};
    int committedDamage{};
};

class CombatEvaluator {
public:
    [[nodiscard]] CombatEstimate evaluate(
        std::span<const UnitSnapshot> friendly,
        std::span<const UnitSnapshot> enemy,
        double requiredRatio,
        double uncertainty) const;

    [[nodiscard]] const UnitSnapshot* selectTarget(
        const UnitSnapshot& attacker,
        std::span<const UnitSnapshot> candidates,
        std::span<const TargetAllocation> allocations = {}) const;

private:
    [[nodiscard]] static double unitPower(
        const UnitSnapshot& unit,
        std::span<const UnitSnapshot> opposition);
};

class TacticalController {
public:
    [[nodiscard]] std::vector<Command> control(
        std::span<const UnitSnapshot> friendly,
        std::span<const UnitSnapshot> enemy,
        const CombatEstimate& estimate,
        Position objective,
        Position retreatPoint,
        const InfluenceMap& influence,
        Position formationCenter = {-1, -1}) const;
};

}  // namespace astra
