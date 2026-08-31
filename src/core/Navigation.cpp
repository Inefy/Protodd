#include "astra/Navigation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <utility>

namespace astra {
namespace {

constexpr int unreachable = std::numeric_limits<int>::max() / 4;

int heuristic(const int x, const int y, const int goalX, const int goalY) noexcept {
    const auto dx = std::abs(x - goalX);
    const auto dy = std::abs(y - goalY);
    return 10 * std::max(dx, dy) + 4 * std::min(dx, dy);
}

}  // namespace

NavigationGrid::NavigationGrid(
    const int width,
    const int height,
    const int cellSize,
    std::vector<std::uint8_t> walkable)
    : width_(std::max(0, width)),
      height_(std::max(0, height)),
      cellSize_(std::max(1, cellSize)),
      walkable_(std::move(walkable)) {
    const auto expected = static_cast<std::size_t>(width_) *
                          static_cast<std::size_t>(height_);
    if (walkable_.size() != expected) {
        width_ = 0;
        height_ = 0;
        walkable_.clear();
    }
}

bool NavigationGrid::empty() const noexcept {
    return width_ <= 0 || height_ <= 0 || walkable_.empty();
}

bool NavigationGrid::walkable(const Position position) const noexcept {
    if (!position.valid() || empty()) return false;
    return cellWalkable(position.x / cellSize_, position.y / cellSize_);
}

bool NavigationGrid::lineWalkable(const Position from, const Position to) const noexcept {
    if (empty() || !from.valid() || !to.valid()) return false;
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
        if (!cellWalkable(x, y)) return false;
        if (x == endX && y == endY) return true;
        const auto twice = error * 2;
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
    const int searchRadius) const noexcept {
    if (empty() || !position.valid()) return {-1, -1};
    const auto originX = std::clamp(position.x / cellSize_, 0, width_ - 1);
    const auto originY = std::clamp(position.y / cellSize_, 0, height_ - 1);
    if (cellWalkable(originX, originY)) return cellCenter(index(originX, originY));
    for (auto radius = 1; radius <= std::max(0, searchRadius); ++radius) {
        for (auto y = originY - radius; y <= originY + radius; ++y) {
            for (auto x = originX - radius; x <= originX + radius; ++x) {
                if (std::max(std::abs(x - originX), std::abs(y - originY)) != radius ||
                    !cellWalkable(x, y)) {
                    continue;
                }
                return cellCenter(index(x, y));
            }
        }
    }
    return {-1, -1};
}

std::vector<Position> NavigationGrid::findPath(
    const Position from,
    const Position to,
    const int maximumExpansions) const {
    if (empty() || maximumExpansions <= 0) return {};
    const auto startPosition = nearestWalkable(from);
    const auto goalPosition = nearestWalkable(to);
    if (!startPosition.valid() || !goalPosition.valid()) return {};
    const auto startX = startPosition.x / cellSize_;
    const auto startY = startPosition.y / cellSize_;
    const auto goalX = goalPosition.x / cellSize_;
    const auto goalY = goalPosition.y / cellSize_;
    const auto start = index(startX, startY);
    const auto goal = index(goalX, goalY);
    if (start == goal) return {startPosition, goalPosition};

    const auto size = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
    std::vector<int> cost(size, unreachable);
    std::vector<int> cameFrom(size, -1);
    std::vector<std::uint8_t> closed(size, 0);
    using QueueEntry = std::pair<int, int>;
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<>> open;
    cost[static_cast<std::size_t>(start)] = 0;
    open.emplace(heuristic(startX, startY, goalX, goalY), start);

    static constexpr std::array directions{
        std::pair{-1, -1}, std::pair{0, -1}, std::pair{1, -1},
        std::pair{-1, 0},                     std::pair{1, 0},
        std::pair{-1, 1},  std::pair{0, 1},  std::pair{1, 1},
    };
    auto expansions = 0;
    while (!open.empty() && expansions < maximumExpansions) {
        const auto [priority, current] = open.top();
        static_cast<void>(priority);
        open.pop();
        const auto currentIndex = static_cast<std::size_t>(current);
        if (closed[currentIndex] != 0U) continue;
        closed[currentIndex] = 1;
        ++expansions;
        if (current == goal) break;
        const auto x = current % width_;
        const auto y = current / width_;
        for (const auto [offsetX, offsetY] : directions) {
            const auto nextX = x + offsetX;
            const auto nextY = y + offsetY;
            if (!cellWalkable(nextX, nextY)) continue;
            if (offsetX != 0 && offsetY != 0 &&
                (!cellWalkable(x + offsetX, y) || !cellWalkable(x, y + offsetY))) {
                continue;
            }
            const auto next = index(nextX, nextY);
            const auto nextIndex = static_cast<std::size_t>(next);
            const auto step = offsetX != 0 && offsetY != 0 ? 14 : 10;
            const auto candidate = cost[currentIndex] + step;
            if (candidate >= cost[nextIndex]) continue;
            cost[nextIndex] = candidate;
            cameFrom[nextIndex] = current;
            open.emplace(candidate + heuristic(nextX, nextY, goalX, goalY), next);
        }
    }
    if (cameFrom[static_cast<std::size_t>(goal)] < 0) return {};

    std::vector<Position> path;
    for (auto current = goal; current >= 0; current = cameFrom[static_cast<std::size_t>(current)]) {
        path.push_back(cellCenter(current));
        if (current == start) break;
    }
    if (path.empty() || path.back() != cellCenter(start)) return {};
    std::ranges::reverse(path);
    return path;
}

Position NavigationGrid::nextWaypoint(
    const Position from,
    const Position to,
    const int lookaheadCells,
    const int maximumExpansions) const {
    if (empty()) return to;
    if (lineWalkable(from, to)) return to;
    const auto path = findPath(from, to, maximumExpansions);
    if (path.empty()) return {-1, -1};
    const auto lookahead = static_cast<std::size_t>(std::max(1, lookaheadCells));
    return path[std::min(lookahead, path.size() - 1U)];
}

int NavigationGrid::index(const int x, const int y) const noexcept {
    return y * width_ + x;
}

bool NavigationGrid::cellWalkable(const int x, const int y) const noexcept {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return false;
    return walkable_[static_cast<std::size_t>(index(x, y))] != 0U;
}

Position NavigationGrid::cellCenter(const int cellIndex) const noexcept {
    const auto x = cellIndex % width_;
    const auto y = cellIndex / width_;
    return {x * cellSize_ + cellSize_ / 2, y * cellSize_ + cellSize_ / 2};
}

}  // namespace astra
