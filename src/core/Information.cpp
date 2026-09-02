#include "astra/Information.hpp"

#include "astra/UnitCatalog.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace astra {
namespace {

constexpr auto index(const EnemyPlan plan) noexcept {
    return static_cast<std::size_t>(plan);
}

int count(const GameState& state, const UnitKind kind) {
    return static_cast<int>(std::ranges::count(state.enemy.units, kind, &UnitSnapshot::kind));
}

bool seen(const GameState& state, const UnitKind kind) {
    return std::ranges::any_of(state.enemy.units, [kind](const UnitSnapshot& unit) {
        return unit.kind == kind;
    });
}

double recencyWeight(const GameState& state, const UnitSnapshot& unit) {
    const auto age = std::max(0, state.frame - unit.lastSeen);
    return std::exp(-static_cast<double>(age) / (24.0 * 90.0));
}

Position homeAnchor(const GameState& state) {
    const auto depot = std::ranges::find_if(state.self.units, [](const UnitSnapshot& unit) {
        return unit.role == UnitRole::resourceDepot || unit.kind == UnitKind::nexus;
    });
    if (depot != state.self.units.end() && depot->position.valid()) return depot->position;

    const auto start = std::ranges::find_if(state.bases, [&state](const BaseSnapshot& base) {
        return base.startLocation && base.ownerId == state.self.id && base.center.valid();
    });
    if (start != state.bases.end()) return start->center;

    const auto owned = std::ranges::find_if(state.bases, [&state](const BaseSnapshot& base) {
        return base.ownerId == state.self.id && base.center.valid();
    });
    return owned != state.bases.end() ? owned->center : Position{-1, -1};
}

bool proxyStructure(const UnitKind kind) {
    return isBuilding(kind) && kind != UnitKind::resourceDepot &&
           kind != UnitKind::commandCenter && kind != UnitKind::hatchery &&
           kind != UnitKind::lair && kind != UnitKind::hive &&
           kind != UnitKind::nexus && kind != UnitKind::refinery &&
           kind != UnitKind::assimilator;
}

bool containStructure(const UnitKind kind) {
    return kind == UnitKind::photonCannon || kind == UnitKind::bunker ||
           kind == UnitKind::sunkenColony;
}

}  // namespace

OpponentModel::OpponentModel() {
    reset(Race::unknown);
}

void OpponentModel::reset(const Race enemyRace) {
    enemyRace_ = enemyRace;
    beliefs_.fill(1.0);
    beliefs_[index(EnemyPlan::unknown)] = 2.5;
    normalize();
    assessment_ = {};
    lastUpdate_ = -1;
}

