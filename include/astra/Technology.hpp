#pragma once

#include "astra/GameState.hpp"

#include <string_view>

namespace astra {

struct TechnologyStats {
    std::string_view name;
    int mineralBase{};
    int gasBase{};
    int mineralFactor{};
    int gasFactor{};
    int maximumLevel{1};
    UnitKind producer{UnitKind::unknown};
    bool research{};

    [[nodiscard]] int mineralCost(int level) const noexcept;
    [[nodiscard]] int gasCost(int level) const noexcept;
};

[[nodiscard]] const TechnologyStats& technologyStats(TechnologyKind kind) noexcept;
[[nodiscard]] int technologyLevel(
    const PlayerSnapshot& player,
    TechnologyKind kind) noexcept;
[[nodiscard]] bool technologyInProgress(
    const PlayerSnapshot& player,
    TechnologyKind kind) noexcept;

}  // namespace astra
