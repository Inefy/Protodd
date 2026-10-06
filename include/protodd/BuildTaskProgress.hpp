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

}  // namespace protodd
