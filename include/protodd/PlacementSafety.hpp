#pragma once

#include "protodd/Navigation.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace protodd {

struct PlacementAccessRoute {
    Position from{-1, -1};
    Position to{-1, -1};
    MovementFootprint footprint{};
    NavigationPathResult baseline{};
    std::uint64_t capturedObstacleVersion{};
};

// Every requirement group needs at least one route alternative. Groups are
// independent: preserving one Nexus lane cannot compensate for sealing a
// Gateway exit. Baseline routes are captured once before trying candidates.
struct PlacementAccessRequirement {
    std::vector<PlacementAccessRoute> alternatives;
    bool reachableBefore{};
};

void capturePlacementAccessBaselines(
    NavigationGrid& navigation,
    std::vector<PlacementAccessRequirement>& requirements,
    int maximumExpansions = 12000);

// Temporarily adds one candidate footprint to the shared navigation overlay,
// verifies every previously reachable access group, and always removes it.
[[nodiscard]] bool placementPreservesAccess(
    NavigationGrid& navigation, std::uint64_t temporaryObstacleId,
    NavigationObstacleBounds candidate,
    std::span<PlacementAccessRequirement> requirements,
    int maximumExpansions = 12000,
    std::span<PlacementAccessRequirement> additionalRequirements = {});

}  // namespace protodd
