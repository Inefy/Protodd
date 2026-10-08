#pragma once

#include "protodd/CommandBus.hpp"
#include "protodd/GameState.hpp"
#include "protodd/InfluenceMap.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace protodd {

class NavigationGrid;
class TacticalTargetModel;

inline constexpr int psionicStormRadiusPixels = 80;
inline constexpr int psionicStormReservationDistance = 112;
inline constexpr double minimumPsionicStormValue = 1.8;
inline constexpr std::size_t maximumEngagementSimulationUnits = 32;
inline constexpr std::size_t maximumEngagementSimulationPairs = 192;
inline constexpr int maximumEngagementRouteSearches = 16;
inline constexpr int maximumEngagementClearanceSamples = 1024;

[[nodiscard]] constexpr bool engagementSimulationWithinBudget(
    const std::size_t friendlyUnits, const std::size_t enemyUnits) noexcept {
    return friendlyUnits <= maximumEngagementSimulationUnits &&
           enemyUnits <= maximumEngagementSimulationUnits - friendlyUnits &&
           (friendlyUnits == 0 || enemyUnits <=
               maximumEngagementSimulationPairs / friendlyUnits);
}

// Edge-to-edge weapon distance and per-volley damage, shared by targeting and
// simulation. Remaining durability permits shields to deplete during a fight.
[[nodiscard]] double weaponDistance(const UnitSnapshot& a, const UnitSnapshot& b) noexcept;
[[nodiscard]] double attackDamage(const UnitSnapshot& attacker, const UnitSnapshot& target,
                                  double remainingDurability = -1.0) noexcept;
[[nodiscard]] double psionicStormValue(
    Position center, std::span<const UnitSnapshot> enemies,
    std::span<const UnitSnapshot> allies) noexcept;
[[nodiscard]] bool psionicStormSafe(
    Position center, std::span<const UnitSnapshot> enemies,
    std::span<const UnitSnapshot> allies) noexcept;
[[nodiscard]] double psionicStormValue(
    Position center, std::span<const UnitSnapshot> enemies,
    std::span<const UnitSnapshot> allies) noexcept;

enum class IncomingProjectileFamily : std::uint8_t {
    unknown,
    dragoonPhaseDisruptor,
    photonCannonOverlay,
};

// One visible single-hit projectile. The adapter admits only supported,
// moving projectiles expected to arrive in more than one and at most 24 frames.
struct IncomingProjectile {
    int id{};
    UnitId source{-1};
    UnitId target{-1};
    Frame sourceFirstSeen{-1};
    Frame targetFirstSeen{-1};
    double impactFrames{};
    IncomingProjectileFamily family{IncomingProjectileFamily::unknown};
    WeaponSnapshot groundWeapon{};
    WeaponSnapshot airWeapon{};
    bool sourceLifetimeVerified{};
};
void accountIncomingDamage(GameState& state, std::span<const IncomingProjectile> projectiles);

enum class FightDecision : std::uint8_t { engage, kite, retreat };

enum class FightMission : std::uint8_t {
    advance,
    lastBaseDefense,
    preserveValuable,
    harassment,
    delay,
    cleanup,
};

struct FightValuation {
    double requiredRatio{1.2};
    std::string_view rationale;
};

[[nodiscard]] FightValuation valueFightMission(
    FightMission mission, double plannedRequiredRatio) noexcept;
[[nodiscard]] std::string_view fightMissionName(FightMission mission) noexcept;

struct SimulationOmissions {
    double combatValue{};
    std::uint32_t unitCount{};
    std::uint32_t roleMask{};
    double groundWeaponValue{};
    double airWeaponValue{};
};

struct CombatEstimate {
    double friendlyPower{};
    double enemyPower{};
    double ratio{};
    double confidence{};
    double simulatedFriendlyRemaining{};
    double simulatedEnemyRemaining{};
    double simulatedFriendlyLoss{};
    double simulatedEnemyLoss{};
    Frame simulatedFrames{};
    int simulatedFriendlyDeaths{};
    int simulatedEnemyDeaths{};
    bool simulatedOutcomeReached{};
    bool simulationRoutingDeferred{};
    SimulationOmissions omittedFriendly{};
    SimulationOmissions omittedEnemy{};
    bool confidenceStaged{};
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
        bool runSimulation = true,
        const NavigationGrid* navigation = nullptr) const;

    [[nodiscard]] const UnitSnapshot* selectTarget(
        const UnitSnapshot& attacker,
        std::span<const UnitSnapshot> candidates,
        std::span<const TargetAllocation> allocations = {},
        const TacticalTargetModel* targetModel = nullptr) const;

private:
    [[nodiscard]] static double unitPower(
        const UnitSnapshot& unit,
        std::span<const UnitSnapshot> opposition);
};

// Combat simulations naturally jitter near a decision boundary as units move
// in and out of range. Require a short run of consistent evidence before a
// squad reverses direction, while still allowing an immediate emergency exit.
struct EngagementContext {
    std::span<const UnitSnapshot> enemies{};
    bool needsDetection{};
    bool detectionReady{true};
    bool retreatRouteFailed{};
    FightMission mission{FightMission::advance};
};

class EngagementTracker {
public:
    [[nodiscard]] std::uint64_t identify(std::span<const UnitSnapshot> members, Frame frame);
    [[nodiscard]] FightDecision stabilize(
        std::uint64_t squadSignature,
        FightDecision proposed,
        double ratio,
        double requiredRatio,
        Frame frame,
        bool contact = true,
        EngagementContext context = {});
    void forgetUnit(UnitId id);
    void reset();

private:
    struct EnemyProfile {
        UnitId id{-1};
        UnitKind kind{UnitKind::unknown};
        Position position{-1, -1};
        double combatValue{};
        bool siegePosition{};
        Frame lastSeen{};
    };

    struct Memory {
        FightDecision decision{FightDecision::retreat};
        FightDecision candidate{FightDecision::retreat};
        Frame candidateSince{};
        Frame changedAt{};
        Frame lastSeen{};
        Frame lastContact{};
        bool hasMission{};
        FightMission mission{FightMission::advance};
        bool hasDetectionState{};
        bool detectionReady{true};
        std::vector<EnemyProfile> enemies;
    };

    struct Group {
        std::uint64_t key{};
        std::vector<UnitId> members;
        std::vector<UnitId> splitSourceMembers;
        Frame splitSourceFrame{-1};
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

enum class TacticalIntent : std::uint8_t { battle, raid, withdraw };

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
        DefenseArea defense = {},
        TacticalIntent intent = TacticalIntent::battle,
        std::span<const UnitSnapshot> support = {},
        const NavigationGrid* navigation = nullptr,
        std::span<const UnitSnapshot> obstacles = {},
        const TacticalTargetModel* targetModel = nullptr,
        bool detectorWaitVolley = true,
        std::span<const Position> reservedStormZones = {},
        Position stagingGoal = {-1, -1}) const;
};

}  // namespace protodd
