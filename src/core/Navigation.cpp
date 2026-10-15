#include "protodd/Navigation.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <queue>
#include <tuple>
#include <utility>

namespace protodd {
namespace {

constexpr int unreachable = std::numeric_limits<int>::max() / 4;
constexpr std::size_t maximumNavigationCells = 1'048'576U;
constexpr std::size_t maximumCachedRoutePoints =
    NavigationGrid::maximumCachedRouteBytes / sizeof(Position);
constexpr std::size_t maximumCachedRoutes = 16U;
std::atomic<std::uint64_t> nextTerrainIdentity{1U};
std::atomic<std::uint64_t> nextCollisionIdentity{1U};

int heuristic(const int x, const int y, const int goalX, const int goalY) noexcept {
    const auto dx = std::abs(x - goalX);
    const auto dy = std::abs(y - goalY);
    return 10 * std::max(dx, dy) + 4 * std::min(dx, dy);
}

struct CachedRoute {
    const NavigationGrid* grid{};
    int width{};
    int height{};
    int cellSize{};
    std::uint64_t obstacleVersion{};
    std::uint64_t terrainIdentity{};
    Position from{-1, -1};
    Position to{-1, -1};
    MovementFootprint footprint{};
    std::unique_ptr<Position[]> points;
    std::size_t pointCount{};
    std::uint64_t lastUsed{};

    void clear() noexcept {
        grid = nullptr;
        width = height = cellSize = 0;
        obstacleVersion = 0;
        terrainIdentity = 0;
        from = to = {-1, -1};
        footprint = {};
        points.reset();
        pointCount = 0;
        lastUsed = 0;
    }
};

// One bounded workspace per calling thread avoids races when offline callers
// query a shared terrain grid concurrently. The indexed heap holds at most one
// entry per walk cell, so every search allocation has a fixed upper bound.
struct SearchWorkspace {
    std::unique_ptr<int[]> costs;
    std::unique_ptr<int[]> parents;
    std::unique_ptr<int[]> heapPositions;
    std::unique_ptr<int[]> heapNodes;
    std::unique_ptr<std::uint32_t[]> generations;
    std::unique_ptr<double[]> candidateCosts;
    std::unique_ptr<double[]> candidatePriorities;
    std::size_t cells{};
    std::size_t heapSize{};
    std::uint32_t generation{};
    std::array<CachedRoute, maximumCachedRoutes> routes;
    std::size_t cachedPoints{};
    std::uint64_t lruClock{};
    std::uint64_t searches{};
    std::uint64_t routeCacheHits{};
    std::uint64_t workspaceResizes{};
    std::uint64_t totalExpansions{};

    [[nodiscard]] bool ensureCells(const std::size_t requested) {
        if (requested > maximumNavigationCells) return false;
        if (requested == cells) return true;
        costs.reset();
        parents.reset();
        heapPositions.reset();
        heapNodes.reset();
        generations.reset();
        candidateCosts.reset();
        candidatePriorities.reset();
        cells = 0;
        heapSize = 0;
        generation = 0;
        try {
            costs.reset(new int[requested]);
            parents.reset(new int[requested]);
            heapPositions.reset(new int[requested]);
            heapNodes.reset(new int[requested]);
            generations.reset(new std::uint32_t[requested]{});
        } catch (...) {
            costs.reset();
            parents.reset();
            heapPositions.reset();
            heapNodes.reset();
            generations.reset();
            throw;
        }
        cells = requested;
        ++workspaceResizes;
        return true;
    }

    void beginSearch() noexcept {
        heapSize = 0;
        if (++generation == 0U) {
            std::fill_n(generations.get(), cells, 0U);
            generation = 1U;
        }
        ++searches;
    }

    void discover(const int node) noexcept {
        const auto slot = static_cast<std::size_t>(node);
        if (generations[slot] == generation) return;
        generations[slot] = generation;
        costs[slot] = unreachable;
        parents[slot] = -1;
        heapPositions[slot] = -1;
    }

    [[nodiscard]] bool precedes(
        const int left, const int right, const int width,
        const int goalX, const int goalY) const noexcept {
        const auto leftX = left % width;
        const auto leftY = left / width;
        const auto rightX = right % width;
        const auto rightY = right / width;
        const auto leftPriority = costs[static_cast<std::size_t>(left)] +
            heuristic(leftX, leftY, goalX, goalY);
        const auto rightPriority = costs[static_cast<std::size_t>(right)] +
            heuristic(rightX, rightY, goalX, goalY);
        return leftPriority < rightPriority ||
            (leftPriority == rightPriority && left < right);
    }

    void siftUp(int position, const int width, const int goalX, const int goalY) noexcept {
        while (position > 0) {
            const auto parent = (position - 1) / 2;
            const auto node = heapNodes[static_cast<std::size_t>(position)];
            const auto parentNode = heapNodes[static_cast<std::size_t>(parent)];
            if (!precedes(node, parentNode, width, goalX, goalY)) break;
            std::swap(heapNodes[static_cast<std::size_t>(position)],
                      heapNodes[static_cast<std::size_t>(parent)]);
            heapPositions[static_cast<std::size_t>(node)] = parent;
            heapPositions[static_cast<std::size_t>(parentNode)] = position;
            position = parent;
        }
    }

    void siftDown(int position, const int width, const int goalX, const int goalY) noexcept {
        for (;;) {
            const auto left = position * 2 + 1;
            if (static_cast<std::size_t>(left) >= heapSize) break;
            const auto right = left + 1;
            auto best = left;
            if (static_cast<std::size_t>(right) < heapSize &&
                precedes(heapNodes[static_cast<std::size_t>(right)],
                         heapNodes[static_cast<std::size_t>(left)], width, goalX, goalY)) {
                best = right;
            }
            const auto node = heapNodes[static_cast<std::size_t>(position)];
            const auto child = heapNodes[static_cast<std::size_t>(best)];
            if (!precedes(child, node, width, goalX, goalY)) break;
            std::swap(heapNodes[static_cast<std::size_t>(position)],
                      heapNodes[static_cast<std::size_t>(best)]);
            heapPositions[static_cast<std::size_t>(node)] = best;
            heapPositions[static_cast<std::size_t>(child)] = position;
            position = best;
        }
    }

    void pushOrDecrease(
        const int node, const int width, const int goalX, const int goalY) noexcept {
        auto& position = heapPositions[static_cast<std::size_t>(node)];
        if (position == -2) return;
        if (position < 0) {
            position = static_cast<int>(heapSize);
            heapNodes[heapSize++] = node;
        }
        siftUp(position, width, goalX, goalY);
    }

    [[nodiscard]] int popMinimum(
        const int width, const int goalX, const int goalY) noexcept {
        const auto result = heapNodes[0];
        heapPositions[static_cast<std::size_t>(result)] = -2;
        --heapSize;
        if (heapSize > 0U) {
            const auto replacement = heapNodes[heapSize];
            heapNodes[0] = replacement;
            heapPositions[static_cast<std::size_t>(replacement)] = 0;
            siftDown(0, width, goalX, goalY);
        }
        return result;
    }

    [[nodiscard]] bool ensureCandidateCosts() {
        if (candidateCosts && candidatePriorities) return true;
        // Allocate at most twice per map size, never once per discovered vertex.
        auto costsBuffer = std::unique_ptr<double[]>{new double[cells]};
        auto prioritiesBuffer = std::unique_ptr<double[]>{new double[cells]};
        candidateCosts = std::move(costsBuffer);
        candidatePriorities = std::move(prioritiesBuffer);
        return true;
    }

    [[nodiscard]] bool candidatePrecedes(const int left, const int right) const noexcept {
        const auto a = candidatePriorities[static_cast<std::size_t>(left)];
        const auto b = candidatePriorities[static_cast<std::size_t>(right)];
        return a < b || (a == b && left < right);
    }

    void candidatePushOrDecrease(const int node) noexcept {
        auto& position = heapPositions[static_cast<std::size_t>(node)];
        if (position == -2) return;
        if (position < 0) {
            position = static_cast<int>(heapSize);
            heapNodes[heapSize++] = node;
        }
        while (position > 0) {
            const auto parent = (position - 1) / 2;
            const auto other = heapNodes[static_cast<std::size_t>(parent)];
            if (!candidatePrecedes(node, other)) break;
            heapNodes[static_cast<std::size_t>(position)] = other;
            heapPositions[static_cast<std::size_t>(other)] = position;
            position = parent;
        }
        heapNodes[static_cast<std::size_t>(position)] = node;
    }

    [[nodiscard]] int candidatePopMinimum() noexcept {
        const auto result = heapNodes[0];
        heapPositions[static_cast<std::size_t>(result)] = -2;
        if (--heapSize == 0U) return result;
        const auto replacement = heapNodes[heapSize];
        int position = 0;
        for (;;) {
            const auto left = position * 2 + 1;
            if (static_cast<std::size_t>(left) >= heapSize) break;
            const auto right = left + 1;
            auto best = left;
            if (static_cast<std::size_t>(right) < heapSize && candidatePrecedes(
                heapNodes[static_cast<std::size_t>(right)], heapNodes[static_cast<std::size_t>(left)])) best = right;
            const auto child = heapNodes[static_cast<std::size_t>(best)];
            if (!candidatePrecedes(child, replacement)) break;
            heapNodes[static_cast<std::size_t>(position)] = child;
            heapPositions[static_cast<std::size_t>(child)] = position;
            position = best;
        }
        heapNodes[static_cast<std::size_t>(position)] = replacement;
        heapPositions[static_cast<std::size_t>(replacement)] = position;
        return result;
    }

