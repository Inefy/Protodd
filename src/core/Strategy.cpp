#include "astra/Strategy.hpp"

#include "astra/UnitCatalog.hpp"

#include <algorithm>
#include <limits>

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

    result.rallyPoint = ourMain(state);
    result.attackTarget = enemyMain(state);
    addInfrastructure(result, state);
    addSafetyReactions(result, threat);
    applyOpeningStyle(result, state, style);

    std::ranges::stable_sort(result.goals, std::greater{}, &ProductionGoal::priority);
    return result;
}

StrategicPlan StrategyEngine::planPvT(
    const GameState& state,
    const ThreatAssessment& threat) const {
    StrategicPlan result;
    result.name = "PvT one-gate observer expansion";
    result.desiredWorkers = std::min(72, 22 + minute(state) * 4);
    result.desiredBases = minute(state) < 7 ? 1 : (minute(state) < 13 ? 2 : 3);
    result.desiredGasWorkers = minute(state) < 2 ? 0 : (minute(state) < 7 ? 3 :
                                                       (minute(state) < 12 ? 6 : 9));
    result.posture = minute(state) < 6 ? Posture::hold : Posture::pressure;
    result.attackThreshold = 1.32;
    result.composition = {{UnitKind::dragoon, 0.55}, {UnitKind::zealot, 0.22},
                          {UnitKind::highTemplar, 0.13}, {UnitKind::arbiter, 0.10}};

    goal(result, GoalKind::build, UnitKind::gateway, minute(state) < 7 ? 1 : 3, 88,
         "baseline production");
    goal(result, GoalKind::build, UnitKind::cyberneticsCore, 1, 92,
         "unlock dragoons and range");
    technologyGoal(result, TechnologyKind::singularityCharge, 1, 91,
                   "range is mandatory for dragoon control", true);
    goal(result, GoalKind::train, UnitKind::dragoon, std::max(3, minute(state) * 2), 82,
         "range control against Terran");
    goal(result, GoalKind::build, UnitKind::roboticsFacility, 1, 78,
         "observers against mines and tech scouting");
    goal(result, GoalKind::build, UnitKind::observatory, 1, 77, "observer access");
    goal(result, GoalKind::train, UnitKind::observer, minute(state) < 12 ? 2 : 4, 84,
         "mine detection and army tracking");

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
    if (threat.aggression > 0.62) {
        result.name = "PvT anti-pressure hold";
        result.posture = Posture::defend;
        result.desiredBases = std::min(result.desiredBases, 2);
        result.attackThreshold = 1.55;
        goal(result, GoalKind::train, UnitKind::dragoon, 8, 98, "survive detected pressure", true);
        goal(result, GoalKind::build, UnitKind::shieldBattery, 1, 90, "front-line sustain");
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
    result.desiredGasWorkers = minute(state) < 4 ? 0 : (minute(state) < 8 ? 3 :
                                                       (minute(state) < 12 ? 6 : 9));
    result.posture = minute(state) < 8 ? Posture::hold : Posture::harass;
    result.attackThreshold = 1.2;
    result.composition = {{UnitKind::zealot, 0.35}, {UnitKind::dragoon, 0.12},
                          {UnitKind::highTemplar, 0.25}, {UnitKind::corsair, 0.18},
                          {UnitKind::archon, 0.10}};

    goal(result, GoalKind::build, UnitKind::forge, 1, 93, "safe natural and upgrades");
    if (minute(state) >= 4 && threat.immediateGround <= 0.45) {
        technologyGoal(result, TechnologyKind::protossGroundWeapons, 1, 87,
                       "zealot attack timing");
    }
    const auto defensiveBases = std::max(1, count(state, UnitKind::nexus));
    goal(result, GoalKind::build, UnitKind::photonCannon,
         std::min(6, defensiveBases * 2), 89,
         "ling and mutalisk coverage");
    goal(result, GoalKind::build, UnitKind::gateway, minute(state) < 8 ? 1 : 4, 84,
         "ground production");
    goal(result, GoalKind::train, UnitKind::zealot, std::max(4, minute(state)), 79,
         "mineral-efficient front line");
    goal(result, GoalKind::build, UnitKind::cyberneticsCore, 1, 82, "air and dragoon access");
    goal(result, GoalKind::build, UnitKind::stargate, 1, 76, "overlord denial and scouting");
    goal(result, GoalKind::train, UnitKind::corsair, threat.air > 0.45 ? 7 : 4, 75,
         "air superiority");
    goal(result, GoalKind::build, UnitKind::citadelOfAdun, 1, 73, "speed and templar path");
    goal(result, GoalKind::build, UnitKind::templarArchives, 1, 71, "storm versus Zerg mass");
    goal(result, GoalKind::train, UnitKind::highTemplar, std::max(2, minute(state) / 3), 72,
         "storm support");

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

    if (threat.immediateGround > 0.45) {
        result.name = "PvZ emergency gateway hold";
        result.posture = Posture::defend;
        result.desiredBases = 1;
        result.desiredGasWorkers = 0;
        goal(result, GoalKind::build, UnitKind::gateway, 2, 99, "anti-rush production", true);
        goal(result, GoalKind::train, UnitKind::zealot, 6, 98, "hold early ground rush", true);
        goal(result, GoalKind::build, UnitKind::photonCannon, 3, 97, "seal mineral line", true);
    } else if (minute(state) >= 3) {
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
    result.desiredBases = minute(state) < 9 ? 1 : (minute(state) < 15 ? 2 : 3);
    result.desiredGasWorkers = minute(state) < 2 ? 0 : (minute(state) < 8 ? 3 :
                                                       (minute(state) < 12 ? 6 : 9));
    result.posture = minute(state) < 6 ? Posture::hold : Posture::pressure;
    result.attackThreshold = 1.18;
    result.composition = {{UnitKind::dragoon, 0.58}, {UnitKind::zealot, 0.14},
                          {UnitKind::reaver, 0.18}, {UnitKind::highTemplar, 0.10}};

    goal(result, GoalKind::build, UnitKind::gateway, minute(state) < 9 ? 2 : 4, 90,
         "tempo and map control");
    goal(result, GoalKind::build, UnitKind::cyberneticsCore, 1, 94, "dragoon access");
    technologyGoal(result, TechnologyKind::singularityCharge, 1, 93,
                   "range wins dragoon contact", true);
    goal(result, GoalKind::train, UnitKind::dragoon, std::max(4, minute(state) * 2), 86,
         "core PvP army");
    goal(result, GoalKind::build, UnitKind::roboticsFacility, 1, 85,
         "reaver pressure and detection");
    goal(result, GoalKind::build, UnitKind::observatory, 1, 83, "DT safety");
    goal(result, GoalKind::train, UnitKind::observer, 2, 88, "DT detection", true);
    goal(result, GoalKind::build, UnitKind::roboticsSupportBay, 1, 69, "reaver access");
    goal(result, GoalKind::train, UnitKind::reaver, 2, 70, "area control");
    goal(result, GoalKind::train, UnitKind::shuttle, 1, 67, "reaver mobility");

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

    if (threat.aggression > 0.6) {
        result.name = "PvP two-gate emergency defense";
        result.posture = Posture::defend;
        result.desiredBases = 1;
        result.attackThreshold = 1.5;
        goal(result, GoalKind::build, UnitKind::shieldBattery, 1, 96, "defensive sustain");
        goal(result, GoalKind::train, UnitKind::zealot, 3, 95, "buffer against pressure");
    }
    return result;
}

void StrategyEngine::addInfrastructure(StrategicPlan& plan, const GameState& state) {
    const auto bases = std::max(1, count(state, UnitKind::nexus));
    const auto workers = countRole(state, UnitRole::worker);
    const auto pylons = count(state, UnitKind::pylon);
    const auto pendingPylons = static_cast<int>(std::ranges::count_if(
        state.self.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::pylon && !unit.completed;
        }));
    const auto projectedSupply = state.self.supplyTotal + pendingPylons * 16;
    const auto supplyNeeded = projectedSupply < 400 &&
                              projectedSupply - state.self.supplyUsed <= 8;
    const auto desiredPylons = std::max(bases, pylons + (supplyNeeded ? 1 : 0));
    goal(plan, GoalKind::build, UnitKind::pylon, desiredPylons, 100,
         "maintain a supply buffer", state.self.supplyTotal - state.self.supplyUsed <= 4);
    goal(plan, GoalKind::train, UnitKind::probe, std::max(workers, plan.desiredWorkers), 74,
         "saturate economy");
    goal(plan, GoalKind::expand, UnitKind::nexus, plan.desiredBases, 65,
         "match economic phase");
    if (plan.desiredGasWorkers > 0) {
        goal(plan, GoalKind::build, UnitKind::assimilator, bases, 80, "fund technology");
    }
}

void StrategyEngine::addSafetyReactions(
    StrategicPlan& plan,
    const ThreatAssessment& threat) {
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

void StrategyEngine::applyOpeningStyle(
    StrategicPlan& plan,
    const GameState& state,
    const OpeningStyle style) {
    switch (style) {
        case OpeningStyle::standard: return;
        case OpeningStyle::aggressive:
            plan.name += " [pressure]";
            plan.posture = minute(state) < 5 ? Posture::hold : Posture::pressure;
            plan.desiredBases = std::max(1, plan.desiredBases - 1);
            plan.attackThreshold = std::max(1.05, plan.attackThreshold - 0.10);
            goal(plan, GoalKind::build, UnitKind::gateway, minute(state) < 8 ? 2 : 5, 91,
                 "opponent-specific pressure production");
            goal(plan, GoalKind::train, UnitKind::dragoon, std::max(5, minute(state) * 2), 87,
                 "opponent-specific pressure army");
            return;
        case OpeningStyle::economic:
            plan.name += " [economic]";
            plan.posture = minute(state) < 9 ? Posture::hold : plan.posture;
            plan.desiredBases = std::min(4, plan.desiredBases + (minute(state) >= 5 ? 1 : 0));
            plan.desiredWorkers = std::min(76, plan.desiredWorkers + 6);
            plan.attackThreshold += 0.12;
            goal(plan, GoalKind::expand, UnitKind::nexus, plan.desiredBases, 72,
                 "opponent-specific economic edge");
            return;
        case OpeningStyle::deceptive:
            plan.name += " [tech switch]";
            plan.attackThreshold += 0.05;
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
