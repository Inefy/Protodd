#include "astra/Strategy.hpp"

#include "astra/UnitCatalog.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace astra {
namespace {

int count(const GameState& state, const UnitKind kind, const bool completedOnly = false) {
    return static_cast<int>(std::ranges::count_if(
        state.self.units,
        [kind, completedOnly](const UnitSnapshot& unit) {
            return unit.kind == kind && (!completedOnly || unit.completed);
        }));
}

int countRole(const GameState& state, const UnitRole role) {
    return static_cast<int>(std::ranges::count(state.self.units, role, &UnitSnapshot::role));
}

int minute(const GameState& state) {
    return state.frame / (24 * 60);
}

bool supplyAtLeast(const GameState& state, const int displayedSupply) {
    return state.self.supplyUsed >= displayedSupply * 2;
}

int recentEnemyCount(
    const GameState& state,
    const UnitKind kind,
    const Frame memory = 90 * 24) {
    return static_cast<int>(std::ranges::count_if(
        state.enemy.units, [kind, memory, &state](const UnitSnapshot& unit) {
            return unit.kind == kind && unit.completed &&
                   (unit.visible || state.frame - unit.lastSeen <= memory);
        }));
}

void setCompositionWeight(
    StrategicPlan& plan,
    const UnitKind kind,
    const double minimumWeight) {
    const auto existing = std::ranges::find(plan.composition, kind,
                                             &CompositionTarget::kind);
    if (existing == plan.composition.end()) {
        plan.composition.push_back({kind, minimumWeight});
    } else {
        existing->weight = std::max(existing->weight, minimumWeight);
    }
}

void normalizeComposition(StrategicPlan& plan) {
    const auto total = std::accumulate(
        plan.composition.begin(), plan.composition.end(), 0.0,
        [](const double sum, const CompositionTarget& target) {
            return sum + std::max(0.0, target.weight);
        });
    if (total <= 0.0) return;
    for (auto& target : plan.composition) target.weight /= total;
}

void goal(
    StrategicPlan& plan,
    const GoalKind kind,
    const UnitKind target,
    const int desired,
    const int priority,
    const std::string_view reason,
    const bool blocking = false) {
    plan.goals.push_back({kind, target, desired, priority, blocking, std::string(reason)});
}

void technologyGoal(
    StrategicPlan& plan,
    const TechnologyKind technology,
    const int desiredLevel,
    const int priority,
    const std::string_view reason,
    const bool blocking = false) {
    const auto kind = technology == TechnologyKind::psionicStorm ||
                              technology == TechnologyKind::stasisField ||
                              technology == TechnologyKind::recall
                          ? GoalKind::research
                          : GoalKind::upgrade;
    plan.goals.push_back({kind, UnitKind::unknown, desiredLevel, priority, blocking,
                          std::string(reason), technology});
}

Position ourMain(const GameState& state) {
    const auto nexus = std::ranges::find(state.self.units, UnitKind::nexus, &UnitSnapshot::kind);
    return nexus != state.self.units.end() ? nexus->position : Position{-1, -1};
}

Position enemyMain(const GameState& state) {
    const auto depot = std::ranges::find_if(state.enemy.units, [](const UnitSnapshot& unit) {
        return unit.role == UnitRole::resourceDepot;
    });
    if (depot != state.enemy.units.end()) {
        return depot->position;
    }

    // Keep attacking known structures after the last depot falls. This avoids
    // the common cleanup failure where an army returns home while a tech
    // building survives elsewhere on the map.
    const auto building = std::ranges::find_if(state.enemy.units, [](const UnitSnapshot& unit) {
        return isBuilding(unit.kind) && unit.position.valid();
    });
    if (building != state.enemy.units.end()) return building->position;

    // Before the enemy start is confirmed, search the stalest plausible start.
    // Explicitly exclude our own start; the previous ownerId != -1 fallback
    // selected Astra's main as soon as its Nexus was observed.
    const BaseSnapshot* candidate = nullptr;
    auto oldest = std::numeric_limits<Frame>::max();
    for (const auto& base : state.bases) {
        if (!base.startLocation || !base.center.valid() || base.ownerId == state.self.id) continue;
        if (base.ownerId == state.enemy.id) return base.center;
        if (base.ownerId == -1 && base.lastScouted < oldest) {
            oldest = base.lastScouted;
            candidate = &base;
        }
    }
    if (candidate != nullptr) return candidate->center;

    const auto visibleTarget = std::ranges::find_if(
        state.enemy.units, [](const UnitSnapshot& unit) {
            return unit.visible && unit.position.valid();
        });
    if (visibleTarget != state.enemy.units.end()) return visibleTarget->position;

    // Finally sweep stale non-owned expansions to reveal hidden buildings.
    oldest = std::numeric_limits<Frame>::max();
    for (const auto& base : state.bases) {
        if (!base.center.valid() || base.ownerId == state.self.id) continue;
        if (base.lastScouted < oldest) {
            oldest = base.lastScouted;
            candidate = &base;
        }
    }
    return candidate != nullptr ? candidate->center : Position{-1, -1};
}

}  // namespace