    [[nodiscard]] bool cachedRoute(
        const NavigationGrid& grid, const Position from, const Position to,
        const MovementFootprint footprint, std::vector<Position>& result) {
        for (auto& route : routes) {
            if (route.grid != &grid || route.width != grid.width() ||
                route.height != grid.height() || route.cellSize != grid.cellSize() ||
                route.terrainIdentity != grid.terrainIdentity() ||
                route.from != from || route.to != to || route.footprint != footprint) continue;

            const auto points = std::span<const Position>{route.points.get(), route.pointCount};
            // Revision checks identify whether the bounded journal overlaps
            // this route; validate every leg as a final guard against copied
            // or reassigned grid objects that reused the same address.
            const auto routeChanged = grid.routeAffectedSince(
                points, from, to, footprint, route.obstacleVersion);
            const auto obstacleRevisionChanged =
                route.obstacleVersion != grid.obstacleVersion();
            auto valid = route.pointCount >= 2U && route.points[0] == from &&
                route.points[route.pointCount - 1U] == to;
            for (std::size_t i = 1; valid && i < route.pointCount; ++i)
                valid = grid.lineWalkable(route.points[i - 1U], route.points[i], footprint);
            if (!valid) {
                cachedPoints -= route.pointCount;
                route.clear();
                continue;
            }
            if (routeChanged || obstacleRevisionChanged)
                route.obstacleVersion = grid.obstacleVersion();
            route.lastUsed = ++lruClock;
            result.assign(route.points.get(), route.points.get() + route.pointCount);
            ++routeCacheHits;
            return true;
        }
        return false;
    }

    void rememberRoute(
        const NavigationGrid& grid, const Position from, const Position to,
        const MovementFootprint footprint, const std::vector<Position>& points) {
        if (points.size() < 2U || points.size() > maximumCachedRoutePoints) return;
        for (auto& route : routes) {
            if (route.grid == &grid && route.from == from && route.to == to &&
                route.footprint == footprint) {
                cachedPoints -= route.pointCount;
                route.clear();
            }
        }
        while (cachedPoints + points.size() > maximumCachedRoutePoints ||
               std::ranges::none_of(routes, [](const CachedRoute& route) {
                   return route.grid == nullptr;
               })) {
            auto* oldest = &routes.front();
            for (auto& route : routes) {
                if (route.lastUsed < oldest->lastUsed) oldest = &route;
            }
            cachedPoints -= oldest->pointCount;
            oldest->clear();
        }
        auto slot = std::ranges::find_if(routes, [](const CachedRoute& route) {
            return route.grid == nullptr;
        });
        auto storedPoints = std::unique_ptr<Position[]>(new Position[points.size()]);
        std::copy(points.begin(), points.end(), storedPoints.get());
        slot->grid = &grid;
        slot->width = grid.width();
        slot->height = grid.height();
        slot->cellSize = grid.cellSize();
        slot->obstacleVersion = grid.obstacleVersion();
        slot->terrainIdentity = grid.terrainIdentity();
        slot->from = from;
        slot->to = to;
        slot->footprint = footprint;
        slot->points = std::move(storedPoints);
        slot->pointCount = points.size();
        slot->lastUsed = ++lruClock;
        cachedPoints += points.size();
    }

    [[nodiscard]] std::size_t workspaceBytes() const noexcept {
        return cells * (sizeof(int) * 4U + sizeof(std::uint32_t)) +
            (candidateCosts ? cells * sizeof(double) : 0U) +
            (candidatePriorities ? cells * sizeof(double) : 0U);
    }
};

SearchWorkspace& searchWorkspace() {
    static thread_local SearchWorkspace workspace;
    return workspace;
}

}  // namespace

struct NavigationSearch::Impl {
    enum class Phase { start, search, reconstruct, materialize, complete };
    SearchWorkspace workspace;
    Phase phase{Phase::complete};
    NavigationPathResult result;
    NavigationPathResult invalid{NavigationStatus::invalidInput, {}};
    std::vector<Position> points;
    Position from{-1, -1}, to{-1, -1};
    MovementFootprint footprint{};
    std::uint64_t terrain{}, collision{};
    std::uint64_t starts{}, expansions{};
    std::size_t stepWork{}, routeNodes{}, routeIndex{};
    int width{}, height{}, cellSize{};
    int start{-1}, goal{-1}, cursor{-1};
    int originX{}, originY{}, radius{}, row{}, column{}, radiusLimit{};
    bool initialized{};
};

NavigationSearch::NavigationSearch() = default;
NavigationSearch::~NavigationSearch() = default;
NavigationSearch::NavigationSearch(NavigationSearch&&) noexcept = default;
NavigationSearch& NavigationSearch::operator=(NavigationSearch&&) noexcept = default;

void NavigationSearch::reset() noexcept { impl_.reset(); }

NavigationSearchDiagnostics NavigationSearch::diagnostics() const noexcept {
    if (!impl_) return {};
    return {impl_->workspace.workspaceBytes(),
            (impl_->points.capacity() + impl_->result.points.capacity()) * sizeof(Position), impl_->stepWork,
            impl_->expansions, impl_->starts};
}

const NavigationPathResult& NavigationSearch::advance(
    const NavigationGrid& grid, const Position from, const Position to,
    const int maximumWork, const MovementFootprint footprint) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    auto& state = *impl_;
    state.stepWork = 0;
    if (grid.empty() || !from.valid() || !to.valid() || maximumWork < 0 ||
        from.x >= grid.width_ * grid.cellSize_ || from.y >= grid.height_ * grid.cellSize_ ||
        to.x >= grid.width_ * grid.cellSize_ || to.y >= grid.height_ * grid.cellSize_)
        return state.invalid;

    if (!state.initialized || state.terrain != grid.terrainIdentity_ ||
        state.collision != grid.collisionIdentity_ || state.from != from || state.to != to ||
        state.footprint != footprint || state.width != grid.width_ ||
        state.height != grid.height_ || state.cellSize != grid.cellSize_) {
        state.initialized = true;
        state.terrain = grid.terrainIdentity_;
        state.collision = grid.collisionIdentity_;
        state.from = from; state.to = to; state.footprint = footprint;
        state.width = grid.width_; state.height = grid.height_; state.cellSize = grid.cellSize_;
        state.phase = Impl::Phase::start;
        state.result = {NavigationStatus::budgetExhausted, {}, true};
        state.points.clear();
        state.routeNodes = state.routeIndex = 0;
        state.expansions = 0;
        state.start = state.cursor = -1;
        state.goal = grid.index(to.x / grid.cellSize_, to.y / grid.cellSize_);
        state.originX = from.x / grid.cellSize_;
        state.originY = from.y / grid.cellSize_;
        state.radius = state.row = state.column = 0;
        state.radiusLimit = std::max(8, (256 + grid.cellSize_ - 1) / grid.cellSize_);
        ++state.starts;
    }
    if (state.phase == Impl::Phase::complete) return state.result;
    if (maximumWork == 0) return state.result;
    const auto fail = [&]() -> const NavigationPathResult& {
        state.phase = Impl::Phase::complete;
        state.result = {NavigationStatus::unreachable, {}};
        state.points.clear();
        return state.result;
    };
    if (!grid.walkable(from, footprint) || !grid.walkable(to, footprint) ||
        !grid.lineWalkable(grid.cellCenter(state.goal), to, footprint)) return fail();

