#include "astra/InfluenceMap.hpp"

#include "astra/UnitCatalog.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace astra {

InfluenceMap::InfluenceMap(const int cellSize)
    : cellSize_(std::max(16, cellSize)) {}

void InfluenceMap::resize(const int widthPixels, const int heightPixels) {
    width_ = std::max(1, (widthPixels + cellSize_ - 1) / cellSize_);
    height_ = std::max(1, (heightPixels + cellSize_ - 1) / cellSize_);
    cells_.assign(static_cast<std::size_t>(width_ * height_), {});
}

void InfluenceMap::update(const GameState& state) {
    if (width_ * cellSize_ < state.mapWidthPixels ||
        height_ * cellSize_ < state.mapHeightPixels || cells_.empty()) {
        resize(state.mapWidthPixels, state.mapHeightPixels);
    }
    std::ranges::fill(cells_, InfluenceCell{});

    for (const auto& unit : state.enemy.units) {
        if (unit.hallucination || !unit.position.valid() || !unit.completed ||
            unit.disabled || (unitStats(unit.kind).requiresPsi && !unit.powered)) {
            continue;
        }
        addThreat(unit, state.frame);
    }
    for (const auto& base : state.bases) {
        if (!base.center.valid()) {
            continue;
        }
        auto& cell = cells_[offset(
            std::clamp(base.center.x / cellSize_, 0, width_ - 1),
            std::clamp(base.center.y / cellSize_, 0, height_ - 1))];
        cell.strategicValue += static_cast<float>(1.0 + base.mineralsRemaining / 5000.0);
    }
}

InfluenceCell InfluenceMap::at(const Position position) const noexcept {
    if (cells_.empty() || !position.valid()) {
        return {};
    }
    const auto x = std::clamp(position.x / cellSize_, 0, width_ - 1);
    const auto y = std::clamp(position.y / cellSize_, 0, height_ - 1);
    return cells_[offset(x, y)];
}

Position InfluenceMap::safestStep(
    const Position from,
    const Position toward,
    const bool flying,
    const bool avoidDetection) const noexcept {
    if (cells_.empty()) {
        return toward;
    }

    static constexpr std::array directions{
        Position{-1, -1}, Position{0, -1}, Position{1, -1}, Position{-1, 0},
        Position{0, 0}, Position{1, 0}, Position{-1, 1}, Position{0, 1},
        Position{1, 1},
    };
    auto best = from;
    auto bestScore = std::numeric_limits<double>::infinity();
    for (const auto direction : directions) {
        const Position candidate{
            std::clamp(from.x + direction.x * cellSize_, 0, width_ * cellSize_ - 1),
            std::clamp(from.y + direction.y * cellSize_, 0, height_ * cellSize_ - 1),
        };
        const auto influence = at(candidate);
        const auto threat = flying ? influence.airThreat : influence.groundThreat;
        const auto progress = distance(candidate, toward) / std::max(1.0, distance(from, toward));
        const auto detectionPenalty = avoidDetection
                                          ? static_cast<double>(influence.detection) * 6.0
                                          : 0.0;
        const auto score = static_cast<double>(threat) * 5.0 + detectionPenalty + progress -
                           static_cast<double>(influence.strategicValue) * 0.05;
        if (score < bestScore) {
            bestScore = score;
            best = candidate;
        }
    }
    return best;
}

std::size_t InfluenceMap::offset(const int x, const int y) const noexcept {
    return static_cast<std::size_t>(y * width_ + x);
}

void InfluenceMap::addThreat(const UnitSnapshot& unit, const Frame currentFrame) {
    const auto age = std::max(0, currentFrame - unit.lastSeen);
    const auto memoryConfidence = unit.visible || isBuilding(unit.kind)
                                      ? 1.0
                                      : std::exp(-static_cast<double>(age) / (24.0 * 12.0));
    if (memoryConfidence < 0.02) return;

    const auto addWeapon = [this, &unit, memoryConfidence](
                               const WeaponSnapshot& weapon,
                               const bool air) {
        if (weapon.damage <= 0 || (!weapon.targetsAir && !weapon.targetsGround)) {
            return;
        }
        const auto dps = static_cast<double>(weapon.damage * std::max(1, weapon.hits)) /
                         static_cast<double>(std::max(1, weapon.cooldown));
        const auto radius = std::max(cellSize_, weapon.maxRange + cellSize_ * 2);
        const auto minX = std::max(0, (unit.position.x - radius) / cellSize_);
        const auto maxX = std::min(width_ - 1, (unit.position.x + radius) / cellSize_);
        const auto minY = std::max(0, (unit.position.y - radius) / cellSize_);
        const auto maxY = std::min(height_ - 1, (unit.position.y + radius) / cellSize_);
        for (auto y = minY; y <= maxY; ++y) {
            for (auto x = minX; x <= maxX; ++x) {
                const Position center{x * cellSize_ + cellSize_ / 2,
                                      y * cellSize_ + cellSize_ / 2};
                const auto range = distance(center, unit.position);
                if (range > radius) {
                    continue;
                }
                const auto falloff = std::max(0.05, 1.0 - range / static_cast<double>(radius));
                auto& cell = cells_[offset(x, y)];
                auto& field = air ? cell.airThreat : cell.groundThreat;
                field += static_cast<float>(
                    dps * falloff * unit.healthFraction() * memoryConfidence);
            }
        }
    };

    addWeapon(unit.groundWeapon, false);
    addWeapon(unit.airWeapon, true);
    if (unit.role == UnitRole::detector || unit.kind == UnitKind::observer ||
        unit.kind == UnitKind::scienceVessel || unit.kind == UnitKind::overlord ||
        unit.kind == UnitKind::photonCannon ||
        unit.kind == UnitKind::missileTurret || unit.kind == UnitKind::sporeColony) {
        const auto radius = std::max(7 * 32, unit.sightRange);
        const auto minX = std::max(0, (unit.position.x - radius) / cellSize_);
        const auto maxX = std::min(width_ - 1, (unit.position.x + radius) / cellSize_);
        const auto minY = std::max(0, (unit.position.y - radius) / cellSize_);
        const auto maxY = std::min(height_ - 1, (unit.position.y + radius) / cellSize_);
        for (auto y = minY; y <= maxY; ++y) {
            for (auto x = minX; x <= maxX; ++x) {
                const Position center{x * cellSize_ + cellSize_ / 2,
                                      y * cellSize_ + cellSize_ / 2};
                if (distanceSquared(center, unit.position) > radius * radius) continue;
                auto& detection = cells_[offset(x, y)].detection;
                detection = std::max(
                    detection, static_cast<float>(memoryConfidence));
            }
        }
    }
}

}  // namespace astra