StrategicPlan StrategyEngine::plan(
    const GameState& state,
    const ThreatAssessment& threat,
    const OpeningStyle style) const {
    StrategicPlan result;
    switch (state.enemy.race) {
        case Race::terran: result = planPvT(state, threat); break;
        case Race::zerg: result = planPvZ(state, threat); break;
        case Race::protoss: result = planPvP(state, threat); break;
        case Race::unknown:
        case Race::random:
            result = planPvP(state, threat);
            result.name = "Safe one-gate core versus unknown";
            break;
    }

    const auto home = ourMain(state);
    result.attackTarget = enemyMain(state);
    // Stage the army in front of the economy instead of on top of the Nexus.
    // A Nexus rally caused melee rushes to make first contact inside the Probe
    // line, where even a numerically adequate army could not form a surround.
    result.rallyPoint = home.valid() && result.attackTarget.valid()
                            ? moveToward(home, result.attackTarget, 160.0)
                            : home;
    addInfrastructure(result, state, threat);
    applyOpeningStyle(result, state, style);
    addAdaptiveCounters(result, state);
    addEconomicRecovery(result, state);
    // Safety runs last so an opponent-specific economic style cannot override
    // direct evidence of an all-in at our main.
    addSafetyReactions(result, threat);

    // Never keep producing workers for bases that the current plan has
    // explicitly postponed. This bounds one-base saturation while preserving
    // enough workers to fund production and a prompt expansion.
    result.desiredWorkers = std::min(
        result.desiredWorkers, std::max(14, result.desiredBases * 22));

    std::ranges::stable_sort(result.goals, std::greater{}, &ProductionGoal::priority);
    return result;
}

StrategicPlan StrategyEngine::planPvT(
    const GameState& state,
    const ThreatAssessment& threat) const {
    StrategicPlan result;
    result.name = "PvT one-gate observer expansion";
    result.desiredWorkers = std::min(72, 22 + minute(state) * 4);
    result.desiredBases = minute(state) < 5 ? 1 : (minute(state) < 11 ? 2 : 3);
    result.desiredGasWorkers = !supplyAtLeast(state, 11) ? 0 :
                               (minute(state) < 7 ? 3 :
                                (minute(state) < 12 ? 6 : 9));
    result.posture = minute(state) < 6 ? Posture::hold : Posture::pressure;
    result.attackThreshold = 1.32;
    result.composition = {{UnitKind::dragoon, 0.55}, {UnitKind::zealot, 0.22},
                          {UnitKind::highTemplar, 0.13}, {UnitKind::arbiter, 0.10}};

    if (supplyAtLeast(state, 9)) {
        goal(result, GoalKind::build, UnitKind::gateway, minute(state) < 6 ? 1 : 3, 88,
             "nine-supply gateway", count(state, UnitKind::gateway) == 0);
    }
    if (supplyAtLeast(state, 10)) {
        goal(result, GoalKind::train, UnitKind::zealot, 1, 94,
             "opening bodyguard before vulnerable dragoon tech",
             count(state, UnitKind::zealot) == 0);
    }
    if (supplyAtLeast(state, 13)) {
        goal(result, GoalKind::build, UnitKind::cyberneticsCore, 1, 92,
             "thirteen-supply cybernetics core", true);
    }
    if (supplyAtLeast(state, 14)) {
        goal(result, GoalKind::train, UnitKind::dragoon, std::max(3, minute(state) * 2), 91,
             "range control against Terran");
        technologyGoal(result, TechnologyKind::singularityCharge, 1, 84,
                       "range completes after the first defensive dragoons");
    }
    if (count(state, UnitKind::dragoon) >= 3 || supplyAtLeast(state, 24)) {
        goal(result, GoalKind::build, UnitKind::roboticsFacility, 1, 78,
             "observers against mines and tech scouting");
        goal(result, GoalKind::build, UnitKind::observatory, 1, 77, "observer access");
        goal(result, GoalKind::train, UnitKind::observer, minute(state) < 12 ? 2 : 4, 84,
             "mine detection and army tracking");
    }

    if (minute(state) >= 10) {
        goal(result, GoalKind::build, UnitKind::citadelOfAdun, 1, 68, "zealot speed path");
        goal(result, GoalKind::build, UnitKind::templarArchives, 1, 64,
             "storm and late-game composition");
        goal(result, GoalKind::train, UnitKind::highTemplar, 4, 60,
             "storm clustered bio and support tanks");
        technologyGoal(result, TechnologyKind::legEnhancements, 1, 69,
                       "speed closes on siege lines");
        technologyGoal(result, TechnologyKind::psionicStorm, 1, 67,
                       "enable templar before mass production", true);
    }
    if (minute(state) >= 15 && threat.air < 0.35) {
        goal(result, GoalKind::build, UnitKind::arbiterTribunal, 1, 52,
             "stasis and recall transition");
        goal(result, GoalKind::train, UnitKind::arbiter, 2, 50, "late-game control");
        technologyGoal(result, TechnologyKind::stasisField, 1, 56,
                       "neutralize clustered siege armies");
        technologyGoal(result, TechnologyKind::recall, 1, 48,
                       "create a late-game positional threat");
    }
    if (minute(state) >= 7) {
        const auto weaponLevel = minute(state) >= 18 ? 3 : (minute(state) >= 12 ? 2 : 1);
        technologyGoal(result, TechnologyKind::protossGroundWeapons, weaponLevel, 63,
                       "scale the core ground army");
    }
    if (minute(state) >= 13) {
        technologyGoal(result, TechnologyKind::khaydarinAmulet, 1, 54,
                       "increase storm availability");
        technologyGoal(result, TechnologyKind::protossGroundArmor,
                       minute(state) >= 19 ? 2 : 1, 51,
                       "improve zealot durability");
    }
    if (threat.combatEnemiesNearMain > 0 || threat.approachingArmyValue >= 2.0 ||
        (minute(state) < 8 && threat.aggression > 0.62)) {
        result.name = "PvT anti-pressure hold";
        result.posture = Posture::defend;
        result.desiredBases = minute(state) < 8 ? 1 : std::min(result.desiredBases, 2);
        result.desiredWorkers = std::min(result.desiredWorkers, 14);
        result.attackThreshold = 1.55;
        goal(result, GoalKind::build, UnitKind::gateway, 2, 99,
             "add emergency anti-pressure throughput", true);
        goal(result, GoalKind::train, UnitKind::zealot, 4, 98,
             "field bodies before vulnerable dragoon tech", true);
        goal(result, GoalKind::train, UnitKind::dragoon, 8, 97,
             "survive detected pressure", true);
        goal(result, GoalKind::build, UnitKind::shieldBattery, 1, 90,
             "front-line sustain");
    }
    return result;
}

