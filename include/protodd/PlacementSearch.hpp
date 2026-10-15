#pragma once

#include "protodd/Geometry.hpp"

#include <algorithm>
#include <span>

namespace protodd {

// Revisit live placement candidates in bounded windows. A completed sweep or
// a shrinking candidate list restarts at the front; no cached tile is trusted.
class PlacementSearchWindow {
public:
    explicit PlacementSearchWindow(int offset, int budget = 256) noexcept
        : offset_(std::max(0, offset)), budget_(std::max(1, budget)) {}
    [[nodiscard]] bool visit(const bool workAvailable = true) noexcept {
        const auto index = visited_++;
        if (index < offset_ || deferred_ >= 0) return false;
        if (!workAvailable || index - offset_ >= budget_) {
            deferred_ = index;
            return false;
        }
        return true;
    }
    [[nodiscard]] int nextOffset() const noexcept {
        return deferred_ >= 0 ? deferred_ : 0;
    }
    [[nodiscard]] bool deferred() const noexcept { return deferred_ >= 0; }
private:
    int offset_{};
    int budget_{};
    int visited_{};
    int deferred_{-1};
};

// Validation may use the search entry captured by its caller. Retire it only
// after the last validation returns; a deferred route keeps the entry alive.
// Even emergency fallbacks must pass occupancy, route and production-exit checks.
enum class PlacementFallbackStatus { rejected, accepted, deferred };

struct PlacementFallbackResult {
    PlacementFallbackStatus status{PlacementFallbackStatus::rejected};
    Position tile{-1, -1};
};

template <typename Validate, typename Retire>
[[nodiscard]] PlacementFallbackResult finishPlacementFallbacks(
    const std::span<const Position> candidates, Validate&& validate, Retire&& retire) {
    for (const auto tile : candidates) {
        if (!tile.valid()) continue;
        const auto status = validate(tile);
        if (status == PlacementFallbackStatus::deferred) return {status, {-1, -1}};
        if (status == PlacementFallbackStatus::accepted) {
            retire();
            return {status, tile};
        }
    }
    retire();
    return {};
}

}  // namespace protodd
