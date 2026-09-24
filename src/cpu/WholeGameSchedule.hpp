#pragma once

#include "WholeGameIntent.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

namespace protodd::cpu {

struct PlannedIntent {
    int delayFrames{};
    Intent intent;
};

struct ScheduledIntent {
    int dueFrame{};
    std::size_t slot{};
    Intent intent;
};

// One cadence prediction owns at most the following 24 frames. A replacement
// discards old decisions; a missed frame never leaks a stale command into the
// next cadence window.
class WholeGameSchedule {
public:
    void clear() noexcept {
        pending_.clear();
        origin_ = -1;
        lastFrame_ = -1;
    }

    void replace(int origin, std::vector<PlannedIntent> plans) {
        if (origin < 0 || plans.size() > 6) {
            throw std::invalid_argument("invalid whole-game cadence plan");
        }
        std::vector<ScheduledIntent> next;
        next.reserve(plans.size());
        for (std::size_t index = 0; index < plans.size(); ++index) {
            if (plans[index].delayFrames < 0 || plans[index].delayFrames >= 24) {
                throw std::invalid_argument("whole-game dispatch delay outside cadence");
            }
            next.push_back({origin + plans[index].delayFrames, index,
                            std::move(plans[index].intent)});
        }
        std::stable_sort(next.begin(), next.end(), [](const auto& left, const auto& right) {
            return left.dueFrame < right.dueFrame;
        });
        pending_ = std::move(next);
        origin_ = origin;
        lastFrame_ = origin - 1;
    }

    [[nodiscard]] std::vector<ScheduledIntent> takeDue(int frame) {
        if (origin_ < 0 || frame < origin_ || frame < lastFrame_ || frame >= origin_ + 24) {
            clear();
            return {};
        }
        lastFrame_ = frame;
        const auto end = std::find_if(pending_.begin(), pending_.end(), [frame](const auto& item) {
            return item.dueFrame > frame;
        });
        std::vector<ScheduledIntent> ready;
        ready.reserve(static_cast<std::size_t>(end - pending_.begin()));
        for (auto it = pending_.begin(); it != end; ++it) ready.push_back(std::move(*it));
        pending_.erase(pending_.begin(), end);
        return ready;
    }

    // A same-frame actor collision can be retried on the next frame, but may
    // never cross the prediction window or reorder ahead of an earlier slot.
    [[nodiscard]] bool defer(ScheduledIntent item, int nextFrame) {
        if (origin_ < 0 || nextFrame <= lastFrame_ || nextFrame >= origin_ + 24) return false;
        item.dueFrame = nextFrame;
        const auto at = std::upper_bound(
            pending_.begin(), pending_.end(), item,
            [](const auto& left, const auto& right) {
                return std::pair(left.dueFrame, left.slot) <
                       std::pair(right.dueFrame, right.slot);
            });
        pending_.insert(at, std::move(item));
        return true;
    }

    [[nodiscard]] std::size_t pending() const noexcept { return pending_.size(); }
    [[nodiscard]] bool hasDue(int frame) const noexcept {
        return origin_ >= 0 && frame >= origin_ && frame < origin_ + 24 &&
               !pending_.empty() && pending_.front().dueFrame <= frame;
    }

private:
    std::vector<ScheduledIntent> pending_;
    int origin_{-1};
    int lastFrame_{-1};
};

}  // namespace protodd::cpu