StrategicPlan StrategyEngine::planPvZ(
    const GameState& state,
    const ThreatAssessment& threat) const {
    StrategicPlan result;
    result.name = "PvZ forge expansion into corsair-templar";
    result.desiredWorkers = std::min(70, 20 + minute(state) * 4);
    result.desiredBases = minute(state) < 3 ? 1 : (minute(state) < 11 ? 2 : 3);
    result.desiredGasWorkers = !supplyAtLeast(state, 14) ? 0 :
                               (minute(state) < 8 ? 3 :
                                (minute(state) < 12 ? 6 : 9));
    result.posture = minute(state) < 8 ? Posture::hold : Posture::harass;
    result.attackThreshold = 1.2;
    result.composition = {{UnitKind::zealot, 0.35}, {UnitKind::dragoon, 0.12},
                          {UnitKind::highTemplar, 0.25}, {UnitKind::corsair, 0.18},
                          {UnitKind::archon, 0.10}};

    // A pool-first Zerg can make contact before a conventional Gateway army
    // has enough surface area. Pause briefly at eight workers and establish a
    // static anchor; resume Probe growth as soon as either that anchor or two
    // Zealots are complete. This remains safe even when the first scout dies.
    if (minute(state) < 4 && count(state, UnitKind::zealot, true) < 2 &&
        count(state, UnitKind::photonCannon, true) == 0) {
        result.desiredWorkers = std::min(result.desiredWorkers, 8);
    }

    if (supplyAtLeast(state, 10) && count(state, UnitKind::gateway) >= 2 &&
        count(state, UnitKind::zealot) >= 2) {
        goal(result, GoalKind::build, UnitKind::forge, 1, 82,
             "forge after two-gate opening safety");
    }
    if (minute(state) >= 4 && threat.immediateGround <= 0.45 &&
        count(state, UnitKind::forge) > 0) {
        technologyGoal(result, TechnologyKind::protossGroundWeapons, 1, 87,
                       "zealot attack timing");
    }
    const auto defensiveBases = std::max(1, count(state, UnitKind::nexus));
    const auto openingGroundSafe = count(state, UnitKind::gateway) >= 2 &&
                                   count(state, UnitKind::zealot) >= 3;
    const auto canCommitToForge = count(state, UnitKind::forge) > 0 ||
                                  openingGroundSafe || minute(state) >= 4;
    if (canCommitToForge &&
        (defensiveBases >= 2 || threat.immediateGround > 0.25 || threat.air > 0.35)) {
        const auto safetyCannons = defensiveBases + (threat.air > 0.45 ? 2 : 0);
        goal(result, GoalKind::build, UnitKind::photonCannon,
             std::min(6, safetyCannons), 89, "ling and mutalisk coverage");
    }
    if (supplyAtLeast(state, 7)) {
        goal(result, GoalKind::build, UnitKind::forge, 1, 100,
             "fortified PvZ opening anchor", true);
        goal(result, GoalKind::build, UnitKind::photonCannon, 1, 99,
             "baseline anti-ling safety before economic commitment", true);
        const auto openingGateways = supplyAtLeast(state, 8) ? 2 : 1;
        goal(result, GoalKind::build, UnitKind::gateway,
             minute(state) < 8 ? openingGateways : 4,
             openingGateways == 1 ? 98 : 97,
             "seven-supply gateway into two-gate Zerg safety",
             count(state, UnitKind::gateway) < openingGateways);
        goal(result, GoalKind::train, UnitKind::zealot,
             std::max(4, minute(state)), 99,
             "opening defenders before Forge economy",
             count(state, UnitKind::zealot) < 3);
    }
    if (supplyAtLeast(state, 15)) {
        goal(result, GoalKind::build, UnitKind::cyberneticsCore, 1, 82,
             "air and dragoon access");
    }
    if (supplyAtLeast(state, 22)) {
        goal(result, GoalKind::build, UnitKind::stargate, 1, 76,
             "overlord denial and scouting");
        goal(result, GoalKind::train, UnitKind::corsair, threat.air > 0.45 ? 7 : 4, 75,
             "air superiority");
    }
    if (supplyAtLeast(state, 28)) {
        goal(result, GoalKind::build, UnitKind::citadelOfAdun, 1, 73,
             "speed and templar path");
        goal(result, GoalKind::build, UnitKind::templarArchives, 1, 71,
             "storm versus Zerg mass");
        goal(result, GoalKind::train, UnitKind::highTemplar,
             std::max(2, minute(state) / 3), 72, "storm support");
    }

    if (minute(state) >= 8) {
        technologyGoal(result, TechnologyKind::legEnhancements, 1, 78,
                       "speed for surrounds and reinforcement");
        technologyGoal(result, TechnologyKind::psionicStorm, 1, 77,
                       "storm is the core anti-swarm tool", true);
    }
    if (minute(state) >= 11) {
        technologyGoal(result, TechnologyKind::khaydarinAmulet, 1, 65,
                       "sustain repeated storms");
        technologyGoal(result, TechnologyKind::protossAirWeapons, 1, 58,
                       "keep corsairs ahead of mutalisks");
    }

    if (threat.combatEnemiesNearMain > 0 || threat.approachingArmyValue >= 2.0 ||
        (minute(state) < 8 && threat.immediateGround > 0.45)) {
        result.name = "PvZ emergency gateway hold";
        result.posture = Posture::defend;
        result.desiredBases = 1;
        result.desiredGasWorkers = 0;
        result.desiredWorkers = std::min(result.desiredWorkers, 12);
        goal(result, GoalKind::build, UnitKind::gateway, 2, 99, "anti-rush production", true);
        goal(result, GoalKind::train, UnitKind::zealot, 8, 98, "hold early ground rush", true);
        if (minute(state) < 6) {
            goal(result, GoalKind::build, UnitKind::shieldBattery, 1, 96,
                 "sustain the anti-ling screen", true);
        }
        goal(result, GoalKind::build, UnitKind::photonCannon, 2, 100,
             "double mineral-line cover against the ground all-in");
        if (count(state, UnitKind::photonCannon, true) >= 2 &&
            count(state, UnitKind::probe) < 10) {
            goal(result, GoalKind::train, UnitKind::probe, 10, 100,
                 "recover mining behind completed static safety", true);
        }
        if (count(state, UnitKind::photonCannon, true) >= 2) {
            goal(result, GoalKind::build, UnitKind::pylon, 2, 100,
                 "secure reinforcement supply inside the Cannon shell", true);
        }
    } else if (minute(state) >= 3 &&
               (count(state, UnitKind::zealot) >= 2 ||
                count(state, UnitKind::photonCannon) >= 1)) {
        goal(result, GoalKind::expand, UnitKind::nexus, 2, 91,
             "forge-fast-expand timing");
    }
    return result;
}

