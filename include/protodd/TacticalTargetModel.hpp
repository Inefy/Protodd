#pragma once

#include "protodd/GameState.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace protodd {

// A small replay-trained target ranker. The combat controller still determines
// whether to attack, which targets are legal, and whether a shot is in range.
class TacticalTargetModel {
public:
    static constexpr std::size_t featureCount = 5;
    static constexpr std::size_t typeCount = 256;

    [[nodiscard]] bool load(const std::filesystem::path& path) noexcept;
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] std::uint64_t scoreCount() const noexcept { return scoreCount_; }
    [[nodiscard]] std::uint64_t comparisonCount() const noexcept { return comparisonCount_; }
    [[nodiscard]] std::uint64_t disagreementCount() const noexcept { return disagreementCount_; }
    [[nodiscard]] std::uint64_t workerOverCombatCount() const noexcept { return workerOverCombatCount_; }
    [[nodiscard]] std::uint64_t buildingOverCombatCount() const noexcept { return buildingOverCombatCount_; }
    [[nodiscard]] std::uint64_t combatOverWorkerCount() const noexcept { return combatOverWorkerCount_; }
    [[nodiscard]] std::uint64_t combatOverBuildingCount() const noexcept { return combatOverBuildingCount_; }
    [[nodiscard]] std::uint64_t threatAbandonedCount() const noexcept { return threatAbandonedCount_; }
    void observeComparison(const UnitSnapshot& attacker, const UnitSnapshot& modelChoice,
                           const UnitSnapshot& heuristicChoice) const noexcept;
    [[nodiscard]] std::array<float, featureCount> features(
        const UnitSnapshot& attacker, const UnitSnapshot& target,
        std::span<const UnitSnapshot> candidates) const noexcept;
    [[nodiscard]] float score(
        const UnitSnapshot& attacker, const UnitSnapshot& target,
        std::span<const UnitSnapshot> candidates) const noexcept;

private:
    std::array<float, featureCount> weights_{};
    std::array<float, typeCount> typeBias_{};
    bool valid_{};
    mutable std::uint64_t scoreCount_{};
    mutable std::uint64_t comparisonCount_{};
    mutable std::uint64_t disagreementCount_{};
    mutable std::uint64_t workerOverCombatCount_{};
    mutable std::uint64_t buildingOverCombatCount_{};
    mutable std::uint64_t combatOverWorkerCount_{};
    mutable std::uint64_t combatOverBuildingCount_{};
    mutable std::uint64_t threatAbandonedCount_{};
};

}  // namespace protodd
