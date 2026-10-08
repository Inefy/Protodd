#pragma once

#include <array>
#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace protodd {

inline constexpr int psionicStormEnergyCost = 75;

struct TemplarMergePolicy {
    // Storm-capable or planned casters remain available to the spell package.
    // Without that package, templar energy has no current combat use.
    int maximumMergeEnergy{};
    // During an imminent fight, retain a second pair for the next Storm cycle.
    std::size_t minimumCasters{};
};

[[nodiscard]] constexpr TemplarMergePolicy templarMergePolicy(
    const bool stormPackageRelevant, const bool fightImminent) noexcept {
    if (!stormPackageRelevant) {
        return {std::numeric_limits<int>::max(), 2};
    }
    return {psionicStormEnergyCost - 1, fightImminent ? 4U : 2U};
}

// Select the two least-energized eligible casters. The caller supplies them
// in stable unit-ID order, which also makes ties deterministic.
template <typename CanMerge>
[[nodiscard]] inline std::optional<std::array<std::size_t, 2>> selectTemplarMergePair(
    const std::span<const int> energies,
    const TemplarMergePolicy policy,
    CanMerge&& canMerge) {
    if (policy.minimumCasters < 2 || energies.size() < policy.minimumCasters)
        return std::nullopt;

    std::vector<std::size_t> eligible;
    eligible.reserve(energies.size());
    for (std::size_t index = 0; index < energies.size(); ++index) {
        const auto energy = energies[index];
        if (energy < 0 || energy > policy.maximumMergeEnergy) continue;
        eligible.push_back(index);
    }
    if (eligible.size() < 2) return std::nullopt;
    std::ranges::sort(eligible, [&energies](const std::size_t left,
                                            const std::size_t right) {
        if (energies[left] != energies[right]) return energies[left] < energies[right];
        return left < right;
    });
    for (std::size_t first = 0; first + 1 < eligible.size(); ++first) {
        for (std::size_t second = first + 1; second < eligible.size(); ++second) {
            auto pair = std::array{eligible[first], eligible[second]};
            if (canMerge(pair[0], pair[1])) {
                std::ranges::sort(pair);
                return pair;
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<std::array<std::size_t, 2>> selectTemplarMergePair(
    const std::span<const int> energies,
    const TemplarMergePolicy policy) {
    return selectTemplarMergePair(energies, policy,
                                  [](const std::size_t, const std::size_t) {
                                      return true;
                                  });
}

}  // namespace protodd