StrategicPlan StrategyEngine::planPvP(
    const GameState& state,
    const ThreatAssessment& threat) const {
    StrategicPlan result;
    result.name = "PvP two-gate robotics control";
    result.desiredWorkers = std::min(66, 18 + minute(state) * 4);
    result.desiredBases = minute(state) < 6 ? 1 : (minute(state) < 12 ? 2 : 3);
    result.desiredGasWorkers = !supplyAtLeast(state, 11) ? 0 :
                               (minute(state) < 8 ? 3 :
                                (minute(state) < 12 ? 6 : 9));
    result.posture = minute(state) < 6 ? Posture::hold : Posture::pressure;
    result.attackThreshold = 1.18;
    result.composition = {{UnitKind::dragoon, 0.58}, {UnitKind::zealot, 0.14},
                          {UnitKind::reaver, 0.18}, {UnitKind::highTemplar, 0.10}};

    if (supplyAtLeast(state, 9)) {
        const auto gatewayTarget = supplyAtLeast(state, 11) ?
                                       (minute(state) < 9 ? 2 : 4) : 1;
        goal(result, GoalKind::build, UnitKind::gateway, gatewayTarget, 97,
             "nine-supply gateway into two-gate control",
             count(state, UnitKind::gateway) < gatewayTarget && gatewayTarget <= 2);
    }
    if (supplyAtLeast(state, 10)) {
        goal(result, GoalKind::train, UnitKind::zealot, 1, 98,
             "bank the first defender while the gateway completes", true);
    }
    if (count(state, UnitKind::gateway) >= 2) {
        goal(result, GoalKind::train, UnitKind::zealot, 3, 96,
             "fill secured opening production with defenders",
             count(state, UnitKind::zealot) < 2);
        if (count(state, UnitKind::zealot) >= 2) {
            goal(result, GoalKind::build, UnitKind::shieldBattery, 1, 93,
                 "sustain the two-gate defensive screen");
        }
    }
    if (supplyAtLeast(state, 13)) {
        const auto productionSecured = count(state, UnitKind::gateway) >= 2 &&
                                       count(state, UnitKind::zealot) >= 1;
        goal(result, GoalKind::build, UnitKind::cyberneticsCore, 1, 90,
             "dragoon access after opening production", productionSecured);
    }
    if (supplyAtLeast(state, 14)) {
        goal(result, GoalKind::train, UnitKind::dragoon, std::max(4, minute(state) * 2), 91,
             "core PvP army");
        technologyGoal(result, TechnologyKind::singularityCharge, 1, 86,
                       "range follows the first defensive dragoons");
    }
    if (count(state, UnitKind::dragoon) >= 3 || supplyAtLeast(state, 24)) {
        goal(result, GoalKind::build, UnitKind::roboticsFacility, 1, 85,
             "reaver pressure and detection");
        goal(result, GoalKind::build, UnitKind::observatory, 1, 83, "DT safety");
        goal(result, GoalKind::train, UnitKind::observer, 2, 88, "DT detection", true);
    }
    if (supplyAtLeast(state, 32)) {
        goal(result, GoalKind::build, UnitKind::roboticsSupportBay, 1, 69,
             "reaver access");
        goal(result, GoalKind::train, UnitKind::reaver, 2, 70, "area control");
        goal(result, GoalKind::train, UnitKind::shuttle, 1, 67, "reaver mobility");
    }

    if (minute(state) >= 8) {
        technologyGoal(result, TechnologyKind::protossGroundWeapons,
                       minute(state) >= 15 ? 2 : 1, 66,
                       "scale dragoon volleys");
    }
    if (minute(state) >= 10) {
        technologyGoal(result, TechnologyKind::reaverCapacity, 1, 64,
                       "increase reaver combat endurance");
        technologyGoal(result, TechnologyKind::scarabDamage, 1, 61,
                       "improve reaver breakpoints");
    }

    if (threat.combatEnemiesNearMain > 0 || threat.approachingArmyValue >= 2.0 ||
        (minute(state) < 8 && threat.aggression > 0.6)) {
        result.name = "PvP two-gate emergency defense";
        result.posture = Posture::defend;
        result.desiredBases = 1;
        result.desiredWorkers = std::min(result.desiredWorkers, 12);
        if (count(state, UnitKind::cyberneticsCore) == 0) {
            result.desiredGasWorkers = 0;
            result.goals.erase(
                std::remove_if(result.goals.begin(), result.goals.end(),
                               [](const ProductionGoal& candidate) {
                                   return candidate.target == UnitKind::cyberneticsCore ||
                                          candidate.target == UnitKind::roboticsFacility ||
                                          candidate.target == UnitKind::observatory ||
                                          candidate.target == UnitKind::roboticsSupportBay ||
                                          candidate.technology ==
                                              TechnologyKind::singularityCharge;
                               }),
                result.goals.end());
            result.composition = {{UnitKind::zealot, 1.0}};
        }
        result.attackThreshold = 1.5;
        goal(result, GoalKind::build, UnitKind::gateway, 2, 100,
             "guarantee two-gate defensive throughput", true);
        goal(result, GoalKind::train, UnitKind::zealot, 8, 99,
             "continuously reinforce against opening pressure", true);
        goal(result, GoalKind::build, UnitKind::shieldBattery, 1, 95,
             "defensive sustain");
    }
    return result;
}