void OpponentModel::update(const GameState& state) {
    if (state.frame == lastUpdate_) {
        return;
    }
    if (state.enemy.race != Race::unknown && state.enemy.race != enemyRace_) {
        reset(state.enemy.race);
    }
    lastUpdate_ = state.frame;

    Beliefs evidence{};
    evidence.fill(1.0);
    const auto minutes = static_cast<double>(state.frame) / (24.0 * 60.0);
    const auto enemyWorkers = std::ranges::count_if(
        state.enemy.units,
        [](const UnitSnapshot& unit) { return isWorker(unit.kind); });
    const auto enemyCombat = std::ranges::count_if(state.enemy.units, [](const UnitSnapshot& unit) {
        return isCombatUnit(unit.kind);
    });
    const auto enemyBases = count(state, UnitKind::commandCenter) +
                            count(state, UnitKind::hatchery) + count(state, UnitKind::lair) +
                            count(state, UnitKind::hive) + count(state, UnitKind::nexus);
    const auto anchor = homeAnchor(state);
    const auto nearMain = [&anchor](const UnitSnapshot& unit, const int radius) {
        return anchor.valid() && unit.position.valid() &&
               distanceSquared(unit.position, anchor) < radius * radius;
    };
    const auto workersNearUs = std::ranges::count_if(
        state.enemy.units, [&nearMain](const UnitSnapshot& unit) {
            return unit.visible && isWorker(unit.kind) && nearMain(unit, 704);
        });
    const auto proxyBuildings = std::ranges::count_if(
        state.enemy.units, [&nearMain](const UnitSnapshot& unit) {
            return unit.visible && proxyStructure(unit.kind) && nearMain(unit, 1248);
        });
    const auto proxyStatic = std::ranges::count_if(
        state.enemy.units, [&nearMain](const UnitSnapshot& unit) {
            return unit.visible && containStructure(unit.kind) && nearMain(unit, 1248);
        });

    if (minutes < 4.0 && enemyWorkers >= 3) {
        evidence[index(EnemyPlan::workerRush)] += static_cast<double>(workersNearUs) * 2.5;
    }

    if (minutes < 6.0) {
        evidence[index(EnemyPlan::proxyRush)] += static_cast<double>(proxyBuildings) * 3.0;
        evidence[index(EnemyPlan::staticContain)] += static_cast<double>(proxyStatic) * 5.0;
    }

    const auto rushUnits = count(state, UnitKind::zergling) + count(state, UnitKind::marine) +
                           count(state, UnitKind::zealot);
    if (minutes < 7.0) {
        evidence[index(EnemyPlan::fastRush)] += rushUnits * 0.7;
        evidence[index(EnemyPlan::heavyPressure)] += static_cast<double>(enemyCombat) * 0.25;
    } else if (enemyCombat >= 10) {
        evidence[index(EnemyPlan::heavyPressure)] += static_cast<double>(enemyCombat) * 0.08;
    }

    if (enemyBases >= 2 && minutes < 8.0) {
        evidence[index(EnemyPlan::fastExpand)] += 5.0;
    }

    const auto advancedTech = seen(state, UnitKind::templarArchives) ||
                              seen(state, UnitKind::roboticsSupportBay) ||
                              seen(state, UnitKind::fleetBeacon) ||
                              seen(state, UnitKind::starport) ||
                              seen(state, UnitKind::scienceVessel) ||
                              seen(state, UnitKind::lair) || seen(state, UnitKind::hive);
    if (advancedTech && minutes < 10.0) {
        evidence[index(EnemyPlan::fastTech)] += 3.5;
    }

    const auto airCount = count(state, UnitKind::wraith) + count(state, UnitKind::mutalisk) +
                          count(state, UnitKind::scout) + count(state, UnitKind::corsair) +
                          count(state, UnitKind::carrier) + count(state, UnitKind::battlecruiser);
    if (seen(state, UnitKind::spire) || seen(state, UnitKind::stargate) ||
        seen(state, UnitKind::starport)) {
        evidence[index(EnemyPlan::airTech)] += 2.0;
    }
    evidence[index(EnemyPlan::airTech)] += airCount * 1.2;

    const auto cloakUnits = count(state, UnitKind::darkTemplar) + count(state, UnitKind::lurker) +
                            count(state, UnitKind::wraith);
    if (seen(state, UnitKind::templarArchives) || seen(state, UnitKind::hydraliskDen) ||
        seen(state, UnitKind::starport)) {
        evidence[index(EnemyPlan::cloakedTech)] += 0.8;
    }
    evidence[index(EnemyPlan::cloakedTech)] += cloakUnits * 2.2;

    // A tempered Bayesian update retains prior knowledge without becoming
    // permanently certain after a single scout observation.
    for (std::size_t i = 0; i < beliefs_.size(); ++i) {
        beliefs_[i] = std::pow(std::max(0.0001, beliefs_[i]), 0.82) * evidence[i];
    }
    normalize();

    double armyValue = 0.0;
    double visibleValue = 0.0;
    for (const auto& unit : state.enemy.units) {
        const auto value = unitStats(unit.kind).combatValue;
        armyValue += value * recencyWeight(state, unit);
        if (unit.visible) {
            visibleValue += value;
        }
    }

    assessment_.mostLikely = mostLikelyPlan();
    assessment_.workerRush = probability(EnemyPlan::workerRush);
    assessment_.proxy = probability(EnemyPlan::proxyRush);
    assessment_.staticContain = probability(EnemyPlan::staticContain);
    const auto localCombat = std::ranges::count_if(
        state.enemy.units, [&nearMain](const UnitSnapshot& unit) {
            return unit.visible && !unit.flying && isCombatUnit(unit.kind) &&
                   nearMain(unit, 896);
        });
    assessment_.enemiesNearMain = static_cast<int>(workersNearUs + proxyBuildings +
                                                   localCombat);
    const auto localPressure = std::clamp(
        static_cast<double>(workersNearUs) * 0.09 +
            static_cast<double>(proxyBuildings) * 0.14 +
            static_cast<double>(localCombat) * 0.12,
        0.0, 1.0);
    assessment_.immediateGround = std::clamp(
        std::max(localPressure,
                 probability(EnemyPlan::workerRush) + probability(EnemyPlan::proxyRush) +
                     probability(EnemyPlan::staticContain) +
                     probability(EnemyPlan::fastRush)),
        0.0, 1.0);
    assessment_.air = std::clamp(probability(EnemyPlan::airTech) + airCount * 0.05, 0.0, 1.0);
    assessment_.cloak = std::clamp(probability(EnemyPlan::cloakedTech) + cloakUnits * 0.08,
                                    0.0, 1.0);
    assessment_.aggression = std::clamp(
        assessment_.immediateGround + probability(EnemyPlan::heavyPressure) * 0.7,
        0.0, 1.0);
    assessment_.expansion = probability(EnemyPlan::fastExpand);
    assessment_.estimatedArmyValue = std::max(visibleValue, armyValue);

    const auto entropy = -std::accumulate(
        beliefs_.begin(), beliefs_.end(), 0.0,
        [](const double sum, const double probability) {
            return probability > 0.0 ? sum + probability * std::log(probability) : sum;
        });
    assessment_.uncertainty = std::clamp(entropy / std::log(static_cast<double>(beliefs_.size())),
                                         0.0, 1.0);
}

