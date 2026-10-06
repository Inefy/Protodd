#pragma once

#include "protodd/GameState.hpp"

namespace protodd::frame_schedule {

inline constexpr Frame strategyPeriod = 24;
inline constexpr Frame strategyOffset = 22;
inline constexpr Frame influencePeriod = 8;
inline constexpr Frame inferencePeriod = 12;
inline constexpr Frame inferenceOffset = 8;
inline constexpr Frame workerPeriod = 12;
inline constexpr Frame workerOffset = 2;
inline constexpr Frame scoutingPeriod = 24;
inline constexpr Frame scoutingOffset = 3;
inline constexpr Frame maintenancePeriod = 24;
inline constexpr Frame maintenanceOffset = 5;
inline constexpr Frame diagnosticsPeriod = 24;
inline constexpr Frame diagnosticsOffset = 20;
inline constexpr Frame stateLogPeriod = 120;
inline constexpr Frame stateLogOffset = 47;
inline constexpr Frame macroPeriod = 3;
inline constexpr Frame macroOffset = 1;
inline constexpr Frame phaseSummaryPeriod = 240;
inline constexpr Frame phaseSummaryOffset = 236;

[[nodiscard]] constexpr bool dueEvery(
    const Frame frame, const Frame period, const Frame offset = 0) noexcept {
    return frame >= 0 && period > 0 && offset >= 0 && offset < period &&
           frame % period == offset;
}

[[nodiscard]] constexpr Frame latestDueAtOrBefore(
    const Frame frame, const Frame period, const Frame offset = 0) noexcept {
    if (frame < 0 || period <= 0 || offset < 0 || offset >= period || frame < offset)
        return -1;
    return frame - (frame - offset) % period;
}

[[nodiscard]] constexpr Frame scaledPeriod(const Frame period, const int cadence) noexcept {
    return period * (cadence > 0 ? cadence : 1);
}

[[nodiscard]] constexpr bool strategyDue(
    const Frame frame, const bool goalsEmpty) noexcept {
    return goalsEmpty || dueEvery(frame, strategyPeriod, strategyOffset);
}

[[nodiscard]] constexpr bool influenceDue(const Frame frame, const int cadence) noexcept {
    return dueEvery(frame, scaledPeriod(influencePeriod, cadence));
}

[[nodiscard]] constexpr bool inferenceDue(const Frame frame, const int cadence) noexcept {
    return frame == 0 ||
           dueEvery(frame, scaledPeriod(inferencePeriod, cadence), inferenceOffset);
}

[[nodiscard]] constexpr bool workersDue(const Frame frame) noexcept {
    return dueEvery(frame, workerPeriod, workerOffset);
}

[[nodiscard]] constexpr bool scoutingDue(const Frame frame, const int cadence) noexcept {
    return dueEvery(frame, scaledPeriod(scoutingPeriod, cadence), scoutingOffset);
}

[[nodiscard]] constexpr bool maintenanceDue(const Frame frame) noexcept {
    return dueEvery(frame, maintenancePeriod, maintenanceOffset);
}

[[nodiscard]] constexpr bool diagnosticsDue(const Frame frame) noexcept {
    return dueEvery(frame, diagnosticsPeriod, diagnosticsOffset);
}

[[nodiscard]] constexpr bool stateLogDue(const Frame frame) noexcept {
    return dueEvery(frame, stateLogPeriod, stateLogOffset);
}

[[nodiscard]] constexpr bool macroCadenceDue(const Frame frame) noexcept {
    return dueEvery(frame, macroPeriod, macroOffset);
}

[[nodiscard]] constexpr bool phaseSummaryDue(const Frame frame) noexcept {
    return dueEvery(frame, phaseSummaryPeriod, phaseSummaryOffset);
}

}  // namespace protodd::frame_schedule
