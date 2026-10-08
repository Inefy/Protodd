#pragma once

#include "protodd/GameState.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>

namespace protodd {

// One per-frame allowance for every BWAPI UnitCommand call. Direct macro and
// shadow-control issuers cannot spend the slots reserved for later bus-routed
// safety and combat commands.
class FrameCommandBudget {
public:
    void beginFrame(const Frame frame, const std::size_t limit,
                    const std::size_t directReserve) noexcept {
        active_ = true;
        frame_ = frame;
        limit_ = limit;
        reserve_ = std::min(limit, directReserve);
        used_ = directUsed_ = 0;
    }

    [[nodiscard]] bool consume(const bool frameBusCommand) noexcept {
        if (!active_) return true;
        if (used_ >= limit_) return false;
        if (!frameBusCommand && directUsed_ >= limit_ - reserve_) return false;
        ++used_;
        if (!frameBusCommand) ++directUsed_;
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return active_ ? limit_ - used_ : std::numeric_limits<std::size_t>::max();
    }

    [[nodiscard]] std::size_t directRemaining() const noexcept {
        if (!active_) return std::numeric_limits<std::size_t>::max();
        const auto directLimit = limit_ - reserve_;
        return directLimit - directUsed_;
    }

    [[nodiscard]] std::size_t used() const noexcept { return used_; }
    [[nodiscard]] std::size_t limit() const noexcept { return limit_; }
    [[nodiscard]] Frame frame() const noexcept { return frame_; }

private:
    bool active_{};
    Frame frame_{-1};
    std::size_t limit_{};
    std::size_t reserve_{};
    std::size_t used_{};
    std::size_t directUsed_{};
};

[[nodiscard]] constexpr std::size_t defaultUrgentCommandReserve(
    const std::size_t limit) noexcept {
    if (limit == 0) return 0;
    return std::min<std::size_t>(8, std::max<std::size_t>(1, limit / 8));
}

}  // namespace protodd
