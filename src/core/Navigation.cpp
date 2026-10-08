#include "protodd/Navigation.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <utility>

namespace protodd {
namespace {

constexpr int unreachable = std::numeric_limits<int>::max() / 4;
constexpr std::size_t maximumNavigationCells = 1'048'576U;
constexpr std::size_t maximumCachedRoutePoints =
    NavigationGrid::maximumCachedRouteBytes / sizeof(Position);
constexpr std::size_t maximumCachedRoutes = 16U;
std::atomic<std::uint64_t> nextTerrainIdentity{1U};

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
    std::size_t cells{};
    std::size_t heapSize{};
    std::uint32_t generation{};
    std::array<CachedRoute, maximumCachedRoutes> routes;
    std::size_t cachedPoints{};
    std::uint64_t lruClock{};
    std::uint64_t searches{};
    std::uint64_t routeCacheHits{};
    std::uint64_t workspaceResizes{};

    [[nodiscard]] bool ensureCells(const std::size_t requested) {
        if (requested > maximumNavigationCells) return false;
        if (requested == cells) return true;
        costs.reset();
        parents.reset();
        heapPositions.reset();
        heapNodes.reset();
        generations.reset();
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
        return cells * (sizeof(int) * 4U + sizeof(std::uint32_t));
    }
};

SearchWorkspace& searchWorkspace() {
    static thread_local SearchWorkspace workspace;
    return workspace;
}

}  // namespace

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
    DefensivePosition best;
    if (!pathResult.reached()) return best;
    const auto& path = pathResult.points;
    auto bestScore = std::numeric_limits<double>::infinity();
    const auto heightAt = [this](const Position point) {
        return static_cast<int>(elevation_[static_cast<std::size_t>(
            index(point.x / cellSize_, point.y / cellSize_))]);
    };
    for (std::size_t i = 3; i + 4 < path.size(); ++i) {
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
        if (score >= bestScore) continue;
        bestScore = score;
        best = {before, path[i], left, right, width, upperEdge};
    }
    return best;
}

bool NavigationGrid::empty() const noexcept {
    return width_ <= 0 || height_ <= 0 || walkable_.empty();
}

bool NavigationGrid::walkable(
    const Position position, const MovementFootprint footprint) const noexcept {
    if (!position.valid() || empty()) return false;
    return footprintWalkable(position, footprint);
}

bool NavigationGrid::lineWalkable(
    const Position from, const Position to, const MovementFootprint footprint) const noexcept {
    if (empty() || !walkable(from, footprint) || !walkable(to, footprint)) return false;
    const auto cellClear = [this, footprint](const int x, const int y) {
        return cellWalkable(x, y) &&
        footprintWalkable(cellCenter(index(x, y)), footprint);
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
        // Match A*'s clearance rule. A diagonal jump across two cell centers
        // must not bypass a blocked orthogonal neighbor at their shared corner.
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
        return {NavigationStatus::budgetExhausted, {}};
    auto& workspace = searchWorkspace();
    std::vector<Position> cachedPoints;
    if (workspace.cachedRoute(*this, from, to, footprint, cachedPoints))
        return {NavigationStatus::reached, std::move(cachedPoints)};
    if (!footprintWalkable(from, footprint))
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
    const auto exactGoal = footprintWalkable(to, footprint);
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
    if (!exactGoal && insideSnapRadius(start)) {
        auto points = std::vector<Position>{from};
        if (startPosition != from) points.push_back(startPosition);
        return {NavigationStatus::partial, std::move(points)};
    }

    const auto size = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
    if (!workspace.ensureCells(size))
        return {NavigationStatus::budgetExhausted, {}};
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
        if (exactGoal && current == goal) {
            reachedGoal = true;
            reachedGoalNode = current;
            break;
        }
        if (!exactGoal && insideSnapRadius(current)) {
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
    if (reachedGoal) {
        const auto reached = exactGoal && reachedGoalNode == goal && destinationReachable();
        auto points = materializePath(
            reachedGoalNode, reached, cellCenter(reachedGoalNode), to);
        if (points.empty()) return {NavigationStatus::unreachable, {}};
        if (reached) workspace.rememberRoute(*this, from, to, footprint, points);
        return {reached ? NavigationStatus::reached : NavigationStatus::partial,
                std::move(points)};
    }

    if (exactGoal && bestSnapNode >= 0) {
        auto points = materializePath(bestSnapNode, false, {}, to);
        if (!points.empty()) return {NavigationStatus::partial, std::move(points)};
    }

    const auto budgetExhausted = expansions >= maximumExpansions &&
                                 workspace.heapSize > 0U;
    if (!budgetExhausted) return {NavigationStatus::unreachable, {}};
    if (bestProgress == start)
        return {NavigationStatus::budgetExhausted, {}};
    auto points = materializePath(bestProgress, false, {}, to);
    if (points.size() < 2) return {NavigationStatus::budgetExhausted, {}};
    return {NavigationStatus::partial, std::move(points)};
}

NavigationDiagnostics NavigationGrid::diagnosticsForCurrentThread() noexcept {
    const auto& workspace = searchWorkspace();
    return {
        workspace.workspaceBytes(),
        workspace.cachedPoints * sizeof(Position),
        workspace.searches,
        workspace.routeCacheHits,
        workspace.workspaceResizes,
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
    const auto scale = std::max(1, (32 + cellSize_ - 1) / cellSize_);
    const auto lookahead = static_cast<std::size_t>(
        std::max(1, lookaheadCells) * scale);
    return {path.status, path.points[std::min(lookahead, path.points.size() - 1U)]};
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
    const Position position, const MovementFootprint footprint) const noexcept {
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
        for (auto x = firstX; x <= lastX; ++x)
            if (!cellWalkable(x, y)) return false;
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

bool NavigationGrid::updateDynamicObstacle(
    const std::uint64_t id, const NavigationObstacleBounds bounds) {
    const auto found = dynamicObstacles_.find(id);
    if (!bounds.valid()) return removeDynamicObstacle(id);
    if (found != dynamicObstacles_.end() && found->second.bounds == bounds) return false;
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
    applyObstacleCells(found->second.cells, false);
    dynamicObstacles_.erase(found);
    return true;
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
    for (const auto& change : changedCells_) {
        if (change.version <= version) continue;
        const auto left = change.x * cellSize_;
        const auto top = change.y * cellSize_;
        const auto right = left + cellSize_ - 1;
        const auto bottom = top + cellSize_ - 1;
        if (right >= minX && left <= maxX && bottom >= minY && top <= maxY) return true;
    }
    return false;
}

}  // namespace protodd
