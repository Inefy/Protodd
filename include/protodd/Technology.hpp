#pragma once

#include "protodd/GameState.hpp"

#include <string_view>

namespace protodd {

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
// Additional engine prerequisite beyond the research/upgrade producer.
[[nodiscard]] UnitKind technologyPrerequisite(TechnologyKind kind, int level) noexcept;
[[nodiscard]] int technologyLevel(
    const PlayerSnapshot& player,
    TechnologyKind kind) noexcept;
[[nodiscard]] bool technologyInProgress(
    const PlayerSnapshot& player,
    TechnologyKind kind) noexcept;

}  // namespace protodd
