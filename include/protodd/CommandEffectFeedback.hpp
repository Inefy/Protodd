#pragma once

#include "protodd/CommandBus.hpp"

#include <limits>

namespace protodd {

// Keep an accepted transport pickup as active only while BWAPI still reports
// its matching order and through the same bounded acknowledgment window used
// by TransportController. A confirmed load remains an effect indefinitely;
// a stale pickup order becomes retryable.
[[nodiscard]] constexpr Frame loadEffectAcknowledgmentWindow(
    const Frame latencyFrames) noexcept {
    if (latencyFrames <= 0) return 12;
    return latencyFrames > std::numeric_limits<Frame>::max() - 12
        ? std::numeric_limits<Frame>::max() : latencyFrames + 12;
}

[[nodiscard]] constexpr bool loadCommandStillConfirmed(
    const bool passengerLoadedByActor,
    const bool matchingPickupOrder,
    const Frame currentFrame,
    const Frame lastCommandFrame,
    const Frame latencyFrames) noexcept {
    if (passengerLoadedByActor) return true;
    if (!matchingPickupOrder || currentFrame < 0 || lastCommandFrame < 0 ||
        currentFrame < lastCommandFrame) return false;
    return currentFrame - lastCommandFrame <=
        loadEffectAcknowledgmentWindow(latencyFrames);
}

[[nodiscard]] constexpr Frame commandEffectObservationWindow(
    const CommandType type,
    const Frame latencyFrames) noexcept {
    switch (type) {
        case CommandType::move:
        case CommandType::attackMove:
        case CommandType::recharge:
            return 24;
        case CommandType::attackUnit:
            return 18;
        case CommandType::load:
            return loadEffectAcknowledgmentWindow(latencyFrames);
        case CommandType::hold:
            return 120;
        default:
            return 0;
    }
}

}  // namespace protodd
