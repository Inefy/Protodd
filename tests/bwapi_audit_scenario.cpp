// Isolated UMS validation module. Never packaged as the playing bot.
#include "BwapiBridge.hpp"
#include "protodd/Technology.hpp"
#include "protodd/Strategy.hpp"
#include "protodd/UnitCatalog.hpp"
#include "protodd/Workers.hpp"
#include <BWAPI.h>
#include <windows.h>
#include <fstream>
#include <algorithm>
#include <limits>

using namespace protodd;
class AuditScenario final : public BWAPI::AIModule {
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
    bool t021Started{}, t021StopRejected{}, t021StopAccepted{}, t021Finished{};
    bool t034Started{}, t034ReplacementChecked{}, t034Finished{};
    int t034Builder{-1}, t034BuildCommands{}, t034AcceptedStops{}, t034AcceptedMoves{};
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
    protodd::Position supplyPylonTarget{-1, -1};
    std::vector<int> initialPylonIds;
    void check(const char* name, bool value) {
        log << "CHECK," << name << ',' << value << '\n';
        if (!value) ++failures;
        log.flush();
    }
public:
    void onStart() override {
        std::ifstream("bwapi-data/read/scenario.txt") >> scenario;
        log.open("bwapi-data/write/scenario.csv");
        log << "START," << scenario << ',' << BWAPI::Broodwar->mapFileName() << '\n'; log.flush();
        BWAPI::Broodwar->setLocalSpeed(0);
        BWAPI::Broodwar->setFrameSkip(
            scenario == "pylon-loss" || scenario == "cannon-blocker" ||
                    scenario == "resource-overlap" || scenario == "worker-issuer" ||
                    scenario == "build-cancel" || scenario == "builder-evacuation" ||
                    scenario == "worker-local-defense" ||
                    scenario == "worker-mining" || scenario == "scout-issuer" ||
                    scenario == "whole-game-issuer" ||
                    scenario == "command-suppression" ||
                    scenario == "observer-safety" ? 1 : 64);
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
    void onEnd(bool won) override { log << "END," << BWAPI::Broodwar->getFrameCount() << ',' << won << '\n'; log.flush(); ExitProcess(0); }
    void onFrame() override {
        if(finished) return;
        const int frame=BWAPI::Broodwar->getFrameCount();
        if(frame<2) { log << "FRAME," << frame << ',' << BWAPI::Broodwar->isPaused() << '\n'; log.flush(); }
        if (frame<24 && scenario!="pylon-loss") return;
        auto state=bridge.observe();
        InfluenceMap influence; influence.update(state);
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
                    check("T021-lease-released-after-old-order-cleared",
                        acknowledged && reservedBefore && !reservedAfter);
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
                bridge.executeWorkers(urgentAssignments);
                check("T034-accepted-Stop-retains-builder-lease",
                      t034AcceptedStops > 0 && reserved());
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
                    bridge.executeWorkers(assignments);
                } else {
                    const auto moveCount = t034AcceptedMoves;
                    bridge.executeWorkers(assignments);
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
                bridge.executeWorkers(std::span{&assignment, 1});
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
                bridge.executeScouts(std::span{&order, 1});
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
            check("worker-local-defense-mining-continues-at-both-bases",
                  mainMining > 0 && naturalMining > 0);

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
            bridge.executeWorkers(assignments);
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
                    bridge.executeWorkers(std::span{&assignment, 1});
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
                        resourceOverlapMineralsBefore - mineralsNow == 515 &&
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
            BWAPI::Unit core = nullptr, robotics = nullptr, observatory = nullptr, reaver = nullptr;
            for (const auto unit : BWAPI::Broodwar->self()->getUnits()) {
                if (unit == nullptr || !unit->exists()) continue;
                const auto type = unit->getType();
                if (type == BWAPI::UnitTypes::Protoss_Gateway) gateways.push_back(unit);
                if (type == BWAPI::UnitTypes::Protoss_Cybernetics_Core) core = unit;
                if (type == BWAPI::UnitTypes::Protoss_Robotics_Facility) robotics = unit;
                if (type == BWAPI::UnitTypes::Protoss_Observatory) observatory = unit;
                if (type == BWAPI::UnitTypes::Protoss_Reaver) reaver = unit;
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
            auto scarabAccepted = false;
            bridge.actionDiagnostic = [this, &scarabAccepted](
                const protodd::bwapi::ActionDiagnostic& d) {
                if (d.source.starts_with("maintenance-")) {
                    log << "ISSUER_ACTION," << BWAPI::Broodwar->getFrameCount() << ','
                        << d.actor << ',' << d.target << ',' << d.type << ',' << d.source
                        << ',' << d.outcome << ',' << d.attempted << ',' << d.accepted << '\n';
                    log.flush();
                }
                if (d.source == "maintenance-scarab" && d.accepted)
                    scarabAccepted = true;
            };
            bridge.runMaintenance();
            bridge.actionDiagnostic = {};
            const auto maintenanceMineralsAfter = BWAPI::Broodwar->self()->minerals();
            const auto maintenanceGasAfter = BWAPI::Broodwar->self()->gas();
            bool scarabQueued = false;
            if (reaver != nullptr) {
                const auto scarabQueue = reaver->getTrainingQueue();
                scarabQueued = scarabQueue.size() == 1 &&
                    scarabQueue.front() == BWAPI::UnitTypes::Protoss_Scarab;
            }
            check("maintenance-scarab-accepted", scarabAccepted && scarabQueued);
            check("maintenance-spend-shares-bank",
                maintenanceMineralsBefore - maintenanceMineralsAfter == 15 &&
                maintenanceGasBefore == maintenanceGasAfter &&
                ledger.minerals == state.self.minerals - 515 &&
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
                << ",scarab=" << scarabAccepted << ",mineralsSpent="
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
                auto commands=TacticalController{}.control(squad,state.enemy.units,estimate,{1100,1000},
                    {300,1000},influence,{800,1000},3,true,{},TacticalIntent::battle,{},nullptr,state.self.units);
                for(const auto& c:commands) if(c.technology==TechnologyKind::psionicStorm) {
                    requested=true; accepted=bridge.execute(c);
                    log << "CAST," << c.targetPosition.x << ',' << c.targetPosition.y << ',' << accepted << '\n';
                }
                check("storm-decision",requested==(scenario=="storm-clear"));
                if(requested) check("cast-accepted",accepted);
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
                auto dt=std::ranges::find_if(state.enemy.units,[](const auto& u){return u.kind==UnitKind::darkTemplar;});
                check("fixture-cloaked-visible",dt!=state.enemy.units.end());
                if(dt!=state.enemy.units.end()) {
                    // For an undetected Dark Templar BWAPI also withholds the
                    // cloak flag. The relevant contract is unavailable health.
                    check("fixture-health-hidden",!dt->detected && dt->hitPoints==0);
                    log << "HIDDEN_DT," << dt->visible << ',' << dt->detected << ',' << dt->cloaked << ',' << dt->hitPoints << '\n';
                    check("hidden-health-threat",influence.at(dt->position).groundThreat>0);
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
