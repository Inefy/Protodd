#pragma once

#include "protodd/Geometry.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <unordered_map>
#include <vector>

namespace protodd {

struct MovementFootprint {
    int left{};
    int right{};
    int up{};
    int down{};
    friend bool operator==(const MovementFootprint&, const MovementFootprint&) = default;
};

struct NavigationDiagnostics {
    std::size_t searchWorkspaceBytes{};
    std::size_t cachedRouteBytes{};
    std::uint64_t searches{};
    std::uint64_t routeCacheHits{};
    std::uint64_t workspaceResizes{};
};

struct NavigationObstacleBounds {
    int left{};
    int top{};
    int right{};
    int bottom{};
    [[nodiscard]] bool valid() const noexcept {
        return right >= left && bottom >= top;
    }
    friend bool operator==(const NavigationObstacleBounds&,
                           const NavigationObstacleBounds&) = default;
};

// `partial` contains only a safe prefix toward the goal. `budgetExhausted`
// has no usable route and says nothing about global reachability; `unreachable`
// means the bounded search fully exhausted its reachable component.
enum class NavigationStatus : std::uint8_t {
    reached,
    partial,
    budgetExhausted,
    unreachable,
    invalidInput,
};

struct NavigationPathResult {
    NavigationStatus status{NavigationStatus::invalidInput};
    std::vector<Position> points;

    [[nodiscard]] bool hasUsablePath() const noexcept {
        return status == NavigationStatus::reached || status == NavigationStatus::partial;
    }
    [[nodiscard]] bool reached() const noexcept {
        return status == NavigationStatus::reached;
    }
};

struct NavigationWaypointResult {
    NavigationStatus status{NavigationStatus::invalidInput};
    Position waypoint{-1, -1};

    [[nodiscard]] bool hasUsableWaypoint() const noexcept {
        return waypoint.valid() &&
            (status == NavigationStatus::reached || status == NavigationStatus::partial);
    }
    [[nodiscard]] bool reached() const noexcept {
        return status == NavigationStatus::reached;
    }
};

// A compact, deterministic terrain grid used by the portable core. The BWAPI
// adapter samples Brood War walk tiles once at game start; no map names or
// tournament-map templates are required.
class NavigationGrid {
public:
    // Search scratch is capped at one million walk cells (20 MiB) per caller
    // thread; oversized maps are rejected before the reusable workspace grows.
    static constexpr std::size_t maximumSearchWorkspaceBytes =
        20U * 1024U * 1024U;
    // Exact completed routes use a separate bounded, revision-checked cache.
    static constexpr std::size_t maximumCachedRouteBytes = 128U * 1024U;

    NavigationGrid() = default;
    NavigationGrid(
        int width,
        int height,
        int cellSize,
        std::vector<std::uint8_t> walkable,
        std::vector<std::uint8_t> elevation = {});

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool walkable(
        Position position, MovementFootprint footprint = {}) const noexcept;
    [[nodiscard]] bool lineWalkable(
        Position from, Position to, MovementFootprint footprint = {}) const noexcept;
    [[nodiscard]] Position nearestWalkable(
        Position position, int searchRadius = -1,
        MovementFootprint footprint = {}) const noexcept;
    [[nodiscard]] NavigationPathResult findPath(
        Position from,
        Position to,
        int maximumExpansions = 12000,
        MovementFootprint footprint = {}) const;
    // Reports the calling thread's reusable search workspace and bounded route cache.
    [[nodiscard]] static NavigationDiagnostics diagnosticsForCurrentThread() noexcept;
    [[nodiscard]] NavigationWaypointResult nextWaypoint(
        Position from,
        Position to,
        int lookaheadCells = 7,
        int maximumExpansions = 12000,
        MovementFootprint footprint = {},
        std::vector<Position>* route = nullptr) const;
    [[nodiscard]] NavigationWaypointResult nearestReachable(
        Position from, Position target, int maximumExpansions = 12000,
        MovementFootprint footprint = {}) const;
    // Select the shortest fully reachable goal from a small legal-search set.
    // Partial routes and exhausted budgets do not count as proof of reachability.
    [[nodiscard]] Position nearestReachableTarget(
        Position from, std::span<const Position> targets,
        int maximumExpansions = 12000,
        MovementFootprint footprint = {}) const;

    // Dynamic obstacles are keyed by stable unit or planned-build identity.
    // Bounds are inclusive pixel coordinates, matching BWAPI collision bounds.
    bool updateDynamicObstacle(std::uint64_t id, NavigationObstacleBounds bounds);
    bool removeDynamicObstacle(std::uint64_t id);
    [[nodiscard]] std::uint64_t obstacleVersion() const noexcept {
        return obstacleVersion_;
    }
    // Copies preserve the same terrain lifetime; replacing a grid with a
    // newly sampled map changes it even when address and dimensions match.
    [[nodiscard]] std::uint64_t terrainIdentity() const noexcept {
        return terrainIdentity_;
    }
    [[nodiscard]] bool routeAffectedSince(
        std::span<const Position> route,
        Position from,
        Position to,
        MovementFootprint footprint,
        std::uint64_t version,
        int marginPixels = 64) const noexcept;

    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] DefensivePosition defensivePosition(Position home, Position outside) const;
    [[nodiscard]] int height() const noexcept { return height_; }
    [[nodiscard]] int cellSize() const noexcept { return cellSize_; }

private:
    int width_{};
    int height_{};
    int cellSize_{32};
    std::uint64_t terrainIdentity_{};
    std::vector<std::uint8_t> walkable_;
    std::vector<std::uint8_t> elevation_;
    std::vector<std::uint16_t> dynamicBlockCounts_;
    struct DynamicObstacle {
        NavigationObstacleBounds bounds;
        std::vector<int> cells;
    };
    struct ChangedCell {
        int x{};
        int y{};
        std::uint64_t version{};
    };
    std::unordered_map<std::uint64_t, DynamicObstacle> dynamicObstacles_;
    std::deque<ChangedCell> changedCells_;
    std::uint64_t obstacleVersion_{};
    std::uint64_t discardedChangeVersion_{};

    [[nodiscard]] int index(int x, int y) const noexcept;
    [[nodiscard]] bool cellWalkable(int x, int y) const noexcept;
    [[nodiscard]] bool footprintWalkable(
        Position position, MovementFootprint footprint) const noexcept;
    [[nodiscard]] std::vector<int> obstacleCells(
        NavigationObstacleBounds bounds) const;
    void applyObstacleCells(const std::vector<int>& cells, bool add);
    [[nodiscard]] Position cellCenter(int index) const noexcept;
};

}  // namespace protodd
