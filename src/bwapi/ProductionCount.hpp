#pragma once
#include <BWAPI/UnitType.h>
#include <BWAPI/Race.h>

namespace protodd::bwapi {
// Zerg eggs are counted from their morph target, never again from raw queues.
// Worker build orders are not production queues (and may survive abandonment).
inline int pendingProductionCount(BWAPI::UnitType producer, BWAPI::UnitType target,
                                 BWAPI::UnitType morphTarget, int matchingQueue) {
    if (producer == BWAPI::UnitTypes::Zerg_Egg)
        return morphTarget == target ? (target.isTwoUnitsInOneEgg() ? 2 : 1) : 0;
    if (producer.isBuilding() && producer.getRace() == BWAPI::Races::Terran &&
        target.whatBuilds().first == producer && !target.isBuilding())
        return matchingQueue;
    return 0;
}
}
