// Isolated UMS validation module. Never packaged as the playing bot.
#include "BwapiBridge.hpp"
#include "WholeGameAction.hpp"
#include "protodd/Technology.hpp"
#include "protodd/Strategy.hpp"
#include "protodd/UnitCatalog.hpp"
#include "protodd/UnitMemory.hpp"
#include "protodd/Workers.hpp"
#include <BWAPI.h>
#include <windows.h>
#include <array>
#include <fstream>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace protodd;
class AuditScenario final : public BWAPI::AIModule {
    struct CombatCorpusLane {
        const char* name{};
        int minX{};
        int maxX{};
        int minY{};
        int maxY{};
        CombatEstimate prediction{};
        int observedTerminalFrame{-1};
        int initialFriendly{};
        int initialEnemy{};
        bool predicted{};
        bool excludedAsUndetected{};
        bool excludedAsUnavailable{};
        double uncertaintyInput{0.15};
    };
    struct CombatCorpusMember {
        const char* lane{};
        UnitId id{-1};
        UnitKind kind{UnitKind::unknown};
        bool friendly{};
        bool combatant{};
        int initialDurability{};
        int maximumDurability{};
        int deathFrame{-1};
    };
    protodd::bwapi::BwapiBridge bridge;
    MacroPlanner planner;
    StrategyEngine strategy;
    std::ofstream log;
    std::string scenario;
    bool checked{}, requested{}, accepted{}, confirmed{}, finished{};
    bool producerPairStarted{}, producerPairTested{}, producerPairInjected{};
    bool powerLossSeeded{}, powerLossTrainSeeded{}, powerLossUpgradeSeeded{};
    bool powerLossActiveBefore{}, powerLossSeedChecksDone{}, powerLossObserved{}, powerLossRestored{};
    bool powerLossRecoveryIssued{}, powerLossOperationsResumed{}, powerLossPausedVerified{};
    bool powerLossDefendersDispatched{};
    bool cannonBlockerStarted{}, cannonBlockerProbeAccepted{}, cannonBlockerProbeDebitVerified{};
    bool resourceOverlapOrdersIssued{};
    int constructionBudgetDeferredFrame{-1}, constructionBudgetMineralsBefore{-1};
    int constructionBudgetGasBefore{-1}, constructionBudgetPylonsBefore{};
    bool constructionBudgetRetried{};
    bool t021Started{}, t021StopRejected{}, t021StopAccepted{}, t021Finished{};
    bool t020Started{}, t020ConstructionStarted{}, t020LegalityChecked{}, t020CancelIssued{};
    int t020Builder{-1}, t020Building{-1}, t020CancelFrame{-1};
    BWAPI::TilePosition t020Site{-1, -1};
    bool t034Started{}, t034ReplacementChecked{}, t034Finished{};
    int t034Builder{-1}, t034BuildCommands{}, t034AcceptedStops{}, t034AcceptedMoves{};
    BWAPI::TilePosition t034Site{-1, -1};
    int t021Builder{-1}, t021BuildCommands{}, t021AcceptedStopCommands{};
    int t021MineralsBefore{-1};
    Position t021Target{-1, -1};
    bool workerIssuerReset{}, workerIssuerStarted{}, workerIssuerAccepted{};
    bool workerDefenseStarted{};
    WorkerManager t034Workers;
    int workerDefenseMainAccepted{}, workerDefenseNaturalAccepted{};
    int workerDefenseOtherAccepted{};
    bool wholeGameIssuerReset{}, wholeGameIssuerStarted{}, wholeGameIssuerAccepted{};
    bool wholeGameLeaseBlocked{}, wholeGameNativeAfterLeaseAccepted{};
    bool commandSuppressionStarted{}, commandAttackActive{}, commandActiveSuppressionChecked{};
    bool commandTargetDied{}, commandStaleTargetRejected{}, commandMoveIssued{};
    bool commandMoveActive{}, commandMoveInterrupted{}, commandCutoffChecked{};
    bool unitMemoryMoveIssued{};
    bool unitMemoryFootprintChecked{};
    bool footprintNativeChecked{};
    bool dynamicNavigationChecked{};
    bool dynamicNavigationMineralDepleted{};
    int dynamicNavigationMineralId{-1};
    int dynamicNavigationStartedFrame{-1};
    std::uint64_t dynamicNavigationMineralVersion{};
    Position dynamicNavigationMineralFrom{-1, -1};
    Position dynamicNavigationMineralTo{-1, -1};
    Position dynamicNavigationRemoteFrom{-1, -1};
    Position dynamicNavigationRemoteTo{-1, -1};
    std::vector<Position> dynamicNavigationMineralRoute;
    std::vector<Position> dynamicNavigationRemoteRoute;
    NavigationGrid dynamicNavigationGrid;
    std::array<int, 4> footprintNativeActorIds{{-1, -1, -1, -1}};
    std::array<MovementFootprint, 4> footprintNativeFootprints{};
    std::array<bool, 4> footprintNativePlanned{};
    std::array<bool, 4> footprintNativeCrossed{};
    std::vector<std::pair<int, int>> footprintNativeGaps;
    int footprintNativeWallTop{};
    int footprintNativeWallBottom{};
    int footprintNativeStartFrame{-1};
    int unitMemoryDarkTemplar{-1}, unitMemoryObserver{-1}, unitMemoryLastSeen{-1};
    int initialEnergy{}, selected{-1}, failures{};
    int producerPairBusy{-1}, producerPairIdle{-1}, producerPairAcceptedActor{-1};
    int powerLossGateway{-1}, powerLossCore{-1}, powerLossPylon{-1};
    int powerLossTrainAtLoss{-1}, powerLossUpgradeAtLoss{-1};
    int powerLossTrainAfterRestore{-1}, powerLossUpgradeAfterRestore{-1};
    int powerLossZealotsAtLoss{};
    int powerLossLossFrame{-1}, powerLossRestoreFrame{-1};
    int cannonBlockerMineralsBefore{-1}, cannonBlockerMineralsAfter{-1};
    int resourceOverlapMineralsBefore{-1}, resourceOverlapMineralsAfterOrders{-1};
    int resourceOverlapGasBefore{-1}, resourceOverlapGasAfterOrders{-1};
    int commandActor{-1}, commandTarget{-1}, commandMoveIssuedFrame{-1};
    int workerIssuerActor{-1}, workerIssuerTarget{-1}, workerIssuerResetFrame{-1};
    int wholeGameIssuerActor{-1}, wholeGameIssuerFrame{-1};
    Frame commandLatency{-1};
    Command commandAttack, commandMove;
    CommandBus suppressionBus;
    std::array<CombatCorpusLane, 6> combatCorpusLanes{{
        {"melee-shields", 700, 1200, 800, 1200},
        {"range-cooldown", 1300, 1900, 800, 1200},
        {"reaver-ammunition", 2100, 2600, 800, 1200},
        {"siege-splash", 2700, 3250, 800, 1200},
        {"detection-elevation", 3300, 3900, 1600, 2000},
        {"high-ground", 1350, 1650, 550, 700},
    }};
    std::vector<CombatCorpusMember> combatCorpusMembers;
    int combatCorpusStartFrame{-1};
    bool combatCorpusStarted{};
    protodd::Position supplyPylonTarget{-1, -1};
    std::vector<int> initialPylonIds;
    void check(const char* name, bool value) {
        log << "CHECK," << name << ',' << value << '\n';
        if (!value) ++failures;
        log.flush();
    }
    void executeWorkerProposals(const std::span<const WorkerAssignment> assignments) {
        CommandBus commands;
        commands.beginFrame(BWAPI::Broodwar->getFrameCount(),
                            BWAPI::Broodwar->getLatencyFrames());
        const auto proposed = bridge.submitWorkerCommands(assignments, commands);
        for (const auto& command : commands.finalize(proposed)) {
            if (bridge.execute(command)) commands.markIssued(command);
        }
    }
    static const char* decisionName(const FightDecision decision) {
        switch (decision) {
            case FightDecision::engage: return "engage";
            case FightDecision::kite: return "kite";
            case FightDecision::retreat: return "retreat";
        }
        return "unknown";
    }
    static bool simulatedCombatant(const UnitSnapshot& unit) {
        return isCombatUnit(unit.kind) || isStaticDefense(unit.kind) || isWorker(unit.kind);
    }
    void runCombatCorpus(const GameState& state, const int frame) {
        const auto native = [](const UnitId id) { return BWAPI::Broodwar->getUnit(id); };
        const auto laneUnits = [](const auto& units, const CombatCorpusLane& lane) {
            std::vector<UnitSnapshot> result;
            for (const auto& unit : units) {
                if (unit.position.x >= lane.minX && unit.position.x < lane.maxX &&
                    unit.position.y >= lane.minY && unit.position.y < lane.maxY)
                    result.push_back(unit);
            }
            return result;
        };
        if (!combatCorpusStarted) {
            combatCorpusStarted = true;
            combatCorpusStartFrame = frame;
            log << "MODEL_SCOPE,terrainElevation=observed-not-simulated,"
                << "pathingAndDynamicMovement=approximate,projectileTravel=approximate,"
                << "splashMovement=discounted,nativeWeaponSplashCoverage=partial,"
                << "spellsAndDisables=unsupported,"
                << "ammunition=observed-start-state,noReplenishmentModeled\n";
            int activeLanes{};
            int detectedDarkTemplars{};
            for (auto& lane : combatCorpusLanes) {
                auto friendly = laneUnits(state.self.units, lane);
                auto enemy = laneUnits(state.enemy.units, lane);
                if (friendly.empty() || enemy.empty()) {
                    if (std::string(lane.name) == "detection-elevation" &&
                        !friendly.empty() && enemy.empty()) {
                        lane.excludedAsUndetected = true;
                        log << "EXCLUDED," << lane.name << ",reason=undetected-enemy"
                            << ",friendlySnapshots=" << friendly.size()
                            << ",enemySnapshots=" << enemy.size() << '\n';
                    } else {
                        lane.excludedAsUnavailable = true;
                        log << "EXCLUDED," << lane.name << ",reason=fixture-unavailable"
                            << ",friendlySnapshots=" << friendly.size()
                            << ",enemySnapshots=" << enemy.size() << '\n';
                    }
                    continue;
                }
                lane.initialFriendly = static_cast<int>(std::ranges::count_if(
                    friendly, simulatedCombatant));
                lane.initialEnemy = static_cast<int>(std::ranges::count_if(
                    enemy, simulatedCombatant));
                lane.uncertaintyInput = 0.15;
                for (const auto* side : {&friendly, &enemy}) {
                    for (const auto& snapshot : *side) {
                        const auto unit = native(snapshot.id);
                        if (unit == nullptr || !unit->exists()) continue;
                        const auto engineWeapon = snapshot.flying
                            ? unit->getType().airWeapon() : unit->getType().groundWeapon();
                        const auto effect = engineWeapon.explosionType();
                        const auto hasSplash = engineWeapon.outerSplashRadius() > 0 &&
                            (effect == BWAPI::ExplosionTypes::Radial_Splash ||
                             effect == BWAPI::ExplosionTypes::Enemy_Splash ||
                             effect == BWAPI::ExplosionTypes::Air_Splash);
                        const auto modeledSplash = snapshot.flying
                            ? snapshot.airWeapon.splashOuter : snapshot.groundWeapon.splashOuter;
                        if (!hasSplash || modeledSplash > 0) continue;
                        lane.uncertaintyInput = std::max(lane.uncertaintyInput, 0.8);
                        log << "UNSUPPORTED_MECHANIC," << lane.name << ",unit=" << snapshot.id
                            << ",mechanic=weapon-splash,engineExplosion=" << effect.getID()
                            << ",engineRadii=" << engineWeapon.innerSplashRadius() << '/'
                            << engineWeapon.medianSplashRadius() << '/'
                            << engineWeapon.outerSplashRadius()
                            << ",modelOuter=" << modeledSplash
                            << ",uncertaintyInput=" << lane.uncertaintyInput << '\n';
                    }
                }
                lane.prediction = CombatEvaluator{}.evaluate(
                    friendly, enemy, 1.0, lane.uncertaintyInput);
                lane.predicted = true;
                ++activeLanes;
                log << "PREDICTED," << lane.name << ",frame=" << frame
                    << ",friendly=" << lane.initialFriendly << ",enemy=" << lane.initialEnemy
                    << ",frames=" << lane.prediction.simulatedFrames
                    << ",terminal=" << lane.prediction.simulatedOutcomeReached
                    << ",friendlyDeaths=" << lane.prediction.simulatedFriendlyDeaths
                    << ",enemyDeaths=" << lane.prediction.simulatedEnemyDeaths
                    << ",friendlyLoss=" << lane.prediction.simulatedFriendlyLoss
                    << ",enemyLoss=" << lane.prediction.simulatedEnemyLoss
                    << ",decision=" << decisionName(lane.prediction.decision)
                    << ",ratio=" << lane.prediction.ratio
                    << ",uncertaintyInput=" << lane.uncertaintyInput
                    << ",confidence=" << lane.prediction.confidence << '\n';

                for (const auto& snapshot : friendly) {
                    const auto unit = native(snapshot.id);
                    if (unit == nullptr || !unit->exists()) continue;
                    const auto combatant = simulatedCombatant(snapshot);
                    if (combatant) {
                        combatCorpusMembers.push_back({lane.name, snapshot.id, snapshot.kind,
                            true, true, unit->getHitPoints() + unit->getShields(),
                            unit->getType().maxHitPoints() + unit->getType().maxShields(), -1});
                    }
                    const auto tile = unit->getTilePosition();
                    const auto weapon = snapshot.groundWeapon;
                    const auto engineWeapon = unit->getType().groundWeapon();
                    log << "MECHANIC," << lane.name << ",friendly," << snapshot.id << ','
                        << unitStats(snapshot.kind).name << ",hp=" << unit->getHitPoints()
                        << ",shields=" << unit->getShields() << ",armor=" << snapshot.armor
                        << ",shieldArmor=" << snapshot.shieldArmor
                        << ",cooldown=" << unit->getGroundWeaponCooldown()
                        << ",ammo=" << snapshot.ammo << ",visible=" << snapshot.visible
                        << ",detected=" << snapshot.detected << ",cloaked=" << snapshot.cloaked
                        << ",damage=" << weapon.damage << ",range=" << weapon.maxRange
                        << ",splash=" << weapon.splashOuter
                        << ",engineExplosion=" << engineWeapon.explosionType().getID()
                        << ",engineSplash=" << engineWeapon.innerSplashRadius() << '/'
                        << engineWeapon.medianSplashRadius() << '/'
                        << engineWeapon.outerSplashRadius()
                        << ",groundHeight=" << BWAPI::Broodwar->getGroundHeight(tile)
                        << '\n';
                }
                for (const auto& snapshot : enemy) {
                    const auto unit = native(snapshot.id);
                    if (unit == nullptr || !unit->exists()) continue;
                    const auto combatant = simulatedCombatant(snapshot);
                    if (combatant) {
                        combatCorpusMembers.push_back({lane.name, snapshot.id, snapshot.kind,
                            false, true, unit->getHitPoints() + unit->getShields(),
                            unit->getType().maxHitPoints() + unit->getType().maxShields(), -1});
                    }
                    if (snapshot.kind == UnitKind::darkTemplar && snapshot.detected)
                        ++detectedDarkTemplars;
                    const auto tile = unit->getTilePosition();
                    const auto weapon = snapshot.groundWeapon;
                    const auto engineWeapon = unit->getType().groundWeapon();
                    log << "MECHANIC," << lane.name << ",enemy," << snapshot.id << ','
                        << unitStats(snapshot.kind).name << ",hp=" << unit->getHitPoints()
                        << ",shields=" << unit->getShields() << ",armor=" << snapshot.armor
                        << ",shieldArmor=" << snapshot.shieldArmor
                        << ",cooldown=" << unit->getGroundWeaponCooldown()
                        << ",ammo=" << snapshot.ammo << ",visible=" << snapshot.visible
                        << ",detected=" << snapshot.detected << ",cloaked=" << snapshot.cloaked
                        << ",damage=" << weapon.damage << ",range=" << weapon.maxRange
                        << ",splash=" << weapon.splashOuter
                        << ",engineExplosion=" << engineWeapon.explosionType().getID()
                        << ",engineSplash=" << engineWeapon.innerSplashRadius() << '/'
                        << engineWeapon.medianSplashRadius() << '/'
                        << engineWeapon.outerSplashRadius()
                        << ",groundHeight=" << BWAPI::Broodwar->getGroundHeight(tile)
                        << '\n';
                }
                int acceptedOrders{};
                for (const auto& actor : friendly) {
                    const auto unit = native(actor.id);
                    if (unit == nullptr || !unit->exists() || !simulatedCombatant(actor)) continue;
                    const UnitSnapshot* targetSnapshot = nullptr;
                    auto bestDistance = std::numeric_limits<double>::infinity();
                    for (const auto& target : enemy) {
                        if (!actor.canAttack(target)) continue;
                        const auto dx = static_cast<double>(actor.position.x - target.position.x);
                        const auto dy = static_cast<double>(actor.position.y - target.position.y);
                        const auto distance = dx * dx + dy * dy;
                        if (distance < bestDistance) {
                            bestDistance = distance;
                            targetSnapshot = &target;
                        }
                    }
                    if (targetSnapshot == nullptr) continue;
                    const auto target = native(targetSnapshot->id);
                    if (target != nullptr && target->exists() && unit->attack(target))
                        ++acceptedOrders;
                }
                log << "ORDERS," << lane.name << ",accepted=" << acceptedOrders << '\n';
                if (std::string(lane.name) == "reaver-ammunition") {
                    check("corpus-empty-reaver-does-not-attack",
                        acceptedOrders == 0 && friendly.front().kind == UnitKind::reaver &&
                        friendly.front().ammo == 0);
                } else {
                    check((std::string("corpus-orders-") + lane.name).c_str(), acceptedOrders > 0);
                }
            }
            int enemyPermanentCloakers{}, undetectedEnemyPermanentCloakers{};
            for (const auto unit : BWAPI::Broodwar->getAllUnits()) {
                if (unit == nullptr || !unit->exists()) continue;
                const auto position = unit->getPosition();
                if (position.x < 3300 || position.x >= 3900) continue;
                if (unit->getPlayer() != BWAPI::Broodwar->self() &&
                    unit->getType().hasPermanentCloak()) {
                    ++enemyPermanentCloakers;
                    if (!unit->isDetected()) ++undetectedEnemyPermanentCloakers;
                }
                log << "DETECTION_NATIVE," << frame << ",id=" << unit->getID()
                    << ",type=" << unit->getType().getName()
                    << ",owner=" << unit->getPlayer()->getID()
                    << ",x=" << position.x << ",y=" << position.y
                    << ",visible=" << unit->isVisible() << ",detected=" << unit->isDetected()
                    << ",cloaked=" << unit->isCloaked()
                    << ",typeHasPermanentCloak=" << unit->getType().hasPermanentCloak()
                    << ",typeCloakable=" << unit->getType().isCloakable() << '\n';
            }
            const auto observedDarkTemplars = static_cast<int>(std::ranges::count_if(
                state.enemy.units, [](const UnitSnapshot& unit) {
                    return unit.kind == UnitKind::darkTemplar;
                }));
            check("corpus-fixture-four-prediction-lanes", activeLanes == 4);
            if (scenario == "combat-corpus-elevation") {
                const auto& highLane = combatCorpusLanes.back();
                const auto friendly = laneUnits(state.self.units, highLane);
                const auto enemy = laneUnits(state.enemy.units, highLane);
                auto friendlyHeight = -1;
                auto enemyHeight = -1;
                if (!friendly.empty()) {
                    const auto unit = native(friendly.front().id);
                    if (unit != nullptr) friendlyHeight = BWAPI::Broodwar->getGroundHeight(
                        unit->getTilePosition());
                }
                if (!enemy.empty()) {
                    const auto unit = native(enemy.front().id);
                    if (unit != nullptr) enemyHeight = BWAPI::Broodwar->getGroundHeight(
                        unit->getTilePosition());
                }
                check("corpus-elevation-high-low-pair",
                    friendlyHeight == 2 && enemyHeight == 0);
            }
            check("corpus-hidden-target-excluded-from-simulation",
                enemyPermanentCloakers == 1 && undetectedEnemyPermanentCloakers == 1 &&
                observedDarkTemplars == 0 &&
                combatCorpusLanes[4].excludedAsUndetected);

            auto minHeight = std::numeric_limits<int>::max();
            auto maxHeight = std::numeric_limits<int>::min();
            std::array<int, 6> heightCounts{};
            std::array<int, 6> sampleX{};
            std::array<int, 6> sampleY{};
            sampleX.fill(-1);
            sampleY.fill(-1);
            int minX{}, minY{}, maxX{}, maxY{};
            for (auto y = 0; y < BWAPI::Broodwar->mapHeight(); ++y) {
                for (auto x = 0; x < BWAPI::Broodwar->mapWidth(); ++x) {
                    const auto height = BWAPI::Broodwar->getGroundHeight(x, y);
                    if (height >= 0 && height < static_cast<int>(heightCounts.size())) {
                        ++heightCounts[static_cast<std::size_t>(height)];
                        auto& sample = sampleX[static_cast<std::size_t>(height)];
                        if (sample < 0) {
                            sample = x;
                            sampleY[static_cast<std::size_t>(height)] = y;
                        }
                    }
                    if (height < minHeight) { minHeight = height; minX = x; minY = y; }
                    if (height > maxHeight) { maxHeight = height; maxX = x; maxY = y; }
                }
            }
            int highTileX{-1}, highTileY{-1}, lowTileX{-1}, lowTileY{-1};
            for (auto highY = 20; highY < BWAPI::Broodwar->mapHeight() - 12 &&
                 highTileX < 0; ++highY) {
                for (auto highX = 24; highX < BWAPI::Broodwar->mapWidth() - 20 &&
                     highTileX < 0; ++highX) {
                    if (BWAPI::Broodwar->getGroundHeight(highX, highY) != 2 ||
                        !BWAPI::Broodwar->isWalkable(highX * 4 + 2, highY * 4 + 2)) continue;
                    for (auto dy = -4; dy <= 4 && highTileX < 0; ++dy) {
                        for (auto dx = -4; dx <= 4 && highTileX < 0; ++dx) {
                            const auto lowX = highX + dx;
                            const auto lowY = highY + dy;
                            const auto separation = dx * dx + dy * dy;
                            if (separation < 4 || separation > 16 || lowX < 1 || lowY < 1 ||
                                lowX >= BWAPI::Broodwar->mapWidth() - 1 ||
                                lowY >= BWAPI::Broodwar->mapHeight() - 1 ||
                                BWAPI::Broodwar->getGroundHeight(lowX, lowY) != 0 ||
                                !BWAPI::Broodwar->isWalkable(lowX * 4 + 2, lowY * 4 + 2)) continue;
                            highTileX = highX; highTileY = highY;
                            lowTileX = lowX; lowTileY = lowY;
                        }
                    }
                }
            }
            log << "TERRAIN_PAIR,found=" << (highTileX >= 0)
                << ",high=" << highTileX << 'x' << highTileY
                << ",low=" << lowTileX << 'x' << lowTileY
                << ",pixelDistance=" << (highTileX >= 0
                    ? std::hypot(static_cast<double>(highTileX - lowTileX),
                                 static_cast<double>(highTileY - lowTileY)) * 32.0 : 0.0)
                << '\n';
            log << "TERRAIN_RANGE,tiles=" << BWAPI::Broodwar->mapWidth() << 'x'
                << BWAPI::Broodwar->mapHeight() << ",min=" << minHeight << '@'
                << minX << 'x' << minY << ",max=" << maxHeight << '@'
                << maxX << 'x' << maxY << '\n';
            for (std::size_t height = 0; height < heightCounts.size(); ++height)
                log << "TERRAIN_CLASS," << height << ",tiles=" << heightCounts[height]
                    << ",sample=" << sampleX[height] << 'x' << sampleY[height] << '\n';
            log << "CORPUS_START," << frame << ",lanes=" << activeLanes
                << ",detectedDarkTemplars=" << detectedDarkTemplars
                << ",enemyPermanentCloakers=" << enemyPermanentCloakers
                << ",undetectedEnemyPermanentCloakers=" << undetectedEnemyPermanentCloakers << '\n';
        }

        bool allLanesResolved = true;
        for (auto& lane : combatCorpusLanes) {
            if (!lane.predicted) {
                if (!lane.excludedAsUndetected && !lane.excludedAsUnavailable)
                    allLanesResolved = false;
                continue;
            }
            int friendlyAlive{}, enemyAlive{};
            for (auto& member : combatCorpusMembers) {
                if (member.lane != std::string(lane.name)) continue;
                const auto unit = native(member.id);
                if (unit == nullptr || !unit->exists()) {
                    if (member.deathFrame < 0) member.deathFrame = frame;
                    continue;
                }
                if (member.friendly) ++friendlyAlive;
                else ++enemyAlive;
            }
            if (lane.observedTerminalFrame < 0 &&
                (friendlyAlive == 0 || enemyAlive == 0))
                lane.observedTerminalFrame = frame;
            if (friendlyAlive > 0 && enemyAlive > 0) allLanesResolved = false;
        }

        if (frame % 24 == 0 || allLanesResolved || frame - combatCorpusStartFrame >= 1200) {
            for (const auto& member : combatCorpusMembers) {
                const auto unit = native(member.id);
                if (unit == nullptr || !unit->exists()) {
                    log << "TRACE," << frame << ',' << member.lane << ','
                        << (member.friendly ? "friendly" : "enemy") << ',' << member.id
                        << ",exists=0,deathFrame=" << member.deathFrame << '\n';
                    continue;
                }
                const auto tile = unit->getTilePosition();
                const auto target = unit->getOrderTarget();
                log << "TRACE," << frame << ',' << member.lane << ','
                    << (member.friendly ? "friendly" : "enemy") << ',' << member.id
                    << ",exists=1,hp=" << unit->getHitPoints()
                    << ",shields=" << unit->getShields()
                    << ",cooldown=" << unit->getGroundWeaponCooldown()
                    << ",ammo=" << unit->getScarabCount()
                    << ",visible=" << unit->isVisible() << ",detected=" << unit->isDetected()
                    << ",cloaked=" << unit->isCloaked()
                    << ",groundHeight=" << BWAPI::Broodwar->getGroundHeight(tile)
                    << ",target=" << (target != nullptr ? target->getID() : -1)
                    << ",order=" << unit->getOrder().toString() << '\n';
            }
            log.flush();
        }

        if (!allLanesResolved && frame - combatCorpusStartFrame < 1200) return;
        for (const auto& lane : combatCorpusLanes) {
            if (!lane.predicted) continue;
            int friendlyAlive{}, enemyAlive{}, friendlyDeaths{}, enemyDeaths{};
            double friendlyLoss{}, enemyLoss{};
            for (const auto& member : combatCorpusMembers) {
                if (member.lane != std::string(lane.name)) continue;
                const auto unit = native(member.id);
                const auto alive = unit != nullptr && unit->exists();
                if (alive) {
                    if (member.friendly) ++friendlyAlive;
                    else ++enemyAlive;
                } else if (member.friendly) ++friendlyDeaths;
                else ++enemyDeaths;
                const auto remaining = alive ? unit->getHitPoints() + unit->getShields() : 0;
                const auto value = unitStats(member.kind).combatValue;
                const auto lost = value * std::clamp(
                    1.0 - static_cast<double>(remaining) /
                        static_cast<double>(std::max(1, member.maximumDurability)), 0.0, 1.0);
                if (member.friendly) friendlyLoss += lost;
                else enemyLoss += lost;
            }
            const auto observedFrames = lane.observedTerminalFrame >= 0
                ? lane.observedTerminalFrame - combatCorpusStartFrame : -1;
            const auto framesError = observedFrames >= 0 &&
                lane.prediction.simulatedOutcomeReached
                ? observedFrames - lane.prediction.simulatedFrames : -1;
            const auto observedResult = friendlyAlive > enemyAlive ? "friendly-favored" :
                friendlyAlive < enemyAlive ? "enemy-favored" : "even";
            const auto decisionAligned = lane.prediction.decision == FightDecision::engage
                ? friendlyAlive >= enemyAlive
                : lane.prediction.decision == FightDecision::retreat
                    ? friendlyAlive <= enemyAlive : false;
            log << "OUTCOME," << lane.name << ",frame=" << frame
                << ",friendlySurvivors=" << friendlyAlive << ",enemySurvivors=" << enemyAlive
                << ",friendlyDeaths=" << friendlyDeaths << ",enemyDeaths=" << enemyDeaths
                << ",friendlyLoss=" << friendlyLoss << ",enemyLoss=" << enemyLoss
                << ",terminalFrame=" << observedFrames
                << ",result=" << observedResult << '\n'
                << "COMPARE," << lane.name << ",predictedFrames="
                << (lane.prediction.simulatedOutcomeReached ? lane.prediction.simulatedFrames : -1)
                << ",observedFrames=" << observedFrames << ",ttkErrorFrames=" << framesError
                << ",predictedFriendlyDeaths=" << lane.prediction.simulatedFriendlyDeaths
                << ",observedFriendlyDeaths=" << friendlyDeaths
                << ",predictedEnemyDeaths=" << lane.prediction.simulatedEnemyDeaths
                << ",observedEnemyDeaths=" << enemyDeaths
                << ",friendlyLossError="
                << (friendlyLoss - lane.prediction.simulatedFriendlyLoss)
                << ",enemyLossError=" << (enemyLoss - lane.prediction.simulatedEnemyLoss)
                << ",predictedDecision=" << decisionName(lane.prediction.decision)
                << ",observedResult=" << observedResult
                << ",decisionAligned=" << decisionAligned
                << ",uncertaintyInput=" << lane.uncertaintyInput
                << ",predictedConfidence=" << lane.prediction.confidence << '\n';
        }
        check("corpus-comparisons-written", std::ranges::all_of(
            combatCorpusLanes, [](const CombatCorpusLane& lane) {
                return lane.predicted || lane.excludedAsUndetected ||
                    lane.excludedAsUnavailable;
            }));
        check("engine-postcondition", failures == 0);
        log << "DONE," << frame << ',' << failures << '\n';
        log.flush();
        finished = true;
        BWAPI::Broodwar->leaveGame();
    }
    void onUnitDestroy(const BWAPI::Unit unit) override {
        if (scenario != "dynamic-navigation" || unit == nullptr ||
            unit->getID() != dynamicNavigationMineralId) return;
        const auto blockedBeforeRemoval = !dynamicNavigationGrid.lineWalkable(
            dynamicNavigationMineralFrom, dynamicNavigationMineralTo);
        bridge.removeNavigationObstacle(dynamicNavigationGrid, unit);
        const auto opened = dynamicNavigationGrid.lineWalkable(
            dynamicNavigationMineralFrom, dynamicNavigationMineralTo);
        const auto affected = dynamicNavigationGrid.routeAffectedSince(
            dynamicNavigationMineralRoute, dynamicNavigationMineralFrom,
            dynamicNavigationMineralTo, {}, dynamicNavigationMineralVersion);
        const auto remoteReusable = !dynamicNavigationGrid.routeAffectedSince(
            dynamicNavigationRemoteRoute, dynamicNavigationRemoteFrom,
            dynamicNavigationRemoteTo, {}, dynamicNavigationMineralVersion);
        log << "DYNAMIC_MINERAL_DEPLETION," << BWAPI::Broodwar->getFrameCount()
            << ",id=" << unit->getID() << ",blockedBefore=" << blockedBeforeRemoval
            << ",opened=" << opened << ",affected=" << affected
            << ",remoteReusable=" << remoteReusable << '\n';
        check("dynamic-navigation-mineral-depletion-event-opens-route",
              blockedBeforeRemoval && opened && affected);
        check("dynamic-navigation-mineral-depletion-keeps-distant-cache",
              remoteReusable);
        dynamicNavigationMineralDepleted = true;
    }
public:
    void onStart() override {
        std::ifstream("bwapi-data/read/scenario.txt") >> scenario;
        log.open("bwapi-data/write/scenario.csv");
        log << "START," << scenario << ',' << BWAPI::Broodwar->mapFileName() << '\n'; log.flush();
        BWAPI::Broodwar->setLocalSpeed(0);
        BWAPI::Broodwar->setFrameSkip(
            scenario == "pylon-loss" || scenario == "cannon-blocker" ||
                    scenario == "resource-overlap" || scenario == "construction-budget" ||
                    scenario == "worker-issuer" ||
                    scenario == "build-cancel" || scenario == "builder-evacuation" ||
                    scenario == "builder-evacuation-started" ||
                    scenario == "worker-local-defense" ||
                    scenario == "worker-mining" || scenario == "scout-issuer" ||
                    scenario == "whole-game-issuer" ||
                    scenario == "command-suppression" ||
                    scenario == "observer-safety" || scenario == "unit-memory" ||
                    scenario == "footprint-native" ||
                    scenario == "dynamic-navigation" ||
                    scenario.starts_with("combat-corpus") ? 1 : 64);
        bridge.buildSelectionDiagnostic = [this](const protodd::bwapi::BuildSelectionDiagnostic& d) {
            if (scenario != "supply-anchor" || d.kind != UnitKind::pylon) return;
            supplyPylonTarget = d.target;
            log << "BUILDSELECT," << d.frame << ",kind=Protoss_Pylon,selected=" << d.selected
                << ",targetX=" << d.target.x << ",targetY=" << d.target.y
                << ",anchorX=" << d.anchor.x << ",anchorY=" << d.anchor.y
                << ",selectedDistance=" << d.selectedDistance << '\n';
            log.flush();
        };
        bridge.onStart();
        if (scenario == "pylon-loss") {
            bridge.actionDiagnostic = [this](const protodd::bwapi::ActionDiagnostic& d) {
                if (d.source != "production-demand") return;
                log << "POWERLOSS_SEED_ACTION," << d.actor << ',' << d.type << ','
                    << d.outcome << ',' << d.attempted << ',' << d.accepted << '\n';
                log.flush();
            };
        }
        if (scenario == "command-suppression") {
            bridge.actionDiagnostic = [this](const protodd::bwapi::ActionDiagnostic& d) {
                if (!d.source.starts_with("T014")) return;
                log << "T014_ACTION," << BWAPI::Broodwar->getFrameCount() << ','
                    << d.actor << ',' << d.target << ',' << d.type << ',' << d.source
                    << ',' << d.outcome << ',' << d.attempted << ',' << d.accepted << '\n';
                log.flush();
            };
        }
        if (scenario == "observer-safety") {
            bridge.actionDiagnostic = [this](const protodd::bwapi::ActionDiagnostic& d) {
                if (!d.source.starts_with("observer-")) return;
                log << "ISSUER_ACTION," << BWAPI::Broodwar->getFrameCount() << ','
                    << d.actor << ',' << d.target << ',' << d.type << ',' << d.source
                    << ',' << d.outcome << ',' << d.attempted << ',' << d.accepted << '\n';
                log.flush();
            };
        }
        if (scenario == "pylon-loss") {
            std::vector<BWAPI::Unit> gateways, cores, pylons;
            for (const auto unit : BWAPI::Broodwar->self()->getUnits()) {
                if (unit == nullptr || !unit->exists()) continue;
                if (unit->getType() == BWAPI::UnitTypes::Protoss_Gateway) gateways.push_back(unit);
                if (unit->getType() == BWAPI::UnitTypes::Protoss_Cybernetics_Core) cores.push_back(unit);
                if (unit->getType() == BWAPI::UnitTypes::Protoss_Pylon) pylons.push_back(unit);
            }
            check("loss-fixture-gateway", gateways.size() == 1);
            check("loss-fixture-cyber-core", cores.size() == 1);
            check("loss-fixture-pylons", pylons.size() == 2);
            if (gateways.size() == 1 && cores.size() == 1 && pylons.size() == 2) {
                const auto gateway = gateways.front();
                const auto core = cores.front();
                const auto pylon = *std::min_element(pylons.begin(), pylons.end(),
                    [gateway](const auto left, const auto right) {
                        return left->getDistance(gateway) < right->getDistance(gateway);
                    });
                powerLossGateway = gateway->getID();
                powerLossCore = core->getID();
                powerLossPylon = pylon->getID();
                log << "POWERLOSS_FIXTURE," << "gateway=" << gateway->getID() << ':'
                    << gateway->getPosition().x << 'x' << gateway->getPosition().y << ':'
                    << gateway->isCompleted() << ':' << gateway->isPowered()
                    << ",core=" << core->getID() << ':' << core->getPosition().x << 'x'
                    << core->getPosition().y << ':' << core->isCompleted() << ':'
                    << core->isPowered() << ",pylon=" << pylon->getID() << ':'
                    << pylon->getPosition().x << 'x' << pylon->getPosition().y << ':'
                    << pylon->isCompleted() << ':' << pylon->getHitPoints() << ':'
                    << pylon->getShields() << '\n';
                check("loss-fixture-initial-power",
                      gateway->isPowered() && core->isPowered());
            }
        }
        log << "READY," << BWAPI::Broodwar->isPaused() << ',' << BWAPI::Broodwar->isInGame() << '\n'; log.flush();
    }
    void onEnd(bool won) override { log << "END," << BWAPI::Broodwar->getFrameCount() << ',' << won << '\n'; log.flush(); }
    void onFrame() override {
        if(finished) return;
        const int frame=BWAPI::Broodwar->getFrameCount();
        if(frame<2) { log << "FRAME," << frame << ',' << BWAPI::Broodwar->isPaused() << '\n'; log.flush(); }
        if (frame<24 && scenario!="pylon-loss") return;
        auto state=bridge.observe();
        InfluenceMap influence; influence.update(state);
        if (scenario.starts_with("combat-corpus")) {
            runCombatCorpus(state, frame);
            return;
        }
        if (scenario == "dynamic-navigation") {
            if (!dynamicNavigationChecked) {
                dynamicNavigationChecked = true;
                const auto allUnits = BWAPI::Broodwar->getAllUnits();
                const auto pylon = std::ranges::find_if(allUnits, [](const BWAPI::Unit unit) {
                    return unit != nullptr && unit->exists() &&
                        unit->getType() == BWAPI::UnitTypes::Protoss_Pylon;
                });
                const auto minerals = BWAPI::Broodwar->getMinerals();
                const auto mineral = std::ranges::find_if(minerals, [](const BWAPI::Unit unit) {
                    return unit != nullptr && unit->exists() && unit->getResources() > 0;
                });
                const auto completeFixture = pylon != allUnits.end() &&
                    mineral != minerals.end();
                check("dynamic-navigation-fixture-has-live-pylon-and-mineral",
                      completeFixture);
                if (!completeFixture) {
                    check("dynamic-navigation-engine-postcondition", false);
                    log << "DONE," << frame << ',' << failures << '\n';
                    log.flush();
                    finished = true;
                    BWAPI::Broodwar->leaveGame();
                    return;
                }

                const auto width = BWAPI::Broodwar->mapWidth() * 4;
                const auto height = BWAPI::Broodwar->mapHeight() * 4;
                dynamicNavigationGrid = NavigationGrid{width, height, 8,
                    std::vector<std::uint8_t>(static_cast<std::size_t>(width) *
                                              static_cast<std::size_t>(height), 1U)};
                const auto pathAcross = [this](const BWAPI::Unit unit) {
                    const auto y = unit->getPosition().y;
                    const Position from{unit->getLeft() - 96, y};
                    const Position to{unit->getRight() + 96, y};
                    std::vector<Position> points;
                    static_cast<void>(dynamicNavigationGrid.nextWaypoint(
                        from, to, 7, 12000, {}, &points));
                    return std::tuple{from, to, points};
                };
                const auto [pylonFrom, pylonTo, pylonRoute] = pathAcross(*pylon);
                const auto [mineralFrom, mineralTo, mineralRoute] = pathAcross(*mineral);
                dynamicNavigationRemoteFrom = {96, height * 8 - 160};
                dynamicNavigationRemoteTo = {480, height * 8 - 160};
                static_cast<void>(dynamicNavigationGrid.nextWaypoint(
                    dynamicNavigationRemoteFrom, dynamicNavigationRemoteTo,
                    7, 12000, {}, &dynamicNavigationRemoteRoute));
                const auto initiallyOpen = pylonRoute.size() == 2 &&
                    mineralRoute.size() == 2 && dynamicNavigationRemoteRoute.size() == 2;
                check("dynamic-navigation-fixture-routes-start-open", initiallyOpen);
                const auto initialVersion = dynamicNavigationGrid.obstacleVersion();
                bridge.initializeNavigationObstacles(dynamicNavigationGrid);
                const auto pylonClosed = !dynamicNavigationGrid.lineWalkable(pylonFrom, pylonTo) &&
                    dynamicNavigationGrid.routeAffectedSince(
                        pylonRoute, pylonFrom, pylonTo, {}, initialVersion);
                const auto mineralClosed = !dynamicNavigationGrid.lineWalkable(
                    mineralFrom, mineralTo) && dynamicNavigationGrid.routeAffectedSince(
                        mineralRoute, mineralFrom, mineralTo, {}, initialVersion);
                const auto remoteReusable = !dynamicNavigationGrid.routeAffectedSince(
                    dynamicNavigationRemoteRoute, dynamicNavigationRemoteFrom,
                    dynamicNavigationRemoteTo, {}, initialVersion);
                std::vector<Position> pylonDetour;
                std::vector<Position> mineralDetour;
                static_cast<void>(dynamicNavigationGrid.nextWaypoint(
                    pylonFrom, pylonTo, 7, 12000, {}, &pylonDetour));
                static_cast<void>(dynamicNavigationGrid.nextWaypoint(
                    mineralFrom, mineralTo, 7, 12000, {}, &mineralDetour));
                const auto promptDetours = pylonDetour.size() > 2 &&
                                           mineralDetour.size() > 2;
                dynamicNavigationMineralId = (*mineral)->getID();
                dynamicNavigationMineralFrom = mineralFrom;
                dynamicNavigationMineralTo = mineralTo;
                dynamicNavigationMineralRoute = mineralRoute;
                dynamicNavigationMineralVersion = dynamicNavigationGrid.obstacleVersion();
                log << "DYNAMIC_OBSTACLES,pylon=" << (*pylon)->getID()
                    << ",bounds=" << (*pylon)->getLeft() << ':' << (*pylon)->getTop() << ':'
                    << (*pylon)->getRight() << ':' << (*pylon)->getBottom()
                    << ",mineral=" << dynamicNavigationMineralId
                    << ",resources=" << (*mineral)->getResources()
                    << ",bounds=" << (*mineral)->getLeft() << ':' << (*mineral)->getTop() << ':'
                    << (*mineral)->getRight() << ':' << (*mineral)->getBottom()
                    << ",versions=" << initialVersion << ':'
                    << dynamicNavigationGrid.obstacleVersion()
                    << ",pylonClosed=" << pylonClosed << ",mineralClosed=" << mineralClosed
                    << ",remoteReusable=" << remoteReusable
                    << ",pylonDetourPoints=" << pylonDetour.size()
                    << ",mineralDetourPoints=" << mineralDetour.size() << '\n';
                check("dynamic-navigation-pylon-closes-cached-route", pylonClosed);
                check("dynamic-navigation-mineral-closes-cached-route", mineralClosed);
                check("dynamic-navigation-unaffected-route-remains-reusable", remoteReusable);
                check("dynamic-navigation-obstacles-produce-prompt-detours", promptDetours);

                const auto pylonVersion = dynamicNavigationGrid.obstacleVersion();
                bridge.removeNavigationObstacle(dynamicNavigationGrid, *pylon);
                const auto pylonOpened = dynamicNavigationGrid.lineWalkable(pylonFrom, pylonTo) &&
                    dynamicNavigationGrid.routeAffectedSince(
                        pylonRoute, pylonFrom, pylonTo, {}, pylonVersion);
                check("dynamic-navigation-pylon-removal-opens-route", pylonOpened);
                check("dynamic-navigation-distant-cache-survives-pylon-removal",
                    !dynamicNavigationGrid.routeAffectedSince(
                        dynamicNavigationRemoteRoute, dynamicNavigationRemoteFrom,
                        dynamicNavigationRemoteTo, {}, pylonVersion));

                auto miningOrders = 0;
                for (const auto worker : allUnits) {
                    if (worker == nullptr || !worker->exists() ||
                        worker->getPlayer() != BWAPI::Broodwar->self() ||
                        worker->getType() != BWAPI::UnitTypes::Protoss_Probe ||
                        worker->getDistance(*mineral) > 256) continue;
                    if (worker->gather(*mineral)) ++miningOrders;
                }
                check("dynamic-navigation-mining-orders-accepted", miningOrders > 0);
                dynamicNavigationStartedFrame = frame;
                log << "DYNAMIC_MINING_STARTED," << frame << ",workers="
                    << miningOrders << ",remaining=" << (*mineral)->getResources() << '\n';
                log.flush();
                if (miningOrders == 0) dynamicNavigationMineralDepleted = true;
                return;
            }
            if (dynamicNavigationMineralDepleted) {
                check("dynamic-navigation-engine-postcondition", failures == 0);
                log << "DONE," << frame << ',' << failures << '\n';
                log.flush();
                finished = true;
                BWAPI::Broodwar->leaveGame();
                return;
            }
            if (dynamicNavigationStartedFrame >= 0 &&
                frame - dynamicNavigationStartedFrame >= 1200) {
                check("dynamic-navigation-mineral-depletes-within-native-window", false);
                check("dynamic-navigation-engine-postcondition", failures == 0);
                log << "DONE," << frame << ',' << failures << '\n';
                log.flush();
                finished = true;
                BWAPI::Broodwar->leaveGame();
            }
            return;
        }
        if (scenario == "footprint-native") {
            constexpr std::array<BWAPI::UnitType, 4> types{
                BWAPI::UnitTypes::Protoss_Probe,
                BWAPI::UnitTypes::Protoss_Zealot,
                BWAPI::UnitTypes::Protoss_Dragoon,
                BWAPI::UnitTypes::Protoss_Reaver};
            constexpr std::array<const char*, 4> names{
                "probe", "zealot", "dragoon", "reaver"};
            constexpr std::array<int, 4> laneCenters{512, 1504, 2496, 3488};
            constexpr Position targetAnchor{0, 3584};
            const auto allUnits = BWAPI::Broodwar->getAllUnits();
            if (!footprintNativeChecked) {
                footprintNativeChecked = true;
                std::array<BWAPI::Unit, 4> actors{};
                std::array<Position, 4> origins{};
                std::array<MovementFootprint, 4> footprints{};
                for (std::size_t i = 0; i < types.size(); ++i) {
                    const auto found = std::ranges::find_if(
                        allUnits, [type = types[i]](const auto unit) {
                            return unit != nullptr && unit->exists() && unit->getType() == type;
                        });
                    if (found == allUnits.end()) continue;
                    actors[i] = *found;
                    footprintNativeActorIds[i] = actors[i]->getID();
                    const auto position = actors[i]->getPosition();
                    origins[i] = {position.x, position.y};
                    const auto type = actors[i]->getType();
                    footprints[i] = {type.dimensionLeft(), type.dimensionRight(),
                                     type.dimensionUp(), type.dimensionDown()};
                    footprintNativeFootprints[i] = footprints[i];
                    log << "FOOTPRINT_ACTOR," << names[i] << ",id=" << actors[i]->getID()
                        << ",position=" << position.x << 'x' << position.y
                        << ",dimensions=" << footprints[i].left << ':' << footprints[i].right
                        << ':' << footprints[i].up << ':' << footprints[i].down << '\n';
                }
                const auto completeFixture = std::ranges::all_of(
                    actors, [](const auto unit) { return unit != nullptr; });
                check("footprint-fixture-four-unit-types", completeFixture);
                const auto grid = bridge.navigationGrid();
                bool validStart = completeFixture && !grid.empty();
                if (validStart) {
                    for (std::size_t i = 0; i < types.size(); ++i) {
                        validStart = validStart && distanceSquared(
                            origins[i], Position{laneCenters[i], 512}) <= 32 * 32 &&
                            grid.walkable(origins[i], footprints[i]);
                    }
                }
                check("footprint-fixture-units-start-in-separate-lanes", validStart);
                if (!completeFixture || !validStart) {
                    check("footprint-native-engine-postcondition", false);
                    log << "DONE," << frame << ',' << failures << '\n';
                    log.flush();
                    finished = true;
                    BWAPI::Broodwar->leaveGame();
                    return;
                }

                struct BlockedFootprint {
                    int left{};
                    int top{};
                    int right{};
                    int bottom{};
                };
                std::vector<BlockedFootprint> blocked;
                for (const auto unit : allUnits) {
                    if (unit == nullptr || !unit->exists() ||
                        unit->getType() != BWAPI::UnitTypes::Protoss_Pylon) continue;
                    blocked.push_back({unit->getLeft(), unit->getTop(),
                                       unit->getRight() + 1, unit->getBottom() + 1});
                }
                log << "FOOTPRINT_STATIC_BLOCKERS," << blocked.size() << '\n';
                const auto cellSize = grid.cellSize();
                int wallTop{};
                int wallBottom{};
                int mostFrequentTopCount{};
                for (const auto& candidate : blocked) {
                    const auto count = std::ranges::count_if(
                        blocked, [top = candidate.top, bottom = candidate.bottom](const auto& item) {
                            return item.top == top && item.bottom == bottom;
                        });
                    if (count > mostFrequentTopCount) {
                        wallTop = candidate.top;
                        wallBottom = candidate.bottom;
                        mostFrequentTopCount = static_cast<int>(count);
                    }
                }
                footprintNativeWallTop = wallTop;
                footprintNativeWallBottom = wallBottom;
                const auto mapRight = grid.width() * cellSize;
                int gapStart{-1};
                for (auto x = 0; x < mapRight; ++x) {
                    const auto covered = std::ranges::any_of(
                        blocked, [x, wallTop, this](const auto& item) {
                            return item.top <= wallTop && item.bottom >= footprintNativeWallBottom &&
                                x >= item.left && x < item.right;
                        });
                    if (!covered && gapStart < 0) gapStart = x;
                    if (covered && gapStart >= 0) {
                        if (x - gapStart >= 8)
                            footprintNativeGaps.emplace_back(gapStart, x);
                        gapStart = -1;
                    }
                }
                if (gapStart >= 0 && mapRight - gapStart >= 8)
                    footprintNativeGaps.emplace_back(gapStart, mapRight);
                log << "FOOTPRINT_WALL_BAND," << wallTop << 'x'
                    << footprintNativeWallBottom << ",gaps=";
                for (const auto& [left, right] : footprintNativeGaps)
                    log << left << ':' << right << ';';
                log << '\n';
                for (const auto unit : allUnits) {
                    if (unit == nullptr || !unit->exists() ||
                        unit->getType() != BWAPI::UnitTypes::Protoss_Pylon) continue;
                    const auto position = unit->getPosition();
                    const auto tile = unit->getTilePosition();
                    const auto type = unit->getType();
                    log << "FOOTPRINT_PYLON," << position.x << 'x' << position.y
                        << ",tile=" << tile.x << 'x' << tile.y
                        << ",dimensions=" << type.dimensionLeft() << ':'
                        << type.dimensionRight() << ':' << type.dimensionUp() << ':'
                        << type.dimensionDown() << ",bounds=" << unit->getLeft() << ':'
                        << unit->getTop() << ':' << unit->getRight() << ':'
                        << unit->getBottom() << '\n';
                }
                std::vector<std::uint8_t> fixtureWalkable;
                fixtureWalkable.reserve(static_cast<std::size_t>(grid.width()) *
                                         static_cast<std::size_t>(grid.height()));
                for (auto cellY = 0; cellY < grid.height(); ++cellY) {
                    for (auto cellX = 0; cellX < grid.width(); ++cellX) {
                        const auto walkable = BWAPI::Broodwar->isWalkable(
                            BWAPI::WalkPosition(cellX, cellY));
                        const auto centerX = cellX * cellSize + cellSize / 2;
                        const auto centerY = cellY * cellSize + cellSize / 2;
                        const auto intersectsBlocker = std::ranges::any_of(
                            blocked, [centerX, centerY](const BlockedFootprint& item) {
                                return centerX >= item.left && centerX < item.right &&
                                    centerY >= item.top && centerY < item.bottom;
                            });
                        fixtureWalkable.push_back(static_cast<std::uint8_t>(
                            walkable && !intersectsBlocker));
                    }
                }
                const NavigationGrid fixtureGrid(grid.width(), grid.height(), cellSize,
                                                  std::move(fixtureWalkable));
                for (std::size_t i = 0; i < types.size(); ++i) {
                    const Position target{laneCenters[i], targetAnchor.y};
                    const auto path = fixtureGrid.findPath(
                        origins[i], target, fixtureGrid.width() * fixtureGrid.height(),
                        footprints[i]);
                    footprintNativePlanned[i] = path.reached();
                    const auto nativeRegionReachable = actors[i]->hasPath(
                        BWAPI::Position(target.x, target.y));
                    const auto accepted = actors[i]->move(BWAPI::Position(target.x, target.y));
                    log << "FOOTPRINT_MOVE," << names[i] << ",planner="
                        << footprintNativePlanned[i] << ",nativeRegionQuery="
                        << nativeRegionReachable << ",accepted=" << accepted << '\n';
                    check("footprint-native-move-command-accepted", accepted);
                }
                footprintNativeStartFrame = frame;
                log << "FOOTPRINT_MOVEMENT_STARTED," << frame << '\n';
                log.flush();
                return;
            }

            for (std::size_t i = 0; i < footprintNativeActorIds.size(); ++i) {
                const auto actor = BWAPI::Broodwar->getUnit(footprintNativeActorIds[i]);
                if (actor == nullptr || !actor->exists() || footprintNativeCrossed[i]) continue;
                const auto position = actor->getPosition();
                const auto footprintOverlapsWall =
                    position.y + footprintNativeFootprints[i].down >= footprintNativeWallTop &&
                    position.y - footprintNativeFootprints[i].up < footprintNativeWallBottom;
                if (i == 0 && footprintOverlapsWall) {
                    const auto fitsGap = std::ranges::any_of(
                        footprintNativeGaps, [&](const auto& gap) {
                            return position.x - footprintNativeFootprints[i].left >= gap.first &&
                                position.x + footprintNativeFootprints[i].right < gap.second;
                        });
                    log << "FOOTPRINT_PROBE_BAND," << frame << ",position="
                        << position.x << 'x' << position.y << ",fitsGap=" << fitsGap << '\n';
                }
                if (footprintOverlapsWall) {
                    for (const auto& [gapLeft, gapRight] : footprintNativeGaps) {
                        if (position.x - footprintNativeFootprints[i].left < gapLeft ||
                            position.x + footprintNativeFootprints[i].right >= gapRight) continue;
                        footprintNativeCrossed[i] = true;
                        log << "FOOTPRINT_CROSSED," << names[i] << ",frame=" << frame
                            << ",position=" << position.x << 'x' << position.y
                            << ",gap=" << gapLeft << ':' << gapRight << '\n';
                        break;
                    }
                }
                if (frame % 96 == 0)
                    log << "FOOTPRINT_PROGRESS," << names[i] << ",position="
                        << position.x << 'x' << position.y << ",crossed="
                        << footprintNativeCrossed[i] << '\n';
            }
            if (frame - footprintNativeStartFrame < 1200) return;

            int mismatches{};
            for (std::size_t i = 0; i < types.size(); ++i) {
                if (footprintNativePlanned[i] != footprintNativeCrossed[i]) ++mismatches;
                log << "FOOTPRINT_RESULT," << names[i] << ",planner="
                    << footprintNativePlanned[i] << ",crossed="
                    << footprintNativeCrossed[i] << '\n';
            }
            const auto narrowLane = footprintNativePlanned[0] && footprintNativePlanned[1] &&
                !footprintNativePlanned[2] && !footprintNativePlanned[3] &&
                footprintNativeCrossed[0] && footprintNativeCrossed[1] &&
                !footprintNativeCrossed[2] && !footprintNativeCrossed[3];
            log << "FOOTPRINT_MISMATCH_COUNT," << mismatches << '\n';
            log.flush();
            check("footprint-narrow-lanes-match-native-movement", narrowLane);
            check("footprint-planner-matches-native-movement", mismatches == 0);
            check("footprint-native-engine-postcondition", narrowLane && mismatches == 0);
            log << "DONE," << frame << ',' << failures << '\n';
            log.flush();
            finished = true;
            BWAPI::Broodwar->leaveGame();
            return;
        }
        if (scenario == "unit-memory") {
            const auto enemyUnits = BWAPI::Broodwar->enemy()->getUnits();
            const auto selfUnits = BWAPI::Broodwar->self()->getUnits();
            const auto pool = std::ranges::find_if(
                enemyUnits, [](const BWAPI::Unit unit) {
                    return unit != nullptr && unit->exists() &&
                        unit->getType() == BWAPI::UnitTypes::Zerg_Spawning_Pool;
                });
            const auto darkTemplar = std::ranges::find_if(
                enemyUnits, [](const BWAPI::Unit unit) {
                    return unit != nullptr && unit->exists() &&
                        unit->getType() == BWAPI::UnitTypes::Protoss_Dark_Templar;
                });
            if (!unitMemoryFootprintChecked && frame >= 24) {
                unitMemoryFootprintChecked = true;
                check("unit-memory-fixture-spawning-pool",
                      pool != enemyUnits.end());
                if (pool != enemyUnits.end()) {
                    const auto type = (*pool)->getType();
                    const auto origin = (*pool)->getTilePosition();
                    auto visibleTiles = std::vector<std::uint8_t>{};
                    visibleTiles.reserve(static_cast<std::size_t>(
                        type.tileWidth() * type.tileHeight()));
                    for (auto y = 0; y < type.tileHeight(); ++y) {
                        for (auto x = 0; x < type.tileWidth(); ++x) {
                            visibleTiles.push_back(static_cast<std::uint8_t>(
                                BWAPI::Broodwar->isVisible(
                                    BWAPI::TilePosition(origin.x + x, origin.y + y))));
                        }
                    }
                    const auto center = BWAPI::TilePosition(
                        origin.x + type.tileWidth() / 2,
                        origin.y + type.tileHeight() / 2);
                    const auto memory = std::ranges::find(
                        state.enemy.units, (*pool)->getID(), &UnitSnapshot::id);
                    log << "UNIT_MEMORY_POOL," << frame << ",id=" << (*pool)->getID()
                        << ",position=" << (*pool)->getPosition().x << 'x'
                        << (*pool)->getPosition().y << ",tile=" << origin.x << 'x'
                        << origin.y << ",size=" << type.tileWidth() << 'x'
                        << type.tileHeight() << ",centerVisible="
                        << BWAPI::Broodwar->isVisible(center) << ",allVisible="
                        << fullyVisibleFootprint(type.tileWidth(), type.tileHeight(),
                                                 visibleTiles) << ",tiles=";
                    for (const auto tile : visibleTiles) log << static_cast<int>(tile);
                    log << '\n';
                    check("unit-memory-center-visible-footprint-partial",
                          BWAPI::Broodwar->isVisible(center) &&
                              !fullyVisibleFootprint(type.tileWidth(), type.tileHeight(),
                                                     visibleTiles));
                    check("unit-memory-partial-building-retained",
                          memory != state.enemy.units.end() && memory->position.valid());
                }
                const auto observer = std::ranges::find_if(selfUnits, [](const BWAPI::Unit unit) {
                    return unit != nullptr && unit->exists() &&
                        unit->getType() == BWAPI::UnitTypes::Protoss_Observer;
                });
                log << "UNIT_MEMORY_DT_INITIAL," << frame << ",id="
                    << (darkTemplar != enemyUnits.end() ? (*darkTemplar)->getID() : -1)
                    << ",visible=" << (darkTemplar != enemyUnits.end() && (*darkTemplar)->isVisible())
                    << ",detected=" << (darkTemplar != enemyUnits.end() && (*darkTemplar)->isDetected())
                    << ",observer=" << (observer != selfUnits.end() ? (*observer)->getID() : -1)
                    << ",observerPosition=" << (observer != selfUnits.end() ? (*observer)->getPosition().x : -1)
                    << 'x' << (observer != selfUnits.end() ? (*observer)->getPosition().y : -1)
                    << ",observerCompleted=" << (observer != selfUnits.end() && (*observer)->isCompleted())
                    << '\n';
            }
            auto observedDarkTemplar = std::ranges::find(
                state.enemy.units, unitMemoryDarkTemplar, &UnitSnapshot::id);
            if (unitMemoryDarkTemplar < 0 && darkTemplar != enemyUnits.end() &&
                (*darkTemplar)->isVisible() && (*darkTemplar)->isDetected()) {
                unitMemoryDarkTemplar = (*darkTemplar)->getID();
                unitMemoryLastSeen = frame;
                const auto observer = std::ranges::find_if(
                    selfUnits, [](const BWAPI::Unit unit) {
                        return unit != nullptr && unit->exists() &&
                            unit->getType() == BWAPI::UnitTypes::Protoss_Observer;
                    });
                if (observer != selfUnits.end()) {
                    unitMemoryObserver = (*observer)->getID();
                    unitMemoryMoveIssued = (*observer)->move(BWAPI::Position(1600, 1600));
                }
                observedDarkTemplar = std::ranges::find(
                    state.enemy.units, unitMemoryDarkTemplar, &UnitSnapshot::id);
                check("unit-memory-dt-detected-before-move",
                      observedDarkTemplar != state.enemy.units.end() &&
                          observedDarkTemplar->visible && observedDarkTemplar->detected);
                check("unit-memory-observer-moves-to-drop-detection",
                      unitMemoryMoveIssued);
            }
            if (unitMemoryDarkTemplar >= 0 && !unitMemoryMoveIssued &&
                unitMemoryObserver >= 0) {
                const auto observer = BWAPI::Broodwar->getUnit(unitMemoryObserver);
                unitMemoryMoveIssued = observer != nullptr && observer->exists() &&
                    observer->move(BWAPI::Position(1600, 1600));
            }
            if (observedDarkTemplar != state.enemy.units.end() &&
                observedDarkTemplar->visible && observedDarkTemplar->detected)
                unitMemoryLastSeen = observedDarkTemplar->lastSeen;
            if (frame >= 900) {
                const auto raw = BWAPI::Broodwar->getUnit(unitMemoryDarkTemplar);
                const auto memory = std::ranges::find(
                    state.enemy.units, unitMemoryDarkTemplar, &UnitSnapshot::id);
                const auto droppedDetection = raw != nullptr && raw->exists() &&
                    raw->isVisible() && !raw->isDetected() &&
                    memory != state.enemy.units.end() && !memory->visible &&
                    !memory->detected && memory->lastSeen == unitMemoryLastSeen;
                check("unit-memory-detection-loss-keeps-last-legal-sample",
                      droppedDetection);
                if (memory != state.enemy.units.end()) {
                    log << "UNIT_MEMORY," << frame << ",dt=" << memory->id
                        << ",lastSeen=" << memory->lastSeen
                        << ",existsConfidence=" << memory->existenceConfidence
                        << ",locationConfidence=" << memory->locationConfidence
                        << ",healthConfidence=" << memory->healthConfidence << '\n';
                }
                check("unit-memory-engine-postcondition", droppedDetection);
                log << "DONE," << frame << ',' << failures << '\n';
                log.flush();
                finished = true;
                BWAPI::Broodwar->leaveGame();
            }
            return;
        }
        if (scenario == "learned-cancel-build") {
            constexpr int frameLimit = 600;
            if (!t020Started && frame >= 24) {
                t020Started = true;
                const auto probes = BWAPI::Broodwar->self()->getUnits();
                const auto probe = std::ranges::find_if(probes, [](const BWAPI::Unit unit) {
                    return unit != nullptr && unit->exists() && unit->getType().isWorker() &&
                        unit->isCompleted();
                });
                check("T020-fixture-completed-builder", probe != probes.end());
                BWAPI::TilePosition site{-1, -1};
                if (probe != probes.end()) {
                    const auto origin = (*probe)->getTilePosition();
                    for (auto radius = 2; radius <= 16 && !site.isValid(); ++radius) {
                        for (auto dy = -radius; dy <= radius && !site.isValid(); ++dy) {
                            for (auto dx = -radius; dx <= radius; ++dx) {
                                const BWAPI::TilePosition candidate(origin.x + dx, origin.y + dy);
                                if (BWAPI::Broodwar->canBuildHere(
                                        candidate, BWAPI::UnitTypes::Protoss_Pylon, *probe)) {
                                    site = candidate;
                                    break;
                                }
                            }
                        }
                    }
                }
                const auto siteFound = site.isValid();
                check("T020-fixture-legal-Pylon-site", siteFound);
                if (!siteFound || probe == probes.end()) {
                    check("T020-stock-engine-postcondition", false);
                    log << "DONE," << frame << ',' << failures << '\n';
                    log.flush();
                    finished = true;
                    BWAPI::Broodwar->leaveGame();
                    return;
                }
                t020Builder = (*probe)->getID();
                t020Site = site;
                const auto accepted = (*probe)->issueCommand(BWAPI::UnitCommand::build(
                    *probe, t020Site, BWAPI::UnitTypes::Protoss_Pylon));
                t020ConstructionStarted = accepted;
                log << "T020_BUILD," << frame << ",builder=" << t020Builder
                    << ",tile=" << t020Site.x << 'x' << t020Site.y
                    << ",accepted=" << accepted << '\n';
                log.flush();
                check("T020-stock-engine-started-Pylon", accepted);
                return;
            }

            const auto selfUnits = BWAPI::Broodwar->self()->getUnits();
            const auto building = std::ranges::find_if(selfUnits, [this](const BWAPI::Unit unit) {
                return unit != nullptr && unit->exists() &&
                    unit->getType() == BWAPI::UnitTypes::Protoss_Pylon &&
                    unit->getTilePosition() == t020Site && unit->isBeingConstructed();
            });
            if (!t020LegalityChecked && t020ConstructionStarted && building != selfUnits.end()) {
                t020LegalityChecked = true;
                const auto actor = *building;
                t020Building = actor->getID();
                const auto token = actor->getID();
                whole_observation::Snapshot observation;
                auto& entity = observation.entities[token];
                entity.id = token;
                entity.relation = 0;
                entity.visible = true;
                entity.completed = actor->isCompleted();
                entity.building = actor->getType().isBuilding();
                const std::map<int, BWAPI::Unit> unitByToken{{token, actor}};
                cpu::Intent cancel;
                cancel.kind = 18;
                cancel.targetMode = 0;
                cancel.actorIds = {token};
                const auto legalCancel = protodd::bwapi::legalWholeGameCommands(
                    cancel, observation, unitByToken, BWAPI::BroodwarPtr);
                check("T020-unfinished-building-cancel-is-legal",
                    legalCancel.size() == 1 && legalCancel.front().actorToken == token &&
                    legalCancel.front().command.getType() ==
                        BWAPI::UnitCommandTypes::Cancel_Construction);

                auto move = cancel;
                move.kind = 1;
                move.targetMode = 2;
                move.targetPixel = std::pair{actor->getPosition().x + 32,
                                             actor->getPosition().y + 32};
                check("T020-unfinished-building-move-rejected",
                    protodd::bwapi::legalWholeGameCommands(
                        move, observation, unitByToken, BWAPI::BroodwarPtr).empty());
                auto train = cancel;
                train.kind = 13;
                train.targetMode = 0;
                train.unitType = BWAPI::UnitTypes::Protoss_Zealot.getID();
                check("T020-unfinished-building-train-rejected",
                    protodd::bwapi::legalWholeGameCommands(
                        train, observation, unitByToken, BWAPI::BroodwarPtr).empty());

                const auto completedBuilding = std::ranges::find_if(
                    selfUnits, [](const BWAPI::Unit unit) {
                        return unit != nullptr && unit->exists() && unit->isCompleted() &&
                            unit->getType().isBuilding();
                    });
                check("T020-fixture-completed-building", completedBuilding != selfUnits.end());
                if (completedBuilding != selfUnits.end()) {
                    const auto completedActor = *completedBuilding;
                    const auto completedToken = completedActor->getID();
                    whole_observation::Snapshot completedObservation;
                    auto& completedEntity = completedObservation.entities[completedToken];
                    completedEntity.id = completedToken;
                    completedEntity.relation = 0;
                    completedEntity.visible = true;
                    completedEntity.completed = true;
                    completedEntity.building = true;
                    const std::map<int, BWAPI::Unit> completedByToken{
                        {completedToken, completedActor}};
                    auto cancelCompleted = cancel;
                    cancelCompleted.actorIds = {completedToken};
                    check("T020-idle-building-cancel-rejected",
                        protodd::bwapi::legalWholeGameCommands(cancelCompleted,
                            completedObservation, completedByToken,
                            BWAPI::BroodwarPtr).empty());
                }

                const auto accepted = legalCancel.size() == 1 &&
                    legalCancel.front().command.getType() ==
                        BWAPI::UnitCommandTypes::Cancel_Construction &&
                    actor->issueCommand(legalCancel.front().command);
                t020CancelIssued = accepted;
                t020CancelFrame = frame;
                log << "T020_CANCEL," << frame << ",building=" << t020Building
                    << ",accepted=" << accepted << ",legalCount=" << legalCancel.size()
                    << '\n';
                log.flush();
                check("T020-stock-engine-accepted-learned-cancel", accepted);
                return;
            }
            if (t020CancelIssued) {
                const auto cancelled = BWAPI::Broodwar->getUnit(t020Building);
                const auto gone = cancelled == nullptr || !cancelled->exists();
                const auto stopped = gone || !cancelled->isBeingConstructed();
                if (!stopped && frame < t020CancelFrame + 48) return;
                log << "T020_CANCEL_STATE," << frame << ",exists=" << !gone
                    << ",constructing=" << (!gone && cancelled->isBeingConstructed())
                    << ",order=" << (gone ? "none" : cancelled->getOrder().toString())
                    << '\n';
                check("T020-stock-engine-construction-cancelled", stopped);
                check("T020-stock-engine-postcondition", failures == 0);
                log << "T020_RESULT," << frame << ",building=" << t020Building
                    << ",gone=" << gone << ",stopped=" << stopped << '\n';
                log << "DONE," << frame << ',' << failures << '\n';
                log.flush();
                finished = true;
                BWAPI::Broodwar->leaveGame();
                return;
            }
            if (frame >= frameLimit) {
                check("T020-stock-engine-postcondition", false);
                log << "DONE," << frame << ',' << failures << '\n';
                log.flush();
                finished = true;
                BWAPI::Broodwar->leaveGame();
            }
            return;
        }
        if (scenario == "build-cancel") {
            constexpr int frameLimit = 240;
            constexpr Position fixtureCenter{2050, 2000};
            const auto site = std::ranges::min_element(state.bases,
                [&](const BaseSnapshot& left, const BaseSnapshot& right) {
                    const auto leftDistance = distanceSquared(left.center, fixtureCenter);
                    const auto rightDistance = distanceSquared(right.center, fixtureCenter);
                    return leftDistance < rightDistance;
                });
            const auto usableSite = site != state.bases.end() &&
                distanceSquared(site->center, fixtureCenter) <= 600 * 600 &&
                site->mineralPatches >= 4 && site->ownerId < 0 && !site->island;
            const auto builderReserved = [this] {
                const auto reserved = bridge.reservedBuilders();
                return std::ranges::find(reserved, t021Builder) != reserved.end();
            };
            const auto probe = std::ranges::find_if(state.self.units, [](const auto& unit) {
                return unit.kind == UnitKind::probe && unit.completed;
            });
            if (!t021Started) {
                t021Started = true;
                check("T021-fixture-remote-mineral-site", usableSite);
                check("T021-fixture-completed-probe", probe != state.self.units.end());
                if (!usableSite || probe == state.self.units.end()) {
                    check("T021-engine-postcondition", false);
                    log << "DONE," << frame << ',' << failures << '\n';
                    log.flush();
                    finished = true;
                    BWAPI::Broodwar->leaveGame();
                    return;
                }

                t021Builder = probe->id;
                t021Target = site->center;
                t021MineralsBefore = BWAPI::Broodwar->self()->minerals();
                MacroAction action{};
                action.action = MacroActionKind::expand;
                action.target = UnitKind::nexus;
                action.priority = 130;
                action.minerals = 400;
                action.reserved = true;
                action.reason = "T021 pending expansion";
                action.constructionSite = {0x21, site->id, site->center};
                StrategicPlan plan;
                plan.desiredBases = 2;
                plan.expansionTarget = site->center;
                bridge.actionDiagnostic = [this](const protodd::bwapi::ActionDiagnostic& d) {
                    if (d.source == "T021 pending expansion" && d.type == "Build" &&
                        d.accepted) ++t021BuildCommands;
                    if (d.source.starts_with("expansion-cancel") &&
                        d.type == "Stop" && d.accepted)
                        ++t021AcceptedStopCommands;
                    if (d.source == "T021 pending expansion" ||
                        d.source == "expansion-cancel") {
                        log << "T021_ACTION," << BWAPI::Broodwar->getFrameCount() << ','
                            << d.actor << ',' << d.type << ',' << d.source << ','
                            << d.outcome << ',' << d.attempted << ',' << d.accepted << '\n';
                        log.flush();
                    }
                };
                bridge.productionPermission = [this](const BWAPI::UnitCommand& command,
                                                       const std::string_view source) {
                    if (!t021StopRejected && source == "expansion-cancel" &&
                        command.getType() == BWAPI::UnitCommandTypes::Stop) {
                        t021StopRejected = true;
                        log << "T021_INJECTED_REJECTION," << BWAPI::Broodwar->getFrameCount()
                            << ",actor=" << command.getUnit()->getID() << '\n';
                        log.flush();
                        return false;
                    }
                    return true;
                };
                const auto issued = bridge.executeMacro(std::span{&action, 1}, plan, influence);
                const auto builder = BWAPI::Broodwar->getUnit(t021Builder);
                const auto last = builder != nullptr ? builder->getLastCommand()
                                                     : BWAPI::UnitCommand{};
                const auto buildOrder = builder != nullptr && builder->exists() &&
                    (builder->getBuildType() == BWAPI::UnitTypes::Protoss_Nexus ||
                     (last.getType() == BWAPI::UnitCommandTypes::Build &&
                      last.getUnitType() == BWAPI::UnitTypes::Protoss_Nexus));
                check("T021-stock-engine-accepted-remote-Nexus-order",
                    issued == 1 && t021BuildCommands == 1 && buildOrder &&
                    builder->getDistance(BWAPI::Position(site->center.x,
                                                         site->center.y)) > 96);
                check("T021-builder-owned-before-cancellation",
                    builderReserved());
                const auto stopped = bridge.cancelExpansion();
                const auto stillReserved = builderReserved();
                check("T021-rejected-Stop-retains-build-lease",
                    t021StopRejected && !stopped && stillReserved && buildOrder &&
                    t021AcceptedStopCommands == 0);

                const auto repeat = bridge.executeMacro(std::span{&action, 1}, plan, influence);
                check("T021-replacement-build-blocked-after-rejection",
                    repeat == 0 && t021BuildCommands == 1 && builderReserved());
                log << "T021_PENDING," << frame << ",builder=" << t021Builder
                    << ",site=" << t021Target.x << 'x' << t021Target.y
                    << ",minerals=" << t021MineralsBefore << ",last="
                    << last.getType().toString() << ",buildCommands="
                    << t021BuildCommands << '\n';
                log.flush();
            } else if (!t021StopAccepted) {
                const auto stopped = bridge.cancelExpansion();
                const auto stillReserved = builderReserved();
                if (t021AcceptedStopCommands > 0) {
                    t021StopAccepted = true;
                    check("T021-retry-Stop-accepted", !stopped && stillReserved &&
                        t021StopRejected && t021AcceptedStopCommands >= 1);
                    const auto actionSite = ConstructionTaskSite{0x21, site->id, t021Target};
                    MacroAction action{};
                    action.action = MacroActionKind::expand;
                    action.target = UnitKind::nexus;
                    action.priority = 130;
                    action.minerals = 400;
                    action.reserved = true;
                    action.reason = "T021 pending expansion";
                    action.constructionSite = actionSite;
                    StrategicPlan plan;
                    plan.desiredBases = 2;
                    plan.expansionTarget = t021Target;
                    const auto retry = bridge.executeMacro(std::span{&action, 1}, plan, influence);
                    check("T021-awaiting-ack-blocks-replacement",
                        retry == 0 && bridge.lastMacroStatus() == "build-cancel-awaiting-ack" &&
                        t021BuildCommands == 1 && builderReserved());
                } else {
                    check("T021-builder-retained-through-delayed-Stop", !stopped && stillReserved);
                    const auto builder = BWAPI::Broodwar->getUnit(t021Builder);
                    const auto last = builder != nullptr ? builder->getLastCommand()
                                                         : BWAPI::UnitCommand{};
                    check("T021-old-build-order-still-actionable",
                        builder != nullptr && builder->exists() &&
                        (builder->getBuildType() == BWAPI::UnitTypes::Protoss_Nexus ||
                         (last.getType() == BWAPI::UnitCommandTypes::Build &&
                          last.getUnitType() == BWAPI::UnitTypes::Protoss_Nexus)));
                }
            } else {
                const auto builder = BWAPI::Broodwar->getUnit(t021Builder);
                const auto oldOrderActionable = builder != nullptr && builder->exists() &&
                    (builder->getBuildType() == BWAPI::UnitTypes::Protoss_Nexus ||
                     (builder->getLastCommand().getType() == BWAPI::UnitCommandTypes::Build &&
                      builder->getLastCommand().getUnitType() == BWAPI::UnitTypes::Protoss_Nexus));
                const auto reservedBefore = builderReserved();
                const auto acknowledged = bridge.cancelExpansion();
                const auto reservedAfter = builderReserved();
                if (oldOrderActionable) {
                    check("T021-live-order-keeps-builder-lease", !acknowledged &&
                        reservedBefore && reservedAfter);
                } else {
                    const auto nexusCount = std::ranges::count_if(state.self.units,
                        [](const auto& unit) { return unit.kind == UnitKind::nexus; });
                    log << "T021_RELEASE," << frame << ",reservedBefore=" << reservedBefore
                        << ",acknowledged=" << acknowledged
                        << ",reservedAfter=" << reservedAfter << '\n';
                    log.flush();
                    check("T021-lease-released-after-old-order-cleared",
                        acknowledged && !reservedAfter);
                    check("T021-no-replacement-Nexus-or-double-spend",
                        nexusCount == 1 && t021BuildCommands == 1 &&
                        BWAPI::Broodwar->self()->minerals() == t021MineralsBefore);
                    check("T021-stock-engine-postcondition", failures == 0);
                    log << "T021_RESULT," << frame << ",rejected=1,acceptedRetries="
                        << t021AcceptedStopCommands << ",buildCommands=" << t021BuildCommands
                        << ",minerals=" << BWAPI::Broodwar->self()->minerals() << '\n';
                    log << "DONE," << frame << ',' << failures << '\n';
                    log.flush();
                    t021Finished = true;
                    finished = true;
                    BWAPI::Broodwar->leaveGame();
                    return;
                }
            }
            if (frame >= frameLimit && !t021Finished) {
                check("T021-rejected-Stop-was-retried", t021StopRejected);
                check("T021-retry-Stop-eventually-accepted", t021StopAccepted);
                check("T021-stock-engine-postcondition", false);
                log << "DONE," << frame << ',' << failures << '\n';
                log.flush();
                finished = true;
                BWAPI::Broodwar->leaveGame();
            }
            return;
        }
        if (scenario == "builder-evacuation-started") {
            constexpr int frameLimit = 600;
            const auto reserved = [this] {
                const auto builders = bridge.reservedBuilders();
                return std::ranges::find(builders, t034Builder) != builders.end();
            };
            bridge.actionDiagnostic = [this](const protodd::bwapi::ActionDiagnostic& d) {
                if (d.actor == t034Builder && d.type == "Build" && d.accepted &&
                    d.source == "T034 started construction") ++t034BuildCommands;
                if (d.actor == t034Builder && d.type == "Stop" && d.accepted &&
                    d.source.starts_with("worker-evacuation")) ++t034AcceptedStops;
                if (d.actor == t034Builder && d.type == "Move" && d.accepted &&
                    d.source == "worker-evacuate") ++t034AcceptedMoves;
            };

            const auto startedPylon = [this]() -> BWAPI::Unit {
                for (const auto unit : BWAPI::Broodwar->self()->getUnits()) {
                    if (unit != nullptr && unit->exists() &&
                        unit->getType() == BWAPI::UnitTypes::Protoss_Pylon &&
                        unit->getTilePosition() == t034Site) {
                        return unit;
                    }
                }
                return nullptr;
            };

            if (!t034Started) {
                t034Started = true;
                const auto nexus = std::ranges::find_if(state.self.units,
                    [](const UnitSnapshot& unit) {
                        return unit.kind == UnitKind::nexus && unit.completed;
                    });
                const auto probe = std::ranges::min_element(state.self.units,
                    [&](const UnitSnapshot& left, const UnitSnapshot& right) {
                        const auto leftDistance = left.kind == UnitKind::probe && left.completed &&
                                nexus != state.self.units.end()
                            ? distanceSquared(left.position, nexus->position)
                            : std::numeric_limits<double>::max();
                        const auto rightDistance = right.kind == UnitKind::probe && right.completed &&
                                nexus != state.self.units.end()
                            ? distanceSquared(right.position, nexus->position)
                            : std::numeric_limits<double>::max();
                        return leftDistance < rightDistance;
                    });
                auto buildTile = BWAPI::TilePosition{-1, -1};
                if (nexus != state.self.units.end() && probe != state.self.units.end() &&
                    probe->kind == UnitKind::probe && probe->completed) {
                    const auto nexusUnit = BWAPI::Broodwar->getUnit(nexus->id);
                    const auto probeUnit = BWAPI::Broodwar->getUnit(probe->id);
                    if (nexusUnit != nullptr && probeUnit != nullptr) {
                        const auto homeTile = nexusUnit->getTilePosition();
                        for (int radius = 3; radius <= 12 && buildTile.x < 0; ++radius) {
                            for (int dx = -radius; dx <= radius && buildTile.x < 0; ++dx) {
                                for (int dy = -radius; dy <= radius && buildTile.x < 0; ++dy) {
                                    const auto absDx = dx < 0 ? -dx : dx;
                                    const auto absDy = dy < 0 ? -dy : dy;
                                    if (std::max(absDx, absDy) != radius) continue;
                                    const BWAPI::TilePosition candidate{
                                        homeTile.x + dx, homeTile.y + dy};
                                    if (BWAPI::Broodwar->canBuildHere(candidate,
                                            BWAPI::UnitTypes::Protoss_Pylon, probeUnit, true)) {
                                        buildTile = candidate;
                                    }
                                }
                            }
                        }
                    }
                }
                check("T034-started-fixture-home-and-builder",
                    nexus != state.self.units.end() && probe != state.self.units.end() &&
                    probe->kind == UnitKind::probe && probe->completed);
                check("T034-started-fixture-legal-local-Pylon-site", buildTile.x >= 0);
                if (nexus == state.self.units.end() || probe == state.self.units.end() ||
                    probe->kind != UnitKind::probe || !probe->completed || buildTile.x < 0) {
                    check("T034-started-stock-engine-postcondition", false);
                    log << "DONE," << frame << ',' << failures << '\n';
                    log.flush();
                    finished = true;
                    BWAPI::Broodwar->leaveGame();
                    return;
                }

                t034Builder = probe->id;
                t034Site = buildTile;
                const auto target = BWAPI::Position(buildTile);
                MacroAction action{};
                action.action = MacroActionKind::build;
                action.target = UnitKind::pylon;
                action.priority = 130;
                action.minerals = 100;
                action.reserved = true;
                action.reason = "T034 started construction";
                action.constructionSite = {0x340, static_cast<int>(nexus->id),
                                            Position{target.x, target.y}};
                StrategicPlan plan;
                plan.desiredBases = 1;
                const auto issued = bridge.executeMacro(std::span{&action, 1}, plan, influence);
                const auto builder = BWAPI::Broodwar->getUnit(t034Builder);
                const auto last = builder != nullptr ? builder->getLastCommand()
                                                     : BWAPI::UnitCommand{};
                const auto buildOrder = builder != nullptr && builder->exists() &&
                    (builder->getBuildType() == BWAPI::UnitTypes::Protoss_Pylon ||
                     (last.getType() == BWAPI::UnitCommandTypes::Build &&
                      last.getUnitType() == BWAPI::UnitTypes::Protoss_Pylon));
                check("T034-started-build-command-accepted",
                    issued == 1 && t034BuildCommands == 1 && buildOrder && reserved());
                log << "T034_STARTED_BUILD," << frame << ",builder=" << t034Builder
                    << ",tile=" << t034Site.x << 'x' << t034Site.y
                    << ",accepted=" << (issued == 1) << '\n';
                log.flush();
            } else {
                const auto pylon = startedPylon();
                if (pylon != nullptr) {
                    const auto builder = BWAPI::Broodwar->getUnit(t034Builder);
                    const auto wasReserved = reserved();
                    check("T034-started-structure-observed", pylon != nullptr);
                    check("T034-started-construction-releases-builder-lease", !wasReserved);
                    check("T034-started-structure-still-incomplete", !pylon->isCompleted());
                    const auto threatenedProbe = std::ranges::find(
                        state.self.units, t034Builder, &UnitSnapshot::id);
                    if (threatenedProbe != state.self.units.end())
                        threatenedProbe->underAttack = true;
                    const auto navigation = bridge.navigationGrid();
                    const auto currentLeases = bridge.reservedBuilders();
                    const auto assignments = t034Workers.assign(
                        state, {}, influence, currentLeases, false, false, &navigation);
                    const auto assignment = std::ranges::find(
                        assignments, t034Builder, &WorkerAssignment::worker);
                    check("T034-started-builder-can-evacuate",
                        assignment != assignments.end() &&
                        assignment->job == WorkerJob::evacuate);
                    const auto movesBefore = t034AcceptedMoves;
                    const auto stopsBefore = t034AcceptedStops;
                    CommandBus workerCommands;
                    workerCommands.beginFrame(BWAPI::Broodwar->getFrameCount(),
                                               BWAPI::Broodwar->getLatencyFrames());
                    const auto proposedWorkerCommands =
                        bridge.submitWorkerCommands(assignments, workerCommands);
                    auto issuedWorkerCommands = workerCommands.finalize(proposedWorkerCommands);
                    const auto workerAssignment = std::ranges::find(
                        assignments, t034Builder, &WorkerAssignment::worker);
                    log << "T034_STARTED_DIAG," << frame
                        << ",constructing=" << (builder != nullptr && builder->isConstructing())
                        << ",idle=" << (builder != nullptr && builder->isIdle())
                        << ",lastCommandFrame=" << (builder != nullptr ? builder->getLastCommandFrame() : -1)
                        << ",position=" << (builder != nullptr ? builder->getPosition().x : -1)
                        << 'x' << (builder != nullptr ? builder->getPosition().y : -1)
                        << ",job=" << (workerAssignment != assignments.end()
                            ? static_cast<int>(workerAssignment->job) : -1)
                        << ",target=" << (workerAssignment != assignments.end()
                            ? workerAssignment->targetPosition.x : -1)
                        << 'x' << (workerAssignment != assignments.end()
                            ? workerAssignment->targetPosition.y : -1)
                        << ",proposed=" << proposedWorkerCommands
                        << ",selected=" << issuedWorkerCommands.size() << '\n';
                    for (const auto& command : issuedWorkerCommands) {
                        const auto accepted = bridge.execute(command);
                        log << "T034_STARTED_COMMAND," << frame
                            << ",type=" << static_cast<int>(command.type)
                            << ",target=" << command.targetPosition.x << 'x'
                            << command.targetPosition.y << ",accepted=" << accepted << '\n';
                        if (accepted) workerCommands.markIssued(command);
                    }
                    log.flush();
                    const auto last = builder != nullptr ? builder->getLastCommand()
                                                         : BWAPI::UnitCommand{};
                    check("T034-started-builder-gets-move-not-stop",
                        t034AcceptedMoves > movesBefore &&
                        t034AcceptedStops == stopsBefore &&
                        last.getType() == BWAPI::UnitCommandTypes::Move);
                    check("T034-started-structure-is-not-cancelled-or-duplicated",
                        pylon->exists() && !pylon->isCompleted() &&
                        startedPylon() != nullptr &&
                        t034BuildCommands == 1);
                    check("T034-started-stock-engine-postcondition", failures == 0);
                    log << "T034_STARTED_RESULT," << frame << ",builder=" << t034Builder
                        << ",acceptedMoves=" << t034AcceptedMoves
                        << ",acceptedStops=" << t034AcceptedStops << '\n';
                    log << "DONE," << frame << ',' << failures << '\n';
                    log.flush();
                    t034Finished = true;
                    finished = true;
                    BWAPI::Broodwar->leaveGame();
                    return;
                }
                if (frame >= frameLimit) {
                    check("T034-started-construction-reached-live-structure", false);
                    check("T034-started-stock-engine-postcondition", false);
                    log << "DONE," << frame << ',' << failures << '\n';
                    log.flush();
                    finished = true;
                    BWAPI::Broodwar->leaveGame();
                }
            }
            return;
        }
        if (scenario == "builder-evacuation") {
            constexpr int frameLimit = 240;
            constexpr Position fixtureCenter{2050, 2000};
            const auto site = std::ranges::min_element(state.bases,
                [&](const BaseSnapshot& left, const BaseSnapshot& right) {
                    return distanceSquared(left.center, fixtureCenter) <
                           distanceSquared(right.center, fixtureCenter);
                });
            const auto usableSite = site != state.bases.end() &&
                distanceSquared(site->center, fixtureCenter) <= 600 * 600 &&
                site->mineralPatches >= 4 && site->ownerId < 0 && !site->island;
            const auto reserved = [this] {
                const auto builders = bridge.reservedBuilders();
                return std::ranges::find(builders, t034Builder) != builders.end();
            };
            bridge.actionDiagnostic = [this](const protodd::bwapi::ActionDiagnostic& d) {
                if (d.actor == t034Builder && d.type == "Build" && d.accepted &&
                    d.source == "T034 pending expansion") ++t034BuildCommands;
                if (d.actor == t034Builder && d.type == "Stop" && d.accepted &&
                    d.source.starts_with("worker-evacuation")) ++t034AcceptedStops;
                if (d.actor == t034Builder && d.type == "Move" && d.accepted &&
                    d.source == "worker-evacuate") ++t034AcceptedMoves;
                if (d.actor == t034Builder && d.source.starts_with("worker-evacuation")) {
                    log << "T034_ACTION," << BWAPI::Broodwar->getFrameCount() << ','
                        << d.actor << ',' << d.type << ',' << d.source << ','
                        << d.outcome << ',' << d.accepted << '\n';
                    log.flush();
                }
            };

            if (!t034Started) {
                t034Started = true;
                const auto probe = std::ranges::find_if(state.self.units, [](const auto& unit) {
                    return unit.kind == UnitKind::probe && unit.completed;
                });
                check("T034-fixture-remote-mineral-site", usableSite);
                check("T034-fixture-completed-probe", probe != state.self.units.end());
                if (!usableSite || probe == state.self.units.end()) {
                    check("T034-stock-engine-postcondition", false);
                    log << "DONE," << frame << ',' << failures << '\n';
                    log.flush();
                    finished = true;
                    BWAPI::Broodwar->leaveGame();
                    return;
                }

                t034Builder = probe->id;
                const auto target = site->center;
                MacroAction action{};
                action.action = MacroActionKind::expand;
                action.target = UnitKind::nexus;
                action.priority = 130;
                action.minerals = 400;
                action.reserved = true;
                action.reason = "T034 pending expansion";
                action.constructionSite = {0x34, site->id, target};
                StrategicPlan plan;
                plan.desiredBases = 2;
                plan.expansionTarget = target;
                const auto issued = bridge.executeMacro(std::span{&action, 1}, plan, influence);
                const auto builder = BWAPI::Broodwar->getUnit(t034Builder);
                const auto last = builder != nullptr ? builder->getLastCommand()
                                                     : BWAPI::UnitCommand{};
                const auto buildOrder = builder != nullptr && builder->exists() &&
                    (builder->getBuildType() == BWAPI::UnitTypes::Protoss_Nexus ||
                     (last.getType() == BWAPI::UnitCommandTypes::Build &&
                      last.getUnitType() == BWAPI::UnitTypes::Protoss_Nexus));
                check("T034-stock-engine-accepted-travelling-build",
                    issued == 1 && t034BuildCommands == 1 && buildOrder &&
                    builder->getDistance(BWAPI::Position(target.x, target.y)) > 96);
                check("T034-builder-starts-leased", reserved());

                const auto navigation = bridge.navigationGrid();
                const UnitId builderId[]{t034Builder};
                const auto safeAssignments = t034Workers.assign(
                    state, {}, influence, builderId, false, false, &navigation);
                const auto safe = std::ranges::find(safeAssignments, t034Builder,
                                                    &WorkerAssignment::worker);
                check("T034-safe-builder-keeps-build-task",
                      safe != safeAssignments.end() && safe->job == WorkerJob::build);
                const auto threatenedProbe = std::ranges::find(
                    state.self.units, t034Builder, &UnitSnapshot::id);
                if (threatenedProbe != state.self.units.end()) threatenedProbe->underAttack = true;
                const auto urgentAssignments = t034Workers.assign(
                    state, {}, influence, builderId, false, false, &navigation);
                const auto urgent = std::ranges::find(urgentAssignments, t034Builder,
                                                      &WorkerAssignment::worker);
                check("T034-danger-overrides-build-lease",
                      urgent != urgentAssignments.end() && urgent->job == WorkerJob::evacuate);
                executeWorkerProposals(urgentAssignments);
                // The Build already claimed this actor in the current BWAPI
                // frame, so the first emergency Stop may be deferred. Keep
                // ownership while the cancellation retry is pending.
                check("T034-Stop-request-retains-builder-lease", reserved());
                const auto replacement = bridge.executeMacro(
                    std::span{&action, 1}, plan, influence);
                t034ReplacementChecked = true;
                check("T034-awaiting-Stop-blocks-replacement",
                      replacement == 0 && t034BuildCommands == 1 && reserved());
            } else {
                const auto builder = BWAPI::Broodwar->getUnit(t034Builder);
                if (builder == nullptr || !builder->exists()) {
                    check("T034-builder-survives-cancellation", false);
                    check("T034-stock-engine-postcondition", false);
                    log << "DONE," << frame << ',' << failures << '\n';
                    log.flush();
                    finished = true;
                    BWAPI::Broodwar->leaveGame();
                    return;
                }
                const auto wasReserved = reserved();
                const UnitId builderId[]{t034Builder};
                const auto threatenedProbe = std::ranges::find(
                    state.self.units, t034Builder, &UnitSnapshot::id);
                if (threatenedProbe != state.self.units.end()) threatenedProbe->underAttack = true;
                const auto navigation = bridge.navigationGrid();
                const auto assignments = t034Workers.assign(
                    state, {}, influence, builderId, false, false, &navigation);
                const auto builderAssignment = std::ranges::find(
                    assignments, t034Builder, &WorkerAssignment::worker);
                if (wasReserved) {
                    check("T034-lease-held-until-old-order-clears",
                          builderAssignment != assignments.end() &&
                          builderAssignment->job == WorkerJob::evacuate);
                    executeWorkerProposals(assignments);
                } else {
                    const auto moveCount = t034AcceptedMoves;
                    executeWorkerProposals(assignments);
                    const auto last = builder->getLastCommand();
                    if (t034AcceptedMoves > moveCount &&
                        last.getType() == BWAPI::UnitCommandTypes::Move) {
                        const auto nexusCount = std::ranges::count_if(state.self.units,
                            [](const auto& unit) { return unit.kind == UnitKind::nexus; });
                        check("T034-lease-released-after-cancel-ack",
                              builderAssignment != assignments.end() &&
                              builderAssignment->job == WorkerJob::evacuate);
                        check("T034-builder-gets-escape-order",
                              t034AcceptedMoves > 0 &&
                              last.getType() == BWAPI::UnitCommandTypes::Move);
                        check("T034-no-started-structure-or-duplicate-Nexus",
                              nexusCount == 1 && t034BuildCommands == 1 &&
                              t034ReplacementChecked);
                        check("T034-Stop-eventually-accepted", t034AcceptedStops > 0);
                        check("T034-stock-engine-postcondition", failures == 0);
                        log << "T034_RESULT," << frame << ",builder=" << t034Builder
                            << ",acceptedStops=" << t034AcceptedStops
                            << ",acceptedMoves=" << t034AcceptedMoves << '\n';
                        log << "DONE," << frame << ',' << failures << '\n';
                        log.flush();
                        t034Finished = true;
                        finished = true;
                        BWAPI::Broodwar->leaveGame();
                        return;
                    }
                }
            }
            if (frame >= frameLimit && !t034Finished) {
                check("T034-Stop-eventually-accepted", t034AcceptedStops > 0);
                check("T034-stock-engine-postcondition", false);
                log << "DONE," << frame << ',' << failures << '\n';
                log.flush();
                finished = true;
                BWAPI::Broodwar->leaveGame();
            }
            return;
        }
        if (scenario == "worker-issuer") {
            constexpr int frameLimit = 240;
            if (!workerIssuerReset) {
                std::vector<BWAPI::Unit> probes;
                for (const auto unit : BWAPI::Broodwar->self()->getUnits())
                    if (unit != nullptr && unit->exists() &&
                        unit->getType() == BWAPI::UnitTypes::Protoss_Probe)
                        probes.push_back(unit);
                BWAPI::Unit threat = nullptr;
                for (const auto unit : BWAPI::Broodwar->getAllUnits())
                    if (unit != nullptr && unit->exists() &&
                        unit->getPlayer() != BWAPI::Broodwar->self() &&
                        unit->getType() == BWAPI::UnitTypes::Zerg_Zergling)
                        threat = unit;
                check("worker-defense-fixture-one-probe-visible-threat",
                    probes.size() == 1 && threat != nullptr && threat->isVisible());
                if (probes.size() == 1 && threat != nullptr && threat->isVisible()) {
                    workerIssuerActor = probes.front()->getID();
                    workerIssuerTarget = threat->getID();
                    const auto moved = probes.front()->issueCommand(
                        BWAPI::UnitCommand::move(probes.front(), BWAPI::Position{640, 600}));
                    check("worker-defense-fixture-move-accepted", moved);
                    workerIssuerReset = moved;
                    workerIssuerResetFrame = frame;
                }
            } else if (!workerIssuerStarted) {
                const auto probe = BWAPI::Broodwar->getUnit(workerIssuerActor);
                if (probe == nullptr || !probe->exists() ||
                    probe->getLastCommandFrame() + 12 >= frame) {
                    if (frame >= frameLimit) {
                        check("worker-defense-command-accepted", workerIssuerAccepted);
                        check("worker-defense-target-observed", false);
                        check("engine-postcondition", false);
                        log << "DONE," << frame << ',' << failures << '\n';
                        log.flush();
                        finished = true;
                        BWAPI::Broodwar->leaveGame();
                    }
                    return;
                }
                bridge.actionDiagnostic = [this, frame](
                    const protodd::bwapi::ActionDiagnostic& d) {
                    if (d.source != "worker-defend") return;
                    workerIssuerAccepted = d.accepted;
                    log << "ISSUER_ACTION," << frame << ',' << d.actor << ',' << d.target
                        << ',' << d.type << ',' << d.source << ',' << d.outcome << ','
                        << d.attempted << ',' << d.accepted << '\n';
                    log.flush();
                };
                WorkerAssignment assignment{workerIssuerActor, WorkerJob::defend,
                    0, workerIssuerTarget, {-1, -1}, 80};
                executeWorkerProposals(std::span{&assignment, 1});
                if (workerIssuerAccepted) {
                    workerIssuerStarted = true;
                    workerIssuerResetFrame = frame;
                    check("worker-defense-command-accepted", true);
                    const auto order = probe->getLastCommand();
                    log << "WORKER_ISSUER," << frame << ",actor=" << workerIssuerActor
                        << ",target=" << workerIssuerTarget << ",order="
                        << order.getType().toString() << ",accepted=1\n";
                    log.flush();
                }
            } else if (workerIssuerStarted && frame - workerIssuerResetFrame >= 24) {
                const auto probe = BWAPI::Broodwar->getUnit(workerIssuerActor);
                const auto target = BWAPI::Broodwar->getUnit(workerIssuerTarget);
                check("worker-defense-target-observed", probe != nullptr && probe->exists() &&
                    target != nullptr && target->exists() && probe->getOrderTarget() == target);
                check("engine-postcondition", failures == 0);
                log << "DONE," << frame << ',' << failures << '\n';
                log.flush();
                finished = true;
                BWAPI::Broodwar->leaveGame();
                return;
            }
            if (!workerIssuerStarted && frame >= frameLimit) {
                check("worker-defense-command-accepted", workerIssuerAccepted);
                check("worker-defense-target-observed", false);
                check("engine-postcondition", false);
                log << "DONE," << frame << ',' << failures << '\n';
                log.flush();
                finished = true;
                BWAPI::Broodwar->leaveGame();
            }
            return;
        }
        if (scenario == "scout-issuer") {
            constexpr int frameLimit = 240;
            constexpr Position scoutTarget{1080, 600};
            if (!workerIssuerReset) {
                std::vector<BWAPI::Unit> probes;
                for (const auto unit : BWAPI::Broodwar->self()->getUnits())
                    if (unit != nullptr && unit->exists() &&
                        unit->getType() == BWAPI::UnitTypes::Protoss_Probe)
                        probes.push_back(unit);
                check("scout-fixture-one-probe", probes.size() == 1);
                if (probes.size() == 1) {
                    workerIssuerActor = probes.front()->getID();
                    workerIssuerReset = probes.front()->issueCommand(
                        BWAPI::UnitCommand::move(probes.front(), BWAPI::Position{640, 600}));
                    check("scout-fixture-move-accepted", workerIssuerReset);
                }
            } else if (!workerIssuerStarted) {
                const auto probe = BWAPI::Broodwar->getUnit(workerIssuerActor);
                if (probe == nullptr || !probe->exists() ||
                    probe->getLastCommandFrame() + std::max(8,
                        BWAPI::Broodwar->getLatencyFrames()) >= frame) {
                    if (frame >= frameLimit) {
                        check("assigned-scout-move-accepted", workerIssuerAccepted);
                        check("assigned-scout-target-observed", false);
                        check("engine-postcondition", false);
                        log << "DONE," << frame << ',' << failures << '\n';
                        log.flush();
                        finished = true;
                        BWAPI::Broodwar->leaveGame();
                    }
                    return;
                }
                bridge.actionDiagnostic = [this, frame](
                    const protodd::bwapi::ActionDiagnostic& d) {
                    if (d.source != "scout-travel") return;
                    workerIssuerAccepted = d.accepted;
                    log << "ISSUER_ACTION," << frame << ',' << d.actor << ',' << d.target
                        << ',' << d.type << ',' << d.source << ',' << d.outcome << ','
                        << d.attempted << ',' << d.accepted << '\n';
                    log.flush();
                };
                ScoutOrder order{workerIssuerActor, scoutTarget, ScoutPurpose::findEnemy, 1.0};
                CommandBus commands;
                commands.beginFrame(frame, BWAPI::Broodwar->getLatencyFrames());
                static_cast<void>(bridge.submitScouts(std::span{&order, 1}, commands));
                for (const auto& command : commands.finalize(1))
                    static_cast<void>(bridge.execute(command));
                if (workerIssuerAccepted) {
                    workerIssuerStarted = true;
                    workerIssuerResetFrame = frame;
                    check("assigned-scout-move-accepted", true);
                    log << "SCOUT_ISSUER," << frame << ",actor=" << workerIssuerActor
                        << ",target=" << scoutTarget.x << 'x' << scoutTarget.y << ",accepted=1\n";
                    log.flush();
                }
            } else if (frame - workerIssuerResetFrame >= 24) {
                const auto probe = BWAPI::Broodwar->getUnit(workerIssuerActor);
                const auto command = probe != nullptr ? probe->getLastCommand()
                                                      : BWAPI::UnitCommand{};
                check("assigned-scout-target-observed", probe != nullptr && probe->exists() &&
                    command.getType() == BWAPI::UnitCommandTypes::Move &&
                    command.getTargetPosition() == BWAPI::Position{scoutTarget.x, scoutTarget.y});
                check("engine-postcondition", failures == 0);
                log << "DONE," << frame << ',' << failures << '\n';
                log.flush();
                finished = true;
                BWAPI::Broodwar->leaveGame();
                return;
            }
            if (!workerIssuerStarted && frame >= frameLimit) {
                check("assigned-scout-move-accepted", workerIssuerAccepted);
                check("assigned-scout-target-observed", false);
                check("engine-postcondition", false);
                log << "DONE," << frame << ',' << failures << '\n';
                log.flush();
                finished = true;
                BWAPI::Broodwar->leaveGame();
            }
            return;
        }
        if (scenario == "whole-game-issuer") {
            constexpr int frameLimit = 240;
            constexpr Frame leaseFrames = 24;
            constexpr Position wholeGameTarget{1080, 600};
            constexpr Position blockedNativeTarget{900, 600};
            constexpr Position releasedNativeTarget{640, 300};
            if (!wholeGameIssuerReset) {
                std::vector<BWAPI::Unit> probes;
                for (const auto unit : BWAPI::Broodwar->self()->getUnits())
                    if (unit != nullptr && unit->exists() &&
                        unit->getType() == BWAPI::UnitTypes::Protoss_Probe)
                        probes.push_back(unit);
                check("whole-game-fixture-one-probe", probes.size() == 1);
                if (probes.size() == 1) {
                    wholeGameIssuerActor = probes.front()->getID();
                    wholeGameIssuerReset = probes.front()->issueCommand(
                        BWAPI::UnitCommand::move(probes.front(), BWAPI::Position{640, 900}));
                    check("whole-game-fixture-move-accepted", wholeGameIssuerReset);
                }
            } else if (!wholeGameIssuerStarted) {
                const auto probe = BWAPI::Broodwar->getUnit(wholeGameIssuerActor);
                if (probe == nullptr || !probe->exists() ||
                    probe->getLastCommandFrame() + std::max(8,
                        BWAPI::Broodwar->getLatencyFrames()) >= frame) {
                    if (frame >= frameLimit) {
                        check("whole-game-move-accepted", wholeGameIssuerAccepted);
                        check("whole-game-lease-blocked-native-order", false);
                        check("whole-game-native-order-after-expiry", false);
                        check("engine-postcondition", false);
                        log << "DONE," << frame << ',' << failures << '\n';
                        log.flush();
                        finished = true;
                        BWAPI::Broodwar->leaveGame();
                    }
                    return;
                }
                bridge.actionDiagnostic = [this](
                    const protodd::bwapi::ActionDiagnostic& d) {
                    if (d.source != "whole-game" &&
                        d.source != "native-after-whole-game-lease") return;
                    log << "ISSUER_ACTION," << BWAPI::Broodwar->getFrameCount() << ','
                        << d.actor << ',' << d.target
                        << ',' << d.type << ',' << d.source << ',' << d.outcome << ','
                        << d.attempted << ',' << d.accepted << '\n';
                    if (d.source == "whole-game") wholeGameIssuerAccepted = d.accepted;
                    if (d.source == "native-after-whole-game-lease")
                        wholeGameNativeAfterLeaseAccepted = d.accepted;
                    log.flush();
                };
                const auto wholeGameAccepted = bridge.executeWholeGame(
                    BWAPI::UnitCommand::move(probe, BWAPI::Position{
                        wholeGameTarget.x, wholeGameTarget.y}), leaseFrames);
                check("whole-game-move-accepted", wholeGameAccepted);
                wholeGameIssuerAccepted = wholeGameAccepted;
                if (!wholeGameAccepted) return;
                wholeGameIssuerStarted = true;
                wholeGameIssuerFrame = frame;
                const Command conflictingNative{wholeGameIssuerActor, CommandType::move, -1,
                    blockedNativeTarget, UnitKind::unknown, 10, frame,
                    "native-during-whole-game-lease"};
                wholeGameLeaseBlocked = !bridge.execute(conflictingNative);
                check("whole-game-lease-blocked-native-order", wholeGameLeaseBlocked);
                const auto activeCommand = probe->getLastCommand();
                check("whole-game-target-observed", activeCommand.getType() ==
                    BWAPI::UnitCommandTypes::Move && activeCommand.getTargetPosition() ==
                    BWAPI::Position{wholeGameTarget.x, wholeGameTarget.y});
                log << "WHOLE_GAME_ISSUER," << frame << ",actor=" << wholeGameIssuerActor
                    << ",target=" << wholeGameTarget.x << 'x' << wholeGameTarget.y
                    << ",lease=" << leaseFrames << ",nativeBlocked="
                    << wholeGameLeaseBlocked << '\n';
                log.flush();
            } else if (frame - wholeGameIssuerFrame >= leaseFrames) {
                const auto probe = BWAPI::Broodwar->getUnit(wholeGameIssuerActor);
                const auto previousCommand = probe != nullptr ? probe->getLastCommand()
                                                              : BWAPI::UnitCommand{};
                check("whole-game-target-persists-through-lease", probe != nullptr &&
                    probe->exists() && previousCommand.getType() ==
                    BWAPI::UnitCommandTypes::Move && previousCommand.getTargetPosition() ==
                    BWAPI::Position{wholeGameTarget.x, wholeGameTarget.y});
                const Command releasedNative{wholeGameIssuerActor, CommandType::move, -1,
                    releasedNativeTarget, UnitKind::unknown, 10, frame,
                    "native-after-whole-game-lease"};
                wholeGameNativeAfterLeaseAccepted = bridge.execute(releasedNative);
                check("whole-game-native-order-after-expiry",
                    wholeGameNativeAfterLeaseAccepted);
                const auto command = probe != nullptr ? probe->getLastCommand()
                                                      : BWAPI::UnitCommand{};
                check("whole-game-expired-lease-releases-native-order", probe != nullptr &&
                    probe->exists() && command.getType() == BWAPI::UnitCommandTypes::Move &&
                    command.getTargetPosition() == BWAPI::Position{
                        releasedNativeTarget.x, releasedNativeTarget.y});
                check("engine-postcondition", failures == 0);
                log << "DONE," << frame << ',' << failures << '\n';
                log.flush();
                finished = true;
                BWAPI::Broodwar->leaveGame();
                return;
            }
            if (!wholeGameIssuerStarted && frame >= frameLimit) {
                check("whole-game-move-accepted", wholeGameIssuerAccepted);
                check("whole-game-lease-blocked-native-order", wholeGameLeaseBlocked);
                check("whole-game-native-order-after-expiry",
                    wholeGameNativeAfterLeaseAccepted);
                check("engine-postcondition", false);
                log << "DONE," << frame << ',' << failures << '\n';
                log.flush();
                finished = true;
                BWAPI::Broodwar->leaveGame();
            }
            return;
        }
        if (scenario == "worker-local-defense") {
            if (workerDefenseStarted) return;
            workerDefenseStarted = true;

            const auto mainBase = std::ranges::min_element(state.bases,
                [](const BaseSnapshot& left, const BaseSnapshot& right) {
                    return distanceSquared(left.center, Position{512, 512}) <
                           distanceSquared(right.center, Position{512, 512});
                });
            const auto naturalBase = std::ranges::min_element(state.bases,
                [](const BaseSnapshot& left, const BaseSnapshot& right) {
                    return distanceSquared(left.center, Position{2050, 2000}) <
                           distanceSquared(right.center, Position{2050, 2000});
                });
            const auto ownedBase = [&state](const BaseSnapshot& base) {
                return base.ownerId == state.self.id;
            };
            const auto mainReady = mainBase != state.bases.end() && ownedBase(*mainBase) &&
                distanceSquared(mainBase->center, Position{512, 512}) <= 320 * 320;
            const auto naturalReady = naturalBase != state.bases.end() &&
                ownedBase(*naturalBase) &&
                distanceSquared(naturalBase->center, Position{2050, 2000}) <= 320 * 320;
            check("worker-local-defense-fixture-two-owned-bases", mainReady && naturalReady);
            if (!mainReady || !naturalReady) {
                check("worker-local-defense-engine-postcondition", false);
                log << "DONE," << frame << ',' << failures << '\n';
                log.flush();
                finished = true;
                BWAPI::Broodwar->leaveGame();
                return;
            }

            const auto closestBase = [&state](const Position position) -> const BaseSnapshot* {
                const BaseSnapshot* result = nullptr;
                auto bestDistance = std::numeric_limits<int>::max();
                for (const auto& base : state.bases) {
                    if (base.ownerId != state.self.id) continue;
                    const auto separation = distanceSquared(position, base.center);
                    if (separation < bestDistance) {
                        bestDistance = separation;
                        result = &base;
                    }
                }
                return result;
            };
            const auto mainThreats = std::ranges::count_if(state.enemy.units,
                [mainBase](const UnitSnapshot& enemy) {
                    return enemy.visible && enemy.kind == UnitKind::zergling &&
                        distanceSquared(enemy.position, mainBase->center) <= 640 * 640;
                });
            const auto naturalThreats = std::ranges::count_if(state.enemy.units,
                [naturalBase](const UnitSnapshot& enemy) {
                    return enemy.visible && enemy.kind == UnitKind::zergling &&
                        distanceSquared(enemy.position, naturalBase->center) <= 640 * 640;
                });
            auto mainScreen = 0;
            auto naturalScreen = 0;
            for (const auto& unit : state.self.units) {
                if (!unit.completed || unit.kind != UnitKind::zealot) continue;
                if (distanceSquared(unit.position, mainBase->center) <= 640 * 640) ++mainScreen;
                if (distanceSquared(unit.position, naturalBase->center) <= 640 * 640) ++naturalScreen;
            }
            check("worker-local-defense-fixture-simultaneous-threats",
                  mainThreats >= 2 && naturalThreats >= 2);
            check("worker-local-defense-fixture-screen-only-at-main",
                  mainScreen >= 2 && naturalScreen == 0);

            const auto navigation = bridge.navigationGrid();
            const auto assignments = WorkerManager{}.assign(
                state, {}, influence, {}, false, false, &navigation);
            int mainDefend = 0, naturalDefend = 0;
            int mainMining = 0, naturalMining = 0;
            int mainEvacuate = 0, naturalEvacuate = 0;
            for (const auto& assignment : assignments) {
                const auto worker = std::ranges::find(state.self.units, assignment.worker,
                                                       &UnitSnapshot::id);
                if (worker == state.self.units.end()) continue;
                const auto* base = closestBase(worker->position);
                if (base == nullptr) continue;
                const auto isMain = base->id == mainBase->id;
                if (assignment.job == WorkerJob::defend) {
                    if (isMain) ++mainDefend;
                    else ++naturalDefend;
                    if (!isMain && distanceSquared(worker->position, naturalBase->center) >
                                       distanceSquared(worker->position, mainBase->center)) {
                        check("worker-local-defense-natural-defender-is-local", false);
                    }
                } else if (assignment.job == WorkerJob::minerals) {
                    if (isMain) ++mainMining;
                    else ++naturalMining;
                } else if (assignment.job == WorkerJob::evacuate) {
                    if (isMain) ++mainEvacuate;
                    else ++naturalEvacuate;
                }
            }
            log << "LOCAL_DEFENSE," << frame << ",mainBase=" << mainBase->id
                << ",mainCenter=" << mainBase->center.x << 'x' << mainBase->center.y
                << ",naturalBase=" << naturalBase->id << ",naturalCenter="
                << naturalBase->center.x << 'x' << naturalBase->center.y
                << ",mainThreats=" << mainThreats << ",naturalThreats=" << naturalThreats
                << ",mainScreen=" << mainScreen << ",naturalScreen=" << naturalScreen
                << ",mainDefend=" << mainDefend << ",naturalDefend=" << naturalDefend
                << ",mainMining=" << mainMining << ",naturalMining=" << naturalMining
                << ",mainEvacuate=" << mainEvacuate
                << ",naturalEvacuate=" << naturalEvacuate << '\n';
            log.flush();
            check("worker-local-defense-screen-suppresses-main-militia", mainDefend == 0);
            check("worker-local-defense-uncovered-natural-gets-militia", naturalDefend > 0);
            check("worker-local-defense-immediate-contact-triggers-local-evacuation",
                  mainEvacuate > 0 && naturalEvacuate > 0);

            bridge.actionDiagnostic = [this, mainBaseId = mainBase->id,
                                       naturalBaseId = naturalBase->id,
                                       &closestBase, &state](
                    const protodd::bwapi::ActionDiagnostic& d) {
                if (d.source != "worker-defend") return;
                const auto worker = std::ranges::find(state.self.units, d.actor,
                                                       &UnitSnapshot::id);
                const auto* base = worker == state.self.units.end()
                    ? nullptr : closestBase(worker->position);
                if (d.accepted) {
                    if (base != nullptr && base->id == mainBaseId)
                        ++workerDefenseMainAccepted;
                    else if (base != nullptr && base->id == naturalBaseId)
                        ++workerDefenseNaturalAccepted;
                    else
                        ++workerDefenseOtherAccepted;
                }
                log << "LOCAL_DEFENSE_ORDER," << BWAPI::Broodwar->getFrameCount() << ','
                    << d.actor << ',' << d.target << ',' << d.outcome << ',' << d.accepted
                    << '\n';
                log.flush();
            };
            executeWorkerProposals(assignments);
            bridge.actionDiagnostic = {};
            check("worker-local-defense-no-main-probe-order",
                  workerDefenseMainAccepted == 0);
            check("worker-local-defense-natural-probe-order-accepted",
                  workerDefenseNaturalAccepted > 0);
            check("worker-local-defense-no-remote-probe-order",
                  workerDefenseOtherAccepted == 0);
            check("worker-local-defense-engine-postcondition", failures == 0);
            log << "DONE," << frame << ',' << failures << '\n';
            log.flush();
            finished = true;
            BWAPI::Broodwar->leaveGame();
            return;
        }
        if (scenario == "worker-mining") {
            constexpr int frameLimit = 240;
            if (!workerIssuerReset) {
                std::vector<BWAPI::Unit> probes;
                for (const auto unit : BWAPI::Broodwar->self()->getUnits())
                    if (unit != nullptr && unit->exists() &&
                        unit->getType() == BWAPI::UnitTypes::Protoss_Probe)
                        probes.push_back(unit);
                const auto patches = BWAPI::Broodwar->getMinerals();
                const auto nexus = std::ranges::find_if(state.self.units,
                    [](const auto& unit) {
                        return unit.kind == UnitKind::nexus && unit.completed;
                    });
                const auto fundedPatch = patches.size() == 1 &&
                    (*patches.begin())->getResources() >= 1000;
                check("worker-mining-fixture-has-funded-patch-and-probe",
                    probes.size() == 1 && fundedPatch && nexus != state.self.units.end());
                if (probes.size() == 1 && fundedPatch && nexus != state.self.units.end()) {
                    workerIssuerActor = probes.front()->getID();
                    workerIssuerTarget = (*patches.begin())->getID();
                    workerIssuerReset = probes.front()->issueCommand(
                        BWAPI::UnitCommand::move(probes.front(), BWAPI::Position{640, 900}));
                    check("worker-mining-fixture-move-accepted", workerIssuerReset);
                }
            } else if (!workerIssuerStarted) {
                const auto probe = BWAPI::Broodwar->getUnit(workerIssuerActor);
                if (probe == nullptr || !probe->exists() ||
                    probe->getLastCommandFrame() + 12 >= frame) {
                    if (frame >= frameLimit) {
                        check("worker-minerals-command-accepted", workerIssuerAccepted);
                        check("worker-mineral-target-observed", false);
                        check("engine-postcondition", false);
                        log << "DONE," << frame << ',' << failures << '\n';
                        log.flush();
                        finished = true;
                        BWAPI::Broodwar->leaveGame();
                    }
                    return;
                }
                bridge.actionDiagnostic = [this, frame](
                    const protodd::bwapi::ActionDiagnostic& d) {
                    if (d.source != "worker-minerals") return;
                    workerIssuerAccepted = d.accepted;
                    log << "ISSUER_ACTION," << frame << ',' << d.actor << ',' << d.target
                        << ',' << d.type << ',' << d.source << ',' << d.outcome << ','
                        << d.attempted << ',' << d.accepted << '\n';
                    log.flush();
                };
                const auto nexus = std::ranges::find_if(state.self.units,
                    [](const auto& unit) {
                        return unit.kind == UnitKind::nexus && unit.completed;
                    });
                if (nexus != state.self.units.end()) {
                    WorkerAssignment assignment{workerIssuerActor, WorkerJob::minerals,
                        nexus->id, -1, nexus->position, 60};
                    executeWorkerProposals(std::span{&assignment, 1});
                    if (workerIssuerAccepted) {
                        workerIssuerStarted = true;
                        workerIssuerResetFrame = frame;
                        check("worker-minerals-command-accepted", true);
                        log << "WORKER_ISSUER," << frame << ",actor=" << workerIssuerActor
                            << ",target=" << workerIssuerTarget << ",accepted=1\n";
                        log.flush();
                    }
                }
            } else if (frame - workerIssuerResetFrame >= 24) {
                const auto probe = BWAPI::Broodwar->getUnit(workerIssuerActor);
                const auto patch = BWAPI::Broodwar->getUnit(workerIssuerTarget);
                check("worker-mineral-target-observed", probe != nullptr && probe->exists() &&
                    patch != nullptr && patch->exists() && probe->isGatheringMinerals() &&
                    probe->getOrderTarget() == patch);
                check("engine-postcondition", failures == 0);
                log << "DONE," << frame << ',' << failures << '\n';
                log.flush();
                finished = true;
                BWAPI::Broodwar->leaveGame();
                return;
            }
            if (!workerIssuerStarted && frame >= frameLimit) {
                check("worker-minerals-command-accepted", workerIssuerAccepted);
                check("worker-mineral-target-observed", false);
                check("engine-postcondition", false);
                log << "DONE," << frame << ',' << failures << '\n';
                log.flush();
                finished = true;
                BWAPI::Broodwar->leaveGame();
            }
            return;
        }
        if (scenario == "pylon-loss") {
            constexpr int frameLimit = 3600;
            const auto gateway = BWAPI::Broodwar->getUnit(powerLossGateway);
            const auto core = BWAPI::Broodwar->getUnit(powerLossCore);
            const auto originalPylon = BWAPI::Broodwar->getUnit(powerLossPylon);
            const auto validStructures = gateway != nullptr && gateway->exists() &&
                core != nullptr && core->exists();
            if (frame >= 24 && validStructures && originalPylon != nullptr &&
                originalPylon->exists() && gateway->isPowered() && core->isPowered()) {
                if (!powerLossTrainSeeded) {
                    powerLossTrainSeeded = bridge.executeProduction(
                        BWAPI::UnitCommand::train(gateway, BWAPI::UnitTypes::Protoss_Zealot));
                }
                if (!powerLossUpgradeSeeded) {
                    powerLossUpgradeSeeded = bridge.executeProduction(
                        BWAPI::UnitCommand::upgrade(core, BWAPI::UpgradeTypes::Singularity_Charge));
                }
                powerLossSeeded = powerLossTrainSeeded && powerLossUpgradeSeeded;
                if (powerLossSeeded && !powerLossSeedChecksDone) {
                    powerLossSeedChecksDone = true;
                    powerLossActiveBefore = gateway->isTraining() &&
                        gateway->getRemainingTrainTime() > 0 && core->isUpgrading() &&
                        core->getRemainingUpgradeTime() > 0;
                    check("loss-seed-training-accepted", powerLossTrainSeeded);
                    check("loss-seed-upgrade-accepted", powerLossUpgradeSeeded);
                    log << "POWERLOSS_SEED," << frame << ",gateway=" << powerLossGateway
                        << ",core=" << powerLossCore << ",pylon=" << powerLossPylon
                        << ",minerals=" << BWAPI::Broodwar->self()->minerals()
                        << ",gas=" << BWAPI::Broodwar->self()->gas() << ",trainRemaining="
                        << gateway->getRemainingTrainTime() << ",upgradeRemaining="
                        << core->getRemainingUpgradeTime() << ",trainActive=" << gateway->isTraining()
                        << ",upgradeActive=" << core->isUpgrading() << '\n';
                }
            }
            if (validStructures && !powerLossObserved) {
                const auto pylonAlive = originalPylon != nullptr && originalPylon->exists();
                if (pylonAlive && gateway->isPowered() && core->isPowered()) {
                    if (!powerLossActiveBefore && gateway->isTraining() &&
                        gateway->getRemainingTrainTime() > 0 && core->isUpgrading() &&
                        core->getRemainingUpgradeTime() > 0) {
                        powerLossActiveBefore = true;
                        check("loss-operations-active-before-destruction", true);
                    }
                } else if (!pylonAlive) {
                    powerLossObserved = true;
                    powerLossLossFrame = frame;
                    BWAPI::Broodwar->setFrameSkip(64);
                    powerLossTrainAtLoss = gateway->getRemainingTrainTime();
                    powerLossUpgradeAtLoss = core->getRemainingUpgradeTime();
                    powerLossZealotsAtLoss = BWAPI::Broodwar->self()->completedUnitCount(
                        BWAPI::UnitTypes::Protoss_Zealot);
                    check("attacking-zerglings-destroyed-pylon", true);
                    check("gateway-lost-power", !gateway->isPowered());
                    check("research-core-lost-power", !core->isPowered());
                    check("active-operations-existed-before-loss", powerLossActiveBefore);
                    std::vector<BWAPI::Unit> defenders, attackers;
                    for (const auto unit : BWAPI::Broodwar->getAllUnits()) {
                        if (unit == nullptr || !unit->exists()) continue;
                        if (unit->getPlayer() == BWAPI::Broodwar->self() &&
                            unit->getType() == BWAPI::UnitTypes::Protoss_Zealot)
                            defenders.push_back(unit);
                        else if (unit->getPlayer() != BWAPI::Broodwar->self() &&
                            unit->getType() == BWAPI::UnitTypes::Zerg_Zergling)
                            attackers.push_back(unit);
                    }
                    auto defenseOrders = 0;
                    for (std::size_t index = 0; index < defenders.size() && !attackers.empty(); ++index) {
                        const auto target = attackers[index % attackers.size()];
                        if (bridge.executeProduction(BWAPI::UnitCommand::attack(
                                defenders[index], target))) ++defenseOrders;
                    }
                    powerLossDefendersDispatched = true;
                    check("local-defenders-dispatched", defenseOrders > 0);
                    log << "POWERLOSS_DEFENSE," << frame << ",defenders=" << defenders.size()
                        << ",attackers=" << attackers.size() << ",accepted="
                        << defenseOrders << '\n';
                    log << "POWERLOSS," << frame << ",gatewayPowered=" << gateway->isPowered()
                        << ",corePowered=" << core->isPowered() << ",train="
                        << gateway->isTraining() << ':' << powerLossTrainAtLoss
                        << ",upgrade=" << core->isUpgrading() << ':'
                        << powerLossUpgradeAtLoss << '\n';
                }
            }
            if (powerLossObserved && !powerLossPausedVerified && validStructures &&
                !gateway->isPowered() && !core->isPowered() &&
                frame - powerLossLossFrame >= 128) {
                const auto trainingPaused = gateway->getRemainingTrainTime() == powerLossTrainAtLoss;
                const auto upgradePaused = core->getRemainingUpgradeTime() == powerLossUpgradeAtLoss;
                check("production-timer-paused-during-power-loss", trainingPaused);
                check("upgrade-timer-paused-during-power-loss", upgradePaused);
                log << "POWERLOSS_PAUSE," << frame << ",elapsed="
                    << frame - powerLossLossFrame << ",trainAtLoss=" << powerLossTrainAtLoss
                    << ",trainNow=" << gateway->getRemainingTrainTime()
                    << ",upgradeAtLoss=" << powerLossUpgradeAtLoss
                    << ",upgradeNow=" << core->getRemainingUpgradeTime() << '\n';
                powerLossPausedVerified = true;
            }
            if (powerLossObserved && !powerLossRestored && validStructures) {
                const auto plan = strategy.plan(state, ThreatAssessment{});
                const auto recoveryGoals = std::ranges::count_if(plan.goals,
                    [](const auto& goal) {
                        return goal.goal == GoalKind::build && goal.target == UnitKind::pylon &&
                            goal.reason == "restore power to disabled production" &&
                            goal.constructionSite.valid();
                    });
                ResourceLedger ledger{state.self.minerals, state.self.gas};
                const auto actions = planner.reconcile(state, plan, ledger);
                const auto issued = bridge.executeMacro(actions, plan, influence);
                powerLossRecoveryIssued = powerLossRecoveryIssued ||
                    std::ranges::any_of(bridge.macroExecutions(), [](const auto& execution) {
                        return execution.action.target == UnitKind::pylon && execution.accepted;
                    });
                const auto completedPylons = std::ranges::count_if(state.self.units,
                    [](const auto& unit) { return unit.kind == UnitKind::pylon && unit.completed; });
                if (gateway->isPowered() && core->isPowered() && completedPylons > 0) {
                    powerLossRestored = true;
                    powerLossRestoreFrame = frame;
                    powerLossTrainAfterRestore = gateway->getRemainingTrainTime();
                    powerLossUpgradeAfterRestore = core->getRemainingUpgradeTime();
                    check("replacement-pylon-command-accepted", powerLossRecoveryIssued);
                    check("replacement-pylon-restored-both-structures", true);
                }
                if (frame % 96 == 0 || powerLossRestored) {
                    log << "POWERLOSS_RECOVERY," << frame << ",goals=" << recoveryGoals
                        << ",issued=" << issued << ",completedPylons=" << completedPylons
                        << ",gatewayPowered=" << gateway->isPowered()
                        << ",corePowered=" << core->isPowered()
                        << ",trainRemaining=" << gateway->getRemainingTrainTime()
                        << ",upgradeRemaining=" << core->getRemainingUpgradeTime() << '\n';
                    for (const auto& execution : bridge.macroExecutions()) {
                        if (execution.action.target != UnitKind::pylon) continue;
                        log << "POWERLOSS_ACTION," << frame << ",reserved="
                            << execution.action.reserved << ",executable="
                            << execution.action.executable << ",outcome=" << execution.outcome
                            << ",accepted=" << execution.accepted << ",reason="
                            << execution.action.reason << ",site="
                            << execution.action.constructionSite.anchor.x << 'x'
                            << execution.action.constructionSite.anchor.y << '\n';
                    }
                    for (const auto& unit : state.self.units) {
                        if (unit.kind != UnitKind::probe) continue;
                        log << "POWERLOSS_PROBE," << frame << ",id=" << unit.id
                            << ",position=" << unit.position.x << 'x' << unit.position.y
                            << ",completed=" << unit.completed << ",carrying="
                            << unit.carryingResources << '\n';
                    }
                }
            }
            if (powerLossRestored && !powerLossOperationsResumed && validStructures &&
                frame - powerLossRestoreFrame >= 24) {
                const auto completedZealots = BWAPI::Broodwar->self()->completedUnitCount(
                    BWAPI::UnitTypes::Protoss_Zealot);
                const auto completedZealot = completedZealots > powerLossZealotsAtLoss;
                const auto trainingProgressed = completedZealot ||
                    ((gateway->isTraining() || gateway->getRemainingTrainTime() > 0) &&
                     gateway->getRemainingTrainTime() < powerLossTrainAtLoss);
                const auto upgradeComplete = BWAPI::Broodwar->self()->getUpgradeLevel(
                    BWAPI::UpgradeTypes::Singularity_Charge) > 0;
                const auto upgradeProgressed = upgradeComplete ||
                    (core->isUpgrading() && core->getRemainingUpgradeTime() > 0 &&
                     core->getRemainingUpgradeTime() < powerLossUpgradeAtLoss);
                if (trainingProgressed && upgradeProgressed) {
                    powerLossOperationsResumed = true;
                    check("training-progressed-after-restoration", trainingProgressed);
                    check("upgrade-progressed-after-restoration", upgradeProgressed);
                    log << "POWERLOSS_RESUMED," << frame << ",restoreFrame="
                        << powerLossRestoreFrame << ",zealots=" << completedZealots
                        << ",zealotIncrease=" << completedZealot
                        << ",trainRemaining=" << gateway->getRemainingTrainTime()
                        << ",upgradeComplete=" << upgradeComplete
                        << ",upgradeRemaining=" << core->getRemainingUpgradeTime() << '\n';
                }
            }
            if (powerLossOperationsResumed || frame >= frameLimit) {
                finished = true;
                check("engine-postcondition", powerLossSeeded && powerLossObserved &&
                    powerLossRecoveryIssued && powerLossRestored && powerLossPausedVerified &&
                    powerLossOperationsResumed);
                log << "DONE," << frame << ',' << failures << '\n';
                log.flush();
                BWAPI::Broodwar->leaveGame();
            }
            return;
        }
        if (scenario == "producer-pair") {
            const auto frameLimit = 1200;
            if (!producerPairStarted) {
                std::vector<BWAPI::Unit> gateways;
                for (const auto unit : BWAPI::Broodwar->self()->getUnits())
                    if (unit != nullptr && unit->exists() &&
                        unit->getType() == BWAPI::UnitTypes::Protoss_Gateway)
                        gateways.push_back(unit);
                std::sort(gateways.begin(), gateways.end(), [](const auto left, const auto right) {
                    return left->getID() < right->getID();
                });
                check("fixture-two-gateways", gateways.size() == 2);
                if (gateways.size() == 2) {
                    producerPairBusy = gateways[0]->getID();
                    producerPairIdle = gateways[1]->getID();
                    check("fixture-both-gateways-powered",
                          gateways[0]->isPowered() && gateways[1]->isPowered());
                    const auto seedAccepted = bridge.executeProduction(
                        BWAPI::UnitCommand::train(gateways[0], BWAPI::UnitTypes::Protoss_Zealot));
                    log << "PAIR_SEED," << frame << ",busy=" << producerPairBusy
                        << ",idle=" << producerPairIdle << ",accepted=" << seedAccepted
                        << ",remaining=" << gateways[0]->getRemainingTrainTime()
                        << ",queue=" << gateways[0]->getTrainingQueue().size() << '\n';
                    check("busy-gateway-seed-accepted", seedAccepted);
                    producerPairStarted = seedAccepted;
                }
            } else if (!producerPairTested) {
                const auto busy = BWAPI::Broodwar->getUnit(producerPairBusy);
                const auto idle = BWAPI::Broodwar->getUnit(producerPairIdle);
                if (busy != nullptr && busy->exists() && idle != nullptr && idle->exists()) {
                    const auto remaining = busy->getRemainingTrainTime();
                    const auto queue = busy->getTrainingQueue();
                    const auto latency = BWAPI::Broodwar->getRemainingLatencyFrames();
                    const auto idleQueue = idle->getTrainingQueue();
                    if ((busy->isTraining() || remaining > 0) && queue.size() == 1 &&
                        remaining > 0 && remaining <= latency &&
                        !idle->isTraining() && idle->getRemainingTrainTime() == 0 &&
                        idleQueue.empty()) {
                        check("busy-finishing-gateway-in-latency-window", true);
                        check("second-gateway-idle", true);
                        ResourceLedger ledger{state.self.minerals, state.self.gas};
                        const auto reserved = ledger.reserve(100, 0);
                        check("retry-resources-reserved", reserved);
                        const auto ledgerMineralsBefore = ledger.minerals;
                        int acceptedActor = -1;
                        bridge.actionDiagnostic = [this, &acceptedActor, frame](
                            const protodd::bwapi::ActionDiagnostic& diagnostic) {
                            if (diagnostic.source != "T026-producer-pair-retry") return;
                            log << "PAIR_ACTION," << frame << ",actor=" << diagnostic.actor
                                << ",type=" << diagnostic.type << ",outcome=" << diagnostic.outcome
                                << ",attempted=" << diagnostic.attempted
                                << ",accepted=" << diagnostic.accepted << '\n';
                            if (diagnostic.accepted) acceptedActor = diagnostic.actor;
                        };
                        bridge.productionPermission = [this, frame](
                            const BWAPI::UnitCommand& command, const std::string_view source) {
                            if (!producerPairInjected && source == "T026-producer-pair-retry" &&
                                command.getType() == BWAPI::UnitCommandTypes::Train &&
                                command.getUnit() != nullptr &&
                                command.getUnit()->getID() == producerPairBusy) {
                                producerPairInjected = true;
                                log << "PAIR_INJECTED_BUSY," << frame << ",actor="
                                    << producerPairBusy << ",source=" << source << '\n';
                                log.flush();
                                return false;
                            }
                            return true;
                        };
                        bridge.setSpendingLedger(&ledger);
                        MacroAction retry{MacroActionKind::train, UnitKind::zealot,
                            100, 100, 0, reserved, "T026-producer-pair-retry"};
                        StrategicPlan plan;
                        const auto issued = bridge.executeMacro(std::span{&retry, 1}, plan, influence);
                        producerPairAcceptedActor = acceptedActor;
                        check("busy-rejection-injected", producerPairInjected);
                        check("retry-command-accepted", issued == 1 && acceptedActor >= 0);
                        check("retry-selected-idle-gateway", acceptedActor == producerPairIdle);
                        check("ledger-charged-once", ledger.minerals == ledgerMineralsBefore - 100 &&
                            ledger.reservedMinerals == 0 && ledger.committedMinerals == 0);
                        log << "PAIR_RESULT," << frame << ",busy=" << producerPairBusy
                            << ",idle=" << producerPairIdle << ",acceptedActor=" << acceptedActor
                            << ",issued=" << issued << ",ledgerMinerals=" << ledger.minerals
                            << ",reserved=" << ledger.reservedMinerals
                            << ",committed=" << ledger.committedMinerals
                            << ",injected=" << producerPairInjected << '\n';
                        bridge.setSpendingLedger(nullptr);
                        bridge.productionPermission = {};
                        bridge.actionDiagnostic = {};
                        producerPairTested = true;
                        confirmed = failures == 0;
                    }
                }
            }
            if (producerPairTested || frame >= frameLimit) {
                finished = true;
                check("engine-postcondition", producerPairTested && confirmed);
                log << "DONE," << frame << ',' << failures << '\n';
                log.flush();
                BWAPI::Broodwar->leaveGame();
            }
            return;
        }
        if (scenario == "cannon-blocker") {
            constexpr int frameLimit = 1200;
            BWAPI::Unit nexus = nullptr;
            for (const auto unit : BWAPI::Broodwar->self()->getUnits()) {
                if (unit != nullptr && unit->exists() &&
                    unit->getType() == BWAPI::UnitTypes::Protoss_Nexus) {
                    nexus = unit;
                    break;
                }
            }
            if (!cannonBlockerStarted) {
                cannonBlockerStarted = true;
                check("fixture-powered-pylon", std::ranges::any_of(state.self.units,
                    [](const auto& unit) { return unit.kind == UnitKind::pylon && unit.completed; }));
                check("fixture-cannon-prerequisite", std::ranges::any_of(state.self.units,
                    [](const auto& unit) { return unit.kind == UnitKind::forge && unit.completed; }));
                check("fixture-cannon-builder", std::ranges::any_of(state.self.units,
                    [](const auto& unit) { return unit.kind == UnitKind::probe && unit.completed; }));
                check("fixture-probe-producer", nexus != nullptr);
                if (nexus == nullptr) failures++;
            }

            StrategicPlan plan;
            plan.goals = {
                {GoalKind::build, UnitKind::photonCannon, 1, 130, true,
                 "T024 obstructed mineral-line Cannon"},
                {GoalKind::train, UnitKind::probe, 2, 100, true,
                 "T024 keep worker production issuing"},
            };
            ResourceLedger ledger{state.self.minerals, state.self.gas};
            auto actions = planner.reconcile(state, plan, ledger);
            const auto cannon = std::ranges::find_if(actions, [](const MacroAction& action) {
                return action.target == UnitKind::photonCannon;
            });
            const auto probe = std::ranges::find_if(actions, [](const MacroAction& action) {
                return action.target == UnitKind::probe &&
                       action.action == MacroActionKind::train;
            });
            if (frame == 24) {
                check("both-funded-goals-reserved", cannon != actions.end() &&
                    probe != actions.end() && cannon->reserved && probe->reserved &&
                    ledger.reservedMinerals == 200 && ledger.committedMinerals == 200);
            }
            const auto mineralsBefore = BWAPI::Broodwar->self()->minerals();
            bridge.actionDiagnostic = [this](const protodd::bwapi::ActionDiagnostic& d) {
                if (d.source != "T024 keep worker production issuing") return;
                log << "CANNON_BLOCKER_ACTION," << d.actor << ',' << d.type << ','
                    << d.outcome << ',' << d.attempted << ',' << d.accepted << '\n';
                log.flush();
            };
            bridge.setSpendingLedger(&ledger);
            const auto issued = bridge.executeMacro(actions, plan, influence);
            bridge.setSpendingLedger(nullptr);
            bridge.actionDiagnostic = {};
            const auto mineralsAfter = BWAPI::Broodwar->self()->minerals();
            const auto& executions = bridge.macroExecutions();
            const auto cannonExecution = std::ranges::find_if(executions,
                [](const protodd::bwapi::MacroExecution& execution) {
                    return execution.action.target == UnitKind::photonCannon;
                });
            const auto probeExecution = std::ranges::find_if(executions,
                [](const protodd::bwapi::MacroExecution& execution) {
                    return execution.action.target == UnitKind::probe &&
                           execution.action.action == MacroActionKind::train;
                });
            const auto cannonOutcome = cannonExecution == executions.end()
                ? std::string("missing") : cannonExecution->outcome;
            if (probeExecution != executions.end() && probeExecution->accepted) {
                cannonBlockerProbeAccepted = true;
                cannonBlockerMineralsBefore = mineralsBefore;
                cannonBlockerMineralsAfter = mineralsAfter;
                cannonBlockerProbeDebitVerified = mineralsBefore - mineralsAfter == 50 &&
                    ledger.minerals == state.self.minerals - 50 &&
                    ledger.committedMinerals == 150;
                if (nexus != nullptr) {
                    const auto probeQueue = nexus->getTrainingQueue();
                    check("probe-queue-observed", probeQueue.size() == 1 &&
                        probeQueue.front() == BWAPI::UnitTypes::Protoss_Probe);
                } else {
                    check("probe-queue-observed", false);
                }
                log << "CANNON_BLOCKER_PROBE," << frame << ",mineralsBefore="
                    << mineralsBefore << ",mineralsAfter=" << mineralsAfter
                    << ",ledger=" << ledger.minerals << ",committed="
                    << ledger.committedMinerals << '\n';
            }
            log << "CANNON_BLOCKER_SEARCH," << frame << ",cannon=" << cannonOutcome
                << ",probeAccepted=" << (probeExecution != executions.end() &&
                                           probeExecution->accepted)
                << ",issued=" << issued << '\n';
            if (cannonExecution != executions.end() && cannonExecution->accepted) {
                check("cannon-stays-unplaceable", false);
                finished = true;
            } else if (cannonExecution != executions.end() &&
                       cannonOutcome.starts_with("blocked-no-placement-retry-")) {
                const auto feedback = bridge.buildBlockerFeedback();
                check("no-placement-feedback-active", std::ranges::any_of(feedback,
                    [frame](const BuildBlockerFeedback& item) {
                        return item.target == UnitKind::photonCannon &&
                               item.reason == BuildBlockerReason::noPlacement &&
                               item.retryAt > frame;
                    }));
                check("cannon-deferred-for-no-placement", true);
                check("probe-command-accepted-during-deferral", cannonBlockerProbeAccepted);
                check("stock-engine-charged-only-probe-cost",
                    cannonBlockerProbeDebitVerified &&
                    cannonBlockerMineralsBefore - cannonBlockerMineralsAfter == 50);

                StrategicPlan continuation;
                continuation.goals = {
                    {GoalKind::build, UnitKind::photonCannon, 1, 130, true,
                     "T024 obstructed mineral-line Cannon"},
                    {GoalKind::train, UnitKind::zealot, 1, 100, true,
                     "T024 continue Gateway production"},
                };
                ResourceLedger continuationLedger{state.self.minerals, state.self.gas};
                auto continuationActions = planner.reconcile(
                    state, continuation, continuationLedger, feedback);
                const auto deferredCannon = std::ranges::find_if(
                    continuationActions, [](const MacroAction& action) {
                        return action.target == UnitKind::photonCannon;
                    });
                const auto gatewayOrder = std::ranges::find_if(
                    continuationActions, [](const MacroAction& action) {
                        return action.target == UnitKind::zealot &&
                               action.action == MacroActionKind::train;
                    });
                check("blocked-cannon-releases-planner-reservation",
                    deferredCannon != continuationActions.end() &&
                    !deferredCannon->reserved && !deferredCannon->executable &&
                    gatewayOrder != continuationActions.end() &&
                    gatewayOrder->reserved && gatewayOrder->executable);
                BWAPI::Unit gateway = nullptr;
                for (const auto unit : BWAPI::Broodwar->self()->getUnits()) {
                    if (unit != nullptr && unit->exists() &&
                        unit->getType() == BWAPI::UnitTypes::Protoss_Gateway) {
                        gateway = unit;
                        break;
                    }
                }
                check("fixture-gateway-producer", gateway != nullptr);
                const auto continuationMineralsBefore = BWAPI::Broodwar->self()->minerals();
                bridge.actionDiagnostic = [this](const protodd::bwapi::ActionDiagnostic& d) {
                    if (d.source != "T024 continue Gateway production") return;
                    log << "CANNON_BLOCKER_CONTINUATION_ACTION," << d.actor << ','
                        << d.type << ',' << d.outcome << ',' << d.attempted << ','
                        << d.accepted << '\n';
                    log.flush();
                };
                bridge.setSpendingLedger(&continuationLedger);
                const auto continuationIssued = bridge.executeMacro(
                    continuationActions, continuation, influence);
                bridge.setSpendingLedger(nullptr);
                bridge.actionDiagnostic = {};
                const auto continuationMineralsAfter = BWAPI::Broodwar->self()->minerals();
                const auto& continuationExecutions = bridge.macroExecutions();
                const auto deferredExecution = std::ranges::find_if(
                    continuationExecutions, [](const protodd::bwapi::MacroExecution& execution) {
                        return execution.action.target == UnitKind::photonCannon;
                    });
                const auto gatewayExecution = std::ranges::find_if(
                    continuationExecutions, [](const protodd::bwapi::MacroExecution& execution) {
                        return execution.action.target == UnitKind::zealot &&
                               execution.action.action == MacroActionKind::train;
                    });
                check("gateway-production-accepted-under-active-blocker",
                    gatewayExecution != continuationExecutions.end() &&
                    gatewayExecution->accepted && continuationIssued == 1);
                check("continuation-keeps-cannon-deferred",
                    deferredExecution != continuationExecutions.end() &&
                    !deferredExecution->accepted &&
                    deferredExecution->outcome.starts_with("blocked-no-placement-retry-"));
                check("stock-engine-charged-only-gateway-cost",
                    continuationMineralsBefore - continuationMineralsAfter == 100 &&
                    continuationLedger.minerals == state.self.minerals - 100 &&
                    continuationLedger.committedMinerals == 0);
                if (gateway != nullptr) {
                    const auto gatewayQueue = gateway->getTrainingQueue();
                    check("gateway-queue-observed", gatewayQueue.size() == 1 &&
                        gatewayQueue.front() == BWAPI::UnitTypes::Protoss_Zealot);
                } else {
                    check("gateway-queue-observed", false);
                }
                log << "CANNON_BLOCKER_CONTINUATION," << frame << ",issued="
                    << continuationIssued << ",mineralsBefore="
                    << continuationMineralsBefore << ",mineralsAfter="
                    << continuationMineralsAfter << ",ledger="
                    << continuationLedger.minerals << '\n';
                check("engine-postcondition", failures == 0);
                finished = true;
            } else if (frame >= frameLimit) {
                check("cannon-deferred-for-no-placement", false);
                check("probe-command-accepted-during-deferral", cannonBlockerProbeAccepted);
                check("stock-engine-charged-only-probe-cost", cannonBlockerProbeDebitVerified);
                check("engine-postcondition", false);
                finished = true;
            }
            if (finished) {
                log << "CANNON_BLOCKER_RESULT," << frame << ",cannon=" << cannonOutcome
                    << ",probeAccepted=" << cannonBlockerProbeAccepted
                    << ",mineralsBefore=" << cannonBlockerMineralsBefore
                    << ",mineralsAfter=" << cannonBlockerMineralsAfter << '\n';
                log << "DONE," << frame << ',' << failures << '\n';
                log.flush();
                BWAPI::Broodwar->leaveGame();
            }
            return;
        }
        if (scenario == "construction-budget") {
            const auto pylonCount = std::ranges::count_if(state.self.units,
                [](const auto& unit) { return unit.kind == UnitKind::pylon; });
            if (constructionBudgetDeferredFrame < 0) {
                constructionBudgetDeferredFrame = frame;
                constructionBudgetMineralsBefore = state.self.minerals;
                constructionBudgetGasBefore = state.self.gas;
                constructionBudgetPylonsBefore = static_cast<int>(pylonCount);
                StrategicPlan plan;
                plan.goals = {
                    {GoalKind::build, UnitKind::pylon, 3, 140, true, "T118 deferred supply"},
                    {GoalKind::upgrade, UnitKind::unknown, 1, 130, true,
                     "T118 funded upgrade", TechnologyKind::singularityCharge},
                    {GoalKind::train, UnitKind::observer, 1, 120, true, "T118 funded detector"},
                    {GoalKind::train, UnitKind::zealot, 1, 110, true, "T118 funded army"},
                };
                ResourceLedger ledger{state.self.minerals, state.self.gas};
                const auto actions = planner.reconcile(state, plan, ledger);
                check("construction-budget-fixture-funded-actions", actions.size() == 4 &&
                    ledger.committedMinerals == 375 && ledger.committedGas == 225);
                bridge.setSpendingLedger(&ledger);
                const auto issued = bridge.executeMacro(actions, plan, influence, {},
                    std::numeric_limits<int>::max(), nullptr, 0);
                bridge.setSpendingLedger(nullptr);
                int deferred{}, trained{}, upgraded{};
                for (const auto& execution : bridge.macroExecutions()) {
                    log << "CONSTRUCTION_BUDGET_ACTION," << frame << ','
                        << static_cast<int>(execution.action.target) << ','
                        << execution.outcome << ',' << execution.accepted << '\n';
                    deferred += execution.action.target == UnitKind::pylon &&
                        execution.outcome == "planning-budget-deferred" && !execution.accepted;
                    trained += execution.action.action == MacroActionKind::train && execution.accepted;
                    upgraded += execution.action.action == MacroActionKind::upgrade && execution.accepted;
                }
                check("construction-budget-defers-only-construction",
                    issued == 3 && deferred == 1 && trained == 2 && upgraded == 1);
                check("construction-budget-retains-unspent-reservation",
                    ledger.committedMinerals == 100 && ledger.committedGas == 0 &&
                    ledger.reservedMinerals == 100 && ledger.reservedGas == 0 &&
                    ledger.minerals == state.self.minerals - 275 &&
                    BWAPI::Broodwar->self()->minerals() == state.self.minerals - 275 &&
                    BWAPI::Broodwar->self()->gas() == state.self.gas - 225);
                check("construction-budget-deferral-does-not-create-lease-or-blocker",
                    bridge.reservedBuilders().empty() && bridge.buildBlockerFeedback().empty());
                int observedTraining{}, observedUpgrade{};
                for (const auto unit : BWAPI::Broodwar->self()->getUnits()) {
                    if (unit == nullptr || !unit->exists()) continue;
                    const auto queue = unit->getTrainingQueue();
                    if (!queue.empty() && (queue.front() == BWAPI::UnitTypes::Protoss_Zealot ||
                        queue.front() == BWAPI::UnitTypes::Protoss_Observer)) ++observedTraining;
                    if (unit->isUpgrading() &&
                        unit->getUpgrade() == BWAPI::UpgradeTypes::Singularity_Charge) ++observedUpgrade;
                }
                check("construction-budget-native-training-and-upgrade-observed",
                    observedTraining == 2 && observedUpgrade == 1);
                return;
            }
            if (!constructionBudgetRetried && frame >= constructionBudgetDeferredFrame + 12) {
                StrategicPlan plan;
                plan.goals = {{GoalKind::build, UnitKind::pylon, 3, 140, true,
                               "T118 deferred supply"}};
                ResourceLedger ledger{state.self.minerals, state.self.gas};
                const auto actions = planner.reconcile(state, plan, ledger);
                bridge.setSpendingLedger(&ledger);
                const auto issued = bridge.executeMacro(actions, plan, influence);
                bridge.setSpendingLedger(nullptr);
                check("construction-budget-retry-accepted-with-allowance", issued == 1 &&
                    std::ranges::any_of(bridge.macroExecutions(), [](const auto& execution) {
                        return execution.action.target == UnitKind::pylon && execution.accepted;
                    }));
                log << "CONSTRUCTION_BUDGET_RETRY," << frame << ',' << issued << '\n';
                constructionBudgetRetried = true;
                return;
            }
            if (constructionBudgetRetried && pylonCount > constructionBudgetPylonsBefore) {
                check("construction-budget-native-building-started-after-deferral", true);
                check("construction-budget-native-debits-match-reservations",
                    constructionBudgetMineralsBefore - state.self.minerals == 375 &&
                    constructionBudgetGasBefore - state.self.gas == 225);
                finished = true;
            } else if (frame >= constructionBudgetDeferredFrame + 360) {
                check("construction-budget-native-building-started-after-deferral", false);
                finished = true;
            }
            if (finished) {
                log << "DONE," << frame << ',' << failures << '\n';
                log.flush();
                BWAPI::Broodwar->leaveGame();
            }
            return;
        }
        if (scenario == "resource-overlap") {
            constexpr int frameLimit = 360;
            if (resourceOverlapOrdersIssued) {
                const auto mineralsNow = BWAPI::Broodwar->self()->minerals();
                const auto gasNow = BWAPI::Broodwar->self()->gas();
                const auto completedDebit = resourceOverlapMineralsAfterOrders - mineralsNow;
                const auto newPylons = std::ranges::count_if(state.self.units,
                    [](const auto& unit) {
                        return unit.kind == UnitKind::pylon && !unit.completed;
                    });
                if (completedDebit == 100) {
                    check("pending-pylon-debit-observed", newPylons >= 1);
                    check("engine-debits-match-full-ledger", gasNow == resourceOverlapGasAfterOrders &&
                        resourceOverlapMineralsBefore - mineralsNow == 540 &&
                        resourceOverlapGasBefore - gasNow == 275);
                    log << "RESOURCE_OVERLAP_DEBIT," << frame << ",pylon=100,"
                        << "incompletePylons=" << newPylons << ",minerals="
                        << mineralsNow << ",gas=" << gasNow << '\n';
                    log << "RESOURCE_OVERLAP_RESULT," << frame
                        << ",categories=macro+upgrade+detection+model+maintenance"
                        << ",mineralsSpent=" << resourceOverlapMineralsBefore - mineralsNow
                        << ",gasSpent=" << resourceOverlapGasBefore - gasNow << '\n';
                    check("engine-postcondition", failures == 0);
                    finished = true;
                } else if (frame >= frameLimit) {
                    check("pending-pylon-debit-observed", false);
                    check("engine-debits-match-full-ledger", false);
                    check("engine-postcondition", false);
                    finished = true;
                }
                if (finished) {
                    log << "DONE," << frame << ',' << failures << '\n';
                    log.flush();
                    BWAPI::Broodwar->leaveGame();
                }
                return;
            }
            std::vector<BWAPI::Unit> gateways;
            BWAPI::Unit core = nullptr, robotics = nullptr, observatory = nullptr;
            BWAPI::Unit reaver = nullptr, carrier = nullptr;
            for (const auto unit : BWAPI::Broodwar->self()->getUnits()) {
                if (unit == nullptr || !unit->exists()) continue;
                const auto type = unit->getType();
                if (type == BWAPI::UnitTypes::Protoss_Gateway) gateways.push_back(unit);
                if (type == BWAPI::UnitTypes::Protoss_Cybernetics_Core) core = unit;
                if (type == BWAPI::UnitTypes::Protoss_Robotics_Facility) robotics = unit;
                if (type == BWAPI::UnitTypes::Protoss_Observatory) observatory = unit;
                if (type == BWAPI::UnitTypes::Protoss_Reaver) reaver = unit;
                if (type == BWAPI::UnitTypes::Protoss_Carrier) carrier = unit;
            }
            std::sort(gateways.begin(), gateways.end(), [](const auto left, const auto right) {
                return left->getID() < right->getID();
            });
            const auto poweredCompleted = [](const BWAPI::Unit unit) {
                return unit != nullptr && unit->exists() && unit->isCompleted() &&
                       unit->isPowered();
            };
            check("overlap-fixture-two-powered-gateways", gateways.size() == 2 &&
                poweredCompleted(gateways[0]) && poweredCompleted(gateways[1]));
            check("overlap-fixture-powered-tech-producers",
                poweredCompleted(core) && poweredCompleted(robotics) &&
                poweredCompleted(observatory));
            check("overlap-fixture-reaver-and-builder", reaver != nullptr &&
                std::ranges::any_of(state.self.units, [](const auto& unit) {
                    return unit.kind == UnitKind::probe && unit.completed;
                }));
            check("overlap-fixture-carrier-without-interceptors", carrier != nullptr &&
                carrier->getInterceptorCount() == 0);
            check("overlap-fixture-gas-bank", state.self.gas >= 300);

            StrategicPlan plan;
            plan.goals = {
                {GoalKind::build, UnitKind::pylon, 3, 140, true,
                 "T025 supply obligation"},
                {GoalKind::upgrade, UnitKind::unknown, 1, 130, true,
                 "T025 range upgrade", TechnologyKind::singularityCharge},
                {GoalKind::train, UnitKind::observer, 1, 120, true,
                 "T025 mobile detection"},
                {GoalKind::train, UnitKind::zealot, 1, 110, true,
                 "T025 native macro production"},
            };
            ResourceLedger ledger{state.self.minerals, state.self.gas};
            const auto actions = planner.reconcile(state, plan, ledger);
            check("macro-obligations-reserve-shared-bank",
                ledger.reservedMinerals == 375 && ledger.reservedGas == 225 &&
                ledger.committedMinerals == 375 && ledger.committedGas == 225);
            const auto mineralsBefore = BWAPI::Broodwar->self()->minerals();
            const auto gasBefore = BWAPI::Broodwar->self()->gas();
            int supplyBuilderId = -1;
            bridge.actionDiagnostic = [this, &supplyBuilderId](
                const protodd::bwapi::ActionDiagnostic& d) {
                if (!d.source.starts_with("T025") && d.source != "production-demand" &&
                    !d.source.starts_with("maintenance-")) return;
                log << "OVERLAP_ACTION," << d.actor << ',' << d.type << ',' << d.source
                    << ',' << d.outcome << ',' << d.attempted << ',' << d.accepted << '\n';
                log.flush();
                if (d.source == "T025 supply obligation" && d.accepted)
                    supplyBuilderId = d.actor;
            };
            bridge.setSpendingLedger(&ledger);
            const auto macroIssued = bridge.executeMacro(actions, plan, influence);
            bool supplyAccepted = false, upgradeAccepted = false;
            bool observerAccepted = false, zealotAccepted = false;
            for (const auto& execution : bridge.macroExecutions()) {
                if (execution.action.target == UnitKind::pylon)
                    supplyAccepted = execution.accepted;
                if (execution.action.technology == TechnologyKind::singularityCharge)
                    upgradeAccepted = execution.accepted;
                if (execution.action.target == UnitKind::observer)
                    observerAccepted = execution.accepted;
                if (execution.action.target == UnitKind::zealot)
                    zealotAccepted = execution.accepted;
            }
            const auto mineralsAfterMacro = BWAPI::Broodwar->self()->minerals();
            const auto gasAfterMacro = BWAPI::Broodwar->self()->gas();
            const auto pylonExecution = std::ranges::find_if(bridge.macroExecutions(),
                [](const protodd::bwapi::MacroExecution& execution) {
                    return execution.action.target == UnitKind::pylon;
                });
            const auto pylonPendingUnpaid = pylonExecution != bridge.macroExecutions().end() &&
                pylonExecution->accepted &&
                !bridge.pendingBuildAlreadyPaid(pylonExecution->action);
            const auto supplyBuilder = supplyBuilderId >= 0
                ? BWAPI::Broodwar->getUnit(supplyBuilderId) : nullptr;
            const auto supplyBuilderCommand = supplyBuilder != nullptr
                ? supplyBuilder->getLastCommand() : BWAPI::UnitCommand{};
            const auto supplyBuilderAssigned = supplyBuilder != nullptr &&
                supplyBuilder->exists() &&
                ((supplyBuilder->isConstructing() &&
                  supplyBuilder->getBuildType() == BWAPI::UnitTypes::Protoss_Pylon) ||
                 (supplyBuilderCommand.getType() == BWAPI::UnitCommandTypes::Build &&
                  supplyBuilderCommand.getUnitType() == BWAPI::UnitTypes::Protoss_Pylon));
            check("macro-supply-upgrade-detection-and-unit-accepted",
                macroIssued == 4 && supplyAccepted && upgradeAccepted &&
                observerAccepted && zealotAccepted);
            check("accepted-pylon-awaits-native-debit",
                pylonPendingUnpaid && supplyBuilderAssigned);
            log << "OVERLAP_PYLON_PENDING," << supplyBuilderId << ",paid="
                << !pylonPendingUnpaid << ",constructing="
                << (supplyBuilder != nullptr && supplyBuilder->isConstructing())
                << ",buildType=" << (supplyBuilder != nullptr
                    ? supplyBuilder->getBuildType().getName() : "missing")
                << ",lastCommand=" << supplyBuilderCommand.getType().toString()
                << ",lastUnitType=" << supplyBuilderCommand.getUnitType().getName() << '\n';
            check("macro-native-debits-and-ledger-charges-accounted",
                mineralsBefore - mineralsAfterMacro == 275 &&
                gasBefore - gasAfterMacro == 225 &&
                ledger.minerals == state.self.minerals - 375 &&
                ledger.gas == state.self.gas - 225 &&
                ledger.committedMinerals == 0 && ledger.committedGas == 0);
            check("upgrade-and-detector-orders-observed",
                core != nullptr && core->isUpgrading() && robotics != nullptr &&
                robotics->getTrainingQueue().size() == 1 &&
                robotics->getTrainingQueue().front() == BWAPI::UnitTypes::Protoss_Observer);
            check("native-gateway-order-observed",
                gateways.size() == 2 && gateways[0]->getTrainingQueue().size() == 1 &&
                gateways[0]->getTrainingQueue().front() == BWAPI::UnitTypes::Protoss_Zealot);

            const auto ledgerMineralsBeforeReject = ledger.minerals;
            const auto ledgerGasBeforeReject = ledger.gas;
            const auto engineMineralsBeforeReject = BWAPI::Broodwar->self()->minerals();
            const auto engineGasBeforeReject = BWAPI::Broodwar->self()->gas();
            auto injectedModelRejection = false;
            bridge.productionPermission = [&injectedModelRejection, &gateways](
                const BWAPI::UnitCommand& command, const std::string_view source) {
                if (!injectedModelRejection && source == "production-demand" &&
                    command.getType() == BWAPI::UnitCommandTypes::Train &&
                    command.getUnit() == gateways[1] &&
                    command.getUnitType() == BWAPI::UnitTypes::Protoss_Dragoon) {
                    injectedModelRejection = true;
                    return false;
                }
                return true;
            };
            const auto rejectedModelOrder = !bridge.executeProduction(
                BWAPI::UnitCommand::train(gateways[1], BWAPI::UnitTypes::Protoss_Dragoon));
            const auto rejectionWasFree = ledger.minerals == ledgerMineralsBeforeReject &&
                ledger.gas == ledgerGasBeforeReject &&
                BWAPI::Broodwar->self()->minerals() == engineMineralsBeforeReject &&
                BWAPI::Broodwar->self()->gas() == engineGasBeforeReject;
            bridge.productionPermission = {};
            const auto modelAccepted = bridge.executeProduction(
                BWAPI::UnitCommand::train(gateways[1], BWAPI::UnitTypes::Protoss_Dragoon));
            check("model-path-rejection-does-not-charge", injectedModelRejection &&
                rejectedModelOrder && rejectionWasFree);
            check("model-path-production-accepted",
                modelAccepted && gateways[1]->getTrainingQueue().size() == 1 &&
                gateways[1]->getTrainingQueue().front() == BWAPI::UnitTypes::Protoss_Dragoon);
            check("model-path-production-cost-shared",
                engineMineralsBeforeReject - BWAPI::Broodwar->self()->minerals() == 125 &&
                engineGasBeforeReject - BWAPI::Broodwar->self()->gas() == 50 &&
                ledger.minerals == ledgerMineralsBeforeReject - 125 &&
                ledger.gas == ledgerGasBeforeReject - 50);

            const auto maintenanceMineralsBefore = BWAPI::Broodwar->self()->minerals();
            const auto maintenanceGasBefore = BWAPI::Broodwar->self()->gas();
            auto scarabAccepted = false, interceptorAccepted = false;
            bridge.actionDiagnostic = [this, &scarabAccepted, &interceptorAccepted](
                const protodd::bwapi::ActionDiagnostic& d) {
                if (d.source.starts_with("maintenance-")) {
                    log << "ISSUER_ACTION," << BWAPI::Broodwar->getFrameCount() << ','
                        << d.actor << ',' << d.target << ',' << d.type << ',' << d.source
                        << ',' << d.outcome << ',' << d.attempted << ',' << d.accepted << '\n';
                    log.flush();
                }
                if (d.source == "maintenance-scarab" && d.accepted)
                    scarabAccepted = true;
                if (d.source == "maintenance-interceptor" && d.accepted)
                    interceptorAccepted = true;
            };
            CommandBus maintenanceCommands;
            maintenanceCommands.beginFrame(frame, state.latencyFrames);
            bridge.runMaintenance(StrategicPlan{}, maintenanceCommands);
            for (const auto& command : maintenanceCommands.finalize())
                static_cast<void>(bridge.execute(command));
            bridge.actionDiagnostic = {};
            const auto maintenanceMineralsAfter = BWAPI::Broodwar->self()->minerals();
            const auto maintenanceGasAfter = BWAPI::Broodwar->self()->gas();
            bool scarabQueued = false;
            if (reaver != nullptr) {
                const auto scarabQueue = reaver->getTrainingQueue();
                scarabQueued = scarabQueue.size() == 1 &&
                    scarabQueue.front() == BWAPI::UnitTypes::Protoss_Scarab;
            }
            bool interceptorQueued = false;
            if (carrier != nullptr) {
                const auto interceptorQueue = carrier->getTrainingQueue();
                interceptorQueued = interceptorQueue.size() == 1 &&
                    interceptorQueue.front() == BWAPI::UnitTypes::Protoss_Interceptor;
            }
            check("maintenance-scarab-accepted", scarabAccepted && scarabQueued);
            check("maintenance-interceptor-accepted", interceptorAccepted &&
                interceptorQueued);
            check("maintenance-spend-shares-bank",
                maintenanceMineralsBefore - maintenanceMineralsAfter == 40 &&
                maintenanceGasBefore == maintenanceGasAfter &&
                ledger.minerals == state.self.minerals - 540 &&
                ledger.gas == state.self.gas - 275 &&
                ledger.reservedMinerals == 0 && ledger.reservedGas == 0);
            check("unpaid-pylon-cost-remains-accounted",
                pylonPendingUnpaid &&
                state.self.minerals - BWAPI::Broodwar->self()->minerals() + 100 ==
                    state.self.minerals - ledger.minerals);
            log << "RESOURCE_OVERLAP_ORDERS," << frame << ",macro=" << macroIssued
                << ",supply=" << supplyAccepted << ",upgrade=" << upgradeAccepted
                << ",observer=" << observerAccepted << ",zealot=" << zealotAccepted
                << ",modelRejected=" << rejectedModelOrder << ",model=" << modelAccepted
                << ",scarab=" << scarabAccepted << ",interceptor=" << interceptorAccepted
                << ",mineralsSpent="
                << mineralsBefore - BWAPI::Broodwar->self()->minerals()
                << ",gasSpent=" << gasBefore - BWAPI::Broodwar->self()->gas() << '\n';
            bridge.productionPermission = {};
            bridge.setSpendingLedger(nullptr);
            resourceOverlapOrdersIssued = true;
            resourceOverlapMineralsBefore = mineralsBefore;
            resourceOverlapMineralsAfterOrders = BWAPI::Broodwar->self()->minerals();
            resourceOverlapGasBefore = gasBefore;
            resourceOverlapGasAfterOrders = BWAPI::Broodwar->self()->gas();
            log << "RESOURCE_OVERLAP_WAITING," << frame << ",minerals="
                << resourceOverlapMineralsAfterOrders << ",gas="
                << resourceOverlapGasAfterOrders << ",pendingPylonCost=100\n";
            log.flush();
            return;
        }
        if (scenario == "observer-safety") {
            log << "OBSERVER_STATE," << frame << ",selfCount=" << state.self.units.size()
                << ",enemyCount=" << state.enemy.units.size() << '\n';
            for (const auto unit : BWAPI::Broodwar->getAllUnits()) {
                if (unit == nullptr || !unit->exists()) continue;
                log << "OBSERVER_NATIVE," << unit->getID() << ','
                    << unit->getType().getName() << ",owner="
                    << (unit->getPlayer() != nullptr ? unit->getPlayer()->getID() : -1)
                    << ",visible=" << unit->isVisible() << ",completed="
                    << unit->isCompleted() << ",position=" << unit->getPosition().x
                    << 'x' << unit->getPosition().y << '\n';
            }
            for (const auto& unit : state.self.units)
                log << "OBSERVER_SELF," << unit.id << ",kind="
                    << static_cast<int>(unit.kind) << ",completed=" << unit.completed << '\n';
            for (const auto& unit : state.enemy.units)
                log << "OBSERVER_ENEMY," << unit.id << ",kind="
                    << static_cast<int>(unit.kind) << ",visible=" << unit.visible << '\n';
            const auto observer = std::ranges::find_if(state.self.units, [](const auto& unit) {
                return unit.kind == UnitKind::observer && unit.completed;
            });
            const auto wraith = std::ranges::find_if(state.enemy.units, [](const auto& unit) {
                return unit.kind == UnitKind::wraith && unit.completed;
            });
            check("observer-safety-fixture", observer != state.self.units.end());
            check("observer-safety-threat-fixture", wraith != state.enemy.units.end());
            if (observer != state.self.units.end() && wraith != state.enemy.units.end()) {
                check("observer-exposed-to-visible-wraith",
                    wraith->visible &&
                    ScoutManager::observerInDanger(state, *observer, influence));
                ScoutManager observerSafety;
                const auto orders = observerSafety.protectObservers(state, influence);
                const auto escape = std::ranges::find_if(orders, [](const Command& command) {
                    return command.source == "observer-evade" &&
                           command.type == CommandType::move;
                });
                check("observer-safety-produces-escape", escape != orders.end() &&
                    escape->actor == observer->id && escape->targetPosition.valid());
                if (escape != orders.end()) {
                    const auto acceptedEscape = bridge.execute(*escape);
                    const auto nativeObserver = BWAPI::Broodwar->getUnit(observer->id);
                    const auto nativeCommand = nativeObserver != nullptr
                        ? nativeObserver->getLastCommand() : BWAPI::UnitCommand{};
                    check("observer-safety-order-accepted",
                        acceptedEscape && nativeCommand.getType() == BWAPI::UnitCommandTypes::Move &&
                        nativeCommand.getTargetPosition() == BWAPI::Position(
                            escape->targetPosition.x, escape->targetPosition.y));
                    log << "ISSUER_ORDER," << frame << ",observer-safety,actor="
                        << observer->id << ",wraith=" << wraith->id << ",source="
                        << escape->source << ",x=" << escape->targetPosition.x
                        << ",y=" << escape->targetPosition.y << ",accepted="
                        << acceptedEscape << '\n';
                }
            }
            check("engine-postcondition", failures == 0);
            log << "DONE," << frame << ',' << failures << '\n';
            log.flush();
            finished = true;
            BWAPI::Broodwar->leaveGame();
            return;
        }
        if (scenario == "command-suppression") {
            constexpr int frameLimit = 1800;
            const auto actor = commandActor >= 0 ? BWAPI::Broodwar->getUnit(commandActor) : nullptr;
            const auto target = commandTarget >= 0 ? BWAPI::Broodwar->getUnit(commandTarget) : nullptr;
            if (!commandSuppressionStarted) {
                std::vector<BWAPI::Unit> zealots, marines;
                for (const auto unit : BWAPI::Broodwar->self()->getUnits()) {
                    if (unit != nullptr && unit->exists() &&
                        unit->getType() == BWAPI::UnitTypes::Protoss_Zealot)
                        zealots.push_back(unit);
                }
                for (const auto unit : BWAPI::Broodwar->getAllUnits()) {
                    if (unit != nullptr && unit->exists() &&
                        unit->getPlayer() != BWAPI::Broodwar->self() &&
                        unit->getType() == BWAPI::UnitTypes::Terran_Marine)
                        marines.push_back(unit);
                }
                check("suppression-fixture-one-zealot-one-marine",
                    zealots.size() == 1 && marines.size() == 1);
                if (zealots.size() == 1 && marines.size() == 1) {
                    commandActor = zealots.front()->getID();
                    commandTarget = marines.front()->getID();
                    commandLatency = BWAPI::Broodwar->getLatencyFrames();
                    check("suppression-fixture-target-visible-and-out-of-range",
                        marines.front()->isVisible() && marines.front()->getHitPoints() <= 10 &&
                        zealots.front()->getDistance(marines.front()) > 32);
                    commandAttack = {commandActor, CommandType::attackUnit, commandTarget,
                        {-1, -1}, UnitKind::unknown, 80, frame, "T014-engine-combat-attack"};
                    const auto acceptedAttack = bridge.execute(commandAttack);
                    check("engine-attack-order-accepted", acceptedAttack);
                    if (acceptedAttack) {
                        suppressionBus.clear();
                        suppressionBus.beginFrame(frame, commandLatency);
                        suppressionBus.markIssued(commandAttack);
                        commandSuppressionStarted = true;
                        log << "T014_START," << frame << ",latency=" << commandLatency
                            << ",actor=" << commandActor << ",target=" << commandTarget
                            << ",targetHp=" << marines.front()->getHitPoints() << '\n';
                    }
                }
            } else if (target == nullptr || !target->exists()) {
                if (!commandTargetDied) {
                    commandTargetDied = true;
                    check("engine-target-died", true);
                    check("dead-target-not-active", !bridge.commandActive(commandAttack));
                    const auto staleAccepted = bridge.execute(commandAttack);
                    commandStaleTargetRejected = !staleAccepted;
                    check("dead-target-order-rejected", commandStaleTargetRejected);
                    commandMove = {commandActor, CommandType::move, -1,
                        {1600, 1200}, UnitKind::unknown, 50, frame,
                        "T014-engine-interrupted-move"};
                    const auto acceptedMove = bridge.execute(commandMove);
                    commandMoveIssued = acceptedMove;
                    commandMoveIssuedFrame = frame;
                    check("interrupted-move-order-accepted", acceptedMove);
                    if (acceptedMove) {
                        suppressionBus.beginFrame(frame, commandLatency);
                        suppressionBus.markIssued(commandMove);
                        log << "T014_MOVE," << frame << ",latency=" << commandLatency
                            << ",actor=" << commandActor << ",x=" << commandMove.targetPosition.x
                            << ",y=" << commandMove.targetPosition.y << '\n';
                    }
                }
            } else if (!commandAttackActive && bridge.commandActive(commandAttack)) {
                commandAttackActive = true;
                check("engine-attack-order-active", true);
                auto activeAttack = commandAttack;
                activeAttack.alreadyActive = bridge.commandActive(commandAttack);
                suppressionBus.beginFrame(frame, commandLatency);
                suppressionBus.submit(activeAttack);
                const auto selectedAttack = suppressionBus.finalize();
                commandActiveSuppressionChecked = selectedAttack.empty();
                check("active-attack-suppressed-by-command-bus",
                    commandActiveSuppressionChecked && suppressionBus.stats().redundant == 1);
                log << "T014_ATTACK_ACTIVE," << frame << ",order="
                    << actor->getOrder().toString() << ",latency=" << commandLatency
                    << ",busSelected=" << selectedAttack.size() << '\n';
            }

            if (commandMoveIssued) {
                const auto age = frame - commandMoveIssuedFrame;
                const auto moveActive = bridge.commandActive(commandMove);
                if (!commandMoveInterrupted) {
                    if (!commandMoveActive && moveActive) {
                        commandMoveActive = true;
                        check("engine-move-order-active", true);
                        Command interrupt{commandActor, CommandType::stop, -1, {-1, -1},
                            UnitKind::unknown, 100, frame, "T014-engine-interrupt"};
                        const auto interrupted = bridge.execute(interrupt);
                        commandMoveInterrupted = interrupted && !bridge.commandActive(commandMove);
                        check("move-interrupted-and-no-longer-active", commandMoveInterrupted);
                        log << "T014_INTERRUPTED," << frame << ",accepted=" << interrupted
                            << ",activeAfter=" << bridge.commandActive(commandMove) << '\n';
                    }
                }
                const auto suppressionWindow = std::max(24, commandLatency + 1);
                if (commandMoveInterrupted && !commandCutoffChecked && age >= suppressionWindow) {
                    auto cutoffMove = commandMove;
                    cutoffMove.alreadyActive = bridge.commandActive(commandMove);
                    // StarCraft may advance several frames between callbacks. Sample the
                    // exact logical boundary while the live fixture supplies its latency.
                    suppressionBus.beginFrame(commandMoveIssuedFrame + suppressionWindow,
                        commandLatency);
                    suppressionBus.submit(cutoffMove);
                    const auto selectedAtCutoff = suppressionBus.finalize();
                    commandCutoffChecked = selectedAtCutoff.empty() &&
                        suppressionBus.stats().redundant == 1;
                    check("move-suppressed-through-configured-cutoff", commandCutoffChecked);
                    log << "T014_CUTOFF," << frame << ",observedAge=" << age
                        << ",sampleAge=" << suppressionWindow << ",window=" << suppressionWindow << ",selected="
                        << selectedAtCutoff.size() << '\n';
                }
                if (commandMoveInterrupted && commandCutoffChecked && age > suppressionWindow) {
                    auto retryMove = commandMove;
                    retryMove.alreadyActive = bridge.commandActive(commandMove);
                    suppressionBus.beginFrame(commandMoveIssuedFrame + suppressionWindow + 1,
                        commandLatency);
                    suppressionBus.submit(retryMove);
                    const auto selectedRetry = suppressionBus.finalize();
                    const auto retried = selectedRetry.size() == 1 &&
                        bridge.execute(selectedRetry.front());
                    check("move-retry-released-after-cutoff", retried);
                    check("engine-postcondition", failures == 0);
                    log << "T014_RETRY," << frame << ",age=" << age
                        << ",window=" << suppressionWindow << ",selected="
                        << selectedRetry.size() << ",accepted=" << retried << '\n';
                    log << "DONE," << frame << ',' << failures << '\n';
                    log.flush();
                    finished = true;
                    BWAPI::Broodwar->leaveGame();
                    return;
                }
            }
            if (frame >= frameLimit) {
                check("engine-target-died", commandTargetDied);
                check("engine-attack-order-active", commandAttackActive);
                check("active-attack-suppressed-by-command-bus", commandActiveSuppressionChecked);
                check("dead-target-order-rejected", commandStaleTargetRejected);
                check("interrupted-move-order-accepted", commandMoveIssued);
                check("engine-move-order-active", commandMoveActive);
                check("move-interrupted-and-no-longer-active", commandMoveInterrupted);
                check("move-suppressed-through-configured-cutoff", commandCutoffChecked);
                check("engine-postcondition", false);
                log << "DONE," << frame << ',' << failures << '\n';
                log.flush();
                finished = true;
                BWAPI::Broodwar->leaveGame();
            }
            return;
        }
        if (!checked) {
            checked=true;
            log << "STATE," << frame << ',' << state.self.units.size() << ',' << state.enemy.units.size()
                << ',' << state.self.minerals << ',' << state.self.gas << '\n';
            for(auto u:BWAPI::Broodwar->getAllUnits()) if(u->exists())
                log << "UNIT," << u->getID() << ',' << u->getType().getName() << ',' << u->getPlayer()->getID()
                    << ',' << u->getPosition().x << ',' << u->getPosition().y << ',' << u->isPowered()
                    << ',' << u->getEnergy() << ',' << u->getHitPoints() << ',' << u->isInvincible() << '\n';
            if(scenario.starts_with("storm")) {
                std::vector<UnitSnapshot> squad;
                for(const auto& u:state.self.units) if(u.kind==UnitKind::highTemplar) squad.push_back(u);
                check("fixture-templar",squad.size()==1);
                check("fixture-enemies",state.enemy.units.size()>=4);
                check("fixture-researched",BWAPI::Broodwar->self()->hasResearched(BWAPI::TechTypes::Psionic_Storm));
                if(!squad.empty()) { selected=squad[0].id; initialEnergy=squad[0].energy; }
                CombatEstimate estimate; estimate.decision=FightDecision::engage;
                const auto planningAllies=scenario=="storm-allies"
                    ? std::span<const UnitSnapshot>{squad}
                    : std::span<const UnitSnapshot>{state.self.units};
                auto commands=TacticalController{}.control(squad,state.enemy.units,estimate,{1100,1000},
                    {300,1000},influence,{800,1000},3,true,{},TacticalIntent::battle,{},nullptr,planningAllies);
                for(const auto& c:commands) if(c.technology==TechnologyKind::psionicStorm) {
                    requested=true; accepted=bridge.execute(c);
                    log << "CAST," << c.targetPosition.x << ',' << c.targetPosition.y << ',' << accepted << '\n';
                    if(scenario=="storm-allies")
                        check("live-exposure-invalidates-stale-cast",!accepted);
                    else
                        check("cast-accepted",accepted);
                    const auto reservations=bridge.reservedStormZones(frame);
                    const auto zoneRecorded=std::ranges::any_of(reservations,[&c](const Position center) {
                        return distanceSquared(center,c.targetPosition)<=8*8;
                    });
                    check("only-accepted-storm-reserves-zone",
                        zoneRecorded==(scenario=="storm-clear" && accepted));
                }
                check("storm-candidate",requested);
                if(scenario=="storm-allies" && requested && !accepted) confirmed=true;
            } else if(scenario=="combat") {
                auto actor=std::ranges::find_if(state.self.units,[](const auto& u){return u.kind==UnitKind::zealot;});
                std::vector<UnitSnapshot> targets;
                for(const auto& u:state.enemy.units) if(u.kind==UnitKind::marine) targets.push_back(u);
                check("fixture-melee",actor!=state.self.units.end() && targets.size()==2);
                check("fixture-invincible",std::ranges::any_of(targets,[](const auto& u){return u.invincible;}));
                if(actor!=state.self.units.end()) {
                    auto target=CombatEvaluator{}.selectTarget(*actor,targets);
                    check("melee-legal-target",target!=nullptr && !target->invincible);
                }
                const auto rawEnemyUnits = BWAPI::Broodwar->enemy()->getUnits();
                const auto rawDt = std::ranges::find_if(
                    rawEnemyUnits, [](const BWAPI::Unit unit) {
                        return unit != nullptr && unit->exists() &&
                            unit->getType() == BWAPI::UnitTypes::Protoss_Dark_Templar &&
                            unit->isVisible() && !unit->isDetected();
                    });
                check("fixture-cloaked-visible",rawDt!=rawEnemyUnits.end());
                if(rawDt!=rawEnemyUnits.end()) {
                    // BWAPI's visible-but-undetected sentinel must not refresh
                    // a bot snapshot with live type, position, health or order data.
                    const auto leaked = std::ranges::any_of(
                        state.enemy.units, [rawDt](const UnitSnapshot& unit) {
                            return unit.id == (*rawDt)->getID();
                        });
                    check("hidden-unit-not-published",!leaked);
                    check("hidden-unit-no-threat-footprint",
                          influence.at(Position{(*rawDt)->getPosition().x,
                                                (*rawDt)->getPosition().y}).groundThreat==0);
                    log << "HIDDEN_DT," << (*rawDt)->isVisible() << ','
                        << (*rawDt)->isDetected() << '\n';
                }
            } else if(scenario=="producer") {
                check("fixture-funded",state.self.minerals>=100 && state.self.gas>=100);
                std::vector<BWAPI::Unit> forges;
                for(auto u:BWAPI::Broodwar->self()->getUnits()) if(u->getType()==BWAPI::UnitTypes::Protoss_Forge) forges.push_back(u);
                std::sort(forges.begin(),forges.end(),[](auto a,auto b){return a->getID()<b->getID();});
                check("fixture-forges",forges.size()==2);
                if(forges.size()==2) {
                    check("fixture-first-unpowered",!forges[0]->isPowered());
                    check("fixture-second-legal",forges[1]->isPowered() && forges[1]->canUpgrade(BWAPI::UpgradeTypes::Protoss_Ground_Armor));
                    log << "ENGINE_CAN_UPGRADE_UNPOWERED," << forges[0]->canUpgrade(BWAPI::UpgradeTypes::Protoss_Ground_Armor) << '\n';
                    selected=forges[1]->getID();
                }
                MacroAction action{MacroActionKind::upgrade,UnitKind::unknown,100,100,100,true,"fixture",TechnologyKind::protossGroundArmor};
                StrategicPlan plan;
                accepted=bridge.executeMacro(std::span{&action,1},plan,influence)>0;
                check("legal-producer-accepted",accepted);
            } else if(scenario=="prerequisite") {
                check("fixture-level-one",BWAPI::Broodwar->self()->getUpgradeLevel(BWAPI::UpgradeTypes::Protoss_Ground_Weapons)==1);
                bool legal=false;for(auto u:BWAPI::Broodwar->self()->getUnits()) if(u->canUpgrade(BWAPI::UpgradeTypes::Protoss_Ground_Weapons))legal=true;
                check("fixture-upgrade-illegal",!legal);
                StrategicPlan plan;
                plan.goals={{GoalKind::upgrade,UnitKind::unknown,2,100,true,"fixture",TechnologyKind::protossGroundWeapons}};
                ResourceLedger ledger{state.self.minerals,state.self.gas};
                auto actions=planner.reconcile(state,plan,ledger);
                check("archives-first",!actions.empty() && actions[0].target==UnitKind::templarArchives && actions[0].reserved);
                accepted=bridge.executeMacro(actions,plan,influence)>0;
                check("archives-order-accepted",accepted);
                for(const auto& e:bridge.macroExecutions())log << "MACRO," << e.outcome << ',' << e.accepted << '\n';
            } else if(scenario=="supply-anchor") {
                std::vector<BWAPI::Unit> pylons;
                for(auto u:BWAPI::Broodwar->self()->getUnits())
                    if(u->getType()==BWAPI::UnitTypes::Protoss_Pylon) pylons.push_back(u);
                for(auto u:pylons) initialPylonIds.push_back(u->getID());
                check("fixture-nexus",std::ranges::any_of(state.self.units,[](const auto& u){return u.kind==UnitKind::nexus;}));
                check("fixture-probe",std::ranges::any_of(state.self.units,[](const auto& u){return u.kind==UnitKind::probe;}));
                StrategicPlan plan;
                plan.rallyPoint={3456,1344};
                plan.expansionTarget=plan.rallyPoint;
                MacroAction action{MacroActionKind::build,UnitKind::pylon,130,100,0,true,
                    "operational supply invariant"};
                accepted=bridge.executeMacro(std::span{&action,1},plan,influence)>0;
                check("supply-command-accepted",accepted);
                check("supply-placement-diagnostic",supplyPylonTarget.valid());
                if(supplyPylonTarget.valid()) {
                    const auto home=protodd::Position{512,528};
                    check("supply-placement-home",
                        distanceSquared(supplyPylonTarget,home)<320*320);
                    check("supply-placement-not-natural",
                        distanceSquared(supplyPylonTarget,home)<
                        distanceSquared(supplyPylonTarget,plan.expansionTarget));
                }
            } else if(scenario=="power-recovery") {
                const auto unpowered = std::ranges::count_if(state.self.units,
                    [](const auto& u) {
                        return u.completed && isBuilding(u.kind) &&
                            unitStats(u.kind).requiresPsi && !u.powered;
                    });
                check("fixture-no-pylons",std::ranges::none_of(state.self.units,
                    [](const auto& u){return u.kind==UnitKind::pylon;}));
                check("fixture-unpowered-buildings",unpowered>=3);
                auto plan=strategy.plan(state,ThreatAssessment{});
                const auto recoveryGoals=std::ranges::count_if(plan.goals,
                    [](const auto& goal) {
                        return goal.goal==GoalKind::build && goal.target==UnitKind::pylon &&
                            goal.reason=="restore power to disabled production" &&
                            goal.constructionSite.valid();
                    });
                check("site-scoped-recovery-goals",recoveryGoals>=1);
                ResourceLedger ledger{state.self.minerals,state.self.gas};
                auto actions=planner.reconcile(state,plan,ledger);
                accepted=bridge.executeMacro(actions,plan,influence)>0;
                check("power-recovery-command-accepted",accepted);
                log << "POWERRECOVERY," << frame << ",unpowered=" << unpowered
                    << ",goals=" << recoveryGoals << ",accepted=" << accepted << '\n';
            }
            log.flush();
        }
        if(scenario=="prerequisite") {
            for(auto u:BWAPI::Broodwar->self()->getUnits()) if(u->getType()==BWAPI::UnitTypes::Protoss_Templar_Archives) confirmed=true;
        } else if(scenario=="producer") {
            auto u=BWAPI::Broodwar->getUnit(selected); if(u && u->isUpgrading()) confirmed=true;
        } else if(scenario=="storm-clear") {
            auto u=BWAPI::Broodwar->getUnit(selected); if(u && u->getEnergy()<initialEnergy-50)confirmed=true;
        } else if(scenario=="storm-allies") {
            confirmed=requested && !accepted;
        } else if(scenario=="supply-anchor") {
            for(auto u:BWAPI::Broodwar->self()->getUnits()) {
                if(u==nullptr || !u->exists() || u->getType()!=BWAPI::UnitTypes::Protoss_Pylon ||
                   std::ranges::find(initialPylonIds,u->getID())!=initialPylonIds.end()) continue;
                if(supplyPylonTarget.valid() &&
                   distanceSquared(Position{u->getPosition().x,u->getPosition().y},
                                   supplyPylonTarget)<=96*96) confirmed=true;
            }
        } else if(scenario=="power-recovery") {
            auto plan=strategy.plan(state,ThreatAssessment{});
            ResourceLedger ledger{state.self.minerals,state.self.gas};
            auto actions=planner.reconcile(state,plan,ledger);
            const auto issued=bridge.executeMacro(actions,plan,influence);
            const auto remaining=std::ranges::count_if(state.self.units,
                [](const auto& u) {
                    return u.completed && isBuilding(u.kind) &&
                        unitStats(u.kind).requiresPsi && !u.powered;
                });
            const auto completedPylons=std::ranges::count_if(state.self.units,
                [](const auto& u){return u.kind==UnitKind::pylon && u.completed;});
            confirmed=remaining==0 && completedPylons>0;
            if(frame%96==0 || confirmed) {
                log << "POWERRECOVERY," << frame << ",unpowered=" << remaining
                    << ",completedPylons=" << completedPylons << ",issued=" << issued << '\n';
                for(const auto& execution:bridge.macroExecutions())
                    if(execution.action.target==UnitKind::pylon)
                        log << "RECOVERYACTION," << frame << ','
                            << execution.outcome << ',' << execution.accepted
                            << ",site=" << execution.action.constructionSite.anchor.x
                            << 'x' << execution.action.constructionSite.anchor.y
                            << ",reason=" << execution.action.reason << '\n';
                for(const auto& u:state.self.units) {
                    if(u.kind==UnitKind::pylon ||
                       (isBuilding(u.kind) && unitStats(u.kind).requiresPsi))
                        log << "POWERUNIT," << frame << ',' << u.id << ','
                            << unitStats(u.kind).name << ',' << u.position.x << ','
                            << u.position.y << ',' << u.completed << ',' << u.powered << '\n';
                }
            }
        } else confirmed=!requested;
        const int limit=scenario=="prerequisite"?720:
            scenario=="power-recovery"?3600:120;
        if(frame>=limit) {
            finished=true;
            check("engine-postcondition",confirmed);
            log << "DONE," << frame << ',' << failures << '\n'; log.flush();
            BWAPI::Broodwar->leaveGame();
        }
    }
};
extern "C" __declspec(dllexport) void gameInit(BWAPI::Game* game){BWAPI::BroodwarPtr=game;}
extern "C" __declspec(dllexport) BWAPI::AIModule* newAIModule(){return new AuditScenario;}
BOOL APIENTRY DllMain(HMODULE,DWORD,LPVOID){return TRUE;}
