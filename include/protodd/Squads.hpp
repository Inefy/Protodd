#pragma once

#include "protodd/Combat.hpp"
#include "protodd/GameState.hpp"
#include "protodd/InfluenceMap.hpp"
#include "protodd/Harassment.hpp"
#include "protodd/Strategy.hpp"

#include <cstdint>
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
    DefenseArea defense;
    bool withdrawing{};
    std::string missionReason;
};

class SquadPlanner {
public:
    void reset() { harassment_.reset(); }
    [[nodiscard]] std::vector<Squad> form(
        const GameState& state,
        std::span<const UnitSnapshot> friendly,
        std::span<const UnitSnapshot> enemy,
        const StrategicPlan& plan,
        Position fallbackRetreat) const;

    [[nodiscard]] std::vector<Command> detectorEscorts(
        const GameState& state,
        std::span<const Squad> squads,
        const InfluenceMap& influence) const;

    [[nodiscard]] static const Squad* selectVanguard(
        std::span<const Squad> squads,
        Position objective) noexcept;
    [[nodiscard]] static Position supportRendezvous(
        const GameState& state, const Squad& squad, Position objective) noexcept;

    [[nodiscard]] static bool mustHoldDefensiveScreen(
        const Squad& squad) noexcept;
    [[nodiscard]] static bool mobileDetectionReady(
        const GameState& state, const Squad& squad) noexcept;

    [[nodiscard]] static DefenseArea defensiveArea(
        const GameState& state, Position rally);

    // Workers and unfinished/ordinary structures are legal tactical targets,
    // but must not inflate the army used for engagement simulation.
    [[nodiscard]] static std::vector<UnitSnapshot> tacticalTargets(
        const Squad& squad, std::span<const UnitSnapshot> hostiles);

    [[nodiscard]] static bool canCounterattack(
        const Squad& squad, const CombatEstimate& estimate, const StrategicPlan& plan);

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
