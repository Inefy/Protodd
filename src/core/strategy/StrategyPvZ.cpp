#include "protodd/Strategy.hpp"
#include "protodd/LocalThreatQueries.hpp"
#include "../StrategyDetail.hpp"

namespace protodd {
using namespace strategy_detail;

StrategicPlan StrategyEngine::planPvZ(
    const GameState& state,
    const ThreatAssessment& threat) const {
    StrategicPlan result;
    const auto zerglings = recentEnemyCount(state, UnitKind::zergling);
    const auto hydraLurkers = recentEnemyCount(state, UnitKind::hydralisk) +
                              recentEnemyCount(state, UnitKind::lurker);
    const auto zergAir = recentEnemyCount(state, UnitKind::mutalisk) +
                         recentEnemyCount(state, UnitKind::guardian) +
                         recentEnemyCount(state, UnitKind::devourer);
    const auto earlyGroundPressure = openingPressureExpected(state, threat) ||
        threat.combatEnemiesNearMain > 0 || activeApproach(state, threat) ||
        (minute(state) < 6 && zerglings > 0);
    const auto gatewayFirst = pvzGatewayOpening_ && !earlyGroundPressure;
    // A Zergling seen at the enemy base is evidence of production, not of an
    // attack on our fortified natural. The default opening treats any recent
    // Zergling as pressure; the replay opening uses immediate spatial threat
    // here so a distant scout does not cancel its economy window for 90 seconds.
    const auto replayOpeningPressure = openingPressureExpected(state, threat) ||
        threat.combatEnemiesNearMain > 0 || activeApproach(state, threat) ||
        threat.immediateGround > 0.45;
    const auto replayEconomyOpening = pvzReplayOpening_ && !replayOpeningPressure &&
        threat.workerRush <= 0.30 && !hardBreachAtMain(state) && minute(state) < 8;
    result.name = replayEconomyOpening ? "PvZ replay-derived fortified expansion" :
        (gatewayFirst ? "PvZ gateway-first mobile opening" :
                        "PvZ fortified gateway economy");
    result.desiredWorkers = std::min(70, 20 + minute(state) * 4);
    result.desiredBases = minute(state) < 3 ? 1 : (minute(state) < 11 ? 2 : 3);
    result.desiredGasWorkers = !supplyAtLeast(state, DisplayedSupply{14}) ? 0 :
                               (minute(state) < 8 ? 3 :
                                (minute(state) < 12 ? 6 : 9));
    result.posture = minute(state) < 8 ? Posture::hold : Posture::harass;
    result.attackThreshold = 1.2;
    result.minimumAttackSize = 14;
    result.composition = {{UnitKind::zealot, 0.58}, {UnitKind::dragoon, 0.32},
                          {UnitKind::archon, 0.10}};

    // The natural Nexus can be committed several minutes before routine
    // supply calls for another Pylon. Power it while the builder is still
    // travelling so a defensive Cannon has a legal footprint when the next
    // Zerg wave arrives, rather than finishing the Pylon after the base falls.
    if (minute(state) < 10 && threat.combatEnemiesNearMain == 0 &&
        !activeApproach(state, threat) && threat.immediateGround < 0.45) {
        const auto home = ourMain(state);
        const auto unpoweredExpansion = std::ranges::any_of(
            state.self.units, [&](const UnitSnapshot& nexus) {
                if (nexus.kind != UnitKind::nexus || !nexus.position.valid() ||
                    !home.valid() || withinPixelRadius(nexus.position, home, PixelRadius{320}))
                    return false;
                return std::ranges::none_of(state.self.units,
                    [&](const UnitSnapshot& pylon) {
                        return pylon.kind == UnitKind::pylon && pylon.position.valid() &&
                               withinPixelRadius(pylon.position, nexus.position, PixelRadius{384});
                    });
            });
        if (unpoweredExpansion)
            goal(result, GoalKind::build, UnitKind::pylon,
                 effectiveUnitCount(state, UnitKind::pylon, UnitCountBasis::observed) + 1, 104,
                 "power the new PvZ natural before pressure", true);
    }

    // A pool-first Zerg can make contact before a conventional Gateway army
    // has enough surface area. Pause briefly at eight workers and establish a
    // static anchor; resume Probe growth as soon as either that anchor or two
    // Zealots are complete. This remains safe even when the first scout dies.
    if (!replayEconomyOpening && minute(state) < 4 &&
        effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed) < 2 &&
        effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::completed) == 0) {
        const auto openingWorkerLimit = gatewayFirst &&
            effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::observed) > 0 ? 11 : 8;
        result.desiredWorkers = std::min(result.desiredWorkers, openingWorkerLimit);
    }

    if (supplyAtLeast(state, DisplayedSupply{10}) && effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::observed) >= 2 &&
        effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::observed) >= 2) {
        goal(result, GoalKind::build, UnitKind::forge, 1, 82,
             "forge after two-gate opening safety");
    }
    if (minute(state) >= 4 && threat.immediateGround <= 0.45 &&
        effectiveUnitCount(state, UnitKind::forge, UnitCountBasis::observed) > 0) {
        technologyGoal(result, TechnologyKind::protossGroundWeapons, 1, 87,
                       "zealot attack timing");
    }
    const auto defensiveBases = std::max(1, effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed));
    const auto openingGroundSafe = effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::observed) >= 2 &&
                                   effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::observed) >= 3;
    const auto canCommitToForge = effectiveUnitCount(state, UnitKind::forge, UnitCountBasis::observed) > 0 ||
                                  openingGroundSafe || minute(state) >= 4;
    if (canCommitToForge &&
        (defensiveBases >= 2 || threat.immediateGround > 0.25 || threat.air > 0.35)) {
        const auto safetyCannons = defensiveBases + (threat.air > 0.45 ? 2 : 0);
        goal(result, GoalKind::build, UnitKind::photonCannon,
             std::min(6, safetyCannons), 89, "ling and mutalisk coverage");
    }
    if (supplyAtLeast(state, DisplayedSupply{7})) {
        if (!gatewayFirst) {
            goal(result, GoalKind::build, UnitKind::forge, 1, 100,
                 "fortified PvZ opening anchor", true);
            goal(result, GoalKind::build, UnitKind::photonCannon, 1, 99,
                 "baseline anti-ling safety before economic commitment", true);
        }
        const auto replaySecondGatewayReady =
            effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::completed) >= 2 &&
            effectiveUnitCount(state, UnitKind::probe, UnitCountBasis::observed) >= 28 &&
            effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::observed) > 0;
        const auto openingGateways = replayEconomyOpening && !replaySecondGatewayReady
            ? 1 : (supplyAtLeast(state, DisplayedSupply{8}) ? 2 : 1);
        // Once the three-unit safety screen is established, additional
        // Gateways and minute-scaled Zealots are throughput goals. Their
        // opening priority must not outbid a newly observed counter deadline.
        const auto openingGatewayPriority = gatewayFirst && openingGateways == 1 ? 100 :
            (minute(state) >= 8 && openingGateways > 1 ? 70 :
             (openingGateways == 1 ? 98 : 97));
        goal(result, GoalKind::build, UnitKind::gateway,
             minute(state) < 8 || replayEconomyOpening ? openingGateways : 4,
             openingGatewayPriority,
             replayEconomyOpening ? "one Gateway while the fortified natural starts" :
                                    "seven-supply gateway into two-gate Zerg safety",
             effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::observed) < openingGateways);
        const auto openingScreenPriority = effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed) < 3 ||
                minute(state) < 8
            ? 99 : 68;
        goal(result, GoalKind::train, UnitKind::zealot,
             replayEconomyOpening ? std::max(3, minute(state)) :
                                    std::max(4, minute(state)), openingScreenPriority,
             replayEconomyOpening ? "opening escort for the fortified natural" :
                                    "opening defenders before Forge economy",
             effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::observed) < (replayEconomyOpening ? 2 : 3));
    }
    if (replayEconomyOpening && effectiveUnitCount(state, UnitKind::pylon, UnitCountBasis::observed) > 0) {
        // Frozen qualified PvZ replays had a median 17 Probes and a Nexus
        // underway by frame 4800, then 26 Probes on two bases by frame 7200.
        // The default eight-worker pause and second early Gateway miss both
        // milestones. Keep one Probe cycle ahead of optional army filler.
        goal(result, GoalKind::train, UnitKind::probe,
             result.desiredWorkers, 101,
             "continuous workers in the replay-derived opening", true);
    }
    // The gateway-first pilot fielded four Zealots early, then reserved the
    // whole mineral bank for a Nexus while both Gateways sat idle. Complete a
    // mobile screen and the ranged-tech checkpoint before that reservation.
    if (pvzGatewayOpening_ && effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed) < 2 && minute(state) < 10) {
        const auto mobileScreen = effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed) >= 6;
        const auto rangedAccess = effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::observed) > 0;
        if (!mobileScreen || !rangedAccess) result.desiredBases = 1;
        if (!mobileScreen) {
            goal(result, GoalKind::train, UnitKind::zealot, 6, 102,
                 "keep both opening Gateways producing before the natural", true);
        } else if (!rangedAccess) {
            goal(result, GoalKind::build, UnitKind::cyberneticsCore, 1, 101,
                 "unlock ranged units before the natural", true);
        }
    }
    // The mobile/Core pilot reached six Zealots before Zerg massed its first
    // large wave, then spent the timing at home covering a Nexus. Use that
    // short window to threaten the nearest known Zerg base. Local combat
    // estimation retains authority to back away from a prepared defense.
    if (pvzGatewayOpening_ && pvzEarlyPressureEligible(state, threat)) {
        const auto home = ourMain(state);
        auto target = enemyMain(state);
        for (const auto& base : state.bases) {
            if (base.ownerId != state.enemy.id || !base.center.valid() ||
                !home.valid()) continue;
            if (!target.valid() ||
                distanceSquared(home, base.center) < distanceSquared(home, target))
                target = base.center;
        }
        if (target.valid()) {
            result.name += " [six-Zealot expansion pressure]";
            result.posture = Posture::pressure;
            result.minimumAttackSize = 6;
            result.attackThreshold = 1.20;
            result.attackTarget = target;
            result.rallyPoint = home.valid() ? moveToward(home, target, 640.0) : target;
            result.desiredBases = 1;
        }
    }
    const auto earlySplashCore = pvzEarlySplash_ && minute(state) >= 5 &&
        effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::completed) >= 2 &&
        effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed) >= 3 &&
        effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::completed) >= 1 &&
        threat.combatEnemiesNearMain == 0 && !activeApproach(state, threat) &&
        threat.immediateGround <= 0.45;
    if (supplyAtLeast(state, DisplayedSupply{15}) || earlySplashCore) {
        goal(result, GoalKind::build, UnitKind::cyberneticsCore, 1,
             earlySplashCore ? 96 : 82,
             earlySplashCore ? "prepare a ranged or splash response behind the opening screen" :
                               "air and dragoon access", earlySplashCore);
    }
    const auto hydraEvidence = hydraLurkers >= 2 ||
        recentEnemyEvidence(state, UnitKind::hydraliskDen);
    const auto airEvidence = zergAir >= 2 || threat.air > 0.45 ||
        recentEnemyEvidence(state, UnitKind::spire) ||
        recentEnemyEvidence(state, UnitKind::greaterSpire);
    // Approved train replays of fast Hydra attacks commonly have a third
    // powered Cannon after the natural's first two are finished. Stage each
    // extra Cannon so this screen cannot displace the second or the Core.
    if (pvzPoweredCannonScreen_ && state.frame >= framesForMinutes(5) &&
        state.frame < framesForMinutes(11) &&
        effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::completed) >= 2 &&
        effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::completed) >= 1 &&
        effectiveUnitCount(state, UnitKind::probe, UnitCountBasis::completed) >= 26 &&
        effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed) >= 4 &&
        !hardBreachAtMain(state)) {
        const auto completedCannons = effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::completed);
        if (completedCannons >= 2 && completedCannons < 4) {
            goal(result, GoalKind::build, UnitKind::photonCannon,
                 completedCannons + 1, 100,
                 "staged powered natural screen against Hydra pressure", true);
        }
    }
    // Begin splash tech after an actual third Cannon completes. Placement can
    // still select the main, so the arena review separately checks natural
    // coverage. Waiting for Hydra contact left the earlier Reaver finishing
    // during the base collapse.
    if (pvzProactiveReaver_ && state.frame >= framesForMinutes(5) &&
        state.frame < framesForMinutes(11) &&
        effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::completed) >= 2 &&
        effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::completed) >= 1 &&
        effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::completed) >= 3 &&
        effectiveUnitCount(state, UnitKind::probe, UnitCountBasis::completed) >= 26 &&
        effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed) >= 4 &&
        !hardBreachAtMain(state)) {
        result.name += " [proactive Reaver behind natural screen]";
        result.desiredGasWorkers = std::max(6, result.desiredGasWorkers);
        goal(result, GoalKind::build, UnitKind::roboticsFacility, 1, 104,
             "begin first Reaver before Hydra contact", true);
        if (effectiveUnitCount(state, UnitKind::roboticsFacility, UnitCountBasis::observed) > 0)
            goal(result, GoalKind::build, UnitKind::roboticsSupportBay, 1, 103,
                 "complete first Reaver technology behind natural screen", true);
        if (effectiveUnitCount(state, UnitKind::roboticsSupportBay, UnitCountBasis::observed) > 0)
            goal(result, GoalKind::train, UnitKind::reaver, 1, 104,
                 "field first splash unit before a Hydra mass", true);
    }
    if (pvzEarlySplash_ && hydraEvidence && !airEvidence &&
        effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed) >= 3 &&
        effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::completed) >= 1 &&
        threat.combatEnemiesNearMain == 0 && !hardBreachAtMain(state)) {
        result.name += " [early Reaver screen]";
        goal(result, GoalKind::build, UnitKind::roboticsFacility, 1, 101,
             "start splash production on first Hydra evidence", true);
        goal(result, GoalKind::build, UnitKind::roboticsSupportBay, 1, 100,
             "finish the first Reaver prerequisite", true);
        goal(result, GoalKind::train, UnitKind::reaver, 1, 100,
             "field splash before the Hydra mass reaches the bases", true);
    }
    const auto establishedTechScreen = minute(state) >= 6 &&
        effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::completed) > 0 &&
        effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::completed) >= 2 &&
        effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed) +
            effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed) >= 4 &&
        threat.combatEnemiesNearMain == 0 && !activeApproach(state, threat) &&
        threat.immediateGround <= 0.45 && !hardBreachAtMain(state);

    // Air tech starts from a legal Spire/flyer sighting or the model's air
    // estimate. The Stargate is the blocking checkpoint; Corsair training is
    // opportunistic so it cannot reserve away the worker cycle.
    if (airEvidence && effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::completed) > 0 &&
        !hardBreachAtMain(state)) {
        result.name += " [observed-air Corsair package]";
        result.desiredGasWorkers = std::max(4, result.desiredGasWorkers);
        goal(result, GoalKind::build, UnitKind::stargate, 1, 96,
             "open a powered Stargate against observed Zerg air", true);
        const auto corsairTarget = zergAir == 0 ? 3 :
            std::clamp(4 + zergAir / 2, 4, 8);
        goal(result, GoalKind::train, UnitKind::corsair, corsairTarget, 94,
             "answer observed Zerg flyers");
        setCompositionWeight(result, UnitKind::corsair, 0.28);
        if (zergAir >= 4) {
            goal(result, GoalKind::train, UnitKind::dragoon,
                 std::max(6, effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::observed)), 89,
                 "protect the ground screen from observed Zerg flyers");
            setCompositionWeight(result, UnitKind::dragoon, 0.22);
            if (effectiveUnitCount(state, UnitKind::corsair, UnitCountBasis::completed) >= 3) {
                technologyGoal(result, TechnologyKind::protossAirWeapons, 1, 78,
                               "upgrade the fielded anti-air package");
            }
        }
    }

    // A medium Zergling screen calls for a bounded Zealot speed package;
    // larger Ling swarms get the separate Storm package after the mobile
    // screen is ready. Hydra/Lurker mass keeps its dedicated Reaver deadline.
    if (establishedTechScreen && hydraLurkers < 7 &&
        zerglings >= 6 && zerglings < 12 &&
        effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed) >= 4) {
        result.name += " [observed-Ling speed package]";
        result.desiredGasWorkers = std::max(4, result.desiredGasWorkers);
        technologyGoal(result, TechnologyKind::legEnhancements, 1, 91,
                       "speed the screened Zealots against observed Zerglings", true);
        const auto zealotTarget = std::min(8, effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::observed) +
                                                 std::clamp(zerglings / 3, 2, 4));
        goal(result, GoalKind::train, UnitKind::zealot, zealotTarget, 88,
             "field a bounded speed-Zealot support group");
        setCompositionWeight(result, UnitKind::zealot, 0.48);
    }

    const auto stormEvidence = establishedTechScreen && hydraLurkers < 7 &&
                               zerglings >= 12;
    if (stormEvidence) {
        result.name += " [observed-swarm Storm package]";
        result.desiredGasWorkers = std::max(6, result.desiredGasWorkers);
        technologyGoal(result, TechnologyKind::psionicStorm, 1, 96,
                       "research Storm against the observed Zergling swarm", true);
        if (technologyLevel(state.self, TechnologyKind::psionicStorm) > 0) {
            const auto templarTarget = std::clamp(2 + zerglings / 12, 2, 4);
            goal(result, GoalKind::train, UnitKind::highTemplar, templarTarget, 90,
                 "field a bounded Storm support group");
            setCompositionWeight(result, UnitKind::highTemplar, 0.24);
            if (effectiveUnitCount(state, UnitKind::highTemplar, UnitCountBasis::completed) >= 2) {
                technologyGoal(result, TechnologyKind::khaydarinAmulet, 1, 76,
                               "extend Storm energy after the first casters reach play");
            }
        }
    }
    const auto archiveWindow = pvzArchivesFirst_ && minute(state) >= 6 &&
        minute(state) < 12 && effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::completed) >= 2 &&
        effectiveUnitCount(state, UnitKind::probe, UnitCountBasis::observed) >= 26 &&
        effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::completed) > 0 &&
        effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::completed) > 0 &&
        effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed) +
            effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed) >= 4 &&
        threat.combatEnemiesNearMain == 0 && !activeApproach(state, threat) &&
        threat.immediateGround <= 0.45 && !hardBreachAtMain(state);
    if (archiveWindow) {
        // Among frozen train-split PvZ survivors at frame 12000, 114/177 had
        // an Archives but only 3/177 had a Support Bay. The default drop
        // package reserves Robotics/Reaver before the anti-swarm spell path.
        goal(result, GoalKind::build, UnitKind::citadelOfAdun, 1, 103,
             "open the replay-derived Templar tech window", true);
        goal(result, GoalKind::build, UnitKind::templarArchives, 1, 102,
             "Archives before optional Reaver harassment", true);
        if (effectiveUnitCount(state, UnitKind::templarArchives, UnitCountBasis::completed) > 0) {
            technologyGoal(result, TechnologyKind::psionicStorm, 1, 102,
                           "research the anti-swarm spell before drops", true);
            goal(result, GoalKind::train, UnitKind::highTemplar, 1, 101,
                 "first anti-swarm spellcaster before drops", true);
        }
    }

    if (threat.combatEnemiesNearMain > 0 || activeApproach(state, threat) ||
        (minute(state) < 8 && threat.immediateGround > 0.45)) {
        result.name = "PvZ emergency gateway hold";
        result.posture = Posture::defend;
        const auto stabilized = effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::completed) >= 2 &&
                                effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed) >= 4;
        result.desiredBases = stabilized
                                  ? std::min(result.desiredBases,
                                             std::max(1, effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed)))
                                  : 1;
        if (!stabilized && minute(state) < 7) result.desiredGasWorkers = 0;
        else result.desiredGasWorkers = std::max(3, result.desiredGasWorkers);
        if (!stabilized) result.desiredWorkers = std::min(result.desiredWorkers, 12);
        goal(result, GoalKind::build, UnitKind::gateway, 2, 99, "anti-rush production", true);
        goal(result, GoalKind::train, UnitKind::zealot, 8, 98, "hold early ground rush", true);
        if (minute(state) < 6) {
            goal(result, GoalKind::build, UnitKind::shieldBattery, 1, 96,
                 "sustain the anti-ling screen", true);
        }
        goal(result, GoalKind::build, UnitKind::photonCannon, 2, 100,
             "double mineral-line cover against the ground all-in");
        if (effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::completed) >= 2 &&
            effectiveUnitCount(state, UnitKind::probe, UnitCountBasis::observed) < 10) {
            goal(result, GoalKind::train, UnitKind::probe, 10, 100,
                 "recover mining behind completed static safety", true);
        }
        if (effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::completed) >= 2) {
            goal(result, GoalKind::build, UnitKind::pylon, 2, 100,
                 "secure reinforcement supply inside the Cannon shell", true);
        }
    } else if (replayEconomyOpening && state.frame >= framesForMinutes(2) &&
               effectiveUnitCount(state, UnitKind::probe, UnitCountBasis::observed) >= 12 &&
               effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::observed) >= 1 &&
               effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::observed) >= 1) {
        result.desiredBases = 2;
        goal(result, GoalKind::expand, UnitKind::nexus, 2, 103,
             "frozen replay timing for the fortified natural", true);
    } else if (!replayEconomyOpening && minute(state) >= 3 &&
               (effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::observed) >= 2 ||
                effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::observed) >= 1)) {
        goal(result, GoalKind::expand, UnitKind::nexus, 2, 91,
             "forge-fast-expand timing");
    }
    if (replayEconomyOpening && effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed) < 2)
        result.desiredBases = std::min(result.desiredBases,
            effectiveUnitCount(state, UnitKind::probe, UnitCountBasis::observed) >= 12 &&
            effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::observed) >= 1 &&
            effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::observed) >= 1 ? 2 : 1);
    return result;
}


}  // namespace protodd
