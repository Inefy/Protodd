#pragma once

#include "astra/GameState.hpp"

#include <string_view>

namespace astra {

struct UnitStats {
    std::string_view name;
    int minerals{};
    int gas{};
    int supply{};  // BWAPI doubled-supply convention.
    int buildTime{};
    double combatValue{};
    bool building{};
    bool producer{};
    bool requiresPsi{};
};

[[nodiscard]] const UnitStats& unitStats(UnitKind kind) noexcept;
[[nodiscard]] bool isBuilding(UnitKind kind) noexcept;
[[nodiscard]] bool isWorker(UnitKind kind) noexcept;
[[nodiscard]] bool isCombatUnit(UnitKind kind) noexcept;
[[nodiscard]] bool isStaticDefense(UnitKind kind) noexcept;

}  // namespace astra

