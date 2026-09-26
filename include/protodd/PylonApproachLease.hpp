#pragma once

namespace protodd {

// Diagnostic policy for a bounded grace period after a travelling Pylon
// builder first reaches the final 128 pixels. The caller records nearSince
// once, rather than resetting the grace timer when the worker moves away.
struct PylonApproachLeaseState {
    int age{};
    int framesSinceProgress{};
    int distanceSquaredToSite{};
    int framesSinceNearArrival{-1};
    bool placingBuilding{};
    bool buildTypeMatches{};
    bool workerCanBuildHere{};
    bool hasPath{};
};

[[nodiscard]] constexpr bool keepApproachingPylonLease(
    const PylonApproachLeaseState& state) noexcept {
    if (state.age < 8 * 24 || state.age >= 18 * 24 ||
        state.framesSinceProgress > 24 || !state.placingBuilding ||
        !state.buildTypeMatches || !state.workerCanBuildHere || !state.hasPath)
        return false;
    if (state.distanceSquaredToSite > 128 * 128)
        return state.framesSinceNearArrival < 0 ||
               state.framesSinceNearArrival < 2 * 24;
    return state.framesSinceNearArrival >= 0 &&
           state.framesSinceNearArrival < 2 * 24;
}

}  // namespace protodd
