#pragma once
#include <algorithm>
namespace protodd::bwapi {
// BWAPI doubled supply: reserve six early slots for concurrent SCV/Marine
// production and depot travel/build time. Keep Zerg's existing policy unchanged.
inline int supplyPlanningBuffer(bool terran, int used) {
    return std::clamp(4 + used / 12, terran ? 12 : 4, 20);
}
}
