#pragma once

#include "protodd/GameState.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>

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

// Consecutive samples support a duration estimate; gaps never imply that an
// unobserved condition continued. Emits a start, heartbeats, and a resolution.
struct EpisodeUpdate {
    Frame since{};
    Frame duration{};
    bool active{};
};

class DiagnosticEpisode {
public:
    [[nodiscard]] std::optional<EpisodeUpdate> sample(
        Frame frame, bool active, Frame threshold, Frame maximumGap = 48) noexcept {
        if (last_ >= 0 && (frame < last_ || frame - last_ > maximumGap)) *this = {};
        last_ = frame;
        if (!active) {
            const auto result = reported_ ? std::optional<EpisodeUpdate>{{since_, frame - since_, false}}
                                          : std::nullopt;
            since_ = emitted_ = -1;
            reported_ = false;
            return result;
        }
        if (since_ < 0) since_ = frame;
        if (frame - since_ < threshold || (reported_ && frame - emitted_ < 120)) return {};
        reported_ = true;
        emitted_ = frame;
        return EpisodeUpdate{since_, frame - since_, true};
    }
private:
    Frame since_{-1};
    Frame last_{-1};
    Frame emitted_{-1};
    bool reported_{};
};

}  // namespace protodd
