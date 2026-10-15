#pragma once

#include "protodd/GameState.hpp"

#include <cstdint>
#include <vector>

namespace protodd {

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
    void updateStorms(std::span<const Position> storms);
    [[nodiscard]] float stormDanger(Position position) const noexcept;
    // This identifies the current ground field and storm inputs for retry
    // invalidation. A changed value grants fresh analysis, not route safety.
    [[nodiscard]] std::uint64_t groundThreatIdentity() const noexcept {
        return groundFieldIdentity_ ^ (stormIdentity_ + 0x9e3779b97f4a7c15ULL +
            (groundFieldIdentity_ << 6U) + (groundFieldIdentity_ >> 2U));
    }

    [[nodiscard]] InfluenceCell at(Position position) const noexcept;
    [[nodiscard]] float maximumGroundThreat(
        Position from,
        Position toward) const noexcept;
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
    std::vector<Position> storms_;
    std::uint64_t groundFieldIdentity_{};
    std::uint64_t stormIdentity_{};

    void updateGroundThreatIdentity() noexcept;

    [[nodiscard]] std::size_t offset(int x, int y) const noexcept;
    void addThreat(const UnitSnapshot& unit, Frame currentFrame);
};

}  // namespace protodd
