#pragma once

#include "protodd/GameState.hpp"

#include <algorithm>
#include <cstdint>

namespace protodd {

enum class BuildTaskPhase : std::uint8_t {
    prepositioning,
    travelling,
    commandPending,
    acknowledged,
    constructing,
};

[[nodiscard]] constexpr bool activeConstructionWaypointOrder(
    const bool moveCommand, const bool moveOrder,
    const Position commandTarget, const Position waypoint) noexcept {
    // LastCommand survives after the unit arrives or its order is lost. Only
    // an actual Move toward this waypoint may suppress a new route leg.
    return moveCommand && moveOrder && commandTarget.valid() && waypoint.valid() &&
        distanceSquared(commandTarget, waypoint) <= 24 * 24;
}

// Give distant builders time proportional to their route, with a hard bound
// for blocked or orbiting workers. Emergency local supply keeps its short
// recovery window instead of inheriting an expansion-length lease.
[[nodiscard]] constexpr Frame buildTaskTravelDeadlineFrames(
    const UnitKind kind, const int distancePixels,
    const int workerSpeedMilliPixelsPerFrame,
    const bool plannedRemotePower) noexcept {
    constexpr Frame fastLocalSupplyFrames = 8 * 24;
    constexpr Frame minimumTravelFrames = 18 * 24;
    constexpr Frame maximumTravelFrames = 60 * 24;
    constexpr Frame routeBufferFrames = 12 * 24;
    const auto distance = std::max(0, distancePixels);
    if (kind == UnitKind::pylon && !plannedRemotePower && distance <= 384)
        return fastLocalSupplyFrames;

    const auto speed = std::max(1, workerSpeedMilliPixelsPerFrame);
    const auto estimatedTravel =
        (static_cast<std::int64_t>(distance) * 1000 + speed - 1) / speed;
    const auto deadline = estimatedTravel * 2 + routeBufferFrames;
    return static_cast<Frame>(std::clamp<std::int64_t>(
        deadline, minimumTravelFrames, maximumTravelFrames));
}

[[nodiscard]] constexpr Frame buildTaskHardTravelDeadlineFrames(
    const UnitKind kind, const int distancePixels,
    const bool plannedRemotePower) noexcept {
    constexpr Frame localSupplyHardCapFrames = 45 * 24;
    constexpr Frame remoteTravelHardCapFrames = 120 * 24;
    const auto distance = std::max(0, distancePixels);
    if (kind == UnitKind::pylon && !plannedRemotePower && distance <= 384)
        return localSupplyHardCapFrames;
    return remoteTravelHardCapFrames;
}

struct BuildTravelProgress {
    Frame travelDeadline{-1};
    Frame hardDeadline{-1};
    int bestDistancePixels{-1};
};

// Extend a travel lease only after the builder has made meaningful progress
// toward the footprint. Movement that leaves the best distance unchanged
// cannot keep an orbiting or blocked Probe leased forever.
[[nodiscard]] constexpr bool recordBuildTravelProgress(
    BuildTravelProgress& progress, const Frame frame,
    const int distancePixels) noexcept {
    constexpr int meaningfulProgressPixels = 24;
    constexpr Frame progressGraceFrames = 24 * 24;
    if (frame < 0 || distancePixels < 0 || progress.hardDeadline <= frame ||
        (progress.bestDistancePixels >= 0 &&
         distancePixels > progress.bestDistancePixels - meaningfulProgressPixels))
        return false;

    progress.bestDistancePixels = distancePixels;
    progress.travelDeadline = std::min(
        progress.hardDeadline,
        std::max(progress.travelDeadline, frame + progressGraceFrames));
    return true;
}

}  // namespace protodd
