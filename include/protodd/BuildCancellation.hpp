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
        return !handoff_ && (lastRequest_ < 0 || frame - lastRequest_ >= retryAfter_);
    }

    [[nodiscard]] bool retryDue(Frame frame) const noexcept {
        return awaiting_ && requestDue(frame);
    }

    // A confirmed replacement order ends construction authority. Retain the
    // reservation until a later observation clears the old order; a Stop retry
    // would cancel the new owner's emergency movement.
    [[nodiscard]] bool handoff(Frame frame, bool confirmed) noexcept {
        if (!confirmed || handoff_) return false;
        awaiting_ = true;
        handoff_ = true;
        lastRequest_ = frame;
        return true;
    }
    [[nodiscard]] bool handoffPending() const noexcept { return handoff_; }
    [[nodiscard]] bool awaiting() const noexcept { return awaiting_; }
    [[nodiscard]] bool acknowledged(bool oldOrderActionable,
                                    Frame observedFrame = -1) noexcept {
        if (!awaiting_ || oldOrderActionable ||
            (handoff_ && observedFrame <= lastRequest_)) return false;
        awaiting_ = false;
        handoff_ = false;
        lastRequest_ = -1;
        return true;
    }

private:
    bool awaiting_{};
    bool handoff_{};
    Frame lastRequest_{-1};
    Frame retryAfter_{12};
};

}  // namespace protodd
