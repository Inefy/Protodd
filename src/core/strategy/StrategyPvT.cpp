#include "protodd/Strategy.hpp"
#include "protodd/LocalThreatQueries.hpp"
#include "../StrategyDetail.hpp"

namespace protodd {
using namespace strategy_detail;

StrategicPlan StrategyEngine::planPvT(
    const GameState& state,
    const ThreatAssessment& threat) const {
    StrategicPlan result;
    result.name = "PvT 28 Nexus";
    result.desiredWorkers = std::min(72, 22 + minute(state) * 4);
    const auto expansionReady = effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed) >= 3 &&
                                supplyAtLeast(state, DisplayedSupply{28});
    result.desiredBases = expansionReady || effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed) >= 2 ?
                              (minute(state) < 11 ? 2 : 3) : 1;
    result.maximumBases = effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed) < 2 ? result.desiredBases : 8;
    result.desiredGasWorkers = !supplyAtLeast(state, DisplayedSupply{11}) ? 0 :
                               (minute(state) < 7 ? 3 :
                                (minute(state) < 12 ? 6 : 9));
    result.posture = minute(state) < 6 || openingPressureExpected(state, threat)
                         ? Posture::hold
                         : Posture::pressure;
    result.attackThreshold = 1.32;
    result.minimumAttackSize = 14;
    result.composition = {{UnitKind::dragoon, 0.55}, {UnitKind::zealot, 0.22},
                          {UnitKind::highTemplar, 0.13}, {UnitKind::arbiter, 0.10}};

    const auto home = ourMain(state);
    const auto enemyHome = enemyMain(state);
    const auto forwardBio = minute(state) < 7 && home.valid() &&
        std::ranges::any_of(state.enemy.units, [home, enemyHome](const UnitSnapshot& enemy) {
            if (!enemy.visible || !enemy.completed || !enemy.position.valid() ||
                (enemy.kind != UnitKind::marine && enemy.kind != UnitKind::firebat)) return false;
            return withinPixelRadius(enemy.position, home, PixelRadius{1600}) ||
                (enemyHome.valid() && distanceSquared(enemy.position, home) <
                                      distanceSquared(enemy.position, enemyHome));
        });
    // Fortify only when current observations justify the economic cost.
    // Unknown Terran openings use the Gateway/Core baseline.
    // A very early Marine is evidence even when the scout sees only one
    // Barracks. Retain that timing warning while the opening crosses the map.
    const auto earlyMarineTiming = std::ranges::any_of(state.enemy.units, [](const UnitSnapshot& enemy) {
        return enemy.kind == UnitKind::marine && enemy.completed && !enemy.hallucination &&
               enemy.firstSeen > 0 && enemy.firstSeen <= framesForMinutes(2);
    });
    const auto productionRush = minute(state) < 6 &&
        (recentEnemyCount(state, UnitKind::barracks) >= 2 || earlyMarineTiming) &&
        recentEnemyCount(state, UnitKind::factory) == 0 &&
        recentEnemyCount(state, UnitKind::commandCenter) <= 1;
    const auto bioOpening = forwardBio || productionRush;
    const auto observedMechUnits = recentEnemyCount(state, UnitKind::vulture) +
        recentEnemyCount(state, UnitKind::siegeTank) +
        recentEnemyCount(state, UnitKind::goliath) +
        recentEnemyCount(state, UnitKind::spiderMine);
    const auto observedFactories = recentEnemyCount(state, UnitKind::factory);
    const auto observedMechProduction = observedFactories >= 2 ||
        (observedFactories > 0 && recentEnemyCount(state, UnitKind::machineShop) > 0);
    const auto mechEvidence = observedMechUnits > 0 || observedMechProduction;
    const auto visibleMechNearHome = home.valid() &&
        std::ranges::any_of(state.enemy.units, [home](const UnitSnapshot& enemy) {
            const auto mechUnit = enemy.kind == UnitKind::vulture ||
                enemy.kind == UnitKind::siegeTank || enemy.kind == UnitKind::goliath ||
                enemy.kind == UnitKind::spiderMine;
            return mechUnit && enemy.visible && enemy.completed && !enemy.disabled &&
                enemy.position.valid() && withinPixelRadius(enemy.position, home, PixelRadius{1600});
        });
    const auto approachingMech = home.valid() &&
        std::ranges::any_of(state.enemy.units, [home](const UnitSnapshot& enemy) {
            return (enemy.kind == UnitKind::vulture || enemy.kind == UnitKind::siegeTank ||
                    enemy.kind == UnitKind::goliath) &&
                approachingCombatAnchor(enemy, home, PixelRadius{2400});
        });
    const auto mechPressure = mechEvidence &&
        (visibleMechNearHome || approachingMech ||
         threat.combatEnemiesNearMain > 0 || activeApproach(state, threat) ||
         threat.immediateGround > 0.45);
    const auto nonBioRushEvidence = threat.workerRush > 0.30 ||
        threat.proxy + threat.staticContain > 0.34;
    const auto ambiguousGroundPressure = threat.combatEnemiesNearMain > 0 ||
        activeApproach(state, threat) || threat.immediateGround > 0.45;
    const auto antiBioDefense = bioOpening || nonBioRushEvidence ||
        (ambiguousGroundPressure && !mechPressure);
    const auto rushEvidence = antiBioDefense || mechPressure;
    if (antiBioDefense && minute(state) < 4 && effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed) == 0 &&
        effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::completed) == 0) {
        result.desiredWorkers = std::min(result.desiredWorkers, 7);
    }
    if (antiBioDefense && supplyAtLeast(state, DisplayedSupply{7})) {
        goal(result, GoalKind::build, UnitKind::forge, 1, bioOpening ? 113 : 100,
             "fortified anti-bio opening anchor", true);
        goal(result, GoalKind::build, UnitKind::photonCannon, 2, bioOpening ? 112 : 99,
             "overlap the intercept before first contact", true);
        goal(result, GoalKind::build, UnitKind::gateway, 1, 98,
             "field mobile defense behind the completed static intercept", true);
    }

    if (supplyAtLeast(state, DisplayedSupply{10}) || (rushEvidence && supplyAtLeast(state, DisplayedSupply{8}))) {
        goal(result, GoalKind::build, UnitKind::gateway, minute(state) < 6 ? 1 : 3, 88,
             "Gateway opening before gas and Core", effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::observed) == 0);
    }
    if (effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::observed) > 0 || supplyAtLeast(state, DisplayedSupply{10})) {
        goal(result, GoalKind::train, UnitKind::zealot, 1, 98,
             "opening bodyguard before vulnerable dragoon tech",
             effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::observed) == 0);
    }
    if (antiBioDefense && (effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::observed) > 0 || supplyAtLeast(state, DisplayedSupply{10}))) {
        goal(result, GoalKind::build, UnitKind::shieldBattery, 1, 89,
             "opening sustain against bio pressure");
    }
    if (supplyAtLeast(state, DisplayedSupply{13})) {
        goal(result, GoalKind::build, UnitKind::cyberneticsCore, 1, 97,
             "thirteen-supply cybernetics core", true);
    }
    if (supplyAtLeast(state, DisplayedSupply{11})) {
        goal(result, GoalKind::build, UnitKind::assimilator, 1, 96,
             "opening gas for Dragoons and range", true);
    }
    if (supplyAtLeast(state, DisplayedSupply{14})) {
        goal(result, GoalKind::train, UnitKind::dragoon, std::max(3, minute(state) * 2), 91,
             "range control against Terran");
        if (effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::observed) >= 1)
            technologyGoal(result, TechnologyKind::singularityCharge, 1, 98,
                           "range follows the first Dragoon before expansion", true);
    }
    if (productionRush && effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::completed) < 2) {
        result.desiredWorkers = std::min(result.desiredWorkers, 16);
        // The observed production is an earlier warning than Marines reaching
        // our perimeter. Spend on the intercept while they cross the map.
        if (state.self.gas >= 100) result.desiredGasWorkers = 0;
    }
    if (effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed) >= 2 || rushEvidence || threat.cloak > 0.28) {
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
    const auto establishedAntiPressureScreen =
        effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::completed) >= 3 &&
        effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::completed) > 0 &&
        effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::completed) >= 2 &&
        effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed) + effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed) >= 4;
    const auto establishedMechResponse =
        effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::completed) > 0 &&
        effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed) >= 4 &&
        effectiveUnitCount(state, UnitKind::observer, UnitCountBasis::completed) > 0;
    const auto localAntiPressure = forwardBio || nonBioRushEvidence ||
        (ambiguousGroundPressure && !mechPressure);
    // Global aggression is useful while the opening screen is incomplete, but
    // it can outlive the rush that raised it. Once the static and ranged
    // checkpoints are complete, only current local evidence keeps this hold
    // active; renewed contact asks for a small reinforcement, not a replay of
    // the opening build.
    if (minute(state) < 10 &&
        (localAntiPressure ||
         (threat.aggression > 0.62 && !establishedAntiPressureScreen))) {
        result.name = "PvT anti-pressure hold";
        result.posture = Posture::defend;
        if (establishedAntiPressureScreen) {
            result.desiredBases = std::min(
                result.desiredBases, std::max(1, effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed)));
        } else {
            result.desiredBases = minute(state) < 8 ? 1 : std::min(result.desiredBases, 2);
            result.desiredWorkers = std::min(result.desiredWorkers, 14);
        }
        result.attackThreshold = 1.55;
        if (!establishedAntiPressureScreen && effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::observed) > 0 &&
            effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::observed) == 0) {
            goal(result, GoalKind::train, UnitKind::zealot, 1, 105,
                 "field one mobile defender before adding more structures", true);
        }
        if (effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::completed) >= 2) {
            goal(result, GoalKind::build, UnitKind::pylon, 2, 102,
                 "give the forward defensive shell redundant power", true);
        }
        // Four Cannons are a screen, not the army. Continuing to replace an
        // aspirational six-Cannon target under fire can consume every mineral
        // and leave completed Gateways idle. Stop at four, then add the third
        // Gateway only after a mobile front line exists.
        const auto gatewayTarget = minute(state) >= 5 &&
                                           effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed) >= 4
                                       ? 3
                                       : 2;
        const auto cannonTarget = establishedAntiPressureScreen
                                      ? effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::observed)
                                      : (minute(state) >= 5 ? 4 : 3);
        const auto defensiveGatewayTarget = establishedAntiPressureScreen
                                                ? effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::observed)
                                                : gatewayTarget;
        goal(result, GoalKind::build, UnitKind::photonCannon, cannonTarget, 101,
             "scale the mineral-line anchor with sustained bio", true);
        goal(result, GoalKind::build, UnitKind::gateway, defensiveGatewayTarget, 100,
             "add emergency anti-pressure throughput", true);
        goal(result, GoalKind::build, UnitKind::shieldBattery, 1, 99,
             "complete a sustainable defensive screen", true);
        const auto screenEstablished =
            effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::completed) >= 3;
        if (screenEstablished) {
            // Once three Cannons are complete this checkpoint is permanent.
            // Tying the priority to a live Zealot count made one combat loss
            // demote the Core before its saved minerals could be spent.
            goal(result, GoalKind::build, UnitKind::cyberneticsCore, 1, 99,
                 "unlock the ranged counter after emergency production", true);
            goal(result, GoalKind::build, UnitKind::assimilator, 1, 99,
                 "fund the ranged counter after emergency production", true);
            const auto cloakWindow =
                effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::completed) >= 4;
            const auto cloakCommitted = effectiveUnitCount(state, UnitKind::citadelOfAdun, UnitCountBasis::observed) > 0 ||
                                        effectiveUnitCount(state, UnitKind::templarArchives, UnitCountBasis::observed) > 0 ||
                                        effectiveUnitCount(state, UnitKind::darkTemplar, UnitCountBasis::observed) > 0;
            if (effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::completed) > 0 &&
                (cloakWindow || cloakCommitted)) {
                result.name += " [anti-bio dark templar]";
                goal(result, GoalKind::build, UnitKind::citadelOfAdun, 1, 99,
                     "cloak transition against sustained opening bio", true);
                if (effectiveUnitCount(state, UnitKind::citadelOfAdun, UnitCountBasis::completed) > 0) {
                    goal(result, GoalKind::build, UnitKind::templarArchives, 1, 99,
                         "complete the cloak transition", true);
                }
                if (effectiveUnitCount(state, UnitKind::templarArchives, UnitCountBasis::completed) > 0) {
                    goal(result, GoalKind::train, UnitKind::darkTemplar, 3, 102,
                         "clear bio and counterattack before detection", true);
                    setCompositionWeight(result, UnitKind::darkTemplar, 0.24);
                }
            }
        }
        const auto zealotTarget = establishedAntiPressureScreen
                                      ? std::min(6, effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed) + 2)
                                      : (minute(state) >= 5 ? 10 : 6);
        goal(result, GoalKind::train, UnitKind::zealot,
             zealotTarget, 98,
             "field bodies before vulnerable dragoon tech", true);
        const auto dragoonInfrastructureReady =
            effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::completed) > 0;
        const auto rangedReservationSafe =
            dragoonInfrastructureReady && state.self.gas >= 50 &&
            effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::completed) >= 2;
        if (dragoonInfrastructureReady && effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::observed) == 0) {
            goal(result, GoalKind::train, UnitKind::dragoon, 1, 104,
                 "field the first ranged defender before support infrastructure",
                 rangedReservationSafe);
        }
        goal(result, GoalKind::train, UnitKind::dragoon, 8,
             dragoonInfrastructureReady ? 99 : 96,
             "transition to ranged defense after its prerequisite completes",
             rangedReservationSafe);
    }
    if (mechPressure && minute(state) < 10) {
        // Tanks, Vultures and Mines need range, mobile detection and room to
        // grow the economy behind a mobile screen. Reusing the Marine-rush
        // Cannon/Zealot queue spends minerals on the wrong engagement shape.
        result.name = "PvT anti-mech response";
        result.posture = Posture::defend;
        result.attackThreshold = std::max(result.attackThreshold, 1.55);
        if (!establishedMechResponse) {
            result.desiredBases = std::min(
                result.desiredBases, std::max(1, effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed)));
        }
    }
    return result;
}


}  // namespace protodd
