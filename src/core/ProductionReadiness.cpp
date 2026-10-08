#include "protodd/ProductionReadiness.hpp"

#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <ranges>

namespace protodd {

ProductionReadinessSnapshot assessProductionReadiness(
    const PlayerSnapshot& player) noexcept {
    ProductionReadinessSnapshot result;
    result.producerSnapshotAvailable = !player.producerSlots.empty();

    for (const auto& unit : player.units) {
        if (unit.kind == UnitKind::gateway && unit.completed && !unit.disabled &&
            !unit.loaded && !unit.hallucination && unit.powered) {
            ++result.usableGateways;
            const auto slot = std::ranges::find(
                player.producerSlots, unit.id, &ProducerSlotSnapshot::id);
            if (slot == player.producerSlots.end()) {
                ++result.unobservedGateways;
            } else if (slot->activeTraining || slot->trainingQueueSize > 0 ||
                       slot->recentTrainCommand || slot->researching ||
                       slot->upgrading || !slot->queuedUnits.empty() ||
                       unit.remainingTrainFrames > 0) {
                ++result.occupiedGateways;
            }
        }

        if (!unit.hallucination && !unit.disabled && !unit.loaded &&
            isCombatUnit(unit.kind)) {
            // BWAPI exposes the active Protoss unit as an incomplete unit in
            // the player's unit list, so this includes the current training.
            ++result.fieldedOrTrainingCombatUnits;
        }
    }

    if (result.producerSnapshotAvailable) {
        for (const auto& slot : player.producerSlots) {
            if (slot.queuedUnits.empty() || !isCombatUnit(slot.queuedUnits.front())) continue;
            const auto producer = std::ranges::find(
                player.units, slot.id, &UnitSnapshot::id);
            if (producer == player.units.end() || !producer->completed ||
                producer->disabled || producer->loaded || producer->hallucination ||
                (unitStats(producer->kind).requiresPsi && !producer->powered)) {
                continue;
            }
            ++result.imminentQueuedCombatUnits;
        }
    } else {
        // Older and hand-authored states provide only the aggregate waiting
        // queue. Preserve that fallback while avoiding deep-queue optimism in
        // the per-producer snapshots supplied by the live bridge.
        result.imminentQueuedCombatUnits = static_cast<int>(std::ranges::count_if(
            player.queuedUnits, [](const UnitKind kind) { return isCombatUnit(kind); }));
    }
    return result;
}

}  // namespace protodd
