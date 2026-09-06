#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace protodd {

struct Position {
    int x{};
    int y{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return x >= 0 && y >= 0;
    }

    friend constexpr bool operator==(const Position&, const Position&) = default;
};

struct BuildingFootprint {
    Position topLeft;
    int width{};
    int height{};
};

[[nodiscard]] constexpr bool separatedByGap(
    const BuildingFootprint a, const BuildingFootprint b, const int gap) noexcept {
    return a.topLeft.x + a.width + gap <= b.topLeft.x ||
           b.topLeft.x + b.width + gap <= a.topLeft.x ||
           a.topLeft.y + a.height + gap <= b.topLeft.y ||
           b.topLeft.y + b.height + gap <= a.topLeft.y;
}

[[nodiscard]] inline double distance(const Position a, const Position b) noexcept {
    return std::hypot(static_cast<double>(a.x - b.x),
                      static_cast<double>(a.y - b.y));
}

[[nodiscard]] constexpr int distanceSquared(const Position a, const Position b) noexcept {
    const auto dx = a.x - b.x;
    const auto dy = a.y - b.y;
    return dx * dx + dy * dy;
}

[[nodiscard]] inline Position moveToward(
    const Position from,
    const Position to,
    const double pixels) noexcept {
    const auto length = distance(from, to);
    if (length <= 0.001) {
        return from;
    }
    const auto scale = std::clamp(pixels / length, 0.0, 1.0);
    return {
        from.x + static_cast<int>(std::lround((to.x - from.x) * scale)),
        from.y + static_cast<int>(std::lround((to.y - from.y) * scale)),
    };
}

}  // namespace protodd
