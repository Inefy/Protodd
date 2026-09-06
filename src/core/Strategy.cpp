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

bool activeApproach(const GameState& state, const ThreatAssessment& threat) noexcept {
    // Early scouting should react to an army crossing the map. Once the game
    // is established, perimeter movement alone is not enough to keep the
    // economy and army permanently defensive; require a current breach or
    // explicit rush evidence instead.
    return state.frame < 10 * 60 * 24 && threat.approachingArmyValue >= 2.0;
}

bool openingPressureExpected(
    const GameState& state,
    const ThreatAssessment& threat) noexcept {
    const auto supported = threat.uncertainty <= 0.75 ||
                           threat.combatEnemiesNearMain > 0 ||
                           activeApproach(state, threat) ||
                           threat.immediateGround > 0.45;
    // A nearly uniform belief distribution still has a numerical winner.
    // Its stale label alone must not hold a ready army at home for 16 minutes.
    return minute(state) < 16 && supported &&
           (threat.mostLikely == EnemyPlan::fastRush ||
            threat.mostLikely == EnemyPlan::heavyPressure);
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
    const auto existing = std::ranges::find(
        plan.goals, technology, &ProductionGoal::technology);
    if (existing != plan.goals.end()) {
        existing->desiredCount = std::max(existing->desiredCount, desiredLevel);
        if (priority > existing->priority) existing->reason = reason;
        existing->priority = std::max(existing->priority, priority);
        existing->blocking = existing->blocking || blocking;
        return;
    }
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

bool hardBreachAtMain(const GameState& state) noexcept {
    const auto home = ourMain(state);
    if (!home.valid()) return false;
    return std::ranges::any_of(
        state.enemy.units, [&home](const UnitSnapshot& enemy) {
            return enemy.visible && !enemy.flying && isCombatUnit(enemy.kind) &&
                   enemy.position.valid() &&
                   distanceSquared(enemy.position, home) <= 320 * 320;
        });
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
    const auto mapAttackTarget = enemyMain(state);
    // Matchup planners can choose a nearer perimeter rally. Preserve that
    // decision; only fill in the map-level defaults when no specialized point
    // was requested. Previously this unconditional assignment erased every
    // PvP forward-intercept rally before combat could use it.
    if (!result.attackTarget.valid()) result.attackTarget = mapAttackTarget;
    if (!result.rallyPoint.valid()) {
        result.rallyPoint = home.valid() && result.attackTarget.valid()
                                ? moveToward(home, result.attackTarget, 160.0)
                                : home;
    }
    applyOpeningStyle(result, state, style);
    addAdaptiveCounters(result, state);
    addEconomicRecovery(result, state);
    // Safety runs last so an opponent-specific economic style cannot override
    // direct evidence of an all-in at our main.
    addSafetyReactions(result, threat);

    const auto visibleGroundContact = std::ranges::any_of(
        state.enemy.units, [&home](const UnitSnapshot& enemy) {
            return enemy.visible && enemy.completed && !enemy.flying &&
                   isCombatUnit(enemy.kind) && enemy.position.valid() &&
                   (!home.valid() || distanceSquared(home, enemy.position) <=
                                       800 * 800);
        });
    const auto visibleProtossContact = state.enemy.race == Race::protoss &&
        std::ranges::any_of(state.enemy.units, [](const UnitSnapshot& enemy) {
            return enemy.visible && enemy.completed && !enemy.flying &&
                   isCombatUnit(enemy.kind) && enemy.position.valid();
        });
    const auto forwardCounter = visibleProtossContact &&
        state.frame >= 7 * 60 * 24 && state.frame < 11 * 60 * 24 &&
        count(state, UnitKind::zealot, true) >= 6 &&
        count(state, UnitKind::dragoon, true) >= 2 &&
        threat.combatEnemiesNearMain == 0 &&
        !visibleGroundContact &&
        !hardBreachAtMain(state) &&
        result.posture != Posture::recover &&
        threat.workerRush <= 0.30;
    if (forwardCounter) {
        // The safety pass may have converted a perimeter sighting back to
        // Defend. If the army is still outside the hard-breach radius, keep
        // the mobile screen fighting in the midfield instead of donating it
        // to a mineral-line surround.
        result.posture = Posture::pressure;
        result.attackThreshold = std::min(result.attackThreshold, 1.30);
        result.minimumAttackSize = std::min(result.minimumAttackSize, 8);
        if (mapAttackTarget.valid()) {
            result.attackTarget = mapAttackTarget;
            result.rallyPoint = mapAttackTarget;
        }
        result.name += " [post-safety forward counter]";
    }

    result.desiredBases = std::min(result.desiredBases, result.maximumBases);

    // Never keep producing workers for bases that the current plan has
    // explicitly postponed. This bounds one-base saturation while preserving
    // enough workers to fund production and a prompt expansion.
    result.desiredWorkers = std::min(
        result.desiredWorkers, std::max(14, result.desiredBases * 22));

    // Infrastructure must follow the final intent: a safety reaction or an
    // opening style can change the economy after the matchup plan is made.
    if (result.posture == Posture::defend) {
        const auto existingBases = std::max(1, count(state, UnitKind::nexus));
        result.desiredBases = std::min(result.desiredBases, existingBases);
    }
    for (auto& objective : result.goals) {
        if (objective.goal == GoalKind::expand) {
            objective.desiredCount = std::min(objective.desiredCount, result.desiredBases);
        } else if (objective.target == UnitKind::probe) {
            objective.desiredCount = std::min(objective.desiredCount, result.desiredWorkers);
        }
    }
    addInfrastructure(result, state, threat);

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
    result.posture = minute(state) < 6 || openingPressureExpected(state, threat)
                         ? Posture::hold
                         : Posture::pressure;
    result.attackThreshold = 1.32;
    result.minimumAttackSize = 14;
    result.composition = {{UnitKind::dragoon, 0.55}, {UnitKind::zealot, 0.22},
                          {UnitKind::highTemplar, 0.13}, {UnitKind::arbiter, 0.10}};

    // A pure Gateway opening cannot field Zealots quickly enough to trade with
    // nonstop Barracks production on every spawn. Establish overlapping
    // Cannon coverage first, then add the Gateway and transition to Dragoons;
    // this also supplies detection and a safe retreat line against unfamiliar
    // Terran openings rather than encoding one opponent by name.
    if (minute(state) < 4 && count(state, UnitKind::zealot, true) == 0 &&
        count(state, UnitKind::photonCannon, true) == 0) {
        result.desiredWorkers = std::min(result.desiredWorkers, 7);
    }
    if (supplyAtLeast(state, 7)) {
        goal(result, GoalKind::build, UnitKind::forge, 1, 100,
             "fortified anti-bio opening anchor", true);
        goal(result, GoalKind::build, UnitKind::photonCannon, 2, 99,
             "overlap the intercept before first contact", true);
        goal(result, GoalKind::build, UnitKind::gateway, 1, 98,
             "field mobile defense behind the completed static intercept", true);
    }

    if (supplyAtLeast(state, 8)) {
        goal(result, GoalKind::build, UnitKind::gateway, minute(state) < 6 ? 1 : 3, 88,
             "eight-supply gateway", count(state, UnitKind::gateway) == 0);
    }
    if (count(state, UnitKind::gateway) > 0 || supplyAtLeast(state, 10)) {
        goal(result, GoalKind::train, UnitKind::zealot, 1, 94,
             "opening bodyguard before vulnerable dragoon tech",
             count(state, UnitKind::zealot) == 0);
        goal(result, GoalKind::build, UnitKind::shieldBattery, 1, 89,
             "opening sustain against bio pressure");
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
    if (threat.combatEnemiesNearMain > 0 || activeApproach(state, threat) ||
        (minute(state) < 8 && threat.aggression > 0.62)) {
        result.name = "PvT anti-pressure hold";
        result.posture = Posture::defend;
        result.desiredBases = minute(state) < 8 ? 1 : std::min(result.desiredBases, 2);
        result.desiredWorkers = std::min(result.desiredWorkers, 14);
        result.attackThreshold = 1.55;
        if (count(state, UnitKind::gateway) > 0 &&
            count(state, UnitKind::zealot) == 0) {
            goal(result, GoalKind::train, UnitKind::zealot, 1, 105,
                 "field one mobile defender before adding more structures", true);
        }
        if (count(state, UnitKind::photonCannon, true) >= 2) {
            goal(result, GoalKind::build, UnitKind::pylon, 2, 102,
                 "give the forward defensive shell redundant power", true);
        }
        // Four Cannons are a screen, not the army. Continuing to replace an
        // aspirational six-Cannon target under fire can consume every mineral
        // and leave completed Gateways idle. Stop at four, then add the third
        // Gateway only after a mobile front line exists.
        const auto cannonTarget = minute(state) >= 5 ? 4 : 3;
        const auto gatewayTarget = minute(state) >= 5 &&
                                           count(state, UnitKind::zealot, true) >= 4
                                       ? 3
                                       : 2;
        goal(result, GoalKind::build, UnitKind::photonCannon, cannonTarget, 101,
             "scale the mineral-line anchor with sustained bio", true);
        goal(result, GoalKind::build, UnitKind::gateway, gatewayTarget, 100,
             "add emergency anti-pressure throughput", true);
        goal(result, GoalKind::build, UnitKind::shieldBattery, 1, 99,
             "complete a sustainable defensive screen", true);
        const auto screenEstablished =
            count(state, UnitKind::photonCannon, true) >= 3;
        if (screenEstablished) {
            // Once three Cannons are complete this checkpoint is permanent.
            // Tying the priority to a live Zealot count made one combat loss
            // demote the Core before its saved minerals could be spent.
            goal(result, GoalKind::build, UnitKind::cyberneticsCore, 1, 99,
                 "unlock the ranged counter after emergency production", true);
            goal(result, GoalKind::build, UnitKind::assimilator, 1, 99,
                 "fund the ranged counter after emergency production", true);
            const auto cloakWindow =
                count(state, UnitKind::photonCannon, true) >= 4;
            const auto cloakCommitted = count(state, UnitKind::citadelOfAdun) > 0 ||
                                        count(state, UnitKind::templarArchives) > 0 ||
                                        count(state, UnitKind::darkTemplar) > 0;
            if (count(state, UnitKind::cyberneticsCore, true) > 0 &&
                (cloakWindow || cloakCommitted)) {
                result.name += " [anti-bio dark templar]";
                goal(result, GoalKind::build, UnitKind::citadelOfAdun, 1, 99,
                     "cloak transition against sustained opening bio", true);
                if (count(state, UnitKind::citadelOfAdun, true) > 0) {
                    goal(result, GoalKind::build, UnitKind::templarArchives, 1, 99,
                         "complete the cloak transition", true);
                }
                if (count(state, UnitKind::templarArchives, true) > 0) {
                    goal(result, GoalKind::train, UnitKind::darkTemplar, 3, 102,
                         "clear bio and counterattack before detection", true);
                    setCompositionWeight(result, UnitKind::darkTemplar, 0.24);
                }
            }
        }
        goal(result, GoalKind::train, UnitKind::zealot,
             minute(state) >= 5 ? 10 : 6, 98,
             "field bodies before vulnerable dragoon tech", true);
        const auto dragoonInfrastructureReady =
            count(state, UnitKind::cyberneticsCore, true) > 0;
        const auto rangedReservationSafe =
            dragoonInfrastructureReady && state.self.gas >= 50 &&
            count(state, UnitKind::photonCannon, true) >= 2;
        if (dragoonInfrastructureReady && count(state, UnitKind::dragoon) == 0) {
            goal(result, GoalKind::train, UnitKind::dragoon, 1, 104,
                 "field the first ranged defender before support infrastructure",
                 rangedReservationSafe);
        }
        goal(result, GoalKind::train, UnitKind::dragoon, 8,
             dragoonInfrastructureReady ? 99 : 96,
             "transition to ranged defense after its prerequisite completes",
             rangedReservationSafe);
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
    result.minimumAttackSize = 14;
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

    if (threat.combatEnemiesNearMain > 0 || activeApproach(state, threat) ||
        (minute(state) < 8 && threat.immediateGround > 0.45)) {
        result.name = "PvZ emergency gateway hold";
        result.posture = Posture::defend;
        result.desiredBases = 1;
        const auto stabilized = count(state, UnitKind::photonCannon, true) >= 2 &&
                                count(state, UnitKind::zealot, true) >= 4;
        if (!stabilized && minute(state) < 7) result.desiredGasWorkers = 0;
        else result.desiredGasWorkers = std::max(3, result.desiredGasWorkers);
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
    result.posture = minute(state) < 6 || openingPressureExpected(state, threat)
                         ? Posture::hold
                         : Posture::pressure;
    // A ranged mirror must meet the reinforcement stream in the open. The
    // old twenty-unit gate left a six-to-ten Dragoon force parked beside the
    // Nexus while BananaBrain's next wave surrounded it. Start with a modest
    // ratio gate, then lower the launch size once our own ranged screen is
    // established; the emergency branch below can still restore a cautious
    // defense when the mineral line is directly breached.
    result.attackThreshold = 1.32;
    result.minimumAttackSize = 14;
    result.composition = {{UnitKind::dragoon, 0.58}, {UnitKind::zealot, 0.14},
                          {UnitKind::reaver, 0.18}, {UnitKind::highTemplar, 0.10}};

    const auto rangedOpening = std::ranges::any_of(
        state.enemy.units, [&state](const UnitSnapshot& unit) {
            const auto tech = unit.kind == UnitKind::cyberneticsCore ||
                              unit.kind == UnitKind::roboticsFacility;
            const auto rangedArmy = unit.kind == UnitKind::dragoon ||
                                    unit.kind == UnitKind::reaver;
            return tech || (rangedArmy &&
                           (unit.visible || state.frame - unit.lastSeen <= 90 * 24));
        });
    // In a melee-only mirror the first five minutes are decided by Gateway
    // throughput, not by an early gas bank. Three gas workers can remove
    // roughly one Zealot's minerals before the first flood arrives; keep gas
    // off until the enemy shows ranged tech (or the opening window closes).
    const auto visibleRangedOpening = std::ranges::any_of(
        state.enemy.units, [](const UnitSnapshot& unit) {
            return unit.visible && unit.completed &&
                   (unit.kind == UnitKind::cyberneticsCore ||
                    unit.kind == UnitKind::roboticsFacility ||
                    unit.kind == UnitKind::dragoon ||
                    unit.kind == UnitKind::reaver);
        });
    if (!visibleRangedOpening && state.frame >= 2 * 60 * 24 &&
        state.frame < 4 * 60 * 24) {
        result.desiredGasWorkers = 0;
    }
    const auto cannonsReady = count(state, UnitKind::photonCannon, true);
    const auto zealotsReady = count(state, UnitKind::zealot, true);
    const auto dragoonsReady = count(state, UnitKind::dragoon, true);
    if (state.frame >= 3 * 60 * 24 && state.frame < 5 * 60 * 24 &&
        zealotsReady >= 2) {
        // Do not let the generic melee gas pause win after the two-unit
        // screen is already fielded. This also covers a hidden enemy Core,
        // which disables the early Forge anchor but still needs our own Core
        // and Robotics Facility to start on time.
        result.desiredGasWorkers = std::max(result.desiredGasWorkers, 3);
    }
    // A missing Core means something different before and after Dragoons have
    // existed. Treat the latter as destroyed infrastructure so an emergency
    // response does not strand a surviving army with an unusable bank.
    const auto coreLost = count(state, UnitKind::cyberneticsCore) == 0 &&
                          count(state, UnitKind::dragoon) > 0;
    const auto twoGateOpening = minute(state) < 8 && !rangedOpening &&
        std::ranges::count_if(state.enemy.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::gateway && unit.completed;
        }) >= 2;
    const auto earlyMeleeAnchor = minute(state) >= 3 && minute(state) < 5 &&
                                  !rangedOpening && zealotsReady >= 2 &&
                                  (threat.combatEnemiesNearMain > 0 ||
                                   threat.approachingArmyValue >= 2.0 ||
                                   threat.mostLikely == EnemyPlan::fastRush);
    const auto home = ourMain(state);
    const auto visibleGroundContact = std::ranges::any_of(
        state.enemy.units, [&home](const UnitSnapshot& enemy) {
            return enemy.visible && enemy.completed && !enemy.flying &&
                   isCombatUnit(enemy.kind) && enemy.position.valid() &&
                   (!home.valid() || distanceSquared(home, enemy.position) <= 800 * 800);
        });
    const auto earlyMeleeScreen = zealotsReady >= 2 || rangedOpening;
    const auto mobileOpening = zealotsReady + dragoonsReady +
                               count(state, UnitKind::reaver, true);
    const auto earlyTechWindow = state.frame >= 4 * 60 * 24 &&
                                 zealotsReady >= 4 &&
                                 threat.combatEnemiesNearMain == 0 &&
                                 !visibleGroundContact &&
                                 !activeApproach(state, threat);
    if (dragoonsReady >= 4) {
        result.attackThreshold = 1.22;
        result.minimumAttackSize = 8;
        const auto enemyArmyUnseen = std::ranges::none_of(
            state.enemy.units, [](const UnitSnapshot& enemy) {
                return enemy.visible && enemy.completed && !enemy.flying &&
                       isCombatUnit(enemy.kind);
            });
        if (state.frame >= 7 * 60 * 24 && enemyArmyUnseen &&
            threat.combatEnemiesNearMain == 0) {
            // Do not wait for the eighth body when the map is empty. A
            // seven-unit ranged screen that reaches the enemy production at
            // eight minutes can stop the next flood from ever assembling.
            result.attackThreshold = 1.15;
            result.minimumAttackSize = 6;
        }
    }
    if (minute(state) < 14 && mobileOpening < 24) {
        result.maximumBases = 2;
        result.desiredBases = std::min(result.desiredBases, 2);
    }
    if (minute(state) < 10 && mobileOpening < 10) {
        result.desiredBases = 1;
        result.maximumBases = std::max(1, count(state, UnitKind::nexus));
    }
    // Do not buy a second Nexus before the mirror's first splash-tech window
    // is secured.  A quiet map can make the expansion heuristic look safe,
    // but BananaBrain's delayed Dragoon wave punishes the lost 400 minerals;
    // finish Robotics, Support Bay, and one Reaver before expanding.
    const auto firstReaverTechReady =
        count(state, UnitKind::roboticsFacility, true) > 0 &&
        count(state, UnitKind::roboticsSupportBay, true) > 0 &&
        count(state, UnitKind::reaver, true) > 0;
    if (minute(state) < 10 && !firstReaverTechReady) {
        result.desiredBases = 1;
        result.maximumBases = std::max(1, count(state, UnitKind::nexus));
    }
    if (minute(state) < 8 && !earlyMeleeScreen) {
        // Composition fills run after fixed goals. Keep them melee-only until
        // the opening screen has two Zealots, otherwise an idle second
        // Gateway can spend the rush window on a Dragoon.
        result.composition = {{UnitKind::zealot, 1.0}};
    }
    const auto canTransition = count(state, UnitKind::cyberneticsCore) > 0 ||
                               (zealotsReady >= 2 && rangedOpening) ||
                               (cannonsReady >= 2 && zealotsReady >= 1 && rangedOpening) ||
                               (cannonsReady >= 3 && zealotsReady >= 4) ||
                               // Two completed Cannons plus two Zealots are
                               // already a meaningful mirror anchor. Do not
                               // wait for a third Cannon while the Core and
                               // gas transition remain permanently erased.
                               (cannonsReady >= 2 && zealotsReady >= 2);

    // Buying a Forge and two Cannons on seven Probes conceded the economy
    // before the first engagement. Establish mobile production while growing
    // workers; direct rush evidence below reserves extra melee and sustain.
    if (supplyAtLeast(state, 9)) {
        goal(result, GoalKind::build, UnitKind::gateway, 1, 100,
             "nine-supply mobile production", true);
    }
    if (supplyAtLeast(state, 10)) {
        goal(result, GoalKind::build, UnitKind::assimilator, 1, 101,
             "opening gas before the Core completes", true);
    }
    if (count(state, UnitKind::gateway) > 0 &&
        count(state, UnitKind::gateway) < 2) {
        // Do not spend the opening Core's minerals while a single Gateway has
        // produced only one body. Two early Zealots give the Probe line time
        // to survive the first mirror contact and let the second Gateway
        // complete without conceding the game to a four-Zealot rush.
        goal(result, GoalKind::train, UnitKind::zealot, 2, 99,
             "field two mobile defenders before ranged tech", true);
    }

    if (supplyAtLeast(state, 9)) {
        const auto roboticsInPlay = count(state, UnitKind::roboticsFacility) > 0;
        const auto reaverOnline = count(state, UnitKind::reaver, true) > 0;
        const auto gatewayTarget = supplyAtLeast(state, 10) ?
                                       (!roboticsInPlay || !reaverOnline ? 2 :
                                        (minute(state) < 12 ? 3 : 4)) : 1;
        goal(result, GoalKind::build, UnitKind::gateway, gatewayTarget, 97,
             "nine-supply gateway into two-gate control",
             count(state, UnitKind::gateway) < gatewayTarget && gatewayTarget <= 2);
    }
    if (count(state, UnitKind::gateway) >= 2) {
        // Keep both early Gateways on melee bodies long enough to contest a
        // mirror flood. Transitioning at three Zealots left BananaBrain's
        // first wave unopposed while the Core consumed the bank.
        const auto earlyZealotTarget =
            count(state, UnitKind::roboticsFacility) > 0
                ? ((!rangedOpening && state.frame < 10 * 60 * 24 &&
                    (threat.mostLikely == EnemyPlan::fastRush ||
                     threat.uncertainty > 0.85)) ? 8 :
                   (minute(state) < 6 ? 6 : 3))
                : 4;
        goal(result, GoalKind::train, UnitKind::zealot,
             earlyZealotTarget, 96,
             "fill secured opening production with defenders",
             count(state, UnitKind::zealot) < 2);
        const auto batteryEvidence = rangedOpening ||
            threat.combatEnemiesNearMain > 0 ||
            threat.approachingArmyValue >= 2.0 ||
            count(state, UnitKind::cyberneticsCore) > 0;
        if (count(state, UnitKind::zealot) >= 2 && batteryEvidence) {
            goal(result, GoalKind::build, UnitKind::shieldBattery, 1, 93,
                 "sustain the two-gate defensive screen");
        }
    }
    if (earlyMeleeAnchor) {
        // Two completed Zealots by the three-minute mark are the only
        // reliable signal available before a fast mirror flood is visible.
        // Start one Forge while the bank is still small; this is a single
        // anchor, not the old blind two-Cannon opening, and it buys time for
        // the Core and ranged transition to complete.
        // This is a low-priority insurance layer, not an emergency reserve.
        // Keep Assimilator/Core/Zealot funding ahead of it; once those costs
        // are protected, a single Forge can finish before the first ranged
        // pressure wave and let the six-minute Cannon fallback execute.
        goal(result, GoalKind::build, UnitKind::forge, 1, 95,
             "early mirror Forge insurance", false);
        if (count(state, UnitKind::forge) > 0 && !rangedOpening) {
            goal(result, GoalKind::build, UnitKind::photonCannon, 1, 94,
                 "one early Cannon behind the Forge insurance", false);
        }
    }
    const auto stabilizingMeleeAnchor = state.frame >= 5 * 60 * 24 &&
                                        state.frame < 8 * 60 * 24 &&
                                        zealotsReady >= 6 &&
                                        !visibleGroundContact &&
                                        !hardBreachAtMain(state) &&
                                        (threat.mostLikely == EnemyPlan::fastRush ||
                                         threat.uncertainty > 0.85 ||
                                         count(state, UnitKind::cyberneticsCore) > 0);
    if (stabilizingMeleeAnchor) {
        // A hidden two-gate flood can cross the map before it becomes visible
        // at the mineral line. Once six Zealots and the first Core bank are
        // secured, reserve one Forge/Cannon without buying the old blind
        // two-Cannon opening. This gives the home screen time to finish while
        // Robotics and the first Reaver remain the primary tech plan.
        goal(result, GoalKind::build, UnitKind::forge, 1, 94,
             "stabilize the six-Zealot mirror screen", false);
        if (count(state, UnitKind::forge) > 0) {
            goal(result, GoalKind::build, UnitKind::photonCannon, 1, 93,
                 "single Cannon behind the six-Zealot screen", false);
        }
    }
    if (state.frame >= 6 * 60 * 24 && state.frame < 9 * 60 * 24 &&
        count(state, UnitKind::forge) > 0 && cannonsReady < 2) {
        // One Cannon buys the opening time; a second one before nine minutes
        // keeps a Dragoon-heavy push from deleting the mineral line while the
        // Core and Robotics chain finishes.  This is still capped at two.
        goal(result, GoalKind::build, UnitKind::photonCannon, 2, 94,
             "second Cannon before the ranged pressure window", false);
    }
    if (supplyAtLeast(state, 12)) {
        const auto quietTechWindow = state.frame >= 5 * 60 * 24 &&
                                     threat.combatEnemiesNearMain == 0 &&
                                     !visibleGroundContact &&
                                     (!activeApproach(state, threat) ||
                                      zealotsReady >= 8 || cannonsReady >= 1);
        const auto screenedTechWindow = state.frame >= 5 * 60 * 24 &&
                                        threat.combatEnemiesNearMain == 0 &&
                                        !visibleGroundContact &&
                                        zealotsReady >= 6;
        const auto productionSecured = count(state, UnitKind::gateway) >= 1 &&
                                       (state.frame < 2 * 60 * 24 ||
                                        (count(state, UnitKind::zealot) >= 4 &&
                                         (rangedOpening || cannonsReady >= 1 ||
                                          count(state, UnitKind::photonCannon) >= 1 ||
                                          quietTechWindow || screenedTechWindow ||
                                          earlyTechWindow)));
        goal(result, GoalKind::build, UnitKind::cyberneticsCore, 1, 98,
             "dragoon access after opening production", productionSecured);
    }
    if (supplyAtLeast(state, 14)) {
        if (earlyMeleeScreen) {
            goal(result, GoalKind::train, UnitKind::dragoon,
                 std::max(4, minute(state) * 2), 91, "core PvP army");
        }
        technologyGoal(result, TechnologyKind::singularityCharge, 1, 86,
                       "range follows the first defensive dragoons");
    }
    if (count(state, UnitKind::dragoon) >= 3) {
        // Reserve the splash-tech investment before routine Dragoon fills can
        // consume the bank. A Reaver is the only cost-effective answer once
        // the mirror reaches a packed ranged fight.
        const auto earlyRobotics = dragoonsReady >= 2;
        goal(result, GoalKind::build, UnitKind::roboticsFacility, 1,
             earlyRobotics ? 104 : 92,
             "reaver pressure and detection", earlyRobotics);
        goal(result, GoalKind::build, UnitKind::observatory, 1, 83, "DT safety");
        goal(result, GoalKind::train, UnitKind::observer, 2, 88, "DT detection", true);
    }
    // Reserve the Support Bay as soon as the Robotics Facility is underway.
    // Waiting until the facility is complete lets routine Gateway production
    // consume the exact 150 minerals/100 gas needed for the first Reaver.
    const auto supportNeeded = state.frame >= 4 * 60 * 24 &&
                               count(state, UnitKind::roboticsFacility) > 0 &&
                               count(state, UnitKind::roboticsSupportBay) == 0;
    const auto roboticsNeeded = state.frame >= 4 * 60 * 24 &&
                                count(state, UnitKind::cyberneticsCore, true) > 0 &&
                                count(state, UnitKind::roboticsFacility) == 0 &&
                                zealotsReady >= 4;
    if (roboticsNeeded) {
        result.desiredGasWorkers = std::max(3, result.desiredGasWorkers);
        goal(result, GoalKind::build, UnitKind::roboticsFacility, 1, 104,
             "reserve the first Reaver tech window", true);
    }
    if (supportNeeded) {
        // In a ranged mirror the first Reaver is a timing unit, not a late
        // composition luxury. Reserve its Support Bay before extra Gateways
        // and routine Dragoon fills consume the mineral/gas bank.
        result.desiredGasWorkers = std::max(6, result.desiredGasWorkers);
        goal(result, GoalKind::build, UnitKind::roboticsSupportBay, 1, 106,
             "early Reaver splash against the mirror army", true);
    }
    const auto cloakedThreat =
        threat.cloak > 0.28 || threat.mostLikely == EnemyPlan::cloakedTech ||
        recentEnemyCount(state, UnitKind::darkTemplar) > 0;
    if (cloakedThreat && count(state, UnitKind::cyberneticsCore, true) > 0) {
        // A Protoss Dark Templar line can end an otherwise stable two-Cannon
        // defense before the normal three-Dragoon trigger asks for Robotics.
        // Reserve the detection chain as soon as the assessment turns covert,
        // then let the first Observer take precedence over routine Gateway
        // fills and the Reaver follow-up.
        result.desiredGasWorkers = std::max(6, result.desiredGasWorkers);
        goal(result, GoalKind::build, UnitKind::roboticsFacility, 1, 108,
             "detection against cloaked Protoss tech", true);
        goal(result, GoalKind::build, UnitKind::observatory, 1, 107,
             "unlock an Observer against Dark Templar", true);
        goal(result, GoalKind::train, UnitKind::observer, 1, 106,
             "field the first emergency Observer", true);
        if (count(state, UnitKind::observer, true) == 0) {
            // Do not send an undetected army across the map into a DT line.
            // Hold the home perimeter, add a static detector, and resume
            // pressure once the first Observer is available.
            result.posture = Posture::defend;
            result.attackThreshold = std::max(result.attackThreshold, 1.55);
            result.minimumAttackSize = std::max(result.minimumAttackSize, 14);
            result.desiredBases = 1;
            goal(result, GoalKind::build, UnitKind::photonCannon, 2, 104,
                 "static detection while the first Observer is pending", true);
        }
    }
    if (supplyAtLeast(state, 28)) {
        goal(result, GoalKind::build, UnitKind::roboticsSupportBay, 1, 69,
             "reaver access");
        goal(result, GoalKind::train, UnitKind::reaver, 2, 70, "area control");
        goal(result, GoalKind::train, UnitKind::shuttle, 1, 67, "reaver mobility");
    }
    if (count(state, UnitKind::roboticsSupportBay) > 0 &&
        mobileOpening >= 8) {
        // One early Reaver changes the first Dragoon contact; do not wait for
        // the later composition filler to notice that the Robotics Facility
        // is idle.
        goal(result, GoalKind::train, UnitKind::reaver, 2, 105,
             "two-Reaver splash before the first full attack wave", true);
    }

    // Robotics is already the normal PvP splash-tech checkpoint.  Attach an
    // Observatory to that same checkpoint instead of waiting for three
    // Dragoons or a fully observed cloak alarm: a hidden Dark Templar can
    // arrive during the Robotics/Support build window, before the reactive
    // branch has any chance to finish detection.  The lower priority leaves
    // the first Reaver reservation ahead of routine Observer production.
    if (count(state, UnitKind::roboticsFacility, true) > 0) {
        goal(result, GoalKind::build, UnitKind::observatory, 1, 90,
             "early PvP detection alongside Robotics", false);
        goal(result, GoalKind::train, UnitKind::observer, 1, 89,
             "early Observer for hidden Protoss tech", true);
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

    const auto evidencedMeleeRush = minute(state) < 8 && !rangedOpening &&
                                    (twoGateOpening ||
                                     hardBreachAtMain(state) ||
                                     // If the mobile screen has already been
                                     // thinned below four Zealots, add a static
                                     // anchor before the next wave arrives.
                                     (threat.combatEnemiesNearMain > 0 &&
                                      zealotsReady < 4));
    if (evidencedMeleeRush) {
        // A Forge is expensive, so only open this branch after direct mirror
        // evidence. It gives the two-gate screen one static anchor without
        // returning to the old blind seven-Probe Cannon opening.
        goal(result, GoalKind::build, UnitKind::forge, 1, 96,
             "conditional static anchor against evidenced Zealot pressure");
        if (count(state, UnitKind::forge) > 0) {
            goal(result, GoalKind::build, UnitKind::photonCannon, 1, 95,
                 "conditional Cannon behind the mobile mirror screen");
        }
    }

    // Once a fast melee opening is actually visible, the mobile screen and a
    // single early Cannon are both more valuable than another delayed ranged
    // tech cycle. This branch is evidence-gated, so an ordinary mirror does
    // not pay the blind Forge tax, but a four-Zealot flood cannot walk through
    // an entirely unanchored mineral line.
    const auto rushStaticNeeded = minute(state) < 8 && !rangedOpening &&
                                   (hardBreachAtMain(state) ||
                                    (threat.combatEnemiesNearMain > 0 &&
                                     zealotsReady < 4));
    const auto preserveEarlyCore = state.frame >= 4 * 60 * 24 &&
                                   zealotsReady >= 4 &&
                                   !hardBreachAtMain(state) &&
                                   // A FastRush that is still outside the
                                   // hard-breach radius is exactly the narrow
                                   // window where starting the Core saves a
                                   // full Robotics/Observer cycle.  Waiting
                                   // for the first completed Cannon pushed
                                   // detection past the DT arrival in the
                                   // live mirror trace; the mobile Zealot
                                   // screen already buys this checkpoint.
                                   (cannonsReady >= 1 ||
                                    state.frame >= 5 * 60 * 24 ||
                                    threat.mostLikely == EnemyPlan::fastRush);

    if (threat.combatEnemiesNearMain > 0 || activeApproach(state, threat) ||
        visibleGroundContact ||
        (minute(state) < 8 && threat.aggression > 0.6)) {
        result.name = "PvP two-gate emergency defense";
        result.posture = Posture::defend;
        result.desiredBases = 1;
        result.desiredWorkers = std::min(result.desiredWorkers, canTransition ? 22 : 12);
        const auto meleeOnlyEmergency = rushStaticNeeded &&
                                        dragoonsReady < 2 &&
                                        !(cannonsReady >= 2 && zealotsReady >= 2) &&
                                        !preserveEarlyCore;
        if ((count(state, UnitKind::cyberneticsCore) == 0 && !canTransition &&
             !preserveEarlyCore) ||
            meleeOnlyEmergency) {
            result.desiredGasWorkers = 0;
            result.goals.erase(
                std::remove_if(result.goals.begin(), result.goals.end(),
                               [](const ProductionGoal& candidate) {
                                   return candidate.target == UnitKind::cyberneticsCore ||
                                          candidate.target == UnitKind::assimilator ||
                                          candidate.target == UnitKind::roboticsFacility ||
                                          candidate.target == UnitKind::observatory ||
                                          candidate.target == UnitKind::roboticsSupportBay ||
                                          candidate.technology ==
                                              TechnologyKind::singularityCharge;
                               }),
                result.goals.end());
            result.composition = {{UnitKind::zealot, 1.0}};
            if (coreLost) {
                // This is a rebuild, not a greedy tech opening. Restore the
                // production prerequisite and gas immediately; otherwise the
                // already-earned Dragoon army can only watch its bank grow.
                result.desiredGasWorkers = std::max(3, result.desiredGasWorkers);
                goal(result, GoalKind::build, UnitKind::cyberneticsCore, 1, 110,
                     "rebuild the destroyed ranged-tech core", true);
                goal(result, GoalKind::build, UnitKind::assimilator, 1, 109,
                     "restore gas after the destroyed ranged-tech core", true);
            }
        }
        if (preserveEarlyCore && count(state, UnitKind::cyberneticsCore) == 0) {
            // Four Zealots and no hard breach are enough to start the ranged
            // transition. Do not let the emergency branch erase this goal
            // while a mirror rush is merely approaching the wall.
            result.desiredGasWorkers = std::max(3, result.desiredGasWorkers);
            goal(result, GoalKind::build, UnitKind::cyberneticsCore, 1, 113,
                 "start ranged tech behind the four-Zealot screen", true);
            goal(result, GoalKind::build, UnitKind::assimilator, 1, 112,
                 "feed the protected early Core", true);
        }
        const auto rushCannonTarget =
            count(state, UnitKind::cyberneticsCore, true) == 0 ? 2 :
            // Once the Core is complete, the next 200 minerals must go into
            // Robotics/Observer access.  A third Cannon here repeatedly
            // starved the blocking Robotics goal in the live DT trace; two
            // completed Cannons are enough to keep the Zealot screen from
            // walking through while splash tech comes online.
            count(state, UnitKind::roboticsFacility) == 0 ? 2 :
            state.self.minerals >= 800 ? 4 :
            (state.frame >= 5 * 60 * 24 &&
             (state.self.minerals >= 200 || zealotsReady >= 4)) ? 3 : 2;
        if (rushStaticNeeded && cannonsReady < rushCannonTarget) {
            goal(result, GoalKind::build, UnitKind::forge, 1, 112,
                 "anchor the mineral line against observed Zealot pressure", true);
            goal(result, GoalKind::build, UnitKind::photonCannon, rushCannonTarget, 111,
                 "overlap the emergency mineral-line anchor", true);
        }
    const auto lateRangedStatic = state.frame >= 6 * 60 * 24 &&
                                   (threat.combatEnemiesNearMain > 0 ||
                                        visibleGroundContact ||
                                        threat.mostLikely == EnemyPlan::heavyPressure) &&
                                      dragoonsReady >= 2 && cannonsReady < 2 &&
                                      // When the threat is cloaked, the
                                      // Observatory/Observer chain is the
                                      // urgent resource sink.  Letting this
                                      // generic ranged-pressure branch outrank
                                      // it repeatedly delayed detection until
                                      // the mineral line was already lost.
                                      !cloakedThreat;
        if (lateRangedStatic) {
            goal(result, GoalKind::build, UnitKind::forge, 1, 110,
                 "restore a home anchor against the ranged push", true);
            goal(result, GoalKind::build, UnitKind::photonCannon, 2, 109,
                 "two-Cannon home screen against the ranged push", true);
        }
        const auto directBreach = hardBreachAtMain(state);
        const auto forwardRangedScreen = dragoonsReady >= 4 && !directBreach &&
                                         !visibleGroundContact;
        const auto visibleMeleeContact = zealotsReady >= 2 &&
            std::ranges::any_of(state.enemy.units, [](const UnitSnapshot& enemy) {
                return enemy.visible && enemy.completed && !enemy.flying &&
                       isCombatUnit(enemy.kind) && enemy.position.valid();
            });
        const auto forwardMeleeScreen = visibleMeleeContact && !directBreach &&
                                        !visibleGroundContact &&
                                        mobileOpening >= 8 &&
                                        (!rangedOpening || zealotsReady >= 8);
        result.attackThreshold = forwardRangedScreen ? 1.22 :
                                 (forwardMeleeScreen ? 1.32 : 1.50);
        result.minimumAttackSize = (forwardRangedScreen || forwardMeleeScreen)
                                       ? 8 : 14;
        if (forwardRangedScreen || forwardMeleeScreen) {
            result.posture = Posture::pressure;
            const auto homePosition = ourMain(state);
            if (homePosition.valid()) {
                const auto nearest = std::ranges::min_element(
                    state.enemy.units, {}, [&homePosition](const UnitSnapshot& enemy) {
                        return enemy.visible && enemy.completed && !enemy.flying &&
                                       isCombatUnit(enemy.kind) && enemy.position.valid()
                                   ? distanceSquared(homePosition, enemy.position)
                                   : std::numeric_limits<int>::max();
                    });
                if (nearest != state.enemy.units.end() && nearest->position.valid()) {
                    result.rallyPoint = moveToward(homePosition, nearest->position, 320.0);
                }
            }
            result.name += forwardRangedScreen ? " [forward ranged intercept]"
                                               : " [forward melee intercept]";
        }
        if (count(state, UnitKind::gateway) > 0 &&
            count(state, UnitKind::zealot) == 0) {
            goal(result, GoalKind::train, UnitKind::zealot, 1, 105,
                 "field one mobile defender before adding more structures", true);
        }
        if (cannonsReady >= 2 || zealotsReady >= 2) {
            goal(result, GoalKind::train, UnitKind::probe, cannonsReady >= 2 ? 10 : 12, 104,
                 "fund sustained mirror defense behind the Cannon screen", true);
            if (count(state, UnitKind::gateway, true) > 0) {
                goal(result, GoalKind::build, UnitKind::shieldBattery, 1, 103,
                     "finish defensive sustain before the ongoing Zealot target", true);
            }
        }
        if (count(state, UnitKind::forge) > 0) {
            const auto emergencyCannonTarget =
                count(state, UnitKind::cyberneticsCore, true) > 0 ? 3 : 2;
            goal(result, GoalKind::build, UnitKind::photonCannon,
                 emergencyCannonTarget, 101,
                 "reinforce an existing static defense investment", true);
        }
        goal(result, GoalKind::build, UnitKind::gateway, 2, 100,
             "guarantee two-gate defensive throughput", true);
        goal(result, GoalKind::train, UnitKind::zealot, 8, 99,
             "continuously reinforce against opening pressure", true);
        goal(result, GoalKind::build, UnitKind::shieldBattery, 1, 97,
             "defensive sustain", true);
    }
    if (canTransition) {
        // A completed screen is a window to build a sustainable ranged army.
        // Repeated melee reservations previously kept twelve Probes replacing
        // Zealots while an opponent with a scouted Core scaled Dragoons.
        result.desiredGasWorkers = std::max(3, result.desiredGasWorkers);
        goal(result, GoalKind::train, UnitKind::probe, 16, 104,
             "grow the income protected by the completed defensive screen", true);
        goal(result, GoalKind::build, UnitKind::cyberneticsCore, 1, 102,
             "convert the defensive window into ranged production", true);
        goal(result, GoalKind::build, UnitKind::assimilator, 1, 102,
             "fund the ranged transition before repeated melee replacement", true);
        const auto gasReady = state.self.gas >= 50;
        if (earlyMeleeScreen) {
            goal(result, GoalKind::train, UnitKind::dragoon, 4, 101,
                 "establish a ranged defensive core", gasReady);
        }
        if (count(state, UnitKind::dragoon, true) >= 2) {
            technologyGoal(result, TechnologyKind::singularityCharge, 1, 102,
                           "contest ranged pressure behind the mobile screen", true);
        }
        if (rangedOpening) {
            const auto meleeScreen = std::clamp(recentEnemyCount(state, UnitKind::zealot) / 2,
                                               2, 4);
            for (auto& objective : result.goals) {
                if (objective.target == UnitKind::zealot) {
                    objective.desiredCount = std::min(objective.desiredCount, meleeScreen);
                } else if (objective.target == UnitKind::photonCannon &&
                           objective.desiredCount > 2) {
                    objective.priority = 95;
                    objective.blocking = false;
                }
            }
            result.composition = {{UnitKind::dragoon, 0.80}, {UnitKind::zealot, 0.20}};
        }
    }
    if (twoGateOpening) {
        // A scouted double Gateway without ranged tech warrants a compact
        // static screen. Buy the first mobile units before that investment,
        // and do not bank an expansion while the first flood is unaccounted for.
        result.name += " [scouted two-gate screen]";
        result.desiredBases = 1;
        result.maximumBases = std::max(1, count(state, UnitKind::nexus));
        result.desiredWorkers = std::min(result.desiredWorkers, 22);
        // Stop gas while the opponent is still melee-only. The Forge/Cannon
        // anchor must begin as soon as 150 minerals are available; leaving
        // three Probes on gas here delayed it until the mineral line was
        // already lost in the live rush trace.
        result.desiredGasWorkers = 0;
        // The first response window is too short for a Forge plus two
        // Cannons, but waiting for the third completed Zealot lets the Forge
        // finish only after the flood reaches the mineral line. Keep the
        // first two bodies mobile, then start one Cannon early; the second
        // Cannon is a follow-up once the mobile screen has four bodies.
        const auto mobileRushOpening = state.frame < 5 * 60 * 24 &&
                                       zealotsReady < 2;
        if (mobileRushOpening) {
            std::erase_if(result.goals, [](const ProductionGoal& candidate) {
                return candidate.target == UnitKind::forge ||
                       candidate.target == UnitKind::photonCannon ||
                       candidate.target == UnitKind::shieldBattery;
            });
            goal(result, GoalKind::train, UnitKind::zealot, 6, 114,
                 "mobile-first response to scouted double production", true);
        } else {
            // Once the mobile screen has four bodies, gas is no longer a
            // luxury: the opponent's next wave is likely Dragoons. Re-enable
            // three gas workers before the eight-minute scouting window ends
            // so the completed Core can actually turn the saved bank into
            // ranged defenders.
            result.desiredGasWorkers = 3;
            goal(result, GoalKind::train, UnitKind::zealot, 2, 105,
                 "field a mobile screen against scouted double production", true);
            goal(result, GoalKind::build, UnitKind::forge, 1, 112,
                 "prepare a static answer to scouted melee production", true);
            const auto staticCannonTarget = state.frame >= 6 * 60 * 24 ||
                                            zealotsReady >= 4 ? 2 : 1;
            goal(result, GoalKind::build, UnitKind::photonCannon,
                 staticCannonTarget, 111,
                 "support the mobile army against a melee flood", true);
        }

        // Once the first ranged screen is online, meet a suspected flood in
        // the open instead of waiting for it to enter the Probe line. The
        // small 160px default rally is deliberately conservative for normal
        // games, but is too close to the Nexus for a scouted two-Gateway all-in.
        // Only do this while the threat is still outside the main; the direct
        // breach branch above must retain control once contact is established.
        const auto hardBreach = hardBreachAtMain(state);
        if (!mobileRushOpening && state.frame >= 4 * 60 * 24 &&
            mobileOpening >= 2 && !hardBreach && !visibleGroundContact) {
            result.posture = Posture::pressure;
            if (home.valid()) {
                const auto nearest = std::ranges::min_element(
                    state.enemy.units, {}, [&home](const UnitSnapshot& enemy) {
                        return enemy.visible && enemy.completed && !enemy.flying &&
                                       isCombatUnit(enemy.kind) && enemy.position.valid()
                                   ? distanceSquared(home, enemy.position)
                                   : std::numeric_limits<int>::max();
                    });
                if (nearest != state.enemy.units.end() && nearest->position.valid()) {
                    result.rallyPoint = moveToward(home, nearest->position, 320.0);
                } else if (result.attackTarget.valid()) {
                    result.rallyPoint = moveToward(home, result.attackTarget, 640.0);
                }
            }
            result.name += " [forward intercept]";
        }
    }

    const auto visibleEnemyArmy = std::ranges::any_of(
        state.enemy.units, [](const UnitSnapshot& enemy) {
            return enemy.visible && enemy.completed && !enemy.flying &&
                   isCombatUnit(enemy.kind);
        });
    const auto earlyMeleeStrike = !rangedOpening &&
                                  state.frame >= 4 * 60 * 24 &&
                                  state.frame < 8 * 60 * 24 &&
                                  mobileOpening >= 4 &&
                                  threat.mostLikely == EnemyPlan::fastExpand &&
                                  !visibleEnemyArmy &&
                                  threat.combatEnemiesNearMain == 0 &&
                                  !activeApproach(state, threat) &&
                                  !hardBreachAtMain(state);
    if (earlyMeleeStrike) {
        // A compact six-Zealot mirror force should pressure production before
        // both sides have enough Dragoons to turn the game into a bank race.
        // This is only enabled after the mobile-first opening is assembled;
        // current contact or a crossing army keeps the emergency branch in
        // control instead.
        const auto target = enemyMain(state);
        result.posture = Posture::attack;
        result.attackThreshold = 1.10;
        result.minimumAttackSize = 5;
        if (target.valid()) {
            result.attackTarget = target;
            result.rallyPoint = target;
        }
        result.name += " [early melee strike]";
    }

    // Once the first ranged screen has a decisive local edge, convert that
    // edge into map pressure instead of repeatedly meeting small waves on
    // the home perimeter.  The previous plan stayed in Pressure with a
    // perimeter rally, so the squad could win several defensive skirmishes
    // yet never force BananaBrain to defend its production.  Require a real
    // visible comparison and a clear main so this cannot turn an unseen
    // all-in into a blind march across the map.
    const auto visibleEnemyPower = [&state]() {
        double power = 0.0;
        for (const auto& unit : state.enemy.units) {
            if (!unit.visible || !unit.completed || unit.flying ||
                !isCombatUnit(unit.kind)) {
                continue;
            }
            power += unitStats(unit.kind).combatValue *
                     std::clamp(unit.healthFraction(), 0.15, 1.0);
        }
        return power;
    }();
    const auto ownMobilePower = [&state]() {
        double power = 0.0;
        for (const auto& unit : state.self.units) {
            if (!unit.completed || unit.flying || !isCombatUnit(unit.kind)) {
                continue;
            }
            power += unitStats(unit.kind).combatValue *
                     std::clamp(unit.healthFraction(), 0.15, 1.0);
        }
        return power;
    }();
    const auto directBreach = hardBreachAtMain(state);
    const auto visibleArmyNearHome = std::ranges::any_of(
        state.enemy.units, [&home](const UnitSnapshot& enemy) {
            return enemy.visible && enemy.completed && !enemy.flying &&
                   isCombatUnit(enemy.kind) && enemy.position.valid() &&
            (!home.valid() || distanceSquared(home, enemy.position) <= 960 * 960);
        });
    const auto earlyTimingAttack = state.frame >= 8 * 60 * 24 + 12 * 24 &&
                                   state.frame < 11 * 60 * 24 &&
                                   mobileOpening >= 8 && !visibleEnemyArmy &&
                                   !visibleArmyNearHome && !directBreach &&
                                   threat.mostLikely != EnemyPlan::fastRush &&
                                   (threat.mostLikely == EnemyPlan::unknown ||
                                    threat.uncertainty > 0.90);
    if (earlyTimingAttack) {
        // When the map is empty, a compact Zealot/Dragoon/Reaver group should
        // hit production before BananaBrain can bank a second wave. The home
        // guard formed by SquadPlanner keeps this from becoming an all-in.
        result.posture = Posture::attack;
        result.attackThreshold = std::min(result.attackThreshold, 1.20);
        result.minimumAttackSize = std::min(result.minimumAttackSize, 10);
        const auto target = enemyMain(state);
        if (target.valid()) {
            result.attackTarget = target;
            result.rallyPoint = target;
        }
        result.name += " [empty-map timing attack]";
    }
    const auto reaverTimingStrike = state.frame >= 9 * 60 * 24 &&
                                    state.frame < 13 * 60 * 24 &&
                                    count(state, UnitKind::reaver, true) >= 1 &&
                                    mobileOpening >= 12 &&
                                    threat.mostLikely != EnemyPlan::heavyPressure &&
                                    threat.combatEnemiesNearMain == 0 &&
                                    !visibleArmyNearHome && !visibleEnemyArmy &&
                                    !directBreach && enemyMain(state).valid();
    if (reaverTimingStrike) {
        // Once the first Reaver is ready, a compact timing attack is safer
        // than parking the army until BananaBrain's next wave is complete.
        // Keep this window bounded and require a clear home so an all-in never
        // overrides the emergency defense branch.
        result.posture = Posture::attack;
        result.attackThreshold = std::min(result.attackThreshold, 1.12);
        result.minimumAttackSize = std::min(result.minimumAttackSize, 10);
        result.attackTarget = enemyMain(state);
        result.rallyPoint = result.attackTarget;
        result.name += " [Reaver timing strike]";
    }
    const auto counterPushWindow = state.frame >= 8 * 60 * 24 &&
                                   mobileOpening >= 16 && !directBreach &&
                                   !visibleArmyNearHome &&
                                   visibleEnemyPower >= 3.0 &&
                                   ownMobilePower >= visibleEnemyPower * 1.35;
    if (counterPushWindow) {
        result.posture = Posture::attack;
        result.attackThreshold = std::min(result.attackThreshold, 1.18);
        result.minimumAttackSize = std::min(result.minimumAttackSize, 8);
        const auto target = enemyMain(state);
        if (target.valid()) {
            result.attackTarget = target;
            result.rallyPoint = target;
        }
        result.name += " [counter-push advantage]";
    }
    if ((roboticsNeeded || supportNeeded) && !hardBreachAtMain(state)) {
        // Protect the Robotics -> Support Bay sequence from composition filler.
        // A brief pause is cheaper than entering the first Reaver fight with
        // an empty gas/mineral bank; direct pressure keeps the normal
        // defensive queue in control.
        const auto preserveDragoonScreen = supportNeeded && dragoonsReady < 6;
        const auto preserveMeleeScreen = !rangedOpening &&
                                         state.frame < 10 * 60 * 24 &&
                                         zealotsReady < 8 &&
                                         (threat.uncertainty > 0.85 ||
                                          threat.mostLikely == EnemyPlan::fastRush);
        result.goals.erase(
            std::remove_if(result.goals.begin(), result.goals.end(),
                           [preserveDragoonScreen, preserveMeleeScreen](
                               const ProductionGoal& candidate) {
                               if (candidate.target == UnitKind::gateway &&
                                   candidate.goal == GoalKind::build &&
                                   candidate.priority < 106) {
                                   return true;
                               }
                               if (candidate.goal != GoalKind::train) return false;
                               switch (candidate.target) {
                                   case UnitKind::zealot: return !preserveMeleeScreen;
                                   case UnitKind::dragoon: return !preserveDragoonScreen;
                                   case UnitKind::reaver:
                                   case UnitKind::highTemplar:
                                   case UnitKind::darkTemplar:
                                   case UnitKind::archon:
                                   case UnitKind::darkArchon:
                                   case UnitKind::scout:
                                   case UnitKind::corsair:
                                   case UnitKind::carrier:
                                   case UnitKind::arbiter:
                                       return true;
                                   default:
                                       return false;
                               }
                           }),
            result.goals.end());
        result.composition.clear();
        result.desiredBases = std::min(result.desiredBases,
                                       std::max(1, count(state, UnitKind::nexus)));
        result.maximumBases = std::max(1, count(state, UnitKind::nexus));
        result.name += roboticsNeeded ? " [reserve Robotics tech]"
                                      : " [reserve Reaver tech]";
    }
    if (openingPressureExpected(state, threat)) {
        // A fixed one-base flood is beaten by compact two-base production,
        // not by taking a third Nexus while the first decisive army is still
        // assembling. The cap disappears after the opening pressure window.
        result.desiredBases = std::min(result.desiredBases, 2);
        result.desiredWorkers = std::min(result.desiredWorkers, 36);
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
    const auto activeProduction = count(state, UnitKind::gateway, true) +
                                  count(state, UnitKind::roboticsFacility, true) +
                                  count(state, UnitKind::stargate, true);
    const auto desiredBuffer = state.self.supplyTotal <= 18
                                   ? 2
                                   : std::clamp(4 + activeProduction * 2, 6, 18);
    const auto projectedSupply = state.self.supplyTotal + pendingPylons * 16;
    // BWAPI's used supply already includes units being trained. The buffer
    // forecasts the next production cycle; adding the queue again bought
    // redundant Pylons while the opening needed combat units.
    const auto projectedUsed = state.self.supplyUsed;
    const auto supplyNeeded = projectedSupply < 400 &&
                              projectedSupply - projectedUsed <= desiredBuffer;
    const auto desiredPylons = std::max(bases, pylons + (supplyNeeded ? 1 : 0));
    const auto openingPylonDeadline = pylons == 0 &&
                                      (state.self.supplyUsed >= 12 ||
                                       state.frame >= 45 * 24);
    goal(plan, GoalKind::build, UnitKind::pylon, desiredPylons, 100,
         "maintain a supply buffer",
         openingPylonDeadline || state.self.supplyTotal - state.self.supplyUsed <= 4);
    goal(plan, GoalKind::train, UnitKind::probe, std::max(workers, plan.desiredWorkers), 93,
         "maintain continuous worker production");
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
                               !activeApproach(state, threat) &&
                               threat.immediateGround <= 0.45;
    if (plan.posture != Posture::defend) {
        goal(plan, GoalKind::expand, UnitKind::nexus, plan.desiredBases,
             expansionSafe ? 95 : 65, "match economic phase", expansionSafe);
    }
    if (plan.desiredGasWorkers > 0) {
        goal(plan, GoalKind::build, UnitKind::assimilator, bases, 80, "fund technology");
    }

    // Size baseline production from the live economy. A useful tournament
    // rule is roughly one continuously-produced combat unit per six workers;
    // keep that throughput behind the intended expansion so it cannot crowd
    // out a planned Nexus.
    const auto gateways = count(state, UnitKind::gateway);
    const auto protectPvPTech = state.enemy.race == Race::protoss &&
        ((count(state, UnitKind::cyberneticsCore) > 0 &&
          count(state, UnitKind::roboticsFacility) == 0 &&
          count(state, UnitKind::zealot, true) >= 4) ||
         (count(state, UnitKind::roboticsFacility) > 0 &&
          count(state, UnitKind::roboticsSupportBay) == 0));
    // A one-base mirror with a healthy bank can sustain four Gateways. The
    // old three-Gateway ceiling left minerals idle while the opponent's
    // production kept scaling, even after our army had stabilized the main.
    const auto productionCeiling = std::clamp(bases * 4, 2, 12);
    auto throughputTarget = std::clamp(
        (workers + 5) / 6, 1, productionCeiling);
    // Scouting multiple enemy production structures is direct evidence that
    // our income-only heuristic may be too slow. Match most of that observed
    // capacity without blindly copying it across asymmetric race mechanics.
    const auto observedParity = std::clamp(
        static_cast<int>(std::ceil(threat.enemyProductionCapacity * 0.8)),
        1, productionCeiling);
    throughputTarget = std::max(throughputTarget, observedParity);
    if (!protectPvPTech && state.frame >= 4 * 60 * 24 && bases >= plan.desiredBases &&
        gateways < throughputTarget) {
        goal(plan, GoalKind::build, UnitKind::gateway, throughputTarget,
             observedParity > (workers + 5) / 6 ? 78 : 73,
             observedParity > (workers + 5) / 6
                 ? "match scouted enemy production capacity"
                 : "match army throughput to the mining economy");
    }
    // A large residual bank still means infrastructure is the bottleneck,
    // even if recent worker losses make the throughput estimate conservative.
    if (!protectPvPTech && state.frame >= 4 * 60 * 24 && state.self.minerals >= 650 &&
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
            // Storm is the actual scaling answer to a packed Marine force. A
            // minute-scaled Dragoon goal otherwise spends every 125-mineral
            // increment before the bank can reach Storm's 200-mineral cost.
            // Make the tech path reserve first, then fill idle Gateways with
            // Templar and routine ranged production.
            const auto frontline =
                count(state, UnitKind::zealot, true) +
                count(state, UnitKind::dragoon, true) +
                count(state, UnitKind::darkTemplar, true);
            const auto spellWindow = plan.posture != Posture::defend || frontline >= 8;
            if (spellWindow) {
                goal(plan, GoalKind::build, UnitKind::citadelOfAdun, 1, 100,
                     "unlock the decisive anti-bio spell", true);
                goal(plan, GoalKind::build, UnitKind::templarArchives, 1, 100,
                     "unlock the decisive anti-bio spell", true);
                technologyGoal(plan, TechnologyKind::psionicStorm, 1, 100,
                               "counter observed bio mass", true);
                goal(plan, GoalKind::train, UnitKind::highTemplar,
                     std::clamp(bio / 3, 3, 7), 98,
                     "punish clustered Terran bio");
            }
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
        plan.desiredGasWorkers = std::max(3, plan.desiredGasWorkers);
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
        plan.desiredGasWorkers = std::max(3, plan.desiredGasWorkers);
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

    // Once a defensive anchor exists, losing every new mineral to an army
    // target is a death spiral: no economy remains to replace that army. A
    // critical worker floor therefore outranks continuing reinforcement, but
    // only after static/army safety exists (or the worker line is almost gone).
    const auto mobileAnchor = count(state, UnitKind::zealot, true) >= 3 ||
                              count(state, UnitKind::dragoon, true) >= 2;
    const auto completedCannons = count(state, UnitKind::photonCannon, true);
    const auto staticRecoveryAnchor = completedCannons >= 3;
    const auto defensiveAnchor = completedCannons > 0 ||
                                 count(state, UnitKind::shieldBattery, true) > 0 ||
                                 mobileAnchor;
    const auto recoveryCanSpend = plan.posture != Posture::defend ||
                                  mobileAnchor || staticRecoveryAnchor || workers <= 3;
    if (state.frame >= 4 * 60 * 24 && completedNexuses > 0 && workers < 8 &&
        recoveryCanSpend && (defensiveAnchor || workers <= 3)) {
        plan.name += " [critical worker floor]";
        plan.desiredWorkers = std::max(plan.desiredWorkers, 8);
        goal(plan, GoalKind::train, UnitKind::probe, 8, 105,
             "rebuild a minimum income behind the defensive screen", true);
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
            plan.minimumAttackSize = std::max(6, plan.minimumAttackSize - 2);
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
            plan.minimumAttackSize += 2;
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
    // A defensive candidate is not evidence by itself: the matchup planner
    // can remain defensive for several frames after a threat has cleared.
    // Only reset the emergency timer for a current breach, a meaningful army
    // approach, explicit rush/proxy evidence, or a very early high-pressure
    // opening. This prevents a stale nearby army from self-sustaining Defend.
    const auto directBreach = hardBreachAtMain(state);
    const auto inferredBreach = threat.combatEnemiesNearMain > 0 &&
                                (state.enemy.units.empty() || directBreach);
    const auto meaningfulApproach = activeApproach(state, threat) &&
                                    (directBreach ||
                                     threat.approachingArmyValue >= 10.0);
    const auto emergencyEvidence = candidate.posture == Posture::recover ||
                                   inferredBreach || meaningfulApproach ||
                                   threat.workerRush > 0.30 ||
                                   threat.proxy + threat.staticContain > 0.34 ||
                                   (state.frame < 8 * 60 * 24 &&
                                    threat.immediateGround > 0.60 &&
                                    (state.enemy.units.empty() || directBreach));

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
