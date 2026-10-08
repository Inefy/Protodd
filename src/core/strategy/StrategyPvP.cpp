#include "protodd/Strategy.hpp"
#include "protodd/LocalThreatQueries.hpp"
#include "../StrategyDetail.hpp"

namespace protodd {
using namespace strategy_detail;

StrategicPlan StrategyEngine::planPvP(
    const GameState& state,
    const ThreatAssessment& threat) const {
    const auto home = ourMain(state);
    const auto expansionSite = nearestExpansionSite(state);
    const auto approachingHomeArmyValue = approachingCombatValueAt(
        state, home, PixelRadius{1536}, expansionSite);
    const auto approachingExpansionArmyValue = approachingCombatValueAt(
        state, expansionSite, PixelRadius{1536});
    const auto homeApproach = approachingHomeArmyValue >= 2.0;
    // Supply and actual structures own the opening. Re-evaluate safety every
    // pass; no stored phase or irreversible script cursor is required.
    // The worker scout can see both Gateways while they are still warping.
    // Waiting for completion leaves the quiet Core opening active through
    // the entire second-Gateway build, after which its own mobile response
    // and Forge are too late for the first Zealot crossing.
    const auto scoutedTwoGatewayConstruction = state.frame < framesForMinutes(5) &&
        std::ranges::count(state.enemy.units, UnitKind::gateway,
                           &UnitSnapshot::kind) >= 2 &&
        std::ranges::none_of(state.enemy.units, [](const UnitSnapshot& enemy) {
            return enemy.kind == UnitKind::cyberneticsCore ||
                   enemy.kind == UnitKind::roboticsFacility ||
                   enemy.kind == UnitKind::dragoon || enemy.kind == UnitKind::reaver;
        });
    const auto meleeEvidence = scoutedTwoGatewayConstruction ||
        recentEnemyCount(state, UnitKind::zealot) >= 2 ||
        (recentEnemyCount(state, UnitKind::gateway) >= 2 &&
         recentEnemyCount(state, UnitKind::cyberneticsCore) == 0);
    const auto pressureEvidence = hardBreachAtMain(state) || meleeEvidence ||
        threat.combatEnemiesNearMain > 0 || homeApproach ||
        threat.immediateGround > 0.45 || threat.workerRush > 0.30 ||
        threat.proxy + threat.staticContain > 0.34 || threat.cloak > 0.20;
    if (minute(state) < 8 && !pressureEvidence) {
        StrategicPlan opening;
        opening.name = "PvP ranged economy into Robo";
        opening.desiredWorkers = 32;
        opening.desiredGasWorkers = supplyAtLeast(state, DisplayedSupply{12}) ? 3 : 0;
        opening.minimumAttackSize = 8;
        opening.attackThreshold = 1.25;
        const auto roboCommitted = effectiveUnitCount(state, UnitKind::roboticsFacility, UnitCountBasis::observed) > 0;
        const auto dragoonsReady = effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed);
        const auto detectorReady = effectiveUnitCount(state, UnitKind::observer, UnitCountBasis::completed) > 0;
        opening.desiredBases = dragoonsReady >= 6 ? 2 : 1;
        opening.maximumBases = opening.desiredBases;
        opening.posture = dragoonsReady >= 6 ?
                              Posture::pressure : Posture::hold;
        const auto rangedScreenCommitted = effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::observed) >= 2;
        // The explicit bodyguard goal owns the first Zealot. Letting the
        // composition filler keep making melee during Core construction
        // occupies the Gateway when its first Dragoons become available.
        opening.composition = rangedScreenCommitted
                                  ? std::vector<CompositionTarget>{{UnitKind::dragoon, 0.85},
                                                                   {UnitKind::zealot, 0.15}}
                                  : std::vector<CompositionTarget>{{UnitKind::dragoon, 1.0}};
        if (supplyAtLeast(state, DisplayedSupply{10}))
            goal(opening, GoalKind::build, UnitKind::gateway, 1, 100, "opening Gateway", true);
        if (supplyAtLeast(state, DisplayedSupply{12}))
            goal(opening, GoalKind::build, UnitKind::assimilator, 1, 99, "opening gas", true);
        if (supplyAtLeast(state, DisplayedSupply{12}) && effectiveUnitCount(state, UnitKind::assimilator, UnitCountBasis::observed) > 0 &&
            effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::observed) > 0) {
            const auto scoutedTech = std::ranges::any_of(state.enemy.units, [](const UnitSnapshot& enemy) {
                return enemy.kind == UnitKind::cyberneticsCore ||
                       enemy.kind == UnitKind::roboticsFacility ||
                       enemy.kind == UnitKind::citadelOfAdun || enemy.kind == UnitKind::templarArchives;
            });
            goal(opening, GoalKind::build, UnitKind::cyberneticsCore, 1, scoutedTech ? 119 : 101,
                 "ranged mirror Core before the optional melee queue", true);
        }
        if (supplyAtLeast(state, DisplayedSupply{14})) {
            goal(opening, GoalKind::train, UnitKind::zealot, 1, 98, "opening bodyguard", true);
            goal(opening, GoalKind::build, UnitKind::cyberneticsCore, 1, 97, "timely ranged access", true);
        }
        if (effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::observed) > 0) {
            goal(opening, GoalKind::train, UnitKind::dragoon, 4, 108, "fund four Dragoons before optional tech", true);
            if (effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::observed) >= 1)
                technologyGoal(opening, TechnologyKind::singularityCharge, 1, 98, "opening range", true);
        }
        if (dragoonsReady >= 4 || roboCommitted) {
            goal(opening, GoalKind::build, UnitKind::roboticsFacility, 1, 95, "splash after the ranged screen", true);
        }
        if (supplyAtLeast(state, DisplayedSupply{22})) {
            const auto throughput = supplyAtLeast(state, DisplayedSupply{32}) ? 3 : 2;
            goal(opening, GoalKind::build, UnitKind::gateway, throughput, 104, "army throughput before optional detection", true);
        }
        if (roboCommitted) {
            if (dragoonsReady >= 6) {
                goal(opening, GoalKind::build, UnitKind::observatory, 1, 86, "scouting after six ranged defenders");
                goal(opening, GoalKind::train, UnitKind::observer, 1, 87, "first optional army scout");
            }
            goal(opening, GoalKind::build, UnitKind::roboticsSupportBay, 1, 95,
                 "prepare splash behind the ranged screen", true);
            if (effectiveUnitCount(state, UnitKind::roboticsSupportBay, UnitCountBasis::observed) > 0)
                goal(opening, GoalKind::train, UnitKind::reaver, 1, 105,
                     "first Reaver with the ranged screen", true);
        }
        if (detectorReady && effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::completed) >= 2) {
            goal(opening, GoalKind::train, UnitKind::observer, 2, 85, "spare detector and tech scout");
        }
        return opening;
    }
    StrategicPlan result;
    result.name = "PvP two-gate robotics control";
    result.desiredWorkers = std::min(66, 18 + minute(state) * 4);
    result.desiredBases = minute(state) < 6 ? 1 : (minute(state) < 12 ? 2 : 3);
    result.desiredGasWorkers = !supplyAtLeast(state, DisplayedSupply{11}) ? 0 :
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
                           (unit.visible || state.frame - unit.lastSeen <= framesForSeconds(90)));
        });
    const auto enemyGatewayCount = static_cast<int>(std::ranges::count_if(
        state.enemy.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::gateway;
        }));
    const auto enemyRangedCount = recentEnemyCount(state, UnitKind::dragoon) +
                                  recentEnemyCount(state, UnitKind::reaver);
    // A two-Gateway mirror that still has no ranged units after the first
    // five-and-a-half minutes is the same production/tech gap Stardust uses
    // to recognize a likely DT transition.  Keep this as a soft suspicion:
    // it raises detection priority and protects the mineral line, but does
    // not replace the direct rush evidence or force an all-in response.
    const auto suspectedCovertTech = state.enemy.race == Race::protoss &&
                                     state.frame >= framesForMinutes(4) + framesForSeconds(12) &&
                                     state.frame < framesForMinutes(11) &&
                                     enemyGatewayCount >= 2 &&
                                     enemyRangedCount == 0;
    const auto enemyCoreScouted = std::ranges::any_of(
        state.enemy.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::cyberneticsCore;
        });
    const auto enemyCoreFinished = std::ranges::any_of(
        state.enemy.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::cyberneticsCore && unit.completed;
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
    // A Gateway-only Protoss opening is the actionable fork for the first
    // static checkpoint.  Once one enemy Gateway is known, stop the second
    // wave at three Zealots long enough to bank the 150 minerals for Forge;
    // otherwise the two-Gateway queue consumes the entire bank and the Forge
    // remains theoretical until the rush is already in the main.
    const auto openingForgeSignal =
        state.enemy.race == Race::protoss &&
        state.frame >= framesForMinutes(2) + framesForSeconds(36) &&
        state.frame < framesForMinutes(6) && enemyGatewayCount >= 1 &&
        enemyRangedCount == 0 && !visibleRangedOpening;
    // `rangedOpening` is intentionally persistent: once a Core or Robotics
    // Facility has been scouted it should continue to unlock our own tech.
    // It must not, however, erase the melee response when the remembered
    // building is hidden and the enemy is still producing only Zealots. Keep
    // a separate, current-pressure signal for opening defense decisions.
    const auto meleeOnlyOpening = enemyGatewayCount >= 2 &&
                                  enemyRangedCount == 0 &&
                                  !visibleRangedOpening &&
                                  (threat.mostLikely == EnemyPlan::fastRush ||
                                   threat.combatEnemiesNearMain > 0 ||
                                   approachingHomeArmyValue >= 1.5 ||
                                   !rangedOpening);
    // The threat label is intentionally allowed to cool when scouting data
    // disappears, but an army already touching the mineral line must not
    // lose its response on the next planning pass.  Stardust keeps an active
    // play/contain state until the engagement is resolved; mirror that with a
    // current-contact signal that supplements (rather than replaces) the
    // persistent two-Gateway opening memory.
    const auto visibleMeleeContactNow = hasVisibleMeleeContact(
        state, home, PixelRadius{800});
    const auto currentMeleeRush = meleeOnlyOpening || visibleMeleeContactNow ||
                                  (threat.mostLikely == EnemyPlan::fastRush &&
                                   approachingHomeArmyValue >= 1.5);
    if (!visibleRangedOpening && state.frame >= framesForMinutes(2) &&
        state.frame < framesForMinutes(4)) {
        result.desiredGasWorkers = 0;
    }
    const auto cannonsReady = effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::completed);
    const auto zealotsReady = effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed);
    const auto dragoonsReady = effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed);
    if (state.frame >= framesForMinutes(3) && state.frame < framesForMinutes(5) &&
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
    const auto coreLost = effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::observed) == 0 &&
                          effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::observed) > 0;
    const auto stableAntiRushAnchor =
        (cannonsReady >= 2 && zealotsReady >= 4) ||
        (effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::completed) > 0 &&
         zealotsReady + dragoonsReady >= 6);
    const auto twoGateOpening = minute(state) < 8 && currentMeleeRush &&
                                !stableAntiRushAnchor;
    const auto earlyMeleeAnchor = minute(state) >= 3 && minute(state) < 5 &&
                                  currentMeleeRush && zealotsReady >= 2 &&
                                  (threat.combatEnemiesNearMain > 0 ||
                                   homeApproach ||
                                   threat.mostLikely == EnemyPlan::fastRush);
    const auto visibleGroundContact = home.valid() &&
        hasVisibleGroundCombatContact(state, home, PixelRadius{800});
    const auto earlyMeleeScreen = zealotsReady >= 2 || rangedOpening;
    const auto mobileOpening = zealotsReady + dragoonsReady +
                               effectiveUnitCount(state, UnitKind::reaver, UnitCountBasis::completed);
    const auto earlyTechWindow = state.frame >= framesForMinutes(4) &&
                                 zealotsReady >= 4 &&
                                 threat.combatEnemiesNearMain == 0 &&
                                 !visibleGroundContact &&
                                 !homeApproach;
    if (dragoonsReady >= 4) {
        result.attackThreshold = 1.22;
        result.minimumAttackSize = 8;
        const auto enemyArmyUnseen = std::ranges::none_of(
            state.enemy.units, [](const UnitSnapshot& enemy) {
                return enemy.visible && enemy.completed && !enemy.flying &&
                       isCombatUnit(enemy.kind);
            });
        if (state.frame >= framesForMinutes(7) && enemyArmyUnseen &&
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
    // Do not buy a second Nexus during an unresolved opening fight. Once the
    // home screen is stable, however, expansion and splash tech must overlap:
    // waiting for a completed Reaver before taking the natural is exactly the
    // one-base turtle that lets BananaBrain bank an unanswerable Dragoon wave.
    const auto firstReaverTechReady =
        effectiveUnitCount(state, UnitKind::roboticsFacility, UnitCountBasis::completed) > 0 &&
        effectiveUnitCount(state, UnitKind::roboticsSupportBay, UnitCountBasis::completed) > 0 &&
        effectiveUnitCount(state, UnitKind::reaver, UnitCountBasis::completed) > 0;
    // A two-Cannon/two-Zealot screen is already a real defensive economy when
    // the enemy army is outside the home perimeter.  Do not let the old
    // "wait for Reaver" gate turn a stable one-base hold into a permanent
    // turtle while the opponent takes its natural and third bases.
    const auto workers = observedRoleCount(state, UnitRole::worker);
    const auto expansionSiteThreat = std::ranges::any_of(
        state.enemy.units, [&expansionSite](const UnitSnapshot& enemy) {
            return enemy.visible && enemy.completed && !enemy.flying &&
                   isCombatUnit(enemy.kind) && enemy.position.valid() &&
                   expansionSite.valid() &&
                   withinPixelRadius(expansionSite, enemy.position, PixelRadius{800});
        }) || approachingExpansionArmyValue >= 2.0;
    if (minute(state) < 10 && mobileOpening < 10 &&
        (!stableAntiRushAnchor || expansionSiteThreat ||
         threat.combatEnemiesNearMain > 0 || homeApproach)) {
        result.desiredBases = stableAntiRushAnchor
                                  ? std::min(result.desiredBases,
                                             std::max(1, effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed)))
                                  : 1;
        result.maximumBases = std::max(1, effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed));
    }
    const auto noVisibleGroundContact = !home.valid() ||
        !hasVisibleGroundCombatContact(state, home, PixelRadius{800});
    // Stardust's economic transition is a state change, not a clocked Nexus:
    // once the home screen is complete and the map is quiet, start banking
    // before the opponent's midgame wave can make the one-base hold
    // permanent.  The five-minute branch is deliberately evidence-gated so
    // it cannot run during a visible rush, breach, or covert-tech alarm.
    const auto earlyStableCounter = state.frame >= framesForMinutes(5) &&
                                    state.frame < framesForMinutes(8) &&
                                    cannonsReady >= 2 && zealotsReady >= 5 &&
                                    workers >= 16 && noVisibleGroundContact &&
                                    !expansionSiteThreat &&
                                    threat.combatEnemiesNearMain == 0 &&
                                    !homeApproach &&
                                    threat.immediateGround <= 0.35 &&
                                    !hardBreachAtMain(state) &&
                                    threat.cloak <= 0.20 &&
                                    threat.mostLikely != EnemyPlan::fastRush;
    const auto stableTechCounter = state.frame >= framesForMinutes(6) &&
                                   state.frame < framesForMinutes(13) &&
                                   workers >= 18 && zealotsReady >= 6 &&
                                   cannonsReady >= 1 &&
                                   effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::completed) > 0 &&
                                   noVisibleGroundContact &&
                                   !expansionSiteThreat &&
                                   threat.combatEnemiesNearMain == 0 &&
                                   !homeApproach &&
                                   threat.immediateGround <= 0.55 &&
                                   !hardBreachAtMain(state) &&
                                   threat.cloak <= 0.20 &&
                                   threat.mostLikely != EnemyPlan::fastRush;
    const auto economicCounterWindow =
        earlyStableCounter ||
        stableTechCounter ||
        (minute(state) >= 6 && cannonsReady >= 2 && zealotsReady >= 2 &&
         workers >= 12 && threat.combatEnemiesNearMain == 0 &&
         !homeApproach && !expansionSiteThreat &&
         threat.immediateGround <= 0.45 &&
         !hardBreachAtMain(state) && threat.cloak <= 0.20);
    if (economicCounterWindow) {
        result.desiredBases = std::max(result.desiredBases, 2);
        result.maximumBases = std::max(result.maximumBases, 2);
        result.posture = Posture::pressure;
        result.attackThreshold = std::min(result.attackThreshold, 1.35);
        result.minimumAttackSize = std::min(result.minimumAttackSize, 8);
        result.name += " [economic counter-window]";
        if (stableTechCounter) result.name += " [stable two-base transition]";
    }
    if (minute(state) < 10 && !firstReaverTechReady && !economicCounterWindow &&
        !stableAntiRushAnchor) {
        result.desiredBases = 1;
        result.maximumBases = std::max(1, effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed));
    }
    if (minute(state) < 8 && !earlyMeleeScreen) {
        // Composition fills run after fixed goals. Keep them melee-only until
        // the opening screen has two Zealots, otherwise an idle second
        // Gateway can spend the rush window on a Dragoon.
        result.composition = {{UnitKind::zealot, 1.0}};
    }
    const auto canTransition = effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::observed) > 0 ||
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
    if (supplyAtLeast(state, DisplayedSupply{9})) {
        goal(result, GoalKind::build, UnitKind::gateway, 1, 100,
             "nine-supply mobile production", true);
    }
    if (supplyAtLeast(state, DisplayedSupply{10})) {
        goal(result, GoalKind::build, UnitKind::assimilator, 1, 101,
             "opening gas before the Core completes", true);
    }
    if (effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::observed) > 0 &&
        effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::observed) < 2) {
        // Do not spend the opening Core's minerals while a single Gateway has
        // produced only one body. Two early Zealots give the Probe line time
        // to survive the first mirror contact and let the second Gateway
        // complete without conceding the game to a four-Zealot rush.
        goal(result, GoalKind::train, UnitKind::zealot, 2, 99,
             "field two mobile defenders before ranged tech", true);
    }

    if (supplyAtLeast(state, DisplayedSupply{9})) {
        const auto roboticsInPlay = effectiveUnitCount(state, UnitKind::roboticsFacility, UnitCountBasis::observed) > 0;
        const auto reaverOnline = effectiveUnitCount(state, UnitKind::reaver, UnitCountBasis::completed) > 0;
        const auto gatewayTarget = supplyAtLeast(state, DisplayedSupply{10}) ?
                                       (!roboticsInPlay || !reaverOnline ? 2 :
                                        (minute(state) < 12 ? 3 : 4)) : 1;
        // BananaBrain's opening is a live production race, not just a tech
        // race.  When the first Gateway exists but the second one has not
        // started, let that throughput checkpoint outrank optional Core/
        // Dragoon prerequisites until the early production pair is secured.
        // The old priority (97) let the generic Dragoon goal materialize a
        // Core first, leaving one Gateway and one Zealot when the rush arrived.
        const auto earlyGatewayPriority = state.frame < framesForMinutes(5) &&
                                          effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::observed) < 2 &&
                                          !visibleRangedOpening ? 115 : 97;
        goal(result, GoalKind::build, UnitKind::gateway, gatewayTarget,
             earlyGatewayPriority,
             "nine-supply gateway into two-gate control",
             effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::observed) < gatewayTarget && gatewayTarget <= 2);
    }
    if (effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::observed) >= 2) {
        // Keep both early Gateways on melee bodies long enough to contest a
        // mirror flood. Transitioning at three Zealots left BananaBrain's
        // first wave unopposed while the Core consumed the bank.
        const auto earlyZealotTarget =
            effectiveUnitCount(state, UnitKind::roboticsFacility, UnitCountBasis::observed) > 0
                ? ((currentMeleeRush && state.frame < framesForMinutes(10) &&
                    (threat.mostLikely == EnemyPlan::fastRush ||
                     threat.uncertainty > 0.85)) ? 8 :
                   (minute(state) < 6 ? 6 : 3))
                : (currentMeleeRush && state.frame >= framesForMinutes(4)
                       ? (effectiveUnitCount(state, UnitKind::forge, UnitCountBasis::observed) > 0 ? 6 : 5)
                       : (openingForgeSignal && effectiveUnitCount(state, UnitKind::forge, UnitCountBasis::observed) == 0 ? 3 : 4));
        goal(result, GoalKind::train, UnitKind::zealot,
             earlyZealotTarget, 96,
             "fill secured opening production with defenders",
             effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::observed) < 2);
        const auto batteryRangedEvidence = visibleRangedOpening ||
            enemyRangedCount > 0 ||
            (rangedOpening && threat.mostLikely != EnemyPlan::fastRush);
        const auto batteryEvidence = batteryRangedEvidence ||
            threat.combatEnemiesNearMain > 0 ||
            homeApproach;
        // Against a melee-only two-Gateway line, the first Battery is a
        // lower-value mineral sink than the Forge/Cannon checkpoint.  If the
        // Battery starts while the Forge is still missing, it can consume the
        // entire 100-mineral margin that would otherwise start the static
        // anchor before contact.  Ranged openings keep the Battery path.
        const auto meleeAnchorPending = currentMeleeRush &&
                                        effectiveUnitCount(state, UnitKind::forge, UnitCountBasis::observed) == 0 &&
                                        cannonsReady == 0 &&
                                        // Four Zealots are the minimum mobile
                                        // escort, not proof that a static
                                        // anchor is no longer needed.  Keep
                                        // the Battery out of the bank until
                                        // the six-body checkpoint or Forge
                                        // completion, whichever comes first.
                                        effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::observed) < 6;
        if (effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::observed) >= 2 && batteryEvidence &&
            !meleeAnchorPending &&
            // A Battery is sustain, not the first emergency anchor. Keep its
            // 100 minerals behind a Cannon investment or the Core so it
            // cannot delay both static defense and ranged transition.
            (cannonsReady >= 1 || effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::observed) > 0 ||
             effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::observed) > 0)) {
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
        if (effectiveUnitCount(state, UnitKind::forge, UnitCountBasis::observed) > 0 && currentMeleeRush) {
            // Overlap the second Cannon once a rush is actually approaching;
            // waiting for the first structure to finish lets the attacker
            // focus it down before the next anchor can even start.
            const auto earlyCannonTarget =
                threat.mostLikely == EnemyPlan::fastRush || visibleGroundContact ||
                        homeApproach
                    ? 2
                    : 1;
            goal(result, GoalKind::build, UnitKind::photonCannon,
                 earlyCannonTarget, 94,
                 "early Cannon screen behind the Forge insurance", false);
        }
    }
    // A remembered first Gateway with no Core, Dragoon, or Reaver by the
    // 2:36 mark is useful information even when the enemy army is
    // still hidden. BananaBrain's two-Gateway/DT lines often keep the second
    // production structure out of the worker scout's vision; waiting for the
    // FastRush label then leaves no mineral window for a Forge. Stage one
    // blocking Forge insurance once two Zealots are fielded. The reservation
    // is allowed to wait behind the current bank, but it prevents a routine
    // Probe/Dragoon spend from consuming the exact 150 minerals needed to
    // start the anchor on the next macro pass.
    const auto staticFirstOpening = state.frame >= framesForMinutes(3) &&
                                    state.frame < framesForMinutes(6) &&
                                    effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::observed) >= 2 &&
                                    enemyGatewayCount >= 1 &&
                                    enemyRangedCount == 0 &&
                                    !visibleRangedOpening &&
                                    effectiveUnitCount(state, UnitKind::forge, UnitCountBasis::observed) == 0 &&
                                    cannonsReady == 0;
    // A confirmed second enemy Gateway is stronger evidence than the usual
    // late single-Gateway prior. Start the first Forge after our second
    // Gateway and first Zealot have begun, so a Cannon can finish before a
    // four-Zealot crossing. This option is isolated until matched live games
    // establish that the earlier static spend repays any lost mobile tempo.
    const auto earlyScoutedTwoGate = pvpScoutedTwoGateAnchor_ &&
        state.frame >= framesForMinutes(2) && state.frame < framesForMinutes(5) &&
        enemyGatewayCount >= 2 && enemyRangedCount == 0 && !visibleRangedOpening &&
        effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::observed) >= 2 && effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::observed) >= 1 &&
        effectiveUnitCount(state, UnitKind::probe, UnitCountBasis::completed) >= 12 &&
        effectiveUnitCount(state, UnitKind::forge, UnitCountBasis::observed) == 0 && cannonsReady == 0 &&
        !hardBreachAtMain(state);
    const auto hiddenMeleeTechSignal = earlyScoutedTwoGate ||
                                       staticFirstOpening || openingForgeSignal ||
                                       (state.frame >= framesForMinutes(6) &&
                                        state.frame < framesForMinutes(8) &&
                                        enemyGatewayCount >= 1 &&
                                        enemyRangedCount == 0 &&
                                        !visibleRangedOpening &&
                                        zealotsReady >= 1);
    if (hiddenMeleeTechSignal) {
        goal(result, GoalKind::build, UnitKind::forge, 1, 118,
             earlyScoutedTwoGate ? "scouted two-Gateway Forge anchor" :
                                   "insurance Forge for an unscouted melee transition", true);
        result.name += earlyScoutedTwoGate ? " [scouted two-gate anchor]" :
                                                  " [melee-tech insurance]";
    }
    const auto stabilizingMeleeAnchor = state.frame >= framesForMinutes(5) &&
                                        state.frame < framesForMinutes(8) &&
                                        zealotsReady >= 6 &&
                                        !hardBreachAtMain(state) &&
                                        (!visibleGroundContact ||
                                         threat.mostLikely == EnemyPlan::fastRush) &&
                                        (threat.mostLikely == EnemyPlan::fastRush ||
                                         threat.uncertainty > 0.85 ||
                                         effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::observed) > 0);
    if (stabilizingMeleeAnchor) {
        // A hidden two-gate flood can cross the map before it becomes visible
        // at the mineral line. Once six Zealots and the first Core bank are
        // secured, reserve one Forge/Cannon without buying the old blind
        // two-Cannon opening. This gives the home screen time to finish while
        // Robotics and the first Reaver remain the primary tech plan.
        goal(result, GoalKind::build, UnitKind::forge, 1, 94,
             "stabilize the six-Zealot mirror screen", false);
        if (effectiveUnitCount(state, UnitKind::forge, UnitCountBasis::observed) > 0) {
            goal(result, GoalKind::build, UnitKind::photonCannon, 1, 93,
                 "single Cannon behind the six-Zealot screen", false);
        }
    }
    if (state.frame >= framesForMinutes(6) && state.frame < framesForMinutes(9) &&
        effectiveUnitCount(state, UnitKind::forge, UnitCountBasis::observed) > 0 && cannonsReady < 2) {
        // One Cannon buys the opening time; a second one before nine minutes
        // keeps a Dragoon-heavy push from deleting the mineral line while the
        // Core and Robotics chain finishes.  This is still capped at two.
        goal(result, GoalKind::build, UnitKind::photonCannon, 2, 94,
             "second Cannon before the ranged pressure window", false);
    }
    if (supplyAtLeast(state, DisplayedSupply{12})) {
        const auto quietTechWindow = state.frame >= framesForMinutes(5) &&
                                     threat.combatEnemiesNearMain == 0 &&
                                     !visibleGroundContact &&
                                     (!homeApproach ||
                                      zealotsReady >= 8 || cannonsReady >= 1);
        const auto screenedTechWindow = state.frame >= framesForMinutes(5) &&
                                        threat.combatEnemiesNearMain == 0 &&
                                        !visibleGroundContact &&
                                        zealotsReady >= 6;
    // A scouted two-Gateway mirror is already enough evidence to start the
    // ranged transition behind two Zealots.  Waiting for four completed
    // Zealots made the Core a reaction to the first flood instead of a
    // production checkpoint, which in turn pushed Robotics/Observer past the
    // hidden-tech timing.  Keep the ordinary quiet-window predicates, but let
    // the observed double-production line unlock the Core earlier.
    const auto earlyMirrorTech = twoGateOpening &&
                                 state.frame >= framesForMinutes(3) + framesForSeconds(24) &&
                                 zealotsReady >= 2 &&
                                 threat.combatEnemiesNearMain == 0 &&
                                 !hardBreachAtMain(state);
    // In an unscouted mirror, do not let the optional range prerequisite
    // pull a 200-mineral Core through the queue before the first melee/static
    // checkpoint.  Four minutes is still an early Core timing, but it leaves
    // the hidden two-Gateway branch one complete Forge window.  Confirmed
    // ranged tech, a Cannon, or synthetic frame-zero unit tests bypass this
    // delay.
    const auto coreTimingWindow = !staticFirstOpening &&
                                  (rangedOpening || enemyCoreScouted ||
                                   cannonsReady >= 1 || state.frame == 0 ||
                                   state.frame >= framesForMinutes(4));
    const auto productionSecured = coreTimingWindow &&
                                   effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::observed) >= 1 &&
                                   (state.frame < framesForMinutes(2) ||
                                        (effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::observed) >= 4 &&
                                         (rangedOpening || cannonsReady >= 1 ||
                                          effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::observed) >= 1 ||
                                          quietTechWindow || screenedTechWindow ||
                                          earlyTechWindow)) ||
                                    earlyMirrorTech);
        goal(result, GoalKind::build, UnitKind::cyberneticsCore, 1, 98,
             "dragoon access after opening production", productionSecured);
    }
    if (supplyAtLeast(state, DisplayedSupply{14})) {
        // Do not let a filler Dragoon goal invent its own Cybernetics Core in
        // the opening.  A generic prerequisite chain cannot see the PvP
        // production checkpoint, so it used to spend 200 minerals on the
        // Core while the second Gateway was still waiting.  The explicit
        // Core goal below (or a known ranged opening) owns this transition.
        const auto rangedProductionUnlocked =
            effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::observed) > 0 || rangedOpening ||
            cannonsReady >= 2;
        if (earlyMeleeScreen && rangedProductionUnlocked) {
            goal(result, GoalKind::train, UnitKind::dragoon,
                 std::max(4, minute(state) * 2), 91, "core PvP army");
        }
        technologyGoal(result, TechnologyKind::singularityCharge, 1, 86,
                       "range follows the first defensive dragoons");
    }
    if (effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::observed) >= 3) {
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
    const auto supportNeeded = state.frame >= framesForMinutes(4) &&
                               effectiveUnitCount(state, UnitKind::roboticsFacility, UnitCountBasis::observed) > 0 &&
                               // In a normal ranged mirror, Support Bay can
                               // build in parallel with the Observatory. Only
                               // serialize it behind detection when the
                               // persistent covert-tech recognizer is active.
                               (!suspectedCovertTech ||
                                effectiveUnitCount(state, UnitKind::observatory, UnitCountBasis::completed) > 0) &&
                               effectiveUnitCount(state, UnitKind::roboticsSupportBay, UnitCountBasis::observed) == 0;
    const auto recentDarkTemplar = recentEnemyCount(state, UnitKind::darkTemplar);
    // Stardust keeps a confirmed melee all-in separate from a covert-tech
    // hypothesis.  Do the same here: during a FastRush, do not let a soft
    // "two Gateways/no ranged units" prior pull the bank into Robotics before
    // the first Cannon is online.  A seen/recent Dark Templar still bypasses
    // this hold immediately.
    const auto holdTechForMeleeRush = currentMeleeRush &&
                                      threat.mostLikely == EnemyPlan::fastRush &&
                                      minute(state) < 8 && cannonsReady == 0 &&
                                      zealotsReady < 6 && recentDarkTemplar == 0;
    // Stardust treats a completed Core as a production checkpoint, not as a
    // reason to keep filling Gateways indefinitely.  Once the first mobile
    // screen or one Cannon is online, start Robotics immediately; otherwise
    // a lost Zealot can make the old four-body predicate false for the entire
    // next pressure cycle and leave the Core idle while the opponent adds
    // ranged production or covert tech.
    const auto earlyDetectionCheckpoint = state.frame >= framesForMinutes(4) + framesForSeconds(12) &&
                                          effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::completed) > 0 &&
                                          effectiveUnitCount(state, UnitKind::roboticsFacility, UnitCountBasis::observed) == 0 &&
                                          !hardBreachAtMain(state) &&
                                          !holdTechForMeleeRush &&
                                          (suspectedCovertTech ||
                                           (state.frame >= framesForMinutes(6) &&
                                            threat.mostLikely != EnemyPlan::fastRush &&
                                            threat.combatEnemiesNearMain == 0 &&
                                            effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::observed) >= 3));
    const auto roboticsCheckpoint = state.frame >= framesForMinutes(6) &&
                                    effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::completed) > 0 &&
                                    (zealotsReady >= 3 || cannonsReady >= 1 ||
                                     enemyCoreScouted || enemyCoreFinished);
    const auto roboticsNeeded = state.frame >= framesForMinutes(4) &&
                                effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::completed) > 0 &&
                                effectiveUnitCount(state, UnitKind::roboticsFacility, UnitCountBasis::observed) == 0 &&
                                !holdTechForMeleeRush &&
                                (earlyDetectionCheckpoint || zealotsReady >= 4 || roboticsCheckpoint ||
                                 (cannonsReady >= 2 &&
                                  (!enemyCoreScouted || enemyCoreFinished)));
    if (roboticsNeeded) {
        result.desiredGasWorkers = std::max(3, result.desiredGasWorkers);
        goal(result, GoalKind::build, UnitKind::roboticsFacility, 1,
             earlyDetectionCheckpoint ? 116 : 104,
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
        threat.cloak > 0.20 || threat.mostLikely == EnemyPlan::cloakedTech ||
        recentEnemyCount(state, UnitKind::darkTemplar) > 0 ||
        suspectedCovertTech;
    if (cloakedThreat && effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::completed) > 0 &&
        (!holdTechForMeleeRush || recentDarkTemplar > 0)) {
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
        if (suspectedCovertTech) result.name += " [suspected covert tech]";
        if (effectiveUnitCount(state, UnitKind::observer, UnitCountBasis::completed) == 0) {
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
    if (supplyAtLeast(state, DisplayedSupply{28})) {
        goal(result, GoalKind::build, UnitKind::roboticsSupportBay, 1, 69,
             "reaver access");
        goal(result, GoalKind::train, UnitKind::reaver, 2, 70, "area control");
        goal(result, GoalKind::train, UnitKind::shuttle, 1, 67, "reaver mobility");
    }
    if (effectiveUnitCount(state, UnitKind::roboticsSupportBay, UnitCountBasis::observed) > 0 &&
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
    // branch has any chance to finish detection.  Detection is deliberately
    // ahead of the Support Bay here; a delayed Reaver is recoverable, while a
    // first Observer that starts after the DT wave has entered the mineral
    // line is not.
    if (effectiveUnitCount(state, UnitKind::roboticsFacility, UnitCountBasis::observed) > 0) {
        // Reserve detection as soon as Robotics is underway, not only after
        // its long build timer completes.  A hidden DT line can arrive during
        // that timer; waiting for a completed facility delayed Observatory
        // and Observer until after the first cloak wave in direct matches.
        const auto observatoryPriority =
            effectiveUnitCount(state, UnitKind::observatory, UnitCountBasis::observed) == 0 ? 111 : 108;
        goal(result, GoalKind::build, UnitKind::observatory, 1,
             observatoryPriority,
             "early PvP detection before splash support", true);
        if (effectiveUnitCount(state, UnitKind::observatory, UnitCountBasis::completed) > 0) {
            goal(result, GoalKind::train, UnitKind::observer, 1, 107,
                 "field the first Observer before the hidden-tech timing", true);
        }
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

    const auto evidencedMeleeRush = minute(state) < 8 && currentMeleeRush &&
                                    (twoGateOpening ||
                                     hardBreachAtMain(state) ||
                                     // If the mobile screen has already been
                                     // thinned below four completed Zealots,
                                     // add one static anchor before the next
                                     // wave arrives.
                                     (threat.combatEnemiesNearMain > 0 &&
                                      zealotsReady < 4));
    if (evidencedMeleeRush) {
        // A Forge is expensive, so only open this branch after direct mirror
        // evidence. It gives the two-gate screen one static anchor without
        // returning to the old blind seven-Probe Cannon opening.
        goal(result, GoalKind::build, UnitKind::forge, 1, 96,
             "conditional static anchor against evidenced Zealot pressure");
        if (effectiveUnitCount(state, UnitKind::forge, UnitCountBasis::observed) > 0) {
            goal(result, GoalKind::build, UnitKind::photonCannon, 1, 95,
                 "conditional Cannon behind the mobile mirror screen");
        }
    }

    // Once a fast melee opening is actually visible, the mobile screen and a
    // single early Cannon are both more valuable than another delayed ranged
    // tech cycle. This branch is evidence-gated, so an ordinary mirror does
    // not pay the blind Forge tax, but a four-Zealot flood cannot walk through
    // an entirely unanchored mineral line.
    const auto rushStaticNeeded = minute(state) < 8 && currentMeleeRush &&
                                   // A visible melee pack inside the home
                                   // perimeter is already actionable evidence;
                                   // waiting for BWAPI's narrower hard-breach
                                   // ring left the emergency Forge at a low
                                   // priority until the Probe line was gone.
                                   (hardBreachAtMain(state) ||
                                    visibleGroundContact ||
                                     threat.combatEnemiesNearMain > 0 ||
                                     homeApproach);
    // A Cannon under construction is already a committed static investment:
    // its minerals are gone and a Probe is tied up. Treat it as an anchor
    // checkpoint for the ranged transition instead of waiting for the long
    // Cannon timer to finish.
    const auto cannonInvestment = effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::observed) > 0;
    // A long Cannon build is not yet a safe mineral-line screen. Do not spend
    // the next 200 minerals on Core while the first anchor is incomplete
    // unless the mobile screen has reached six Zealots.
    const auto staticCheckpoint = cannonsReady >= 1 ||
                                  (cannonInvestment && zealotsReady >= 6);
    const auto preserveEarlyCore =
        state.frame >= framesForMinutes(4) &&
        // A FastRush that is still outside the hard-breach radius is exactly
        // the narrow window where starting the Core saves a full
        // Robotics/Observer cycle. Once one Cannon is complete, keep that
        // checkpoint even if the front line briefly touches the Nexus: the
        // old Zealot-only predicate erased the Core after one combat loss and
        // left the bot permanently melee-only.
        (zealotsReady >= 4 && !hardBreachAtMain(state) && staticCheckpoint);
    const auto preserveCoreBehindCannon =
        // Once a Cannon has actually been started, do not let the emergency
        // melee branch erase the Core until the static screen is finished.
        // The old six-minute gate was too late: on a committed rush the bot
        // could reach five Zealots plus an unfinished Cannon, repeatedly
        // cancel the Core, and die before Dragoons/Observers came online.
        state.frame >= framesForMinutes(4) && staticCheckpoint && zealotsReady >= 4;

    // A visible army just outside the 448px emergency ring is the midfield,
    // not the mineral line.  Stardust keeps a stable vanguard there so the
    // opponent cannot walk from the edge of vision to the Nexus for free.
    // Treat this as a pressure contact when our compact screen can contest it;
    // only a hard breach or a clearly superior approach should collapse the
    // whole plan back to Defend.
    const auto midfieldVisibleCount = static_cast<int>(std::ranges::count_if(
        state.enemy.units, [](const UnitSnapshot& enemy) {
            return enemy.visible && enemy.completed && !enemy.flying &&
                   isCombatUnit(enemy.kind) && enemy.position.valid();
        }));
    const auto midfieldContestable = midfieldVisibleCount <=
                                     mobileOpening + cannonsReady;
    const auto midfieldContact = visibleGroundContact &&
                                 !hardBreachAtMain(state) &&
                                 threat.combatEnemiesNearMain == 0 &&
                                 mobileOpening >= 5 &&
                                 (zealotsReady >= 5 || cannonsReady >= 1) &&
                                 !cloakedThreat &&
                                 (threat.immediateGround <= 0.55 ||
                                  (midfieldContestable &&
                                   threat.immediateGround <= 0.80));
    if (midfieldContact) {
        result.posture = Posture::pressure;
        result.attackThreshold = std::min(result.attackThreshold, 1.35);
        result.minimumAttackSize = std::min(result.minimumAttackSize, 6);
        const auto nearest = std::ranges::min_element(
            state.enemy.units, {}, [&home](const UnitSnapshot& enemy) {
                return enemy.visible && enemy.completed && !enemy.flying &&
                               isCombatUnit(enemy.kind) && enemy.position.valid()
                           ? distanceSquared(home, enemy.position)
                           : std::numeric_limits<int>::max();
            });
        if (nearest != state.enemy.units.end() && nearest->position.valid()) {
            const auto proposedRally = moveToward(home, nearest->position, 320.0);
            constexpr auto rallyMemory = framesForSeconds(12);
            constexpr auto rallyShift = 224;
            const auto rallyExpired = pvpMidfieldRallyLastFrame_ < 0 ||
                state.frame < pvpMidfieldRallyLastFrame_ ||
                state.frame - pvpMidfieldRallyLastFrame_ > rallyMemory;
            if (rallyExpired || !pvpMidfieldRally_.valid() ||
                distanceSquared(proposedRally, pvpMidfieldRally_) >=
                    rallyShift * rallyShift) {
                pvpMidfieldRally_ = proposedRally;
            }
            pvpMidfieldRallyLastFrame_ = state.frame;
            result.rallyPoint = pvpMidfieldRally_;
        }
        result.name += " [midfield pressure]";
    }

    if (threat.combatEnemiesNearMain > 0 ||
        (homeApproach && !midfieldContact) ||
        (visibleGroundContact && !midfieldContact)) {
        result.name = "PvP two-gate emergency defense";
        result.posture = Posture::defend;
        result.desiredBases = stableAntiRushAnchor
                                  ? std::min(result.desiredBases,
                                             std::max(1, effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed)))
                                  : 1;
        if (!stableAntiRushAnchor) {
            result.desiredWorkers = std::min(result.desiredWorkers,
                                             canTransition ? 22 : 12);
        }
        const auto meleeOnlyEmergency = rushStaticNeeded &&
                                        dragoonsReady < 2 &&
                                        // One completed Cannon is already a
                                        // meaningful static checkpoint.  Do
                                        // not keep erasing Robotics after the
                                        // wall exists: that left a completed
                                        // Core idle until the DT wave was on
                                        // top of the mineral line.
                                        cannonsReady == 0 &&
                                        zealotsReady < 6 &&
                                        !preserveEarlyCore &&
                                        !preserveCoreBehindCannon;
        if ((effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::observed) == 0 && !canTransition &&
             !preserveEarlyCore && !preserveCoreBehindCannon) ||
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
        if ((preserveEarlyCore || preserveCoreBehindCannon) &&
            effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::observed) == 0) {
            // Four Zealots and no hard breach are enough to start the ranged
            // transition. Do not let the emergency branch erase this goal
            // while a mirror rush is merely approaching the wall.
            result.desiredGasWorkers = std::max(3, result.desiredGasWorkers);
            goal(result, GoalKind::build, UnitKind::cyberneticsCore, 1, 113,
                 "start ranged tech behind the four-Zealot screen", true);
            goal(result, GoalKind::build, UnitKind::assimilator, 1, 112,
                 "feed the protected early Core", true);
        }
        const auto widenRushScreen =
            hardBreachAtMain(state) ||
            (cannonsReady >= 1 &&
             (visibleGroundContact || threat.combatEnemiesNearMain > 0 ||
              homeApproach));
        const auto secondCannonSafe = workers >= 14 || zealotsReady >= 6 ||
                                      hardBreachAtMain(state);
        const auto rushCannonTarget =
            // One Cannon is the pre-contact insurance anchor.  Once that
            // anchor is complete and the approaching melee pack is visible,
            // overlap a second before the first structure is focused down;
            // waiting for a literal hard breach loses the wall and the Probe
            // line in the same engagement.
            !widenRushScreen ? 1 :
            // One Cannon plus the mobile screen is the right emergency
            // trade while the Core is missing. Starting two long Cannon
            // builds at once delayed the Core until eight minutes and let a
            // Dragoon/DT transition scale uncontested.
            effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::completed) == 0 ? 1 :
            effectiveUnitCount(state, UnitKind::roboticsFacility, UnitCountBasis::observed) == 0 ? (secondCannonSafe ? 2 : 1) :
            state.self.minerals >= 800 ? 4 : 3;
        if (rushStaticNeeded && cannonsReady < rushCannonTarget) {
            goal(result, GoalKind::build, UnitKind::forge, 1, 112,
                 "anchor the mineral line against observed Zealot pressure", true);
            goal(result, GoalKind::build, UnitKind::photonCannon, rushCannonTarget, 111,
                 "overlap the emergency mineral-line anchor", true);
        }
    const auto lateRangedStatic = state.frame >= framesForMinutes(6) &&
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
                                        (!currentMeleeRush || zealotsReady >= 8);
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
        if (effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::observed) > 0 &&
            effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::observed) == 0) {
            goal(result, GoalKind::train, UnitKind::zealot, 1, 105,
                 "field one mobile defender before adding more structures", true);
        }
        if (cannonsReady >= 2 || zealotsReady >= 2) {
            // Income is a live invariant during the hold. Once the line drops
            // below fourteen workers, queue the next Probe ahead of optional
            // Core/Cannon waits; otherwise those blocking goals can reserve
            // the entire bank and turn a recoverable raid into a zero-worker
            // death spiral.
            const auto probePriority = workers < 14 &&
                                       state.enemy.race == Race::protoss &&
                                       visibleGroundContact
                                           ? 123
                                           : 104;
            goal(result, GoalKind::train, UnitKind::probe, cannonsReady >= 2 ? 10 : 12,
                 probePriority,
                 "fund sustained mirror defense behind the Cannon screen", true);
            if (effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::completed) > 0) {
                goal(result, GoalKind::build, UnitKind::shieldBattery, 1, 103,
                     "finish defensive sustain before the ongoing Zealot target", true);
            }
        }
        if (effectiveUnitCount(state, UnitKind::forge, UnitCountBasis::observed) > 0) {
        const auto emergencyCannonTarget =
                workers < 8 ? cannonsReady :
                cannonsReady >= 2 ? 2 :
                !hardBreachAtMain(state) ? 1 :
                effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::completed) > 0 ? 3 : 2;
        goal(result, GoalKind::build, UnitKind::photonCannon,
             emergencyCannonTarget, 101,
             "reinforce an existing static defense investment", true);
        }
        goal(result, GoalKind::build, UnitKind::gateway, 2, 100,
             "guarantee two-gate defensive throughput", true);
        const auto emergencyMeleeTarget = cannonsReady >= 2 &&
                                                  effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::completed) > 0 &&
                                                  (!enemyCoreScouted || enemyCoreFinished)
                                              ? 4
                                              : 8;
        goal(result, GoalKind::train, UnitKind::zealot, emergencyMeleeTarget, 99,
             "continuously reinforce against opening pressure", true);
        const auto directMeleePressure = currentMeleeRush &&
            (threat.combatEnemiesNearMain > 0 || visibleGroundContact ||
             hardBreachAtMain(state));
        if (directMeleePressure) {
            // A completed Core must not turn the only free Gateway into a
            // Dragoon while Zealots are fighting at the mineral line.  Keep
            // the emergency composition melee-only until the rush is cleared
            // or the two-Cannon screen can safely absorb the transition.
            result.composition = {{UnitKind::zealot, 1.0}};
        }
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
        const auto meleePressure = currentMeleeRush &&
            (threat.combatEnemiesNearMain > 0 || visibleGroundContact ||
             hardBreachAtMain(state));
        if (earlyMeleeScreen &&
            (!meleePressure || zealotsReady >= 6 || cannonsReady >= 2)) {
            goal(result, GoalKind::train, UnitKind::dragoon, 4, 101,
                 "establish a ranged defensive core", gasReady);
        }
        if (effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed) >= 2) {
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
        result.maximumBases = std::max(1, effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed));
        result.desiredWorkers = std::min(result.desiredWorkers, 22);
        // Stop gas while the opponent is still melee-only. The Forge/Cannon
        // anchor must begin as soon as 150 minerals are available; leaving
        // three Probes on gas here delayed it until the mineral line was
        // already lost in the live rush trace.
        const auto gasTransitionWindow = state.frame >= framesForMinutes(3) + framesForSeconds(24) &&
                                         zealotsReady >= 2 &&
                                         effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::completed) > 0 &&
                                         threat.combatEnemiesNearMain == 0 &&
                                         !hardBreachAtMain(state);
        result.desiredGasWorkers = gasTransitionWindow ? 3 : 0;
        // Pay for the first escort, then start the static chain while it
        // trains. Forge plus Cannon construction outlasts another unit cycle.
        const auto mobileRushOpening = state.frame < framesForMinutes(4) &&
                                       effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::observed) == 0;
        // A covert-tech suspicion is normally enough to reserve Robotics and
        // detection before the first visible ranged unit.  It must not win
        // over a current two-Gateway melee opening, though: in the live
        // mirror trace the suspicion was raised one planning pass before
        // FastRush was recognized, so the Forge was skipped at 150 minerals
        // and the later Cannon could not finish before contact.  The
        // melee-only signal is deliberately current and takes precedence.
        const auto detectionFirst = earlyDetectionCheckpoint &&
                                     !currentMeleeRush &&
                                     !hardBreachAtMain(state);
        if (mobileRushOpening) {
            // Keep the early hidden-melee insurance Forge alive once the
            // rush label catches up.  The old cleanup erased every static
            // goal in this window, so a two-Gateway opponent could trigger
            // the exact frame where the Forge became affordable and leave
            // us with no Cannon path at all.  Only the insurance Forge is
            // preserved; Cannons and Batteries still wait for the mobile
            // screen as intended.
            const auto preserveEmergencyForge = hiddenMeleeTechSignal ||
                                                 rushStaticNeeded;
            std::erase_if(result.goals, [preserveEmergencyForge](
                              const ProductionGoal& candidate) {
                if (candidate.target == UnitKind::forge &&
                    preserveEmergencyForge) {
                    return false;
                }
                return candidate.target == UnitKind::forge ||
                       candidate.target == UnitKind::photonCannon ||
                       candidate.target == UnitKind::shieldBattery;
            });
            goal(result, GoalKind::train, UnitKind::zealot, 6, 114,
                 "mobile-first response to scouted double production", true);
        } else {
            if (effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::observed) == 0) {
                goal(result, GoalKind::build, UnitKind::forge, 1, 120,
                     "start the melee intercept behind the first paid escort", true);
                goal(result, GoalKind::build, UnitKind::photonCannon, 1, 120,
                     "complete the first melee anchor before more infrastructure", true);
            }
            // Once the mobile screen has four bodies, gas is no longer a
            // luxury: the opponent's next wave is likely Dragoons. Re-enable
            // three gas workers before the eight-minute scouting window ends
            // so the completed Core can actually turn the saved bank into
            // ranged defenders.
            result.desiredGasWorkers = 3;
            goal(result, GoalKind::train, UnitKind::zealot, 2, 105,
                 "field a mobile screen against scouted double production", true);
            if (!detectionFirst) {
                goal(result, GoalKind::build, UnitKind::forge, 1, 112,
                     "prepare a static answer to scouted melee production", true);
            }
            // One Cannon is enough to stabilize a mobile-first opening. Only
            // buy the second mineral sink when the army is actually moving
            // toward the main (or a hard breach is already present); a timer
            // alone made the bot spend the exact minerals needed for the Core
            // and Robotics transition while BananaBrain's army grew.
            const auto staticCannonTarget =
                (hardBreachAtMain(state) || threat.immediateGround > 0.25 ||
                 threat.approachingArmyValue >= 4.0) &&
                        (state.frame >= framesForMinutes(6) || zealotsReady >= 4)
                    ? 2
                    : 1;
            if (!detectionFirst || staticCannonTarget > 0) {
                goal(result, GoalKind::build, UnitKind::photonCannon,
                     detectionFirst ? std::min(1, staticCannonTarget) : staticCannonTarget,
                     111, "support the mobile army against a melee flood", true);
            }
        }

        // Once the first ranged screen is online, meet a suspected flood in
        // the open instead of waiting for it to enter the Probe line. The
        // small 160px default rally is deliberately conservative for normal
        // games, but is too close to the Nexus for a scouted two-Gateway all-in.
        // Only do this while the threat is still outside the main; the direct
        // breach branch above must retain control once contact is established.
        const auto hardBreach = hardBreachAtMain(state);
        if (!mobileRushOpening && state.frame >= framesForMinutes(4) &&
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

        // A compact pressure group is still useful before the full ranged
        // army exists.  Stardust locks an attack/contain target once the map
        // is quiet and its home screen is stable; mirror that behavior here
        // instead of leaving four-to-six Zealots parked at the Nexus until a
        // fourteen-unit threshold becomes reachable.  Require two completed
        // Cannons (or an Observer) while covert tech is suspected so this is
        // not a blind march into an undetected DT line.
        const auto perimeterEnemyCount = static_cast<int>(std::ranges::count_if(
            state.enemy.units, [&home](const UnitSnapshot& enemy) {
                return enemy.visible && enemy.completed && !enemy.flying &&
                       isCombatUnit(enemy.kind) && enemy.position.valid() &&
                       (!home.valid() || withinPixelRadius(home, enemy.position, PixelRadius{900}));
            }));
        const auto safeVisibleContact = visibleGroundContact &&
                                        cannonsReady >= 2 &&
                                        perimeterEnemyCount <= std::max(2, mobileOpening / 2);
        const auto compactMeleeTiming = state.frame >= framesForMinutes(5) &&
                                        state.frame < framesForMinutes(11) &&
                                        mobileOpening >= 4 &&
                                        (cannonsReady >= 2 ||
                                         effectiveUnitCount(state, UnitKind::observer, UnitCountBasis::completed) > 0) &&
                                        !hardBreach &&
                                        (!visibleGroundContact || safeVisibleContact) &&
                                        threat.combatEnemiesNearMain == 0 &&
                                        threat.immediateGround <= 0.35 &&
                                        (threat.uncertainty >= 0.90 ||
                                         threat.mostLikely == EnemyPlan::fastExpand);
        if (compactMeleeTiming) {
            result.posture = Posture::pressure;
            result.attackThreshold = std::min(result.attackThreshold, 1.22);
            result.minimumAttackSize = std::min(result.minimumAttackSize, 4);
            const auto target = enemyMain(state);
            if (target.valid()) {
                result.attackTarget = target;
                result.rallyPoint = target;
            }
            result.name += " [compact melee timing]";
        }

        // A remembered enemy Core is useful for our tech schedule, but it is
        // not evidence that the current fight is ranged.  While the opponent
        // still has no recent Dragoon/Reaver and our static anchor is missing,
        // keep every Gateway on Zealots.  Otherwise the generic transition
        // goal can spend the exact 125 gas/ minerals on a Dragoon while the
        // first melee wave is already crossing the map.
        if (currentMeleeRush && zealotsReady < 6 && cannonsReady == 0) {
            result.composition = {{UnitKind::zealot, 1.0}};
            result.goals.erase(
                std::remove_if(result.goals.begin(), result.goals.end(),
                               [](const ProductionGoal& candidate) {
                                   return candidate.goal == GoalKind::train &&
                                          candidate.target == UnitKind::dragoon &&
                                          candidate.priority < 114;
                               }),
                result.goals.end());
        }
    }

    const auto visibleEnemyArmy = std::ranges::any_of(
        state.enemy.units, [](const UnitSnapshot& enemy) {
            return enemy.visible && enemy.completed && !enemy.flying &&
                   isCombatUnit(enemy.kind);
        });
    const auto earlyMeleeStrike = currentMeleeRush &&
                                  state.frame >= framesForMinutes(4) &&
                                  state.frame < framesForMinutes(8) &&
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
            (!home.valid() || withinPixelRadius(home, enemy.position, PixelRadius{960}));
        });
    const auto earlyTimingAttack = state.frame >= framesForMinutes(8) + framesForSeconds(12) &&
                                   state.frame < framesForMinutes(11) &&
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
    const auto reaverTimingStrike = state.frame >= framesForMinutes(9) &&
                                    state.frame < framesForMinutes(13) &&
                                    effectiveUnitCount(state, UnitKind::reaver, UnitCountBasis::completed) >= 1 &&
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
    const auto counterPushWindow = state.frame >= framesForMinutes(8) &&
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
    // A defended main is not a reason to stay on one base forever. Once the
    // first Robotics/Observer chain and a three-Cannon shell are complete, a
    // healthy 18-worker economy can start the natural even while a small
    // enemy group remains on the perimeter. Require a local power edge and no
    // hard breach so this is a controlled Stardust-style phase transition,
    // not a blind Nexus during an all-in.
    const auto defensiveEconomyWindow = state.frame >= framesForMinutes(10) &&
                                        workers >= 18 && mobileOpening >= 10 &&
                                        effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::completed) >= 3 &&
                                        effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::completed) > 0 &&
                                        effectiveUnitCount(state, UnitKind::roboticsFacility, UnitCountBasis::completed) > 0 &&
                                        effectiveUnitCount(state, UnitKind::observatory, UnitCountBasis::completed) > 0 &&
                                        !directBreach &&
                                        threat.combatEnemiesNearMain <= 2 &&
                                        (!activeApproach(state, threat) ||
                                         ownMobilePower >= visibleEnemyPower * 1.30) &&
                                        threat.immediateGround <= 0.55 &&
                                        (visibleEnemyPower <= 0.1 ||
                                         ownMobilePower >= visibleEnemyPower * 1.20);
    if (defensiveEconomyWindow) {
        result.desiredBases = std::max(result.desiredBases, 2);
        result.maximumBases = std::max(result.maximumBases, 2);
        result.posture = Posture::pressure;
        result.attackThreshold = std::min(result.attackThreshold, 1.30);
        result.minimumAttackSize = std::min(result.minimumAttackSize, 10);
        result.name += " [defensive economic transition]";
    }
    if ((roboticsNeeded || supportNeeded) && !hardBreachAtMain(state) &&
        !economicCounterWindow) {
        // Protect the Robotics -> Support Bay sequence from composition filler.
        // A brief pause is cheaper than entering the first Reaver fight with
        // an empty gas/mineral bank; direct pressure keeps the normal
        // defensive queue in control.
        const auto preserveDragoonScreen = supportNeeded && dragoonsReady < 6;
        const auto preserveMeleeScreen = currentMeleeRush &&
                                         state.frame < framesForMinutes(10) &&
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
                                       std::max(1, effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed)));
        result.maximumBases = std::max(1, effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed));
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


}  // namespace protodd