    auto& workspace = state.workspace;
    const auto goalX = to.x / grid.cellSize_;
    const auto goalY = to.y / grid.cellSize_;
    static constexpr std::array directions{
        std::pair{-1, -1}, std::pair{0, -1}, std::pair{1, -1},
        std::pair{-1, 0},                     std::pair{1, 0},
        std::pair{-1, 1},  std::pair{0, 1},  std::pair{1, 1},
    };
    while (state.stepWork < static_cast<std::size_t>(maximumWork)) {
        if (state.phase == Impl::Phase::start) {
            // Match nearestWalkable's deterministic perimeter order, spreading
            // even the exact-origin escape search across the caller's budget.
            const auto x = state.originX + state.column;
            const auto y = state.originY + state.row;
            ++state.stepWork;
            const auto candidate = grid.index(x, y);
            const auto clear = grid.cellWalkable(x, y) &&
                grid.footprintWalkable(grid.cellCenter(candidate), footprint);
            if (std::abs(state.row) == state.radius) ++state.column;
            else state.column = state.column == -state.radius ? state.radius : state.radius + 1;
            if (state.column > state.radius) {
                ++state.row; state.column = -state.radius;
            }
            if (state.row > state.radius) {
                ++state.radius; state.row = state.column = -state.radius;
            }
            if (!clear) {
                if (state.radius > state.radiusLimit) return fail();
                continue;
            }
            if (!grid.lineWalkable(from, grid.cellCenter(candidate), footprint)) return fail();
            state.start = candidate;
            const auto cells = static_cast<std::size_t>(grid.width_) *
                static_cast<std::size_t>(grid.height_);
            if (!workspace.ensureCells(cells)) return state.result;
            workspace.beginSearch();
            workspace.discover(candidate);
            workspace.costs[static_cast<std::size_t>(candidate)] = 0;
            workspace.pushOrDecrease(candidate, grid.width_, goalX, goalY);
            state.phase = Impl::Phase::search;
        } else if (state.phase == Impl::Phase::search) {
            if (workspace.heapSize == 0) return fail();
            const auto current = workspace.popMinimum(grid.width_, goalX, goalY);
            ++state.stepWork; ++state.expansions;
            if (current == state.goal) {
                state.cursor = current;
                state.phase = Impl::Phase::reconstruct;
                continue;
            }
            const auto x = current % grid.width_, y = current / grid.width_;
            for (const auto [dx, dy] : directions) {
                const auto nextX = x + dx, nextY = y + dy;
                if (!grid.cellWalkable(nextX, nextY) ||
                    !grid.footprintWalkable(grid.cellCenter(grid.index(nextX, nextY)), footprint)) continue;
                if (dx != 0 && dy != 0 &&
                    (!grid.cellWalkable(x + dx, y) ||
                     !grid.footprintWalkable(grid.cellCenter(grid.index(x + dx, y)), footprint) ||
                     !grid.cellWalkable(x, y + dy) ||
                     !grid.footprintWalkable(grid.cellCenter(grid.index(x, y + dy)), footprint))) continue;
                const auto next = grid.index(nextX, nextY);
                const auto slot = static_cast<std::size_t>(next);
                workspace.discover(next);
                if (workspace.heapPositions[slot] == -2) continue;
                const auto cost = workspace.costs[static_cast<std::size_t>(current)] +
                    (dx != 0 && dy != 0 ? 14 : 10);
                if (cost >= workspace.costs[slot]) continue;
                workspace.costs[slot] = cost;
                workspace.parents[slot] = current;
                workspace.pushOrDecrease(next, grid.width_, goalX, goalY);
            }
        } else if (state.phase == Impl::Phase::reconstruct) {
            ++state.stepWork;
            // The frontier is no longer needed once its goal is settled.
            // Reuse that bounded array for the reverse parent chain; avoid
            // vector growth copying a long prefix in a single work unit.
            if (state.routeNodes >= workspace.cells) return fail();
            workspace.heapNodes[state.routeNodes++] = state.cursor;
            if (state.cursor == state.start) {
                state.points.reserve(state.routeNodes + 2U);
                state.points.push_back(from);
                state.routeIndex = state.routeNodes;
                state.phase = Impl::Phase::materialize;
                continue;
            }
            state.cursor = workspace.parents[static_cast<std::size_t>(state.cursor)];
            if (state.cursor < 0) return fail();
        } else if (state.phase == Impl::Phase::materialize) {
            if (state.routeIndex > 0) {
                const auto point = grid.cellCenter(workspace.heapNodes[--state.routeIndex]);
                if (point != state.points.back()) state.points.push_back(point);
                ++state.stepWork;
                if (state.routeIndex > 0) continue;
            }
            if (state.points.back() != to) state.points.push_back(to);
            state.result = {NavigationStatus::reached, std::move(state.points)};
            state.phase = Impl::Phase::complete;
            return state.result;
        }
    }
    return state.result;
}

struct NavigationWorkQueue::Impl {
    struct Job {
        std::uint64_t id{}, owner{}, terrain{}, collision{};
        Position from{-1,-1}, to{-1,-1};
        MovementFootprint footprint{};
        std::int64_t requested{};
        NavigationOriginPolicy originPolicy{NavigationOriginPolicy::movingActor};
        bool complete{};
        NavigationPathResult result{NavigationStatus::budgetExhausted, {}, true};
        std::size_t joinedIndex{};
        std::int64_t created{};
    };
    static constexpr std::size_t maximumJobs = 256;
    static constexpr std::size_t maximumRouteBytes = NavigationGrid::maximumCachedRouteBytes;
    std::deque<Job> jobs;
    std::vector<std::uint64_t> priorityOwners;
    std::vector<std::uint64_t> inspectionOwners;
    NavigationSearch search;
    std::uint64_t nextId{}, active{}, completed{}, invalidated{};
    std::uint64_t searchStarts{}, geometryRestarts{}, cacheEvictions{}, expiredJobs{};
    std::uint64_t geometryInvalidations{}, blockedOrigins{}, failedCurrentJoins{}, admissionDeferred{};
    std::int64_t frame{-1};
    std::size_t frameWork{};
    int allowance{};

    [[nodiscard]] std::size_t routeBytes() const noexcept {
        std::size_t bytes{};
        for (const auto& job : jobs) bytes += job.result.points.capacity() * sizeof(Position);
        return bytes;
    }
};

NavigationWorkQueue::NavigationWorkQueue() = default;
NavigationWorkQueue::~NavigationWorkQueue() = default;
void NavigationWorkQueue::reset() noexcept { impl_.reset(); newSearchAdmission_ = true; }

void NavigationWorkQueue::forgetOwner(const std::uint64_t owner) noexcept {
    if (!impl_) return;
    auto& state = *impl_;
    std::erase_if(state.jobs, [&state, owner](const Impl::Job& job) {
        if (job.owner != owner) return false;
        if (state.active == job.id) {
            state.search.reset();
            state.active = 0;
        }
        ++state.invalidated;
        return true;
    });
    std::erase(state.priorityOwners, owner);
    std::erase(state.inspectionOwners, owner);
}

void NavigationWorkQueue::prioritizeOwners(const std::span<const std::uint64_t> owners) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    const auto selected = owners.first(std::min(owners.size(), Impl::maximumJobs));
    impl_->priorityOwners.assign(selected.begin(), selected.end());
}

void NavigationWorkQueue::prioritizeInspectionOwners(
    const std::span<const std::uint64_t> owners) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    const auto selected = owners.first(std::min(owners.size(), Impl::maximumJobs));
    impl_->inspectionOwners.assign(selected.begin(), selected.end());
}

NavigationWorkDiagnostics NavigationWorkQueue::diagnostics() const noexcept {
    if (!impl_) return {};
    NavigationWorkDiagnostics result{impl_->frameWork,
        static_cast<std::size_t>(std::ranges::count_if(impl_->jobs,
            [](const Impl::Job& job) { return !job.complete; })),
        impl_->routeBytes(), impl_->completed, impl_->invalidated};
    result.searchStarts = impl_->searchStarts;
    result.geometryRestarts = impl_->geometryRestarts;
    result.cacheEvictions = impl_->cacheEvictions;
    result.expiredJobs = impl_->expiredJobs;
    result.geometryInvalidations = impl_->geometryInvalidations;
    result.blockedOrigins = impl_->blockedOrigins;
    result.failedCurrentJoins = impl_->failedCurrentJoins;
    result.admissionDeferred = impl_->admissionDeferred;
    const auto active = std::ranges::find_if(impl_->jobs, [this](const Impl::Job& job) {
        return !job.complete && job.id == impl_->active;
    });
    if (active != impl_->jobs.end()) {
        result.activeJob = active->id;
        result.activeOwner = active->owner;
        result.activeExpansions = impl_->search.diagnostics().totalExpansions;
        result.activeAgeFrames = std::max<std::int64_t>(0, impl_->frame - active->created);
        result.activeFrom = active->from;
        result.activeTo = active->to;
    }
    return result;
}

void NavigationWorkQueue::beginFrame(
    const NavigationGrid& navigation, const std::int64_t frame, const int maximumWork) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    auto& state = *impl_;
    if (state.frame != frame) {
        state.frame = frame;
        state.frameWork = 0;
        state.allowance = std::max(0, maximumWork);
        std::erase_if(state.jobs, [&state](const Impl::Job& job) {
            const auto expired = state.frame - job.requested > 240;
            if (expired) ++state.expiredJobs;
            return expired;
        });
    }
    advance(navigation);
}

