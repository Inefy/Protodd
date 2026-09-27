#include "protodd/TacticalTargetModel.hpp"
#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>

namespace protodd {
namespace {
constexpr char magic[8]{'P', 'T', 'T', 'A', 'C', 'T', '1', '\0'};
constexpr std::uint32_t version = 1;
}

bool TacticalTargetModel::load(const std::filesystem::path& path) noexcept {
    valid_ = false;
    scoreCount_ = 0;
    comparisonCount_ = disagreementCount_ = 0;
    workerOverCombatCount_ = buildingOverCombatCount_ = 0;
    combatOverWorkerCount_ = combatOverBuildingCount_ = 0;
    threatAbandonedCount_ = 0;
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    char header[8]{};
    std::uint32_t fileVersion{};
    std::uint32_t featuresInFile{};
    input.read(header, sizeof(header));
    input.read(reinterpret_cast<char*>(&fileVersion), sizeof(fileVersion));
    input.read(reinterpret_cast<char*>(&featuresInFile), sizeof(featuresInFile));
    if (!input || !std::equal(std::begin(header), std::end(header), std::begin(magic)) ||
        fileVersion != version || featuresInFile != featureCount) return false;
    input.read(reinterpret_cast<char*>(weights_.data()),
               static_cast<std::streamsize>(sizeof(float) * weights_.size()));
    input.read(reinterpret_cast<char*>(typeBias_.data()),
               static_cast<std::streamsize>(sizeof(float) * typeBias_.size()));
    if (!input || input.peek() != std::char_traits<char>::eof()) return false;
    const auto reasonable = [](const float value) {
        return std::isfinite(value) && std::abs(value) <= 100.0F;
    };
    valid_ = std::ranges::all_of(weights_, reasonable) &&
             std::ranges::all_of(typeBias_, reasonable);
    return valid_;
}

std::array<float, TacticalTargetModel::featureCount> TacticalTargetModel::features(
    const UnitSnapshot& attacker, const UnitSnapshot& target,
    const std::span<const UnitSnapshot> candidates) const noexcept {
    int neighbours = 0;
    for (const auto& candidate : candidates) {
        if (candidate.id != target.id && candidate.visible && candidate.position.valid() &&
            distanceSquared(candidate.position, target.position) <= 96 * 96) ++neighbours;
    }
    return {
        static_cast<float>(std::min(1280.0, distance(attacker.position, target.position)) / 640.0),
        static_cast<float>(std::log1p(std::max(0, target.hitPoints)) / 8.0),
        static_cast<float>(std::log1p(std::max(0, target.shields)) / 8.0),
        attacker.orderTargetId == target.id ? 1.0F : 0.0F,
        static_cast<float>(std::min(8, neighbours) / 8.0),
    };
}

float TacticalTargetModel::score(const UnitSnapshot& attacker, const UnitSnapshot& target,
                                 const std::span<const UnitSnapshot> candidates) const noexcept {
    if (!valid_ || target.typeId < 0 || target.typeId >= static_cast<int>(typeCount))
        return -std::numeric_limits<float>::infinity();
    ++scoreCount_;
    const auto values = features(attacker, target, candidates);
    float result = typeBias_[static_cast<std::size_t>(target.typeId)];
    for (std::size_t i = 0; i < featureCount; ++i) result += weights_[i] * values[i];
    return result;
}

void TacticalTargetModel::observeComparison(
    const UnitSnapshot& attacker, const UnitSnapshot& modelChoice,
    const UnitSnapshot& heuristicChoice) const noexcept {
    if (!valid_) return;
    ++comparisonCount_;
    if (modelChoice.id == heuristicChoice.id) return;
    ++disagreementCount_;
    if (isCombatUnit(heuristicChoice.kind) && isWorker(modelChoice.kind))
        ++workerOverCombatCount_;
    if (isCombatUnit(heuristicChoice.kind) && isBuilding(modelChoice.kind))
        ++buildingOverCombatCount_;
    if (isWorker(heuristicChoice.kind) && isCombatUnit(modelChoice.kind))
        ++combatOverWorkerCount_;
    if (isBuilding(heuristicChoice.kind) && isCombatUnit(modelChoice.kind))
        ++combatOverBuildingCount_;
    if (heuristicChoice.canAttack(attacker) && !modelChoice.canAttack(attacker))
        ++threatAbandonedCount_;
}

}  // namespace protodd
