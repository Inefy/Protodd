#include "protodd/ProductionReadiness.hpp"

#include <iostream>

int main() {
    using namespace protodd;
    int failures = 0;
    const auto check = [&failures](const bool ok, const char* message) {
        if (ok) return;
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    };

    PlayerSnapshot player;
    auto gateway = UnitSnapshot{};
    gateway.id = 10;
    gateway.kind = UnitKind::gateway;
    gateway.completed = true;
    auto disabledGateway = gateway;
    disabledGateway.id = 11;
    disabledGateway.powered = false;
    auto zealot = UnitSnapshot{};
    zealot.kind = UnitKind::zealot;
    zealot.completed = true;
    auto trainingDragoon = UnitSnapshot{};
    trainingDragoon.kind = UnitKind::dragoon;
    trainingDragoon.completed = false;
    player.units = {gateway, disabledGateway, zealot, trainingDragoon};
    player.producerSlots = {
        {10, UnitKind::gateway, true, 1, 100, 0, false, false, false,
         {UnitKind::dragoon, UnitKind::zealot}},
        {11, UnitKind::gateway, true, 1, 100, 0, false, false, false,
         {UnitKind::zealot}},
    };
    const auto measured = assessProductionReadiness(player);
    check(measured.producerSnapshotAvailable && measured.usableGateways == 1 &&
              measured.occupiedGateways == 1 && measured.unobservedGateways == 0,
          "only completed powered Gateways contribute to usable occupied capacity");
    check(measured.fieldedOrTrainingCombatUnits == 2 &&
              measured.imminentQueuedCombatUnits == 1 &&
              measured.armyReadySoon() == 3,
          "readiness counts the current training unit and only the next queued combat unit");

    player.producerSlots[0].queuedUnits = {UnitKind::probe, UnitKind::dragoon};
    const auto workerFirst = assessProductionReadiness(player);
    check(workerFirst.imminentQueuedCombatUnits == 0,
          "a combat unit behind a noncombat queue item is not counted as imminent");

    player.producerSlots.clear();
    player.queuedUnits = {UnitKind::dragoon, UnitKind::zealot};
    const auto legacy = assessProductionReadiness(player);
    check(!legacy.producerSnapshotAvailable && legacy.usableGateways == 1 &&
              legacy.occupiedGateways == 0 &&
              legacy.armyReadySoon() == 4,
          "legacy aggregate queues retain readiness fallback without claiming utilization data");

    player.producerSlots = {{99, UnitKind::gateway, false, 0, 0, 0,
                             false, false, false, {}}};
    const auto missingSlot = assessProductionReadiness(player);
    check(missingSlot.producerSnapshotAvailable && missingSlot.usableGateways == 1 &&
              missingSlot.unobservedGateways == 1 && missingSlot.occupiedGateways == 0,
          "a usable Gateway missing its slot record remains explicitly unobserved");

    return failures == 0 ? 0 : 1;
}