void StrategyEngine::addInfrastructure(
    StrategicPlan& plan,
    const GameState& state,
    const ThreatAssessment& threat) {
    const auto bases = std::max(1, count(state, UnitKind::nexus));
    const auto completedBases = std::max(1, count(state, UnitKind::nexus, true));
    const auto workers = countRole(state, UnitRole::worker);
    const auto pylons = count(state, UnitKind::pylon);
    const auto pendingPylons = static_cast<int>(std::ranges::count_if(
        state.self.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::pylon && !unit.completed;
        }));
    const auto queuedSupply = std::accumulate(
        state.self.queuedUnits.begin(), state.self.queuedUnits.end(), 0,
        [](const int total, const UnitKind kind) { return total + unitStats(kind).supply; });
    const auto activeProduction = count(state, UnitKind::gateway, true) +
                                  count(state, UnitKind::roboticsFacility, true) +
                                  count(state, UnitKind::stargate, true);
    const auto desiredBuffer = state.self.supplyTotal <= 18
                                   ? 2
                                   : std::clamp(4 + activeProduction * 2, 6, 18);
    const auto projectedSupply = state.self.supplyTotal + pendingPylons * 16;
    const auto projectedUsed = state.self.supplyUsed + queuedSupply;
    const auto supplyNeeded = projectedSupply < 400 &&
                              projectedSupply - projectedUsed <= desiredBuffer;
    const auto desiredPylons = std::max(bases, pylons + (supplyNeeded ? 1 : 0));
    const auto openingPylonDeadline = pylons == 0 &&
                                      (state.self.supplyUsed >= 12 ||
                                       state.frame >= 45 * 24);
    goal(plan, GoalKind::build, UnitKind::pylon, desiredPylons, 100,
         "maintain a supply buffer",
         openingPylonDeadline || state.self.supplyTotal - state.self.supplyUsed <= 4);
    goal(plan, GoalKind::train, UnitKind::probe, std::max(workers, plan.desiredWorkers), 74,
         "saturate economy");
    // An expansion cannot be bought opportunistically while every idle
    // producer keeps spending the same income. Once the strategic phase calls
    // for another base and the main is clear, reserve its full cost ahead of
    // routine workers, tech, and composition fills. Direct pressure cancels
    // the reservation immediately so a Nexus never starves emergency units.
    const auto expansionDue = plan.desiredBases > bases;
    const auto expansionReady = bases == completedBases &&
                                workers >= bases * 12;
    const auto expansionSafe = expansionDue && expansionReady &&
                               plan.posture != Posture::defend &&
                               plan.posture != Posture::recover &&
                               threat.combatEnemiesNearMain == 0 &&
                               threat.approachingArmyValue < 2.0 &&
                               threat.immediateGround <= 0.45;
    goal(plan, GoalKind::expand, UnitKind::nexus, plan.desiredBases,
         expansionSafe ? 85 : 65, "match economic phase", expansionSafe);
    if (plan.desiredGasWorkers > 0) {
        goal(plan, GoalKind::build, UnitKind::assimilator, bases, 80, "fund technology");
    }

    // Size baseline production from the live economy. A useful tournament
    // rule is roughly one continuously-produced combat unit per six workers;
    // keep that throughput behind the intended expansion so it cannot crowd
    // out a planned Nexus.
    const auto gateways = count(state, UnitKind::gateway);
    const auto productionCeiling = std::clamp(bases * 3, 2, 12);
    auto throughputTarget = std::clamp(
        (workers + 5) / 6, 1, productionCeiling);
    // Scouting multiple enemy production structures is direct evidence that
    // our income-only heuristic may be too slow. Match most of that observed
    // capacity without blindly copying it across asymmetric race mechanics.
    const auto observedParity = std::clamp(
        static_cast<int>(std::ceil(threat.enemyProductionCapacity * 0.8)),
        1, productionCeiling);
    throughputTarget = std::max(throughputTarget, observedParity);
    if (state.frame >= 4 * 60 * 24 && bases >= plan.desiredBases &&
        gateways < throughputTarget) {
        goal(plan, GoalKind::build, UnitKind::gateway, throughputTarget,
             observedParity > (workers + 5) / 6 ? 78 : 73,
             observedParity > (workers + 5) / 6
                 ? "match scouted enemy production capacity"
                 : "match army throughput to the mining economy");
    }
    // A large residual bank still means infrastructure is the bottleneck,
    // even if recent worker losses make the throughput estimate conservative.
    if (state.frame >= 4 * 60 * 24 && state.self.minerals >= 650 &&
        bases >= plan.desiredBases && gateways < productionCeiling) {
        goal(plan, GoalKind::build, UnitKind::gateway, gateways + 1, 62,
             "convert sustained mineral surplus into army production");
    }
}