void NavigationWorkQueue::advance(const NavigationGrid& navigation) {
    auto& state = *impl_;
    const auto searchAllowance = state.allowance - std::min(128, state.allowance / 4);
    while (state.frameWork < static_cast<std::size_t>(searchAllowance)) {
        auto job = std::ranges::find_if(state.jobs, [&state](const Impl::Job& candidate) {
            return !candidate.complete && candidate.id == state.active;
        });
        if (job == state.jobs.end())
            job = std::ranges::find_if(state.jobs, [&state](const Impl::Job& candidate) {
                return !candidate.complete &&
                    std::ranges::find(state.priorityOwners,candidate.owner) != state.priorityOwners.end();
            });
        if (job == state.jobs.end())
            job = std::ranges::find_if(state.jobs, [&state](const Impl::Job& candidate) {
                return !candidate.complete &&
                    std::ranges::find(state.inspectionOwners,candidate.owner) != state.inspectionOwners.end();
            });
        if (job == state.jobs.end())
            job = std::ranges::find_if(state.jobs,
                [](const Impl::Job& candidate) { return !candidate.complete; });
        if (job == state.jobs.end()) return;
        const auto continuingActive = state.active == job->id;
        if (state.active != job->id) {
            state.active = job->id;
        }
        const auto startsBefore = state.search.diagnostics().starts;
        const auto& result = state.search.advance(navigation, job->from, job->to,
            searchAllowance - static_cast<int>(state.frameWork), job->footprint);
        const auto searchDiagnostics = state.search.diagnostics();
        state.searchStarts += searchDiagnostics.starts - startsBefore;
        if (continuingActive && startsBefore > 0 && searchDiagnostics.starts > startsBefore)
            ++state.geometryRestarts;
        state.frameWork += searchDiagnostics.stepWork;
        if (result.searchBudgetExhausted) return;
        job->result = result;
        job->complete = true;
        job->terrain = navigation.terrainIdentity();
        job->collision = navigation.collisionIdentity();
        ++state.completed;
        // Keep the active search's allocation reusable for the next query.
        // Completed route metadata is separate and LRU-bounded.
        while (state.routeBytes() > Impl::maximumRouteBytes) {
            auto oldest = state.jobs.end();
            for (auto it = state.jobs.begin(); it != state.jobs.end(); ++it) {
                if (!it->complete || it->result.points.empty() || it->id == job->id) continue;
                if (oldest == state.jobs.end() || it->requested < oldest->requested) oldest = it;
            }
            if (oldest == state.jobs.end()) break;
            const auto completedId = job->id;
            state.jobs.erase(oldest);
            ++state.cacheEvictions;
            job = std::ranges::find_if(state.jobs, [completedId](const Impl::Job& candidate) {
                return candidate.id == completedId;
            });
        }
    }
}

NavigationPathResult NavigationWorkQueue::query(
    const std::uint64_t owner, const NavigationGrid& navigation,
    const Position from, const Position to, const MovementFootprint footprint,
    const NavigationOriginPolicy originPolicy) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    auto& state = *impl_;
    const auto pending = NavigationPathResult{NavigationStatus::budgetExhausted, {}, true};
    auto found = std::ranges::find_if(state.jobs, [owner,from,to,footprint,originPolicy](const Impl::Job& job) {
        return job.owner == owner && job.to == to && job.footprint == footprint &&
            job.originPolicy == originPolicy &&
            (originPolicy == NavigationOriginPolicy::movingActor || job.from == from);
    });
    if (found != state.jobs.end()) {
        found->requested = state.frame;
        const auto geometryChanged = found->complete &&
            (found->terrain != navigation.terrainIdentity() ||
             found->collision != navigation.collisionIdentity());
        const auto originBlocked = !navigation.walkable(found->from, footprint);
        if (geometryChanged || originBlocked) {
            if (geometryChanged) ++state.geometryInvalidations;
            if (originBlocked) ++state.blockedOrigins;
            if (state.active == found->id) { state.search.reset(); state.active = 0; }
            state.jobs.erase(found); found = state.jobs.end(); ++state.invalidated;
        }
    }
    if (found == state.jobs.end() && !newSearchAdmission_) {
        // A later candidate may already have an immediately clear chord.
        // Prove that under the same allowance, but do not add another unknown
        // frontier while the selected builder's question is still pending.
        if (state.frameWork >= static_cast<std::size_t>(state.allowance)) return pending;
        ++state.frameWork;
        if (navigation.lineWalkable(from,to,footprint))
            return {NavigationStatus::reached,{from,to}};
        ++state.admissionDeferred;
        return pending;
    }
    if (found == state.jobs.end()) {
        if (state.jobs.size() >= Impl::maximumJobs) {
            auto oldest = state.jobs.end();
            for (auto it = state.jobs.begin(); it != state.jobs.end(); ++it)
                if (it->complete && (oldest == state.jobs.end() || it->requested < oldest->requested)) oldest = it;
            if (oldest == state.jobs.end()) return pending;
            state.jobs.erase(oldest);
            ++state.cacheEvictions;
        }
        state.jobs.push_back({++state.nextId,owner,0,0,from,to,footprint,state.frame,originPolicy});
        found = std::prev(state.jobs.end());
        found->created = state.frame;
        if (navigation.lineWalkable(from,to,footprint)) {
            found->complete = true;
            found->terrain = navigation.terrainIdentity();
            found->collision = navigation.collisionIdentity();
            found->result = {NavigationStatus::reached,{from,to}};
            ++state.completed;
        }
    }
    const auto id = found->id;
    advance(navigation);
    found = std::ranges::find_if(state.jobs, [id](const Impl::Job& job) { return job.id == id; });
    if (found == state.jobs.end() || !found->complete) return pending;
    if (from == found->from) {
        found->joinedIndex = 0;
        return found->result;
    }

    // Moving along a proved route is not a new reachability question. Search
    // a bounded window around the last join, rather than only its old prefix.
    if (found->result.reached()) {
        // A coalesced straight proof has no intermediate vertices to join.
        // Revalidate the current chord before returning it, without replacing
        // the retained origin used to anchor still-pending detour candidates.
        if (found->result.points.size() == 2) {
            if (state.frameWork >= static_cast<std::size_t>(state.allowance)) return pending;
            ++state.frameWork;
            if (navigation.lineWalkable(from, to, footprint))
                return {NavigationStatus::reached, {from, to}};
        }
        constexpr std::size_t joinWindow = 128, joinLookback = 32;
        const auto begin = found->joinedIndex > joinLookback
            ? found->joinedIndex - joinLookback : 0;
        const auto end = std::min(found->result.points.size(), begin + joinWindow);
        std::array<std::pair<int, std::size_t>, joinWindow> candidates;
        auto candidateCount = std::size_t{};
        // Distance ranking is bounded to128 metadata entries and does not
        // perform collision checks. Each actual clearance proof below charges
        // the shared allowance. Prefer progress when distances tie.
        for (auto i = begin; i < end; ++i) {
            const auto point = found->result.points[i];
            const auto dx = static_cast<std::int64_t>(from.x) - point.x;
            const auto dy = static_cast<std::int64_t>(from.y) - point.y;
            if (std::abs(dx) > 256 || std::abs(dy) > 256) continue;
            const auto squared = dx * dx + dy * dy;
            if (squared <= 256 * 256)
                candidates[candidateCount++] = {static_cast<int>(squared), i};
        }
        std::sort(candidates.begin(), candidates.begin() + candidateCount,
            [](const auto& a, const auto& b) {
                return a.first != b.first ? a.first < b.first : a.second > b.second;
            });
        for (auto candidate = std::size_t{}; candidate < candidateCount; ++candidate) {
            if (state.frameWork >= static_cast<std::size_t>(state.allowance)) return pending;
            ++state.frameWork;
            const auto i = candidates[candidate].second;
            const auto point = found->result.points[i];
            if (!navigation.lineWalkable(from, point, footprint)) continue;
            found->joinedIndex = i;
            auto result = NavigationPathResult{NavigationStatus::reached, {from}};
            result.points.insert(result.points.end(), found->result.points.begin() +
                static_cast<std::ptrdiff_t>(i + static_cast<std::size_t>(point == from)),
                                 found->result.points.end());
            return result;
        }
    } else {
        if (state.frameWork >= static_cast<std::size_t>(state.allowance)) return pending;
        ++state.frameWork;
        if (distanceSquared(from, found->from) <= 256 * 256 &&
            navigation.lineWalkable(from, found->from, footprint)) return found->result;
    }
    if (state.active == found->id) { state.search.reset(); state.active = 0; }
    state.jobs.erase(found); ++state.invalidated; ++state.failedCurrentJoins;
    // The new current-origin query gets its FIFO turn on a subsequent request.
    state.jobs.push_back({++state.nextId,owner,0,0,from,to,footprint,state.frame,originPolicy});
    state.jobs.back().created = state.frame;
    return pending;
}

Position NavigationWorkQueue::retainedOrigin(
    const std::uint64_t owner, const Position to, const MovementFootprint footprint) const noexcept {
    if (!impl_) return {-1,-1};
    const auto found = std::ranges::find_if(impl_->jobs, [owner,to,footprint](const Impl::Job& job) {
        return job.owner == owner && job.to == to && job.footprint == footprint &&
            job.originPolicy == NavigationOriginPolicy::movingActor;
    });
    return found == impl_->jobs.end() ? Position{-1,-1} : found->from;
}

NavigationGrid::NavigationGrid(
    const int width,
    const int height,
    const int cellSize,
    std::vector<std::uint8_t> walkable,
    std::vector<std::uint8_t> elevation)
    : width_(std::max(0, width)),
      height_(std::max(0, height)),
      cellSize_(std::max(1, cellSize)),
      terrainIdentity_(nextTerrainIdentity.fetch_add(1U, std::memory_order_relaxed)),
      walkable_(std::move(walkable)), elevation_(std::move(elevation)) {
    const auto expected = static_cast<std::size_t>(width_) *
                          static_cast<std::size_t>(height_);
    if (expected > maximumNavigationCells || walkable_.size() != expected) {
        width_ = 0;
        height_ = 0;
        walkable_.clear();
        elevation_.clear();
        return;
    }
    if (elevation_.size() != expected) elevation_.assign(expected, 0);
    if (!walkable_.empty()) dynamicBlockCounts_.assign(expected, 0U);
}

