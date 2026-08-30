#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace astra {

struct Position {
    int x{};
    int y{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return x >= 0 && y >= 0;
    }

    friend constexpr bool operator==(const Position&, const Position&) = default;
};

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

}  // namespace astra