void StrategyEngine::addAdaptiveCounters(
    StrategicPlan& plan,
    const GameState& state) {
    // Convert legal observations into concrete production changes. Broad
    // air/cloak alarms keep bases alive; these matchup-aware counters prevent
    // the standing composition from continuing into a unit mix it cannot beat.
    if (state.enemy.race == Race::terran) {
        const auto bio = recentEnemyCount(state, UnitKind::marine) +
                         recentEnemyCount(state, UnitKind::medic) +
                         recentEnemyCount(state, UnitKind::firebat) +
                         recentEnemyCount(state, UnitKind::ghost);
        const auto mines = recentEnemyCount(state, UnitKind::spiderMine);
        const auto mech = recentEnemyCount(state, UnitKind::vulture) +
                          recentEnemyCount(state, UnitKind::siegeTank) +
                          recentEnemyCount(state, UnitKind::goliath) + mines;
        const auto capitalAir = recentEnemyCount(state, UnitKind::battlecruiser) +
                                recentEnemyCount(state, UnitKind::wraith) +
                                recentEnemyCount(state, UnitKind::valkyrie);

        if (mines > 0 || recentEnemyCount(state, UnitKind::siegeTank) >= 2) {
            goal(plan, GoalKind::train, UnitKind::observer, 3, 92,
                 "track mines and siege lines", true);
        }
        if (bio >= 7 && minute(state) >= 7) {
            plan.name += " [anti-bio storm]";
            goal(plan, GoalKind::train, UnitKind::highTemplar,
                 std::clamp(bio / 3, 3, 7), 88, "punish clustered Terran bio");
            technologyGoal(plan, TechnologyKind::psionicStorm, 1, 90,
                           "counter observed bio mass", true);
            setCompositionWeight(plan, UnitKind::highTemplar, 0.24);
            setCompositionWeight(plan, UnitKind::zealot, 0.30);
        }
        if (mech >= 7) {
            plan.name += " [anti-mech mobility]";
            technologyGoal(plan, TechnologyKind::legEnhancements, 1, 86,
                           "close on observed siege composition");
            goal(plan, GoalKind::train, UnitKind::zealot,
                 std::clamp(mech, 8, 18), 83, "absorb mines and surround tanks");
            setCompositionWeight(plan, UnitKind::zealot, 0.34);
            setCompositionWeight(plan, UnitKind::arbiter, 0.14);
        }
        if (capitalAir >= 3) {
            plan.name += " [anti-air fleet]";
            goal(plan, GoalKind::train, UnitKind::dragoon,
                 std::clamp(8 + capitalAir * 2, 10, 20), 95,
                 "counter observed Terran air", true);
            setCompositionWeight(plan, UnitKind::dragoon, 0.68);
        }
    } else if (state.enemy.race == Race::zerg) {
        const auto hydraLurker = recentEnemyCount(state, UnitKind::hydralisk) +
                                 recentEnemyCount(state, UnitKind::lurker);
        const auto zergAir = recentEnemyCount(state, UnitKind::mutalisk) +
                             recentEnemyCount(state, UnitKind::guardian) +
                             recentEnemyCount(state, UnitKind::devourer);
        const auto lateGround = recentEnemyCount(state, UnitKind::ultralisk) +
                                recentEnemyCount(state, UnitKind::defiler);

        if (hydraLurker >= 7) {
            plan.name += " [anti-hydra splash]";
            goal(plan, GoalKind::build, UnitKind::roboticsSupportBay, 1, 84,
                 "unlock reavers against observed ground mass");
            goal(plan, GoalKind::train, UnitKind::reaver,
                 std::clamp(hydraLurker / 5, 2, 4), 85,
                 "splash clustered hydralisks and lurkers");
            goal(plan, GoalKind::train, UnitKind::observer, 3, 91,
                 "maintain lurker detection", true);
            setCompositionWeight(plan, UnitKind::reaver, 0.16);
            setCompositionWeight(plan, UnitKind::highTemplar, 0.28);
        }
        if (zergAir >= 4) {
            plan.name += " [anti-air control]";
            goal(plan, GoalKind::train, UnitKind::corsair,
                 std::clamp(4 + zergAir / 2, 5, 10), 94,
                 "win air control against observed Zerg flyers", true);
            goal(plan, GoalKind::train, UnitKind::dragoon,
                 std::clamp(zergAir, 6, 12), 89,
                 "protect ground army from Zerg flyers");
            setCompositionWeight(plan, UnitKind::corsair, 0.28);
            setCompositionWeight(plan, UnitKind::dragoon, 0.22);
        }
        if (lateGround >= 3) {
            goal(plan, GoalKind::train, UnitKind::highTemplar, 6, 88,
                 "zone ultralisks and defiler support");
            goal(plan, GoalKind::train, UnitKind::reaver, 3, 82,
                 "add durable late-game ground splash");
            setCompositionWeight(plan, UnitKind::highTemplar, 0.30);
            setCompositionWeight(plan, UnitKind::archon, 0.18);
        }
    } else if (state.enemy.race == Race::protoss) {
        const auto reavers = recentEnemyCount(state, UnitKind::reaver);
        const auto enemyFleet = recentEnemyCount(state, UnitKind::carrier) +
                                recentEnemyCount(state, UnitKind::scout) +
                                recentEnemyCount(state, UnitKind::corsair);
        if (reavers >= 2) {
            goal(plan, GoalKind::train, UnitKind::observer, 3, 89,
                 "maintain vision over enemy reavers");
            goal(plan, GoalKind::train, UnitKind::reaver, 3, 82,
                 "contest enemy reaver control");
        }
        if (enemyFleet >= 3) {
            plan.name += " [anti-carrier fleet]";
            goal(plan, GoalKind::train, UnitKind::dragoon,
                 std::clamp(8 + enemyFleet * 2, 10, 20), 94,
                 "pressure observed Protoss air", true);
            goal(plan, GoalKind::train, UnitKind::scout,
                 std::clamp(enemyFleet, 3, 6), 86,
                 "focus high-value Protoss capital ships");
            setCompositionWeight(plan, UnitKind::dragoon, 0.62);
            setCompositionWeight(plan, UnitKind::scout, 0.16);
        }
    }
    normalizeComposition(plan);
}