DefensivePosition NavigationGrid::defensivePosition(
    const Position home, const Position outside) const {
    const auto pathResult = findPath(home, outside);
    if (!pathResult.reached()) return {};
    DefensivePositionScan scan;
    scanDefensivePosition(home, pathResult.points, scan, std::numeric_limits<int>::max());
    return scan.best;
}

void NavigationGrid::scanDefensivePosition(
    const Position home, const std::span<const Position> path,
    DefensivePositionScan& scan, const int maximumVertices) const {
    if (empty() || maximumVertices <= 0) return;
    const auto heightAt = [this](const Position point) {
        return static_cast<int>(elevation_[static_cast<std::size_t>(
            index(point.x / cellSize_, point.y / cellSize_))]);
    };
    for (auto remaining = maximumVertices;
         remaining > 0 && scan.nextVertex + 4 < path.size(); --remaining) {
        const auto i = scan.nextVertex++;
        const auto radius = distance(home, path[i]);
        if (radius < 224.0 || radius > 1400.0) continue;
        const auto before = path[i - 3];
        const auto after = path[i + 3];
        const auto length = distance(before, after);
        if (length < cellSize_) continue;
        const auto nx = -(after.y - before.y) / length;
        const auto ny = (after.x - before.x) / length;
        const auto edge = [&](const int sign) {
            auto last = path[i];
            for (int offset = cellSize_; offset <= 384; offset += cellSize_) {
                const Position sample{path[i].x + static_cast<int>(std::lround(nx * offset * sign)),
                                      path[i].y + static_cast<int>(std::lround(ny * offset * sign))};
                if (!walkable(sample) || !lineWalkable(last, sample)) break;
                last = sample;
            }
            return last;
        };
        const auto left = edge(-1);
        const auto right = edge(1);
        const auto width = static_cast<int>(distance(left, right)) + cellSize_;
        const auto upperEdge = heightAt(before) > heightAt(after);
        // Open plains are not a choke. A wider high-ground edge is useful,
        // but do not pretend a whole plateau can be covered by a small army.
        if (width > (upperEdge ? 512 : 320)) continue;
        const auto score = width * 1.5 + radius * 0.25 - (upperEdge ? 180.0 : 0.0);
        if (score >= scan.bestScore) continue;
        scan.bestScore = score;
        scan.best = {before, path[i], left, right, width, upperEdge};
    }
}

bool NavigationGrid::empty() const noexcept {
    return width_ <= 0 || height_ <= 0 || walkable_.empty();
}

bool NavigationGrid::walkable(
    const Position position, const MovementFootprint footprint) const noexcept {
    if (!position.valid() || empty()) return false;
    if (footprintWalkable(position, footprint)) return true;

    // A native actor can be clear of a structure while its pixel footprint
    // touches a walk cell whose center lies inside that structure. Keep the
    // conservative cell model for search states, but resolve a rejected exact
    // endpoint against real dynamic bounds. Static terrain is still checked
    // across every touched cell; an actual overlapping obstacle is never waived.
    const auto minX = position.x - std::max(0, footprint.left);
    const auto maxX = position.x + std::max(0, footprint.right);
    const auto minY = position.y - std::max(0, footprint.up);
    const auto maxY = position.y + std::max(0, footprint.down);
    if (minX < 0 || minY < 0 || maxX >= width_ * cellSize_ ||
        maxY >= height_ * cellSize_) return false;
    for (auto y = minY / cellSize_; y <= maxY / cellSize_; ++y)
        for (auto x = minX / cellSize_; x <= maxX / cellSize_; ++x)
            if (walkable_[static_cast<std::size_t>(index(x, y))] == 0U) return false;
    for (const auto& [id, obstacle] : dynamicObstacles_) {
        static_cast<void>(id);
        const auto& bounds = obstacle.bounds;
        if (bounds.right >= minX && bounds.left <= maxX &&
            bounds.bottom >= minY && bounds.top <= maxY) return false;
    }
    return true;
}

bool NavigationGrid::lineWalkable(
    const Position from, const Position to, const MovementFootprint footprint) const noexcept {
    if (empty() || !walkable(from, footprint) || !walkable(to, footprint)) return false;
    const auto rasterClear = [this, from, to, footprint](const bool includeDynamicObstacles) {
        const auto cellClear = [this, footprint, includeDynamicObstacles](const int x, const int y) {
            return x >= 0 && y >= 0 && x < width_ && y < height_ &&
                footprintWalkable(cellCenter(index(x, y)), footprint, includeDynamicObstacles);
        };
        auto x = from.x / cellSize_;
        auto y = from.y / cellSize_;
        const auto endX = to.x / cellSize_;
        const auto endY = to.y / cellSize_;
        const auto dx = std::abs(endX - x);
        const auto stepX = x < endX ? 1 : -1;
        const auto dy = -std::abs(endY - y);
        const auto stepY = y < endY ? 1 : -1;
        auto error = dx + dy;
        for (;;) {
            if (!cellClear(x, y)) return false;
            if (x == endX && y == endY) return true;
            const auto twice = error * 2;
            // Preserve A*'s static-terrain clearance and corner rule.
            if (twice >= dy && twice <= dx &&
                (!cellClear(x + stepX, y) || !cellClear(x, y + stepY))) return false;
            if (twice >= dy) {
                error += dy;
                x += stepX;
            }
            if (twice <= dx) {
                error += dx;
                y += stepY;
            }
        }
    };
    if (rasterClear(true)) return true;
    if (dynamicObstacles_.empty() || !rasterClear(false)) return false;

    // Rounding a clear native endpoint to a cell center can put its body
    // inside an adjacent building. Resolve only dynamic geometry exactly:
    // sweep the actor's center against each obstacle expanded by its body.
    // This also checks the whole segment, not just its clear endpoints.
    for (const auto& [id, obstacle] : dynamicObstacles_) {
        static_cast<void>(id);
        const auto& bounds = obstacle.bounds;
        auto enter = 0.0;
        auto leave = 1.0;
        const auto intersectsAxis = [&enter, &leave](const double origin, const double delta,
                                                    const double low, const double high) {
            if (delta == 0.0) return origin >= low && origin <= high;
            auto first = (low - origin) / delta;
            auto last = (high - origin) / delta;
            if (first > last) std::swap(first, last);
            enter = std::max(enter, first);
            leave = std::min(leave, last);
            return enter <= leave;
        };
        if (intersectsAxis(from.x, static_cast<double>(to.x) - from.x,
                           static_cast<double>(bounds.left) - std::max(0, footprint.right),
                           static_cast<double>(bounds.right) + std::max(0, footprint.left)) &&
            intersectsAxis(from.y, static_cast<double>(to.y) - from.y,
                           static_cast<double>(bounds.top) - std::max(0, footprint.down),
                           static_cast<double>(bounds.bottom) + std::max(0, footprint.up)))
            return false;
    }
    return true;
}

Position NavigationGrid::nearestWalkable(
    const Position position,
    const int searchRadius,
    const MovementFootprint footprint) const noexcept {
    if (empty() || !position.valid()) return {-1, -1};
    const auto originX = std::clamp(position.x / cellSize_, 0, width_ - 1);
    const auto originY = std::clamp(position.y / cellSize_, 0, height_ - 1);
    if (footprintWalkable(cellCenter(index(originX, originY)), footprint))
        return cellCenter(index(originX, originY));
    const auto radiusLimit = searchRadius < 0
        ? std::max(8, (256 + cellSize_ - 1) / cellSize_)
        : std::max(0, searchRadius);
    for (auto radius = 1; radius <= radiusLimit; ++radius) {
        for (auto y = originY - radius; y <= originY + radius; ++y) {
            for (auto x = originX - radius; x <= originX + radius; ++x) {
                if (std::max(std::abs(x - originX), std::abs(y - originY)) != radius ||
                    !cellWalkable(x, y) ||
                    !footprintWalkable(cellCenter(index(x, y)), footprint)) {
                    continue;
                }
                return cellCenter(index(x, y));
            }
        }
    }
    return {-1, -1};
}


