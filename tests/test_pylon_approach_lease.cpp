#include "protodd/PylonApproachLease.hpp"

#include <iostream>

int main() {
    using protodd::PylonApproachLeaseState;
    using protodd::keepApproachingPylonLease;
    int failed = 0;
    const auto check = [&failed](bool value) { if (!value) ++failed; };

    // Archived active final approach: the earlier rule released at 126px.
    const PylonApproachLeaseState approaching{
        216, 4, 126 * 126, 0, true, true, true, true};
    check(keepApproachingPylonLease(approaching));
    auto state = approaching;
    state.framesSinceNearArrival = 47;
    check(keepApproachingPylonLease(state));
    state.framesSinceNearArrival = 48;
    check(!keepApproachingPylonLease(state));

    // A worker cannot reset the grace period by backing away from the site.
    state.distanceSquaredToSite = 140 * 140;
    check(!keepApproachingPylonLease(state));
    state.framesSinceNearArrival = -1;
    check(keepApproachingPylonLease(state));

    state = approaching;
    state.framesSinceProgress = 49;  // Archived stalled-position case.
    check(!keepApproachingPylonLease(state));
    state = approaching;
    state.workerCanBuildHere = false;
    check(!keepApproachingPylonLease(state));
    state = approaching;
    state.hasPath = false;
    check(!keepApproachingPylonLease(state));
    state = approaching;
    state.placingBuilding = false;
    check(!keepApproachingPylonLease(state));
    state = approaching;
    state.buildTypeMatches = false;
    check(!keepApproachingPylonLease(state));
    state = approaching;
    state.age = 18 * 24;
    check(!keepApproachingPylonLease(state));
    state = approaching;
    state.framesSinceNearArrival = -1;
    check(!keepApproachingPylonLease(state));

    if (failed) std::cerr << failed << " Pylon approach lease checks failed\n";
    return failed ? 1 : 0;
}
