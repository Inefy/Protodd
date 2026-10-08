#pragma once

#include "protodd/GameState.hpp"

#include <optional>

namespace protodd {

// An empty anchor means the query is map-wide. Call sites that require a
// valid home anchor should check it before constructing this query.
[[nodiscard]] bool hasVisibleGroundCombatContact(
    const GameState& state,
    std::optional<Position> anchor,
    PixelRadius radius) noexcept;

[[nodiscard]] bool hasVisibleMeleeContact(
    const GameState& state,
    Position anchor,
    PixelRadius radius) noexcept;

}  // namespace protodd
