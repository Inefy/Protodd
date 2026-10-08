#pragma once

#include "protodd/GameState.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>

namespace protodd {

// The adapter supplies one byte per tile in row-major footprint order. An
// incomplete or malformed sample is deliberately not evidence of vacancy.
[[nodiscard]] inline bool fullyVisibleFootprint(
    const int widthTiles,
    const int heightTiles,
    const std::span<const std::uint8_t> visibleTiles) noexcept {
    if (widthTiles <= 0 || heightTiles <= 0 ||
        visibleTiles.size() != static_cast<std::size_t>(widthTiles) *
                                   static_cast<std::size_t>(heightTiles)) {
        return false;
    }
    return std::ranges::all_of(visibleTiles, [](const auto visible) {
        return visible != 0;
    });
}

struct EnemyMemoryReconciliation {
    bool forget{};
    UnitSnapshot snapshot;
};

// `freshObservation` must only contain a currently visible and detected enemy
// sample. Empty-footprint evidence is true only when the complete last-known
// footprint is visible and the fresh observation is absent.
[[nodiscard]] EnemyMemoryReconciliation reconcileEnemyMemory(
    const UnitSnapshot& previous,
    std::optional<UnitSnapshot> freshObservation,
    Frame currentFrame,
    bool emptyFootprintFullyVisible,
    bool typeCanFly) noexcept;

}  // namespace protodd
