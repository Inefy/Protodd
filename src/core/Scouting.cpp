#include "astra/Scouting.hpp"

#include "astra/UnitCatalog.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

namespace astra {
namespace {

struct Candidate {
    Position position;
    ScoutPurpose purpose;
    double value;
};

double routeRisk(
    const InfluenceMap& influence,
    const Position from,
    const Position to,
    const bool flying) {
    constexpr auto samples = 8;
    auto average = 0.0;
    auto peak = 0.0;
    for (auto step = 1; step <= samples; ++step) {
        const auto ratio = static_cast<double>(step) / static_cast<double>(samples);
        const Position point{
            from.x + static_cast<int>(std::lround((to.x - from.x) * ratio)),
            from.y + static_cast<int>(std::lround((to.y - from.y) * ratio)),
        };
        const auto cell = influence.at(point);
        const auto risk = static_cast<double>(flying ? cell.airThreat : cell.groundThreat);
        average += risk;
        peak = std::max(peak, risk);
    }
    return average / static_cast<double>(samples) + peak * 0.65;
}

Position friendlyMain(const GameState& state) {
    const auto depot = std::ranges::find_if(state.self.units, [](const UnitSnapshot& unit) {
        return unit.role == UnitRole::resourceDepot || unit.kind == UnitKind::nexus;
    });
    if (depot != state.self.units.end()) return depot->position;
    const auto base = std::ranges::find_if(state.bases, [&state](const BaseSnapshot& candidate) {
        return candidate.ownerId == state.self.id && candidate.center.valid();
    });
    return base != state.bases.end() ? base->center : Position{-1, -1};
}

}  // namespace

UnitId selectOpeningWorkerScout(
    const GameState& state,
    const std::span<const UnitId> previousScouts,
    const std::span<const UnitId> unavailableWorkers) noexcept {
    const auto enemyLocated = std::ranges::any_of(
        state.enemy.units, [](const UnitSnapshot& unit) {
            return unit.role == UnitRole::resourceDepot;
        });
    const auto enemyArmySeen = std::ranges::any_of(
        state.enemy.units, [](const UnitSnapshot& unit) {
            return unit.completed && isCombatUnit(unit.kind);
        });
    if (state.frame >= 6 * 60 * 24 || enemyLocated || enemyArmySeen ||
        std::ranges::none_of(state.self.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::pylon;
        })) {
        return -1;
    }

    const auto eligible = [&unavailableWorkers](const UnitSnapshot& unit) {
        return unit.kind == UnitKind::probe && unit.completed &&
               !unit.carryingResources && !unit.underAttack &&
               std::ranges::find(unavailableWorkers, unit.id) ==
                   unavailableWorkers.end();
    };
    for (const auto previous : previousScouts) {
        const auto candidate = std::ranges::find(
            state.self.units, previous, &UnitSnapshot::id);
        if (candidate != state.self.units.end() && eligible(*candidate)) {
            return candidate->id;
        }
    }

    const UnitSnapshot* selected = nullptr;
    for (const auto& unit : state.self.units) {
        if (eligible(unit) && (selected == nullptr || unit.id < selected->id)) {
            selected = &unit;
        }
    }
    return selected != nullptr ? selected->id : -1;
}

void ScoutManager::reset() noexcept {
    previousOrders_.clear();
}

std::vector<ScoutOrder> ScoutManager::assign(
    const GameState& state,
    const std::span<const UnitId> availableScouts,
    const InfluenceMap& influence,
    const ThreatAssessment& threat) {
    std::vector<Candidate> candidates;
    candidates.reserve(state.bases.size() + 4);
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
            value += state.frame < 5 * 60 * 24 ? 18.0 : 12.0;
            purpose = ScoutPurpose::findEnemy;
        }
        if (base.ownerId == state.enemy.id) {
            value += 5.0 + threat.uncertainty * 5.0 +
                     std::max(threat.air, threat.cloak) * 4.0;
            purpose = ScoutPurpose::checkTech;
        } else if (base.ownerId == -1) {
            value += threat.expansion * 8.0;
        }
        candidates.push_back({base.center, purpose, value});
    }
    if (enemyDepot != state.enemy.units.end()) {
        candidates.push_back({enemyDepot->position, ScoutPurpose::checkTech,
                              8.0 + threat.uncertainty * 7.0 +
                                  std::max(threat.air, threat.cloak) * 5.0});
    }

    long long armyX = 0;
    long long armyY = 0;
    auto armyCount = 0;
    for (const auto& enemy : state.enemy.units) {
        if (!isCombatUnit(enemy.kind) || !enemy.position.valid() ||
            state.frame - enemy.lastSeen > 20 * 24) {
            continue;
        }
        armyX += enemy.position.x;
        armyY += enemy.position.y;
        ++armyCount;
    }
    if (armyCount > 0) {
        candidates.push_back({
            {static_cast<int>(armyX / armyCount), static_cast<int>(armyY / armyCount)},
            ScoutPurpose::watchArmy,
            5.0 + threat.aggression * 9.0,
        });
    }

    const auto enemyTransport = std::ranges::find_if(
        state.enemy.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::shuttle || unit.kind == UnitKind::dropship;
        });
    const auto ourMain = friendlyMain(state);
    if (enemyTransport != state.enemy.units.end() && ourMain.valid()) {
        candidates.push_back({
            {(enemyTransport->position.x + ourMain.x) / 2,
             (enemyTransport->position.y + ourMain.y) / 2},
            ScoutPurpose::patrolDropPath,
            10.0 + threat.air * 5.0,
        });
    }

    std::unordered_set<std::size_t> claimed;
    std::vector<ScoutOrder> orders;
    std::unordered_map<UnitId, ScoutOrder> nextOrders;
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
            const auto risk = routeRisk(influence, scout->position,
                                        candidates[i].position, scout->flying);
            const auto travel = distance(scout->position, candidates[i].position) / 1000.0;
            const auto riskWeight = scout->kind == UnitKind::probe
                                        ? 8.0
                                        : (scout->kind == UnitKind::observer ? 3.0 : 1.8);
            auto score = candidates[i].value - risk * riskWeight - travel;
            const auto previous = previousOrders_.find(scoutId);
            if (previous != previousOrders_.end() &&
                previous->second.purpose == candidates[i].purpose &&
                distanceSquared(previous->second.target, candidates[i].position) < 128 * 128 &&
                distanceSquared(scout->position, candidates[i].position) > 112 * 112) {
                score += 3.0;
            }
            if (score > bestScore) {
                bestScore = score;
                bestIndex = i;
            }
        }
        if (!candidates.empty() && !claimed.contains(bestIndex)) {
            claimed.insert(bestIndex);
            ScoutOrder order{scoutId, candidates[bestIndex].position,
                             candidates[bestIndex].purpose, bestScore};
            orders.push_back(order);
            nextOrders.emplace(scoutId, order);
        }
    }
    previousOrders_ = std::move(nextOrders);
    return orders;
}

}  // namespace astra
