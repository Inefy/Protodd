#pragma once

#include "astra/Combat.hpp"
#include "astra/GameState.hpp"
#include "astra/InfluenceMap.hpp"
#include "astra/Strategy.hpp"

#include <span>
#include <vector>

namespace astra {

enum class SquadRole : std::uint8_t { mainArmy, baseDefense, harassment };

struct Squad {
    int id{};
    SquadRole role{SquadRole::mainArmy};
    std::vector<UnitSnapshot> units;
    std::vector<UnitSnapshot> enemies;
    Position center{-1, -1};
    Position objective{-1, -1};
    Position retreat{-1, -1};
    double requiredRatio{1.2};
    bool needsDetection{};
};

class SquadPlanner {
public:
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

private:
    [[nodiscard]] static Position centroid(std::span<const UnitSnapshot> units) noexcept;
    [[nodiscard]] static std::vector<std::vector<UnitSnapshot>> connectedGroups(
        std::span<const UnitSnapshot> units,
        int linkDistance);
    [[nodiscard]] static std::vector<UnitSnapshot> localEnemies(
        std::span<const UnitSnapshot> enemies,
        std::span<const UnitSnapshot> units,
        Position objective,
        int radius);
};

[[nodiscard]] std::string_view squadRoleName(SquadRole role) noexcept;

}  // namespace astra

