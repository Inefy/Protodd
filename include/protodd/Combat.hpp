#pragma once

#include "protodd/CommandBus.hpp"
#include "protodd/GameState.hpp"
#include "protodd/InfluenceMap.hpp"

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace protodd {

// Edge-to-edge weapon distance and per-volley damage, shared by targeting and
// simulation. Remaining durability permits shields to deplete during a fight.
[[nodiscard]] double weaponDistance(const UnitSnapshot& a, const UnitSnapshot& b) noexcept;
[[nodiscard]] double attackDamage(const UnitSnapshot& attacker, const UnitSnapshot& target,
                                  double remainingDurability = -1.0) noexcept;

// One visible single-hit projectile. The adapter admits only supported,
// moving projectiles expected to arrive within the next second.
struct IncomingProjectile {
    int id{};
    UnitId source{-1};
    UnitId target{-1};
};
void accountIncomingDamage(GameState& state, std::span<const IncomingProjectile> projectiles);

enum class FightDecision : std::uint8_t { engage, kite, retreat };

struct CombatEstimate {
    double friendlyPower{};
    double enemyPower{};
    double ratio{};
    double confidence{};
    double simulatedFriendlyRemaining{};
    double simulatedEnemyRemaining{};
    FightDecision decision{FightDecision::retreat};
    bool advanceBlocked{};
    bool holdScreen{};
};

struct TargetAllocation {
    UnitId target{-1};
    double committedDamage{};
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
    [[nodiscard]] std::uint64_t identify(std::span<const UnitSnapshot> members, Frame frame);
    [[nodiscard]] FightDecision stabilize(
        std::uint64_t squadSignature,
        FightDecision proposed,
        double ratio,
        double requiredRatio,
        Frame frame,
        bool contact = true);
    void reset();

private:
    struct Memory {
        FightDecision decision{FightDecision::retreat};
        FightDecision candidate{FightDecision::retreat};
        Frame candidateSince{};
        Frame changedAt{};
        Frame lastSeen{};
        Frame lastContact{};
    };

    struct Group {
        std::uint64_t key{};
        std::vector<UnitId> members;
        Frame lastSeen{};
    };

    std::unordered_map<std::uint64_t, Memory> memory_;
    std::vector<Group> groups_;
    std::uint64_t nextKey_{1};
};

struct DefenseArea {
    Position center{-1, -1};
    int pursuitRadius{};
    Position economyCenter{-1, -1};
    Position front{-1, -1};

    [[nodiscard]] bool active() const noexcept {
        return center.valid() && pursuitRadius > 0;
    }
    [[nodiscard]] bool contains(Position position) const noexcept {
        if (economyCenter.valid() && distanceSquared(economyCenter, position) <= 256 * 256)
            return true;
        if (front.valid()) {
            const auto inward = static_cast<long long>(position.x - front.x) * (center.x - front.x) +
                                static_cast<long long>(position.y - front.y) * (center.y - front.y);
            if (inward < 0) return false;
        }
        return distanceSquared(center, position) <= pursuitRadius * pursuitRadius;
    }
};

class TacticalController {
public:
    [[nodiscard]] std::vector<Command> recharge(
        std::span<const UnitSnapshot> friendly, bool defending) const;
    [[nodiscard]] std::vector<Command> control(
        std::span<const UnitSnapshot> friendly,
        std::span<const UnitSnapshot> enemy,
        const CombatEstimate& estimate,
        Position objective,
        Position retreatPoint,
        const InfluenceMap& influence,
        Position formationCenter = {-1, -1},
        int latencyFrames = 0,
        bool psionicStormAvailable = false,
        DefenseArea defense = {}) const;
};

}  // namespace protodd
