#pragma once

#include "protodd/Geometry.hpp"

#include <cstdint>
#include <vector>

namespace protodd {

// A compact, deterministic terrain grid used by the portable core. The BWAPI
// adapter samples Brood War walk tiles once at game start; no map names or
// tournament-map templates are required.
class NavigationGrid {
public:
    NavigationGrid() = default;
    NavigationGrid(
        int width,
        int height,
        int cellSize,
        std::vector<std::uint8_t> walkable,
        std::vector<std::uint8_t> elevation = {});

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool walkable(Position position) const noexcept;
    [[nodiscard]] bool lineWalkable(Position from, Position to) const noexcept;
    [[nodiscard]] Position nearestWalkable(Position position, int searchRadius = 8) const noexcept;
    [[nodiscard]] std::vector<Position> findPath(
        Position from,
        Position to,
        int maximumExpansions = 12000) const;
    [[nodiscard]] Position nextWaypoint(
        Position from,
        Position to,
        int lookaheadCells = 7,
        int maximumExpansions = 12000) const;

    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] DefensivePosition defensivePosition(Position home, Position outside) const;
    [[nodiscard]] int height() const noexcept { return height_; }
    [[nodiscard]] int cellSize() const noexcept { return cellSize_; }

private:
    int width_{};
    int height_{};
    int cellSize_{32};
    std::vector<std::uint8_t> walkable_;
    std::vector<std::uint8_t> elevation_;

    [[nodiscard]] int index(int x, int y) const noexcept;
    [[nodiscard]] bool cellWalkable(int x, int y) const noexcept;
    [[nodiscard]] Position cellCenter(int index) const noexcept;
};

}  // namespace protodd