NavigationCandidateSelection NavigationGrid::bestReachableCandidate(
    const Position from, const std::span<const NavigationCandidate> candidates,
    const int maximumExpansions, const MovementFootprint footprint) const {
    NavigationCandidateSelection result;
    if (empty() || !from.valid() || maximumExpansions < 0 || candidates.size() > 64U ||
        !walkable(from, footprint)) return result;
    struct Goal { Position position; double penalty; int cell; bool direct; };
    std::vector<Goal> goals;
    goals.reserve(candidates.size());
    const auto consider = [&result](const Position target, const double score) {
        if (score < result.score || (score == result.score &&
            (target.y < result.target.y ||
             (target.y == result.target.y && target.x < result.target.x)))) {
            result.target = target;
            result.score = score;
        }
    };
    for (const auto& candidate : candidates) {
        if (!candidate.position.valid() || !std::isfinite(candidate.penalty) ||
            candidate.penalty < 0.0 || !walkable(candidate.position, footprint)) continue;
        const auto direct = lineWalkable(from, candidate.position, footprint);
        goals.push_back({candidate.position, candidate.penalty,
            index(candidate.position.x / cellSize_, candidate.position.y / cellSize_), direct});
        if (direct) consider(candidate.position, distance(from, candidate.position) + candidate.penalty);
    }
    if (goals.empty()) return result;
    const auto competitive = std::ranges::any_of(goals, [&result, from](const Goal& goal) {
        return !goal.direct && distance(from, goal.position) + goal.penalty < result.score;
    });
    if (!competitive) {
        result.status = result.target.valid() ? NavigationStatus::reached : NavigationStatus::unreachable;
        return result;
    }
    if (maximumExpansions == 0) {
        result.searchBudgetExhausted = true;
        result.status = result.target.valid() ? NavigationStatus::reached : NavigationStatus::budgetExhausted;
        return result;
    }
    const auto startPosition = nearestWalkable(from, -1, footprint);
    if (!startPosition.valid() || !lineWalkable(from, startPosition, footprint)) {
        result.status = result.target.valid() ? NavigationStatus::reached : NavigationStatus::unreachable;
        return result;
    }
    const auto start = index(startPosition.x / cellSize_, startPosition.y / cellSize_);
    auto minX = goals.front().position.x, maxX = minX;
    auto minY = goals.front().position.y, maxY = minY;
    auto minimumPenalty = goals.front().penalty;
    for (const auto& goal : goals) {
        minX = std::min(minX, goal.position.x); maxX = std::max(maxX, goal.position.x);
        minY = std::min(minY, goal.position.y); maxY = std::max(maxY, goal.position.y);
        minimumPenalty = std::min(minimumPenalty, goal.penalty);
    }
    // Distance to a box containing every goal is an admissible, consistent
    // bound. Compute one distance per vertex instead of scoring all goals on
    // every edge; final selection still uses each exact endpoint and penalty.
    const auto lowerBound = [minX, maxX, minY, maxY, minimumPenalty, this](const int node) {
        const auto point = cellCenter(node);
        const auto dx = std::max({minX - point.x, 0, point.x - maxX});
        const auto dy = std::max({minY - point.y, 0, point.y - maxY});
        return std::hypot(static_cast<double>(dx), static_cast<double>(dy)) + minimumPenalty;
    };
    auto& diagnostics = searchWorkspace();
    if (!diagnostics.ensureCells(walkable_.size()) || !diagnostics.ensureCandidateCosts()) {
        result.searchBudgetExhausted = true;
        result.status = result.target.valid() ? NavigationStatus::reached : NavigationStatus::budgetExhausted;
        return result;
    }
    diagnostics.beginSearch();
    diagnostics.discover(start);
    const auto initialCost = distance(from, startPosition);
    diagnostics.candidateCosts[static_cast<std::size_t>(start)] = initialCost;
    diagnostics.candidatePriorities[static_cast<std::size_t>(start)] = initialCost + lowerBound(start);
    diagnostics.candidatePushOrDecrease(start);
    static constexpr std::array directions{
        std::pair{-1, -1}, std::pair{0, -1}, std::pair{1, -1},
        std::pair{-1, 0},                      std::pair{1, 0},
        std::pair{-1, 1},  std::pair{0, 1},  std::pair{1, 1}};
    while (diagnostics.heapSize != 0U && result.expandedNodes < maximumExpansions) {
        const auto current = diagnostics.heapNodes[0];
        if (diagnostics.candidatePriorities[static_cast<std::size_t>(current)] > result.score) break;
        (void)diagnostics.candidatePopMinimum();
        const auto cost = diagnostics.candidateCosts[static_cast<std::size_t>(current)];
        ++result.expandedNodes;
        ++diagnostics.totalExpansions;
        const auto point = cellCenter(current);
        for (const auto& goal : goals) {
            if (current == goal.cell && lineWalkable(point, goal.position, footprint))
                consider(goal.position, cost + distance(point, goal.position) + goal.penalty);
        }
        const auto x = current % width_, y = current / width_;
        for (const auto [dx, dy] : directions) {
            const auto nx = x + dx, ny = y + dy;
            if (!cellWalkable(nx, ny) ||
                !footprintWalkable(cellCenter(index(nx, ny)), footprint)) continue;
            if (dx != 0 && dy != 0 &&
                (!cellWalkable(x + dx, y) ||
                 !footprintWalkable(cellCenter(index(x + dx, y)), footprint) ||
                 !cellWalkable(x, y + dy) ||
                 !footprintWalkable(cellCenter(index(x, y + dy)), footprint))) continue;
            const auto next = index(nx, ny);
            const auto nextCost = cost + (dx != 0 && dy != 0 ? std::sqrt(2.0) : 1.0) * cellSize_;
            const auto slot = static_cast<std::size_t>(next);
            const auto known = diagnostics.generations[slot] == diagnostics.generation;
            if (known && (diagnostics.heapPositions[slot] == -2 ||
                          nextCost >= diagnostics.candidateCosts[slot])) continue;
            const auto priorityNext = nextCost + lowerBound(next);
            if (priorityNext > result.score) continue;
            diagnostics.discover(next);
            diagnostics.candidateCosts[slot] = nextCost;
            diagnostics.candidatePriorities[slot] = priorityNext;
            diagnostics.candidatePushOrDecrease(next);
        }
    }
    result.searchBudgetExhausted = result.expandedNodes >= maximumExpansions &&
        diagnostics.heapSize != 0U && diagnostics.candidatePriorities[
            static_cast<std::size_t>(diagnostics.heapNodes[0])] <= result.score;
    result.status = result.target.valid() ? NavigationStatus::reached :
        result.searchBudgetExhausted ? NavigationStatus::budgetExhausted : NavigationStatus::unreachable;
    return result;
}

