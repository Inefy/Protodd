#pragma once

#include "astra/GameState.hpp"
#include "astra/Information.hpp"
#include "astra/InfluenceMap.hpp"

#include <span>
#include <unordered_map>
#include <vector>

namespace astra {

enum class ScoutPurpose : std::uint8_t {
    findEnemy,
    checkTech,
    checkExpansion,
    watchArmy,
    patrolDropPath,
};

struct ScoutOrder {
    UnitId scout{};
    Position target{-1, -1};
    ScoutPurpose purpose{ScoutPurpose::findEnemy};
    double score{};
};

class ScoutManager {
public:
    void reset() noexcept;

    [[nodiscard]] std::vector<ScoutOrder> assign(
        const GameState& state,
        std::span<const UnitId> availableScouts,
        const InfluenceMap& influence,
        const ThreatAssessment& threat);

private:
    std::unordered_map<UnitId, ScoutOrder> previousOrders_;
};

}  // namespace astra
