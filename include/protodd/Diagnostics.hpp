#pragma once

#include "protodd/GameState.hpp"

#include <algorithm>
#include <cstdint>

namespace protodd {

// Integrate the previous observed gauge over elapsed frames. Duplicate
// samples and game resets cannot inflate durations. Between samples this
// is an estimate, explicitly distinct from a unit-destroy callback.
class FrameIntegral {
public:
    void sample(Frame frame, int value) noexcept {
        if (lastFrame_ >= 0 && frame < lastFrame_) reset();
        if (lastFrame_ >= 0)
            total_ += static_cast<std::int64_t>(frame - lastFrame_) * value_;
        lastFrame_ = frame;
        value_ = std::max(0, value);
    }
    void reset() noexcept { lastFrame_ = -1; value_ = 0; total_ = 0; }
    [[nodiscard]] std::int64_t total() const noexcept { return total_; }
private:
    Frame lastFrame_{-1};
    int value_{};
    std::int64_t total_{};
};

struct PhaseTiming {
    std::uint64_t calls{};
    std::int64_t totalUs{};
    std::int64_t peakUs{};
    void record(std::int64_t us) noexcept {
        us = std::max<std::int64_t>(0, us);
        ++calls;
        totalUs += us;
        peakUs = std::max(peakUs, us);
    }
};

}  // namespace protodd