NavigationPathResult NavigationGrid::findPath(
    const Position from,
    const Position to,
    const int maximumExpansions,
    const MovementFootprint footprint) const {
    if (empty() || !from.valid() || !to.valid() || maximumExpansions < 0 ||
        from.x >= width_ * cellSize_ || from.y >= height_ * cellSize_ ||
        to.x >= width_ * cellSize_ || to.y >= height_ * cellSize_) {
        return {NavigationStatus::invalidInput, {}};
    }
    if (maximumExpansions == 0)
        return {NavigationStatus::budgetExhausted, {}, true};
    auto& workspace = searchWorkspace();
    std::vector<Position> cachedPoints;
    if (workspace.cachedRoute(*this, from, to, footprint, cachedPoints))
        return {NavigationStatus::reached, std::move(cachedPoints)};
    if (!walkable(from, footprint))
        return {NavigationStatus::unreachable, {}};
    const auto startPosition = nearestWalkable(from, -1, footprint);
    if (!startPosition.valid() || !lineWalkable(from, startPosition, footprint)) {
        return {NavigationStatus::unreachable, {}};
    }
    const auto startX = startPosition.x / cellSize_;
    const auto startY = startPosition.y / cellSize_;
    const auto goalX = to.x / cellSize_;
    const auto goalY = to.y / cellSize_;
    const auto start = index(startX, startY);
    const auto exactGoal = walkable(to, footprint);
    const auto goal = exactGoal ? index(goalX, goalY) : -1;
    const auto snapRadius = std::max(8, (256 + cellSize_ - 1) / cellSize_);
    const auto insideSnapRadius = [this, goalX, goalY, snapRadius](const int candidate) {
        const auto candidateX = candidate % width_;
        const auto candidateY = candidate / width_;
        return std::max(std::abs(candidateX - goalX), std::abs(candidateY - goalY)) <=
               snapRadius;
    };
    const auto snapDistanceSquared = [this, to](const int candidate) {
        const auto point = cellCenter(candidate);
        const auto dx = static_cast<std::int64_t>(point.x) - to.x;
        const auto dy = static_cast<std::int64_t>(point.y) - to.y;
        return dx * dx + dy * dy;
    };
    // The broad snap radius is a search/fallback region, not an arrival
    // tolerance. A blocked retreat point can be inside a nearby building;
    // stopping anywhere in that region used to strand actors over 200px away.
    // Seek proximity comparable to a clear target-side cell, then fall back
    // to the closest proved reachable node if that terrain is disconnected.
    const auto snappedGoal = exactGoal ? Position{-1, -1}
                                      : nearestWalkable(to, snapRadius, footprint);
    const auto snappedGoalDistance = snappedGoal.valid()
        ? snapDistanceSquared(index(snappedGoal.x / cellSize_, snappedGoal.y / cellSize_))
        : std::int64_t{-1};
    const auto closeToSnappedGoal = [&](const int candidate) {
        return snappedGoalDistance >= 0 && insideSnapRadius(candidate) &&
               snapDistanceSquared(candidate) <= snappedGoalDistance;
    };
    const auto destinationReachable = [&] {
        return exactGoal && lineWalkable(cellCenter(goal), to, footprint);
    };
    const auto materializePath = [this, from, start, footprint, &workspace](
                                     const int destination, const bool appendGoal,
                                     const Position goalPosition,
                                     const Position requestedGoal) {
        std::vector<Position> points;
        for (auto current = destination; current >= 0;
             current = workspace.parents[static_cast<std::size_t>(current)]) {
            points.push_back(cellCenter(current));
            if (current == start) break;
            if (points.size() > workspace.cells) return std::vector<Position>{};
        }
        if (points.empty() || points.back() != cellCenter(start)) return points;
        std::ranges::reverse(points);
        if (points.front() != from) points.insert(points.begin(), from);
        if (appendGoal && lineWalkable(goalPosition, requestedGoal, footprint) &&
            points.back() != requestedGoal) {
            points.push_back(requestedGoal);
        }
        return points;
    };
    if (exactGoal && start == goal) {
        auto points = std::vector<Position>{from};
        if (startPosition != from) points.push_back(startPosition);
        const auto reached = destinationReachable();
        if (reached && to != points.back()) points.push_back(to);
        if (reached) workspace.rememberRoute(*this, from, to, footprint, points);
        return {reached ? NavigationStatus::reached : NavigationStatus::partial,
                std::move(points)};
    }
    if (!exactGoal && closeToSnappedGoal(start)) {
        auto points = std::vector<Position>{from};
        if (startPosition != from) points.push_back(startPosition);
        return {NavigationStatus::partial, std::move(points)};
    }

    const auto size = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
    if (!workspace.ensureCells(size))
        return {NavigationStatus::budgetExhausted, {}, true};
    workspace.beginSearch();
    workspace.discover(start);
    workspace.costs[static_cast<std::size_t>(start)] = 0;
    workspace.pushOrDecrease(start, width_, goalX, goalY);
    auto bestProgress = start;
    auto bestProgressHeuristic = heuristic(startX, startY, goalX, goalY);
    auto bestProgressCost = 0;
    auto bestSnapNode = insideSnapRadius(start) ? start : -1;
    auto bestSnapDistance = bestSnapNode >= 0
        ? snapDistanceSquared(bestSnapNode) : std::numeric_limits<std::int64_t>::max();
    auto bestSnapCost = 0;
    auto reachedGoal = false;
    auto reachedGoalNode = -1;

    static constexpr std::array directions{
        std::pair{-1, -1}, std::pair{0, -1}, std::pair{1, -1},
        std::pair{-1, 0},                     std::pair{1, 0},
        std::pair{-1, 1},  std::pair{0, 1},  std::pair{1, 1},
    };
    auto expansions = 0;
    while (workspace.heapSize > 0U && expansions < maximumExpansions) {
        const auto current = workspace.popMinimum(width_, goalX, goalY);
        const auto currentIndex = static_cast<std::size_t>(current);
        ++expansions;
        ++workspace.totalExpansions;
        if (exactGoal && current == goal) {
            reachedGoal = true;
            reachedGoalNode = current;
            break;
        }
        if (!exactGoal && closeToSnappedGoal(current)) {
            reachedGoal = true;
            reachedGoalNode = current;
            break;
        }
        const auto x = current % width_;
        const auto y = current / width_;
        for (const auto [offsetX, offsetY] : directions) {
            const auto nextX = x + offsetX;
            const auto nextY = y + offsetY;
            if (!cellWalkable(nextX, nextY) ||
                !footprintWalkable(cellCenter(index(nextX, nextY)), footprint)) continue;
            if (offsetX != 0 && offsetY != 0 &&
                (!cellWalkable(x + offsetX, y) ||
                 !footprintWalkable(cellCenter(index(x + offsetX, y)), footprint) ||
                 !cellWalkable(x, y + offsetY) ||
                 !footprintWalkable(cellCenter(index(x, y + offsetY)), footprint))) {
                continue;
            }
            const auto next = index(nextX, nextY);
            const auto nextIndex = static_cast<std::size_t>(next);
            workspace.discover(next);
            if (workspace.heapPositions[nextIndex] == -2) continue;
            const auto step = offsetX != 0 && offsetY != 0 ? 14 : 10;
            const auto candidate = workspace.costs[currentIndex] + step;
            if (candidate >= workspace.costs[nextIndex]) continue;
            workspace.costs[nextIndex] = candidate;
            workspace.parents[nextIndex] = current;
            workspace.pushOrDecrease(next, width_, goalX, goalY);
            if (insideSnapRadius(next)) {
                const auto snapDistance = snapDistanceSquared(next);
                if (snapDistance < bestSnapDistance ||
                    (snapDistance == bestSnapDistance &&
                     (candidate < bestSnapCost ||
                      (candidate == bestSnapCost && next < bestSnapNode)))) {
                    bestSnapNode = next;
                    bestSnapDistance = snapDistance;
                    bestSnapCost = candidate;
                }
            }
            const auto progressHeuristic = heuristic(nextX, nextY, goalX, goalY);
            if (progressHeuristic < bestProgressHeuristic ||
                (progressHeuristic == bestProgressHeuristic &&
                 (candidate < bestProgressCost ||
                  (candidate == bestProgressCost && next < bestProgress)))) {
                bestProgress = next;
                bestProgressHeuristic = progressHeuristic;
                bestProgressCost = candidate;
            }
        }
    }
    const auto budgetExhausted = !reachedGoal && expansions >= maximumExpansions &&
                                 workspace.heapSize > 0U;
    if (reachedGoal) {
        const auto reached = exactGoal && reachedGoalNode == goal && destinationReachable();
        auto points = materializePath(
            reachedGoalNode, reached, cellCenter(reachedGoalNode), to);
        if (points.empty()) return {NavigationStatus::unreachable, {}};
        if (reached) workspace.rememberRoute(*this, from, to, footprint, points);
        return {reached ? NavigationStatus::reached : NavigationStatus::partial,
                std::move(points)};
    }

    if (bestSnapNode >= 0) {
        auto points = materializePath(bestSnapNode, false, {}, to);
        if (!points.empty()) return {NavigationStatus::partial, std::move(points), budgetExhausted};
    }

    if (!budgetExhausted) return {NavigationStatus::unreachable, {}};
    if (bestProgress == start)
        return {NavigationStatus::budgetExhausted, {}, true};
    auto points = materializePath(bestProgress, false, {}, to);
    if (points.size() < 2) return {NavigationStatus::budgetExhausted, {}, true};
    return {NavigationStatus::partial, std::move(points), true};
}

NavigationDiagnostics NavigationGrid::diagnosticsForCurrentThread() noexcept {
    const auto& workspace = searchWorkspace();
    return {
        workspace.workspaceBytes(),
        workspace.cachedPoints * sizeof(Position),
        workspace.searches,
        workspace.routeCacheHits,
        workspace.workspaceResizes,
        workspace.totalExpansions,
    };
}

NavigationWaypointResult NavigationGrid::nextWaypoint(
    const Position from,
    const Position to,
    const int lookaheadCells,
    const int maximumExpansions,
    const MovementFootprint footprint,
    std::vector<Position>* route) const {
    if (empty() || !from.valid() || !to.valid() || maximumExpansions < 0) {
        if (route != nullptr) route->clear();
        return {NavigationStatus::invalidInput, {-1, -1}};
    }
    if (lineWalkable(from, to, footprint)) {
        if (route != nullptr) {
            route->clear();
            route->push_back(from);
            if (to != from) route->push_back(to);
        }
        return {NavigationStatus::reached, to};
    }
    const auto path = findPath(from, to, maximumExpansions, footprint);
    if (route != nullptr) *route = path.points;
    if (!path.hasUsablePath() || path.points.empty())
        return {path.status, {-1, -1}};
    return {path.status, path.points[navigationWaypointIndex(
        path.points.size(), cellSize_, lookaheadCells)]};
}

NavigationWaypointResult NavigationGrid::nearestReachable(
    const Position from,
    const Position target,
    const int maximumExpansions,
    const MovementFootprint footprint) const {
    const auto path = findPath(from, target, maximumExpansions, footprint);
    if (!path.hasUsablePath() || path.points.empty())
        return {path.status, {-1, -1}};
    return {path.status, path.points.back()};
}

Position NavigationGrid::nearestReachableTarget(
    const Position from,
    const std::span<const Position> targets,
    const int maximumExpansions,
    const MovementFootprint footprint) const {
    if (empty() || !from.valid() || maximumExpansions < 0) return {-1, -1};

    auto best = Position{-1, -1};
    auto bestDistance = std::numeric_limits<double>::max();
    for (const auto target : targets) {
        if (!target.valid()) continue;
        const auto path = findPath(from, target, maximumExpansions, footprint);
        if (!path.reached()) continue;

        auto routeDistance = 0.0;
        auto previous = from;
        for (const auto point : path.points) {
            routeDistance += distance(previous, point);
            previous = point;
        }
        if (routeDistance < bestDistance) {
            best = target;
            bestDistance = routeDistance;
        }
    }
    return best;
}

int NavigationGrid::index(const int x, const int y) const noexcept {
    return y * width_ + x;
}

bool NavigationGrid::cellWalkable(const int x, const int y) const noexcept {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return false;
    const auto cell = static_cast<std::size_t>(index(x, y));
    return walkable_[cell] != 0U && dynamicBlockCounts_[cell] == 0U;
}

