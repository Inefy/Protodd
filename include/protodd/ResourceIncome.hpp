#pragma once

#include "protodd/GameState.hpp"

#include <array>
#include <cstddef>

namespace protodd {

struct ResourceIncomeEstimate {
    int mineralsPerMinute{-1};
    int gasPerMinute{-1};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return mineralsPerMinute >= 0 && gasPerMinute >= 0;
    }
};

// A bounded rolling estimate from the engine's cumulative gathered-resource
// counters. Samples are taken once per second and the rate spans up to the
// latest 30 seconds, long enough to smooth individual return trips.
class ResourceIncomeTracker {
public:
    void reset() noexcept;
    [[nodiscard]] ResourceIncomeEstimate update(
        Frame frame, int gatheredMinerals, int gatheredGas) noexcept;

private:
    struct Sample {
        Frame frame{-1};
        int minerals{};
        int gas{};
    };

    static constexpr std::size_t sampleCapacity = 31;
    std::array<Sample, sampleCapacity> samples_{};
    std::size_t sampleCount_{};
    std::size_t nextSample_{};
    Frame lastObservedFrame_{-1};

    void append(Sample sample) noexcept;
};

}  // namespace protodd
