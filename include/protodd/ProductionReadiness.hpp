#pragma once

#include "protodd/GameState.hpp"

namespace protodd {

struct ProductionReadinessSnapshot {
    int usableGateways{};
    int occupiedGateways{};
    int unobservedGateways{};
    int fieldedOrTrainingCombatUnits{};
    int imminentQueuedCombatUnits{};
    bool producerSnapshotAvailable{};

    [[nodiscard]] constexpr int armyReadySoon() const noexcept {
        return fieldedOrTrainingCombatUnits + imminentQueuedCombatUnits;
    }
};

// Summarize real producer slots without treating incomplete, unpowered, or
// disabled Gateways as usable capacity. For army readiness, count the current
// fielded/training force and only the next waiting combat unit per producer.
[[nodiscard]] ProductionReadinessSnapshot assessProductionReadiness(
    const PlayerSnapshot& player) noexcept;

}  // namespace protodd
