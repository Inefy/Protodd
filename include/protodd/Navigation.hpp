#pragma once

#include "protodd/Geometry.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <limits>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace protodd {

// Lookahead is measured in32-pixel steps independently of walk-grid resolution.
// A proved prefix may turn corners before reaching its command waypoint.
[[nodiscard]] constexpr std::size_t navigationWaypointIndex(
    const std::size_t pointCount, const int cellSize, const int lookaheadCells) noexcept {
    if (pointCount <= 1) return 0;
    const auto scale = std::uint64_t{1} +
        static_cast<std::uint64_t>(31 / std::max(1, cellSize));
    const auto lookahead = static_cast<std::uint64_t>(std::max(1, lookaheadCells)) * scale;
    return static_cast<std::size_t>(std::min<std::uint64_t>(lookahead, pointCount - 1));
}

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
    // Nodes popped by synchronous path/candidate searches on this thread. Cache hits
    // and input rejection add no work; incremental searches report separately.
    std::uint64_t totalExpansions{};
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

[[nodiscard]] constexpr std::string_view navigationStatusName(
    const NavigationStatus status) noexcept {
    switch (status) {
        case NavigationStatus::reached: return "reached";
        case NavigationStatus::partial: return "partial";
        case NavigationStatus::budgetExhausted: return "budget-exhausted";
        case NavigationStatus::unreachable: return "unreachable";
        case NavigationStatus::invalidInput: return "invalid-input";
    }
    return "invalid-input";
}

struct NavigationCandidate {
    Position position;
    double penalty{};
};

struct NavigationCandidateSelection {
    Position target{-1, -1};
    double score{std::numeric_limits<double>::infinity()};
    NavigationStatus status{NavigationStatus::invalidInput};
    bool searchBudgetExhausted{};
    int expandedNodes{};
};

struct NavigationPathResult {
    NavigationStatus status{NavigationStatus::invalidInput};
    std::vector<Position> points;
    // A partial route may also be the closest safe prefix after a complete
    // component search. Only this flag (or budgetExhausted status) says the
    // search ran out of work before settling reachability.
    bool searchBudgetExhausted{};

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

struct DefensivePositionScan {
    std::size_t nextVertex{3};
    double bestScore{std::numeric_limits<double>::infinity()};
    DefensivePosition best{};
};

// A compact, deterministic terrain grid used by the portable core. The BWAPI
// adapter samples Brood War walk tiles once at game start; no map names or
// tournament-map templates are required.
class NavigationGrid {
public:
    // Search scratch is capped at one million walk cells (36 MiB, including candidate scores) per caller
    // thread; oversized maps are rejected before the reusable workspace grows.
    static constexpr std::size_t maximumSearchWorkspaceBytes =
        36U * 1024U * 1024U;
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
    // Evaluate up to 64 body-clear goals in one bounded search tree. A valid
    // selection is fully reachable; budget exhaustion may leave a better goal
    // unresolved, but never grants reachability to an unproved candidate.
    [[nodiscard]] NavigationCandidateSelection bestReachableCandidate(
        Position from, std::span<const NavigationCandidate> candidates,
        int maximumExpansions = 4096, MovementFootprint footprint = {}) const;
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

    // A temporary obstacle probe preserves exact terrain and collision geometry
    // without duplicating the source grid's historical change journal. Older
    // route revisions are conservatively invalidated on the copy.
    [[nodiscard]] NavigationGrid copyForTemporaryObstacles() const;
    // Temporary bodies choose an unused key so they cannot replace a planned
    // building or another existing obstacle with the same actor-derived key.
    bool addTemporaryObstacle(NavigationObstacleBounds bounds);

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
    // Unlike the per-grid cell journal revision, this identifies exact dynamic
    // geometry even when two copies evolve through the same revision count.
    [[nodiscard]] std::uint64_t collisionIdentity() const noexcept {
        return collisionIdentity_;
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
    // Scan a bounded number of vertices from an already completed route.
    // Each vertex examines at most two384-pixel cross sections.
    void scanDefensivePosition(Position home, std::span<const Position> route,
                               DefensivePositionScan& scan, int maximumVertices) const;
    [[nodiscard]] int height() const noexcept { return height_; }
    [[nodiscard]] int cellSize() const noexcept { return cellSize_; }

private:
    friend class NavigationSearch;
    friend class NavigationObstacleOverlay;
    int width_{};
    int height_{};
    int cellSize_{32};
    std::uint64_t terrainIdentity_{};
    std::uint64_t collisionIdentity_{};
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
        Position position, MovementFootprint footprint,
        bool includeDynamicObstacles = true) const noexcept;
    [[nodiscard]] std::vector<int> obstacleCells(
        NavigationObstacleBounds bounds) const;
    void applyObstacleCells(const std::vector<int>& cells, bool add);
    [[nodiscard]] Position cellCenter(int index) const noexcept;
};

// Placement probes temporarily add collision geometry on the caller's grid.
// If nothing else changed during the probe, removing it restores the exact
// original collision identity so unrelated retained searches can continue.
// An occupied obstacle ID is rejected instead of replacing another owner.
class NavigationObstacleOverlay {
public:
    NavigationObstacleOverlay(NavigationGrid& grid, std::uint64_t id,
                              NavigationObstacleBounds bounds);
    ~NavigationObstacleOverlay();
    NavigationObstacleOverlay(const NavigationObstacleOverlay&) = delete;
    NavigationObstacleOverlay& operator=(const NavigationObstacleOverlay&) = delete;
    [[nodiscard]] bool added() const noexcept { return added_; }
private:
    NavigationGrid& grid_;
    std::uint64_t id_{}, originalIdentity_{}, overlayIdentity_{};
    NavigationObstacleBounds bounds_;
    bool added_{};
};

struct NavigationSearchDiagnostics {
    std::size_t workspaceBytes{};
    std::size_t outputBytes{};
    std::size_t stepWork{};
    std::uint64_t totalExpansions{};
    std::uint64_t starts{};
};

// An owned exact-route search, independent of ordinary findPath scratch.
// Each work unit checks an origin cell, expands one cell, reconstructs one
// vertex or materializes one vertex. Fixed-size endpoint checks and bounded
// workspace/output allocations are outside that count. Pending
// calls return no usable prefix. Repeating the same query continues its
// frontier; changing an endpoint, footprint, terrain or exact collision state
// discards it. Owners must retain a stable query while its actor moves, then
// validate the current actor's connection and current threats before dispatch.
class NavigationSearch {
public:
    static constexpr std::size_t maximumOutputBytes =
        (1'048'576U + 2U) * sizeof(Position);
    NavigationSearch();
    ~NavigationSearch();
    NavigationSearch(NavigationSearch&&) noexcept;
    NavigationSearch& operator=(NavigationSearch&&) noexcept;
    NavigationSearch(const NavigationSearch&) = delete;
    NavigationSearch& operator=(const NavigationSearch&) = delete;