void StrategyEngine::addSafetyReactions(
    StrategicPlan& plan,
    const ThreatAssessment& threat) {
    if (threat.workerRush > 0.30) {
        plan.name += " [worker-rush hold]";
        plan.posture = Posture::defend;
        plan.desiredBases = 1;
        plan.attackThreshold = std::max(plan.attackThreshold, 1.55);
        goal(plan, GoalKind::build, UnitKind::gateway, 1, 100,
             "complete the first anti-worker combat unit", true);
        goal(plan, GoalKind::train, UnitKind::zealot, 3, 99,
             "end the worker rush without prolonged economic damage", true);
    }

    if (threat.proxy + threat.staticContain > 0.34 ||
        (threat.enemiesNearMain >= 2 && threat.proxy > 0.18)) {
        plan.name += threat.staticContain > threat.proxy
                         ? " [break static contain]"
                         : " [break proxy]";
        plan.posture = Posture::defend;
        plan.desiredBases = 1;
        plan.attackThreshold = std::max(plan.attackThreshold, 1.65);
        plan.goals.erase(
            std::remove_if(plan.goals.begin(), plan.goals.end(),
                           [](const ProductionGoal& candidate) {
                               return candidate.goal == GoalKind::expand;
                           }),
            plan.goals.end());
        goal(plan, GoalKind::build, UnitKind::gateway, 2, 100,
             "replace greed with proxy-breaking production", true);
        goal(plan, GoalKind::train, UnitKind::zealot, 5, 99,
             "clear unfinished or unsupported proxy structures", true);
        goal(plan, GoalKind::build, UnitKind::shieldBattery, 1, 95,
             "sustain the main-base defense");
    }

    if (threat.cloak > 0.28) {
        goal(plan, GoalKind::build, UnitKind::roboticsFacility, 1, 97,
             "detected cloaked threat", true);
        goal(plan, GoalKind::build, UnitKind::observatory, 1, 96,
             "unlock mobile detection", true);
        goal(plan, GoalKind::train, UnitKind::observer, 3, 99,
             "maintain detection coverage", true);
        goal(plan, GoalKind::build, UnitKind::photonCannon, 3, 91,
             "base detection coverage");
    }
    if (threat.air > 0.42) {
        goal(plan, GoalKind::train, UnitKind::dragoon, 10, 94, "mobile anti-air", true);
        goal(plan, GoalKind::build, UnitKind::photonCannon, 5, 90,
             "mineral-line anti-air");
    }
}

void StrategyEngine::addEconomicRecovery(
    StrategicPlan& plan,
    const GameState& state) {
    const auto workers = countRole(state, UnitRole::worker);
    const auto nexuses = count(state, UnitKind::nexus);
    const auto completedNexuses = count(state, UnitKind::nexus, true);
    const auto pendingNexuses = nexuses - completedNexuses;
    const auto activeBases = static_cast<int>(std::ranges::count_if(
        state.bases, [&state](const BaseSnapshot& base) {
            return base.ownerId == state.self.id && base.mineralsRemaining > 1000;
        }));

    if (nexuses == 0 && workers > 0) {
        plan.name = "Emergency Nexus recovery";
        plan.posture = Posture::recover;
        plan.desiredBases = 1;
        plan.desiredWorkers = std::max(12, workers);
        goal(plan, GoalKind::expand, UnitKind::nexus, 1, 100,
             "replace the lost economy anchor", true);
    }

    if (state.frame >= 4 * 60 * 24 && completedNexuses > 0 &&
        workers < std::min(12, completedNexuses * 8)) {
        const auto defending = plan.posture == Posture::defend;
        plan.name += " [worker recovery]";
        if (!defending) plan.posture = Posture::recover;
        plan.desiredWorkers = std::max(plan.desiredWorkers, completedNexuses * 14);
        goal(plan, GoalKind::train, UnitKind::probe,
             std::max(8, completedNexuses * 10), defending ? 70 : 96,
             "recover after severe worker losses", workers < 6 && !defending);
    }

    const auto depletedEconomy = completedNexuses > 0 && activeBases < completedNexuses;
    const auto saturatedEconomy = activeBases > 0 && workers >= activeBases * 20;
    if (pendingNexuses == 0 && completedNexuses < 8 &&
        (depletedEconomy || saturatedEconomy)) {
        plan.desiredBases = std::max(plan.desiredBases, completedNexuses + 1);
        goal(plan, GoalKind::expand, UnitKind::nexus, plan.desiredBases, 86,
             depletedEconomy ? "replace a mined-out base" : "expand a saturated economy",
             depletedEconomy);
    }

    const auto unpowered = std::ranges::any_of(
        state.self.units, [](const UnitSnapshot& unit) {
            return unit.completed && isBuilding(unit.kind) &&
                   unitStats(unit.kind).requiresPsi && !unit.powered;
        });
    if (unpowered) {
        const auto pylons = count(state, UnitKind::pylon);
        goal(plan, GoalKind::build, UnitKind::pylon, pylons + 1, 98,
             "restore power to disabled production", true);
    }

    // A large mineral bank means production, not another passive combat-unit
    // target, is the bottleneck. Scale infrastructure with the live economy.
    if (state.self.minerals >= 900 && completedNexuses > 0) {
        const auto targetGateways = std::clamp(completedNexuses * 3, 3, 12);
        goal(plan, GoalKind::build, UnitKind::gateway, targetGateways, 73,
             "convert excess mineral bank into production");
        if (state.self.gas >= 500 && minute(state) >= 12) {
            goal(plan, GoalKind::build, UnitKind::stargate,
                 std::clamp(completedNexuses, 1, 4), 61,
                 "add a late-game production branch");
        }
    }
}

