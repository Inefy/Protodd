#include "astra/Runtime.hpp"

#include <algorithm>

namespace astra {

void FrameBudget::reset() noexcept {
    stats_ = {};
    reducedUntil_ = 0;
    emergencyUntil_ = 0;
}

void FrameBudget::record(
    const Frame frame,
    const std::int64_t elapsedMicroseconds) noexcept {
    const auto microseconds = std::max<std::int64_t>(0, elapsedMicroseconds);
    const auto milliseconds = static_cast<double>(microseconds) / 1000.0;
    ++stats_.samples;
    stats_.movingAverageMs = stats_.samples == 1
                                 ? milliseconds
                                 : stats_.movingAverageMs * 0.92 + milliseconds * 0.08;
    stats_.peakMs = std::max(stats_.peakMs, milliseconds);
    if (microseconds >= 42000) ++stats_.over42ms;
    if (microseconds >= 55000) ++stats_.over55ms;
    if (microseconds >= 1000000) ++stats_.overOneSecond;
    if (microseconds >= 10000000) ++stats_.overTenSeconds;

    if (microseconds >= 40000) {
        emergencyUntil_ = std::max(emergencyUntil_, frame + 8 * 24);
        reducedUntil_ = std::max(reducedUntil_, frame + 15 * 24);
    } else if (microseconds >= 28000 || stats_.movingAverageMs >= 20.0) {
        reducedUntil_ = std::max(reducedUntil_, frame + 8 * 24);
    }
}

RuntimeLoad FrameBudget::load(const Frame frame) const noexcept {
    if (frame < emergencyUntil_) return RuntimeLoad::emergency;
    if (frame < reducedUntil_) return RuntimeLoad::reduced;
    return RuntimeLoad::normal;
}

bool FrameBudget::allowSimulation(const Frame frame) const noexcept {
    return load(frame) == RuntimeLoad::normal;
}

int FrameBudget::expensiveCadenceMultiplier(const Frame frame) const noexcept {
    switch (load(frame)) {
        case RuntimeLoad::normal: return 1;
        case RuntimeLoad::reduced: return 2;
        case RuntimeLoad::emergency: return 4;
    }
    return 1;
}

int FrameBudget::navigationInterval(const Frame frame) const noexcept {
    switch (load(frame)) {
        case RuntimeLoad::normal: return 24;
        case RuntimeLoad::reduced: return 48;
        case RuntimeLoad::emergency: return 96;
    }
    return 24;
}

std::size_t FrameBudget::combatCommandLimit(const Frame frame) const noexcept {
    switch (load(frame)) {
        case RuntimeLoad::normal: return 96U;
        case RuntimeLoad::reduced: return 64U;
        case RuntimeLoad::emergency: return 40U;
    }
    return 96U;
}

const RuntimeStats& FrameBudget::stats() const noexcept {
    return stats_;
}

std::string_view runtimeLoadName(const RuntimeLoad load) noexcept {
    switch (load) {
        case RuntimeLoad::normal: return "normal";
        case RuntimeLoad::reduced: return "reduced";
        case RuntimeLoad::emergency: return "emergency";
    }
    return "invalid";
}

}  // namespace astra
