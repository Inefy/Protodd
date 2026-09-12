#pragma once

#include <algorithm>

namespace protodd {

// Revisit live placement candidates in bounded windows. A completed sweep or
// a shrinking candidate list restarts at the front; no cached tile is trusted.
class PlacementSearchWindow {
public:
    explicit PlacementSearchWindow(int offset, int budget = 256) noexcept
        : offset_(std::max(0, offset)), budget_(std::max(1, budget)) {}
    [[nodiscard]] bool visit() noexcept {
        const auto index = visited_++;
        return index >= offset_ && index - offset_ < budget_;
    }
    [[nodiscard]] int nextOffset() const noexcept {
        return visited_ > offset_ && visited_ - offset_ > budget_ ? offset_ + budget_ : 0;
    }
private:
    int offset_{};
    int budget_{};
    int visited_{};
};

}  // namespace protodd