    // The result reference remains valid until the next advance/reset/move.
    [[nodiscard]] const NavigationPathResult& advance(
        const NavigationGrid& navigation, Position from, Position to,
        int maximumWork = 2048, MovementFootprint footprint = {});
    void reset() noexcept;
    [[nodiscard]] NavigationSearchDiagnostics diagnostics() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct NavigationWorkDiagnostics {
    std::size_t frameWork{};
    std::size_t queued{};
    std::size_t cachedRouteBytes{};
    std::uint64_t completed{};
    std::uint64_t invalidated{};
    std::uint64_t searchStarts{}, geometryRestarts{}, cacheEvictions{}, expiredJobs{};
    std::uint64_t geometryInvalidations{}, blockedOrigins{}, failedCurrentJoins{}, admissionDeferred{};
    std::uint64_t activeJob{}, activeOwner{}, activeExpansions{};
    std::int64_t activeAgeFrames{};
    Position activeFrom{-1, -1}, activeTo{-1, -1};
};

enum class NavigationOriginPolicy : std::uint8_t {
    movingActor,
    fixed,
};

// Construction callers share one retained frontier and one frame allowance.
// A FIFO queue stores query metadata, never a workspace per actor. Current
// actor positions must connect to a settled route before it is returned.
class NavigationWorkQueue {
public:
    NavigationWorkQueue();
    ~NavigationWorkQueue();
    NavigationWorkQueue(const NavigationWorkQueue&) = delete;
    NavigationWorkQueue& operator=(const NavigationWorkQueue&) = delete;
    void reset() noexcept;
    // Remove only this caller's proofs/frontier without refreshing frame work.
    void forgetOwner(std::uint64_t owner) noexcept;
    void beginFrame(const NavigationGrid& navigation, std::int64_t frame,
                    int maximumWork = 2048);
    // Accepted construction leases get the next turn after the active query
    // settles; its unfinished frontier is never discarded to change priority.
    void prioritizeOwners(std::span<const std::uint64_t> owners);
    // Selected prospective builders follow accepted leases, then ordinary
    // FIFO requests. An active frontier always retains its current turn.
    void prioritizeInspectionOwners(std::span<const std::uint64_t> owners);
    [[nodiscard]] NavigationPathResult query(
        std::uint64_t owner, const NavigationGrid& navigation,
        Position from, Position to, MovementFootprint footprint = {},
        NavigationOriginPolicy originPolicy = NavigationOriginPolicy::movingActor);
    // Candidate detour geometry uses the retained baseline origin. Actual
    // execution still requires a proved route from the current actor position.
    [[nodiscard]] Position retainedOrigin(std::uint64_t owner, Position to,
                                         MovementFootprint footprint = {}) const noexcept;
    [[nodiscard]] NavigationWorkDiagnostics diagnostics() const noexcept;
    [[nodiscard]] bool newSearchAdmission() const noexcept { return newSearchAdmission_; }
    void setNewSearchAdmission(bool allowed) noexcept { newSearchAdmission_ = allowed; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool newSearchAdmission_{true};
    void advance(const NavigationGrid& navigation);
};


// Limit one inspection pass without changing the queue's normal FIFO callers.
class NavigationSearchAdmissionScope {
public:
    explicit NavigationSearchAdmissionScope(NavigationWorkQueue& queue) noexcept
        : queue_(queue), previous_(queue.newSearchAdmission()) {}
    ~NavigationSearchAdmissionScope() { queue_.setNewSearchAdmission(previous_); }
    NavigationSearchAdmissionScope(const NavigationSearchAdmissionScope&) = delete;
    NavigationSearchAdmissionScope& operator=(const NavigationSearchAdmissionScope&) = delete;
    void restrictNewSearches() noexcept { queue_.setNewSearchAdmission(false); }
private:
    NavigationWorkQueue& queue_;
    bool previous_;
};

}  // namespace protodd
