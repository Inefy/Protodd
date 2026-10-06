#pragma once

#include "protodd/Combat.hpp"
#include "protodd/GameState.hpp"
#include "protodd/InfluenceMap.hpp"
#include "protodd/Harassment.hpp"
#include "protodd/Strategy.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace protodd {

enum class SquadRole : std::uint8_t { mainArmy, baseDefense, harassment };

struct Squad {
    int id{};
    // Changes whenever the role, objective, retreat anchor, or membership
    // changes. Runtime caches use this instead of the transient vector index.
    std::uint64_t signature{};
    // Combat decision memory survives reinforcement and a moving objective;
    // routing still uses the complete signature above.
    std::uint64_t engagementKey{};
    SquadRole role{SquadRole::mainArmy};
    std::vector<UnitSnapshot> units;
    std::vector<UnitSnapshot> enemies;
    Position center{-1, -1};
    Position objective{-1, -1};
    Position retreat{-1, -1};
    double requiredRatio{1.2};
    bool needsDetection{};
    bool emergencyDefense{};
    DefenseArea defense;
    bool withdrawing{};
    std::string missionReason;
};

enum class MainArmyTravelMode : std::uint8_t {
    assemble,
    joinVanguard,
    attack,
};

class SquadPlanner {
public:
    void reset() { harassment_.reset(); }
    void forgetUnit(UnitId id) { harassment_.forgetUnit(id); }
    [[nodiscard]] static std::optional<Position> threatenedNaturalRally(
        const GameState& state, const StrategicPlan& plan);
    [[nodiscard]] std::vector<Squad> form(
        const GameState& state,
        std::span<const UnitSnapshot> friendly,
        std::span<const UnitSnapshot> enemy,
        const StrategicPlan& plan,
        Position fallbackRetreat, const NavigationGrid* navigation = nullptr,
        bool emergencyConsolidation = false,
        bool limitStaticCoverage = false) const;

    [[nodiscard]] std::vector<Command> detectorEscorts(
        const GameState& state,
        std::span<const Squad> squads,
        const InfluenceMap& influence,
        bool mobilizeReserveAgainstLurkers = false,
        bool centerBlockedMainEscort = false,
        bool mobilizeContestedReserve = false,
        bool directSafeRendezvous = false) const;

    [[nodiscard]] static const Squad* selectVanguard(
        std::span<const Squad> squads,
        Position objective) noexcept;
    [[nodiscard]] static MainArmyTravelMode mainArmyTravelMode(
        const Squad& squad, const Squad* vanguard, bool aggressive,
        int minimumAttackSize) noexcept;
    [[nodiscard]] static Position favorableTerranFrontTarget(
        const GameState& state, const Squad& squad,
        const CombatEstimate& estimate) noexcept;
    [[nodiscard]] static std::vector<Command> supportEscorts(
        const Squad& squad, Position objective);

    [[nodiscard]] static bool mustHoldDefensiveScreen(
        const Squad& squad) noexcept;
    [[nodiscard]] static bool mobileDetectionReady(
        const GameState& state, const Squad& squad) noexcept;

    [[nodiscard]] static DefenseArea defensiveArea(
        const GameState& state, Position rally);
    [[nodiscard]] static bool shouldCoverExpansion(
        const GameState& state, const StrategicPlan& plan,
        bool coverForwardThird = false) noexcept;
    [[nodiscard]] static bool survivingBaseUnderThreat(
        const GameState& state) noexcept;
    [[nodiscard]] static DefenseArea expansionDefense(
        const Squad& squad, Position assembly, Position expansion,
        DefenseArea currentDefense = {}) noexcept;

    // Mining workers and unfinished/ordinary structures are legal tactical
    // targets without inflating the local army. form() separately admits
    // visible workers that are already surrounding or pursuing a member.
    [[nodiscard]] static std::vector<UnitSnapshot> tacticalTargets(
        const Squad& squad, std::span<const UnitSnapshot> hostiles);
    // Evaluation context only: ownership and issued orders stay with each squad.
    [[nodiscard]] static std::vector<UnitSnapshot> combatSupport(
        const Squad& squad, std::span<const UnitSnapshot> friendly,
        const NavigationGrid* navigation = nullptr);

    [[nodiscard]] static bool canCounterattack(
        const Squad& squad, const CombatEstimate& estimate, const StrategicPlan& plan);
    [[nodiscard]] static DefenseArea defensiveEngagementArea(
        const Squad& squad, const CombatEstimate& estimate);
    [[nodiscard]] static Position reinforcementDestination(
        const Squad& squad, const Squad& vanguard, Position attackTarget);

private:
    mutable HarassmentPlanner harassment_;
    [[nodiscard]] static Position centroid(std::span<const UnitSnapshot> units) noexcept;
    [[nodiscard]] static std::vector<std::vector<UnitSnapshot>> connectedGroups(
        std::span<const UnitSnapshot> units,
        int linkDistance);
    [[nodiscard]] static std::vector<UnitSnapshot> localEnemies(
        std::span<const UnitSnapshot> enemies,
        std::span<const UnitSnapshot> units,
        Position objective,
        int radius,
        Frame frame);
};

[[nodiscard]] std::string_view squadRoleName(SquadRole role) noexcept;

}  // namespace protodd
