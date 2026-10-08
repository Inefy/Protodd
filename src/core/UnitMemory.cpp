#include "protodd/UnitMemory.hpp"

#include "protodd/UnitCatalog.hpp"

#include <utility>

namespace protodd {

EnemyMemoryReconciliation reconcileEnemyMemory(
    const UnitSnapshot& previous,
    std::optional<UnitSnapshot> freshObservation,
    const Frame currentFrame,
    const bool emptyFootprintFullyVisible,
    const bool typeCanFly) noexcept {
    if (freshObservation.has_value()) {
        auto current = std::move(*freshObservation);
        current.inheritObservationHistory(previous);
        current.updateMemoryConfidence(currentFrame);
        return {false, std::move(current)};
    }

    auto remembered = previous;
    if (isBuilding(remembered.kind) && emptyFootprintFullyVisible) {
        if (!typeCanFly) return {true, {}};
        if (remembered.position.valid()) remembered.lastPosition = remembered.position;
        remembered.position = {-1, -1};
        remembered.locationConfidence = 0.0;
    }
    remembered.visible = false;
    remembered.detected = false;
    remembered.underAttack = false;
    remembered.updateMemoryConfidence(currentFrame);
    return {false, std::move(remembered)};
}

}  // namespace protodd
