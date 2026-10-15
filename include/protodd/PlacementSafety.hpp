#pragma once

#include "protodd/Navigation.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace protodd {

struct PlacementAccessProof;

struct PlacementAccessRoute {
    Position from{-1, -1};
    Position to{-1, -1};
    MovementFootprint footprint{};
    NavigationPathResult baseline{};
    std::uint64_t capturedObstacleVersion{};
    // Immutable owned copy of the validated path and its exact geometry.
    // Comparing the full path also detects edits to these public route fields.
    std::shared_ptr<const PlacementAccessProof> validatedBaseline{};
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

enum class PlacementAccessCaptureStatus : std::uint8_t { complete, deferred };

// The caller begins one shared frame allowance on the queue. An unfinished
// alternative retains its frontier and must not be treated as unreachable.
[[nodiscard]] PlacementAccessCaptureStatus capturePlacementAccessBaselinesBounded(
    NavigationGrid& navigation,
    std::vector<PlacementAccessRequirement>& requirements,
    NavigationWorkQueue& work);

// Temporarily adds one candidate footprint to the shared navigation overlay,
// verifies every previously reachable access group, and always removes it.
[[nodiscard]] bool placementPreservesAccess(
    NavigationGrid& navigation, std::uint64_t temporaryObstacleId,
    NavigationObstacleBounds candidate,
    std::span<PlacementAccessRequirement> requirements,
    int maximumExpansions = 12000,
    std::span<PlacementAccessRequirement> additionalRequirements = {});

}  // namespace protodd
