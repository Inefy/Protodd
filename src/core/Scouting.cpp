#include "astra/Scouting.hpp"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace astra {
namespace {

struct Candidate {
    Position position;
    ScoutPurpose purpose;
    double value;
};

}  // namespace

std::vector<ScoutOrder> ScoutManager::assign(
    const GameState& state,
    const std::span<const UnitId> availableScouts,
    const InfluenceMap& influence) const {
    std::vector<Candidate> candidates;
    candidates.reserve(state.bases.size() + 2);
    const auto enemyDepot = std::ranges::find_if(state.enemy.units, [](const UnitSnapshot& unit) {
        return unit.role == UnitRole::resourceDepot;
    });

    for (const auto& base : state.bases) {
        if (!base.center.valid() || base.ownerId == state.self.id) {
            continue;
        }
        const auto staleSeconds = std::max(0, state.frame - base.lastScouted) / 24.0;
        auto value = std::min(12.0, 1.0 + staleSeconds / 15.0);
        auto purpose = ScoutPurpose::checkExpansion;
        if (base.startLocation && enemyDepot == state.enemy.units.end()) {
            value += 12.0;
            purpose = ScoutPurpose::findEnemy;
        }
        if (base.ownerId == state.enemy.id) {
            value += 5.0;
            purpose = ScoutPurpose::checkTech;
        }
        candidates.push_back({base.center, purpose, value});
    }
    if (enemyDepot != state.enemy.units.end()) {
        candidates.push_back({enemyDepot->position, ScoutPurpose::checkTech, 9.0});
    }

    std::unordered_set<std::size_t> claimed;
    std::vector<ScoutOrder> orders;
    orders.reserve(availableScouts.size());
    for (const auto scoutId : availableScouts) {
        const auto scout = state.findUnit(scoutId);
        if (!scout) {
            continue;
        }
        std::size_t bestIndex = 0;
        auto bestScore = -std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (claimed.contains(i)) {
                continue;
            }
            const auto threat = influence.at(candidates[i].position);
            const auto risk = scout->flying ? threat.airThreat : threat.groundThreat;
            const auto travel = distance(scout->position, candidates[i].position) / 1000.0;
            const auto score = candidates[i].value - static_cast<double>(risk) * 3.0 - travel;
            if (score > bestScore) {
                bestScore = score;
                bestIndex = i;
            }
        }
        if (!candidates.empty() && !claimed.contains(bestIndex)) {
            claimed.insert(bestIndex);
            orders.push_back({scoutId, candidates[bestIndex].position,
                              candidates[bestIndex].purpose, bestScore});
        }
    }
    return orders;
}

}  // namespace astra