void StrategyEngine::applyOpeningStyle(
    StrategicPlan& plan,
    const GameState& state,
    const OpeningStyle style) {
    const auto emergency = plan.posture == Posture::defend ||
                           plan.posture == Posture::recover;
    switch (style) {
        case OpeningStyle::standard: return;
        case OpeningStyle::aggressive:
            plan.name += " [pressure]";
            if (!emergency) {
                plan.posture = minute(state) < 5 ? Posture::hold : Posture::pressure;
            }
            if (minute(state) < 8) {
                plan.desiredBases = std::max(1, plan.desiredBases - 1);
            }
            plan.attackThreshold = std::max(1.05, plan.attackThreshold - 0.10);
            if (supplyAtLeast(state, 10)) {
                goal(plan, GoalKind::build, UnitKind::gateway,
                     minute(state) < 8 ? 2 : 5, 91,
                     "opponent-specific pressure production");
            }
            if (supplyAtLeast(state, 14)) {
                goal(plan, GoalKind::train, UnitKind::dragoon,
                     std::max(5, minute(state) * 2), 87,
                     "opponent-specific pressure army");
            }
            return;
        case OpeningStyle::economic:
            plan.name += " [economic]";
            // Opponent-history exploration is a preference, not authority to
            // ignore a rush that is already visible inside our main.
            if (emergency) return;
            plan.posture = minute(state) < 9 ? Posture::hold : plan.posture;
            plan.desiredBases = std::min(4, plan.desiredBases + (minute(state) >= 5 ? 1 : 0));
            plan.desiredWorkers = std::min(76, plan.desiredWorkers + 6);
            plan.attackThreshold += 0.12;
            goal(plan, GoalKind::expand, UnitKind::nexus, plan.desiredBases, 72,
                 "opponent-specific economic edge");
            return;
        case OpeningStyle::deceptive:
            plan.name += " [tech switch]";
            if (emergency) return;
            plan.attackThreshold += 0.05;
            if (minute(state) < 5 && !supplyAtLeast(state, 24)) return;
            if (state.enemy.race == Race::zerg) {
                goal(plan, GoalKind::build, UnitKind::roboticsFacility, 1, 79,
                     "reaver tech switch");
                goal(plan, GoalKind::build, UnitKind::roboticsSupportBay, 1, 78,
                     "reaver tech switch");
                goal(plan, GoalKind::train, UnitKind::reaver, 2, 77,
                     "punish static anti-air");
                goal(plan, GoalKind::train, UnitKind::shuttle, 1, 76,
                     "deliver tech switch");
            } else {
                goal(plan, GoalKind::build, UnitKind::citadelOfAdun, 1, 82,
                     "dark templar tech switch");
                goal(plan, GoalKind::build, UnitKind::templarArchives, 1, 81,
                     "dark templar tech switch");
                goal(plan, GoalKind::train, UnitKind::darkTemplar, 3, 80,
                     "punish weak detection");
            }
            return;
        case OpeningStyle::count: return;
    }
}

StrategicPlan StrategicDirector::stabilize(
    StrategicPlan candidate,
    const GameState& state,
    const ThreatAssessment& threat) {
    constexpr auto clearWindow = 8 * 24;
    const auto emergencyEvidence = candidate.posture == Posture::defend ||
                                   candidate.posture == Posture::recover ||
                                   threat.combatEnemiesNearMain > 0 ||
                                   threat.approachingArmyValue >= 2.0;

    if (!initialized_) {
        initialized_ = true;
        posture_ = candidate.posture;
        if (emergencyEvidence) lastEmergencyFrame_ = state.frame;
        return candidate;
    }

    if (emergencyEvidence) {
        posture_ = candidate.posture == Posture::recover
                       ? Posture::recover
                       : Posture::defend;
        lastEmergencyFrame_ = state.frame;
        candidate.posture = posture_;
        return candidate;
    }

    if ((posture_ == Posture::defend || posture_ == Posture::recover) &&
        lastEmergencyFrame_ >= 0 &&
        state.frame - lastEmergencyFrame_ < clearWindow) {
        candidate.posture = posture_;
        candidate.attackThreshold = std::max(candidate.attackThreshold, 1.40);
        candidate.name += " [regrouping after defense]";
        return candidate;
    }

    posture_ = candidate.posture;
    return candidate;
}

void StrategicDirector::reset() noexcept {
    posture_ = Posture::hold;
    lastEmergencyFrame_ = -1;
    initialized_ = false;
}

std::string_view postureName(const Posture posture) noexcept {
    switch (posture) {
        case Posture::hold: return "Hold";
        case Posture::defend: return "Defend";
        case Posture::pressure: return "Pressure";
        case Posture::attack: return "Attack";
        case Posture::harass: return "Harass";
        case Posture::recover: return "Recover";
    }
    return "Invalid";
}

std::string_view openingStyleName(const OpeningStyle style) noexcept {
    switch (style) {
        case OpeningStyle::standard: return "standard";
        case OpeningStyle::aggressive: return "aggressive";
        case OpeningStyle::economic: return "economic";
        case OpeningStyle::deceptive: return "deceptive";
        case OpeningStyle::count: break;
    }
    return "invalid";
}

}  // namespace astra
