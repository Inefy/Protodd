#pragma once

#include <BWAPI/UnitType.h>

namespace protodd::bwapi {

// The runtime cloak bit can be false for our permanently cloaked units.
// Their legally observed type still supplies that intrinsic capability once
// construction finishes. Toggle cloaking retains its current runtime state.
[[nodiscard]] inline bool observedUnitCloaked(
    const BWAPI::UnitType type, const bool runtimeCloaked,
    const bool completed) noexcept {
    return runtimeCloaked || (completed && type.hasPermanentCloak());
}

} // namespace protodd::bwapi
