#pragma once

#include "astra/GameState.hpp"

#include <vector>

namespace astra {

struct InfluenceCell {
    float groundThreat{};
    float airThreat{};
    float detection{};
    float strategicValue{};
};

class InfluenceMap {
public:
    explicit InfluenceMap(int cellSize = 64);

    void resize(int widthPixels, int heightPixels);
    void update(const GameState& state);

    [[nodiscard]] InfluenceCell at(Position position) const noexcept;
    [[nodiscard]] Position safestStep(
        Position from,
        Position toward,
        bool flying,
        bool avoidDetection = false) const noexcept;
    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }

private:
    int cellSize_;
    int width_{};
    int height_{};
    std::vector<InfluenceCell> cells_;

    [[nodiscard]] std::size_t offset(int x, int y) const noexcept;
    void addThreat(const UnitSnapshot& unit, Frame currentFrame);
};

}  // namespace astra
