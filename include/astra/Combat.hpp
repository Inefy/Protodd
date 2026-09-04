#pragma once

#include "astra/CommandBus.hpp"
#include "astra/GameState.hpp"
#include "astra/InfluenceMap.hpp"

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace astra {

enum class FightDecision : std::uint8_t { engage, kite, retreat };

struct CombatEstimate {
    double friendlyPower{};
    double enemyPower{};
    double ratio{};
    double confidence{};
    double simulatedFriendlyRemaining{};
    double simulatedEnemyRemaining{};
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
        double uncertainty,
        bool runSimulation = true) const;

    [[nodiscard]] const UnitSnapshot* selectTarget(
        const UnitSnapshot& attacker,
        std::span<const UnitSnapshot> candidates,
        std::span<const TargetAllocation> allocations = {}) const;

private:
    [[nodiscard]] static double unitPower(
        const UnitSnapshot& unit,
        std::span<const UnitSnapshot> opposition);
};

// Combat simulations naturally jitter near a decision boundary as units move
// in and out of range. Require a short run of consistent evidence before a
// squad reverses direction, while still allowing an immediate emergency exit.
class EngagementTracker {
public:
    [[nodiscard]] FightDecision stabilize(
        std::uint64_t squadSignature,
        FightDecision proposed,
        double ratio,
        double requiredRatio,
        Frame frame);
    void reset();

private:
    struct Memory {
        FightDecision decision{FightDecision::retreat};
        FightDecision candidate{FightDecision::retreat};
        int consecutive{};
        Frame lastSeen{};
    };

    std::unordered_map<std::uint64_t, Memory> memory_;
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
        Position formationCenter = {-1, -1},
        int latencyFrames = 0,
        bool psionicStormAvailable = false) const;
};

}  // namespace astra