double OpponentModel::probability(const EnemyPlan plan) const noexcept {
    return beliefs_[index(plan)];
}

const ThreatAssessment& OpponentModel::assessment() const noexcept {
    return assessment_;
}

EnemyPlan OpponentModel::mostLikelyPlan() const noexcept {
    const auto found = std::max_element(beliefs_.begin() + 1, beliefs_.end());
    return static_cast<EnemyPlan>(std::distance(beliefs_.begin(), found));
}

void OpponentModel::normalize() noexcept {
    const auto total = std::accumulate(beliefs_.begin(), beliefs_.end(), 0.0);
    if (total <= 0.0) {
        beliefs_.fill(1.0 / static_cast<double>(beliefs_.size()));
        return;
    }
    for (auto& belief : beliefs_) {
        belief /= total;
    }
}

std::string_view enemyPlanName(const EnemyPlan plan) noexcept {
    switch (plan) {
        case EnemyPlan::unknown: return "Unknown";
        case EnemyPlan::workerRush: return "WorkerRush";
        case EnemyPlan::proxyRush: return "ProxyRush";
        case EnemyPlan::staticContain: return "StaticContain";
        case EnemyPlan::fastRush: return "FastRush";
        case EnemyPlan::heavyPressure: return "HeavyPressure";
        case EnemyPlan::fastExpand: return "FastExpand";
        case EnemyPlan::fastTech: return "FastTech";
        case EnemyPlan::airTech: return "AirTech";
        case EnemyPlan::cloakedTech: return "CloakedTech";
        case EnemyPlan::count: break;
    }
    return "Invalid";
}

}  // namespace astra
