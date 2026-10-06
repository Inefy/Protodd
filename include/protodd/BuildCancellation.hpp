#pragma once

#include "protodd/GameState.hpp"

#include <algorithm>

namespace protodd {

// A build reservation remains owned until the old engine order is observed
// as non-actionable. Rejected Stop requests never advance this state.
class BuildCancellation {
public:
    [[nodiscard]] bool request(Frame frame, int latencyFrames, bool accepted) noexcept {
        lastRequest_ = frame;
        retryAfter_ = std::max(12, latencyFrames + 6);
        if (accepted) awaiting_ = true;
        return accepted;
    }

    [[nodiscard]] bool requestDue(Frame frame) const noexcept {
        return lastRequest_ < 0 || frame - lastRequest_ >= retryAfter_;
    }

    [[nodiscard]] bool retryDue(Frame frame) const noexcept {
        return awaiting_ && requestDue(frame);
    }

    [[nodiscard]] bool awaiting() const noexcept { return awaiting_; }
    [[nodiscard]] bool acknowledged(bool oldOrderActionable) noexcept {
        if (!awaiting_ || oldOrderActionable) return false;
        awaiting_ = false;
        lastRequest_ = -1;
        return true;
    }

private:
    bool awaiting_{};
    Frame lastRequest_{-1};
    Frame retryAfter_{12};
};

}  // namespace protodd