bool NavigationGrid::footprintWalkable(
    const Position position, const MovementFootprint footprint,
    const bool includeDynamicObstacles) const noexcept {
    if (!position.valid() || empty()) return false;
    const auto left = std::max(0, footprint.left);
    const auto right = std::max(0, footprint.right);
    const auto up = std::max(0, footprint.up);
    const auto down = std::max(0, footprint.down);
    const auto minX = position.x - left;
    const auto maxX = position.x + right;
    const auto minY = position.y - up;
    const auto maxY = position.y + down;
    if (minX < 0 || minY < 0 || maxX >= width_ * cellSize_ ||
        maxY >= height_ * cellSize_) return false;
    const auto firstX = minX / cellSize_;
    const auto lastX = maxX / cellSize_;
    const auto firstY = minY / cellSize_;
    const auto lastY = maxY / cellSize_;
    for (auto y = firstY; y <= lastY; ++y)
        for (auto x = firstX; x <= lastX; ++x) {
            const auto cell = static_cast<std::size_t>(index(x, y));
            if (walkable_[cell] == 0U ||
                (includeDynamicObstacles && dynamicBlockCounts_[cell] != 0U)) return false;
        }
    return true;
}

Position NavigationGrid::cellCenter(const int cellIndex) const noexcept {
    const auto x = cellIndex % width_;
    const auto y = cellIndex / width_;
    return {x * cellSize_ + cellSize_ / 2, y * cellSize_ + cellSize_ / 2};
}

std::vector<int> NavigationGrid::obstacleCells(
    const NavigationObstacleBounds bounds) const {
    std::vector<int> cells;
    if (empty() || !bounds.valid()) return cells;
    auto firstX = std::clamp(bounds.left / cellSize_, 0, width_ - 1);
    auto lastX = std::clamp(bounds.right / cellSize_, 0, width_ - 1);
    auto firstY = std::clamp(bounds.top / cellSize_, 0, height_ - 1);
    auto lastY = std::clamp(bounds.bottom / cellSize_, 0, height_ - 1);
    const auto centerX = [this](const int x) { return x * cellSize_ + cellSize_ / 2; };
    const auto centerY = [this](const int y) { return y * cellSize_ + cellSize_ / 2; };
    while (firstX <= lastX && centerX(firstX) < bounds.left) ++firstX;
    while (lastX >= firstX && centerX(lastX) > bounds.right) --lastX;
    while (firstY <= lastY && centerY(firstY) < bounds.top) ++firstY;
    while (lastY >= firstY && centerY(lastY) > bounds.bottom) --lastY;
    if (firstX > lastX || firstY > lastY) return cells;
    cells.reserve(static_cast<std::size_t>(lastX - firstX + 1) *
                  static_cast<std::size_t>(lastY - firstY + 1));
    for (auto y = firstY; y <= lastY; ++y)
        for (auto x = firstX; x <= lastX; ++x)
            cells.push_back(index(x, y));
    return cells;
}

void NavigationGrid::applyObstacleCells(const std::vector<int>& cells, const bool add) {
    constexpr std::size_t maximumChangeJournal = 65536;
    for (const auto cellIndex : cells) {
        const auto cell = static_cast<std::size_t>(cellIndex);
        auto& count = dynamicBlockCounts_[cell];
        const auto wasBlocked = count != 0U;
        if (add) {
            if (count == std::numeric_limits<std::uint16_t>::max()) continue;
            ++count;
        } else {
            if (count == 0U) continue;
            --count;
        }
        const auto isBlocked = count != 0U;
        if (wasBlocked == isBlocked) continue;
        ++obstacleVersion_;
        changedCells_.push_back({cellIndex % width_, cellIndex / width_, obstacleVersion_});
        if (changedCells_.size() > maximumChangeJournal) {
            discardedChangeVersion_ = changedCells_.front().version;
            changedCells_.pop_front();
        }
    }
}

NavigationGrid NavigationGrid::copyForTemporaryObstacles() const {
    NavigationGrid result;
    result.width_ = width_;
    result.height_ = height_;
    result.cellSize_ = cellSize_;
    result.terrainIdentity_ = terrainIdentity_;
    result.collisionIdentity_ = collisionIdentity_;
    result.walkable_ = walkable_;
    result.elevation_ = elevation_;
    result.dynamicBlockCounts_ = dynamicBlockCounts_;
    result.dynamicObstacles_ = dynamicObstacles_;
    result.obstacleVersion_ = obstacleVersion_;
    result.discardedChangeVersion_ = obstacleVersion_;
    return result;
}

bool NavigationGrid::addTemporaryObstacle(const NavigationObstacleBounds bounds) {
    if (!bounds.valid()) return false;
    auto id = std::numeric_limits<std::uint64_t>::max();
    while (dynamicObstacles_.contains(id)) --id;
    return updateDynamicObstacle(id, bounds);
}

bool NavigationGrid::updateDynamicObstacle(
    const std::uint64_t id, const NavigationObstacleBounds bounds) {
    const auto found = dynamicObstacles_.find(id);
    if (!bounds.valid()) return removeDynamicObstacle(id);
    if (found != dynamicObstacles_.end() && found->second.bounds == bounds) return false;
    collisionIdentity_ = nextCollisionIdentity.fetch_add(1U, std::memory_order_relaxed);
    if (found != dynamicObstacles_.end()) {
        applyObstacleCells(found->second.cells, false);
        dynamicObstacles_.erase(found);
    }
    auto cells = obstacleCells(bounds);
    if (!cells.empty()) {
        applyObstacleCells(cells, true);
        dynamicObstacles_.emplace(id, DynamicObstacle{bounds, std::move(cells)});
    }
    return true;
}

bool NavigationGrid::removeDynamicObstacle(const std::uint64_t id) {
    const auto found = dynamicObstacles_.find(id);
    if (found == dynamicObstacles_.end()) return false;
    collisionIdentity_ = nextCollisionIdentity.fetch_add(1U, std::memory_order_relaxed);
    applyObstacleCells(found->second.cells, false);
    dynamicObstacles_.erase(found);
    return true;
}

NavigationObstacleOverlay::NavigationObstacleOverlay(
    NavigationGrid& grid, const std::uint64_t id, const NavigationObstacleBounds bounds)
    : grid_(grid), id_(id), originalIdentity_(grid.collisionIdentity_), bounds_(bounds) {
    if (!bounds.valid() || grid_.dynamicObstacles_.contains(id_)) return;
    added_ = grid_.updateDynamicObstacle(id_,bounds_);
    overlayIdentity_ = grid_.collisionIdentity_;
}

NavigationObstacleOverlay::~NavigationObstacleOverlay() {
    if (!added_) return;
    const auto unchanged = grid_.collisionIdentity_ == overlayIdentity_;
    const auto found = grid_.dynamicObstacles_.find(id_);
    // A caller that deliberately replaced this ID owns its replacement.
    if (found != grid_.dynamicObstacles_.end() && found->second.bounds != bounds_) return;
    if (found != grid_.dynamicObstacles_.end()) grid_.removeDynamicObstacle(id_);
    if (unchanged) grid_.collisionIdentity_ = originalIdentity_;
}

bool NavigationGrid::routeAffectedSince(
    const std::span<const Position> route,
    const Position from,
    const Position to,
    const MovementFootprint footprint,
    const std::uint64_t version,
    const int marginPixels) const noexcept {
    if (version >= obstacleVersion_) return false;
    if (version < discardedChangeVersion_) return true;
    if (route.empty() && (!from.valid() || !to.valid())) return true;

    auto minX = route.empty() ? std::min(from.x, to.x) : route.front().x;
    auto maxX = route.empty() ? std::max(from.x, to.x) : route.front().x;
    auto minY = route.empty() ? std::min(from.y, to.y) : route.front().y;
    auto maxY = route.empty() ? std::max(from.y, to.y) : route.front().y;
    for (const auto point : route) {
        minX = std::min(minX, point.x);
        maxX = std::max(maxX, point.x);
        minY = std::min(minY, point.y);
        maxY = std::max(maxY, point.y);
    }
    if (route.empty()) {
        minX = std::min(minX, to.x); maxX = std::max(maxX, to.x);
        minY = std::min(minY, to.y); maxY = std::max(maxY, to.y);
    }
    const auto margin = std::max(0, marginPixels);
    minX -= std::max(0, footprint.left) + margin;
    maxX += std::max(0, footprint.right) + margin;
    minY -= std::max(0, footprint.up) + margin;
    maxY += std::max(0, footprint.down) + margin;
    // Cell revisions are appended in increasing order. A recent proof only
    // needs the suffix after its revision, even when the journal is saturated.
    const auto firstChange = std::upper_bound(
        changedCells_.begin(), changedCells_.end(), version,
        [](const std::uint64_t revision, const auto& change) {
            return revision < change.version;
        });
    for (auto entry = firstChange; entry != changedCells_.end(); ++entry) {
        const auto& change = *entry;
        const auto left = change.x * cellSize_;
        const auto top = change.y * cellSize_;
        const auto right = left + cellSize_ - 1;
        const auto bottom = top + cellSize_ - 1;
        if (right >= minX && left <= maxX && bottom >= minY && top <= maxY) return true;
    }
    return false;
}

}  // namespace protodd
