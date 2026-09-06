#include "AstraModule.hpp"

#include "astra/Technology.hpp"
#include "astra/UnitCatalog.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {

std::uint64_t stableSeed(const std::string_view value) {
    std::uint64_t result = 1469598103934665603ULL;
    for (const auto character : value) {
        result ^= static_cast<unsigned char>(character);
        result *= 1099511628211ULL;
    }
    return result;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return input ? std::string(std::istreambuf_iterator<char>(input),
                               std::istreambuf_iterator<char>())
                 : std::string{};
}

}  // namespace

namespace astra::bwapi {

void AstraModule::onStart() {
    BWAPI::Broodwar->setCommandOptimizationLevel(2);
    BWAPI::Broodwar->setLatCom(true);
    bridge_.onStart();
    state_ = bridge_.observe();
    navigation_ = bridge_.navigationGrid();
    opponent_.reset(state_.enemy.race);
    strategicDirector_.reset();
    influence_ = InfluenceMap(64);
    commands_.clear();
    engagements_.reset();
    transports_.reset();
    scouts_.reset();
    frameBudget_.reset();
    detectorEscorts_.clear();
    leasedScouts_.clear();
    advanceWaypoints_.clear();
    navigationSignatures_.clear();
    navigationRefresh_ = -1;
    firstCounterattackFrame_ = -1;
    maintenanceMineralReserve_ = 0;
    maintenanceGasReserve_ = 0;

    std::error_code error;
    std::filesystem::create_directories("bwapi-data/write", error);
    opponentName_ = BWAPI::Broodwar->enemy() ? BWAPI::Broodwar->enemy()->getName() : "unknown";
    mapName_ = BWAPI::Broodwar->mapName();
    const auto historyFile = OpponentHistory::filename(opponentName_);
    history_.parse(readFile(std::filesystem::path("bwapi-data/read") / historyFile));
    history_.merge(readFile(std::filesystem::path("bwapi-data/write") / historyFile));
    openingStyle_ = history_.choose(opponentName_, mapName_,
                                    stableSeed(opponentName_ + "|" + mapName_));
    log_.open("bwapi-data/write/AstraBot.log", std::ios::app);
    if (log_) {
        log_ << "START," << BWAPI::Broodwar->mapName() << ','
             << opponentName_ << ',' << openingStyleName(openingStyle_) << '\n';
        log_ << "MATCH,seed=" << BWAPI::Broodwar->getRandomSeed()
             << ",map_hash=" << BWAPI::Broodwar->mapHash() << '\n';
    }
}

void AstraModule::onEnd(const bool winner) {
    history_.record(opponentName_, mapName_, openingStyle_, winner);
    std::ofstream historyOutput(std::filesystem::path("bwapi-data/write") /
                                    OpponentHistory::filename(opponentName_),
                                std::ios::binary | std::ios::trunc);
    if (historyOutput) historyOutput << history_.serialize();
    if (log_) {
        const auto& runtime = frameBudget_.stats();
        log_ << "PERF_SUMMARY," << runtime.samples << ',' << runtime.movingAverageMs << ','
             << runtime.peakMs << ',' << runtime.over42ms << ',' << runtime.over55ms << ','
             << runtime.overOneSecond << ',' << runtime.overTenSeconds << '\n';
        log_ << "END," << (winner ? "win" : "loss") << ',' << state_.frame << '\n';
        log_.flush();
    }
}

void AstraModule::onFrame() {
    const auto started = std::chrono::steady_clock::now();
    try {
        runFrame();
    } catch (const std::exception& error) {
        const auto frame = BWAPI::Broodwar->getFrameCount();
        if (log_ && frame - lastErrorFrame_ >= 24) {
            log_ << "ERROR," << frame << ',' << error.what() << '\n';
            log_.flush();
            lastErrorFrame_ = frame;
        }
    } catch (...) {
        const auto frame = BWAPI::Broodwar->getFrameCount();
        if (log_ && frame - lastErrorFrame_ >= 24) {
            log_ << "ERROR," << frame << ",unknown\n";
            log_.flush();
            lastErrorFrame_ = frame;
        }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
    const auto frame = BWAPI::Broodwar->getFrameCount();
    frameBudget_.record(frame, elapsed);
    if (log_ && elapsed >= 28000) {
        log_ << "PERF," << frame << ',' << elapsed << ','
             << runtimeLoadName(frameBudget_.load(frame)) << '\n';
    }
}

void AstraModule::runFrame() {
    if (BWAPI::Broodwar->isReplay() || BWAPI::Broodwar->isPaused() ||
        BWAPI::Broodwar->self() == nullptr || BWAPI::Broodwar->enemy() == nullptr) {
        return;
    }
    state_ = bridge_.observe();
    if (state_.self.race != Race::protoss) {
        BWAPI::Broodwar->drawTextScreen(8, 8, "AstraBot requires Protoss");
        return;
    }

    const auto cadence = frameBudget_.expensiveCadenceMultiplier(state_.frame);
    // Work is staggered to keep frame time predictable under tournament load.
    if (state_.frame % (8 * cadence) == 0) influence_.update(state_);
    if (state_.frame % (12 * cadence) == 0) opponent_.update(state_);
    if (state_.frame % 24 == 0 || plan_.goals.empty()) updateStrategy();
    if (state_.frame % 6 == 1) updateMacro();
    if (state_.frame % 12 == 2) updateWorkers();
    if (state_.frame % (24 * cadence) == 3) updateScouting();
    const auto combatCadence = std::max(1, state_.latencyFrames) *
                               (frameBudget_.load(state_.frame) == RuntimeLoad::emergency ? 2 : 1);
    if (state_.frame % combatCadence == 0) {
        updateCombat(frameBudget_.allowSimulation(state_.frame),
                     frameBudget_.navigationInterval(state_.frame),
                     frameBudget_.combatCommandLimit(state_.frame));
    }
    if (state_.frame % 24 == 5) {
        bridge_.runMaintenance(maintenanceMineralReserve_, maintenanceGasReserve_);
    }
    if (state_.frame % (24 * 15) == 0) logDecision();

    if (frameBudget_.load(state_.frame) == RuntimeLoad::normal) {
        bridge_.drawDebug(plan_, opponent_.assessment(), fight_);
    }
}

void AstraModule::onUnitDiscover(const BWAPI::Unit unit) { bridge_.remember(unit); }
void AstraModule::onUnitShow(const BWAPI::Unit unit) { bridge_.remember(unit); }
void AstraModule::onUnitDestroy(const BWAPI::Unit unit) { bridge_.forget(unit); }
void AstraModule::onUnitMorph(const BWAPI::Unit unit) { bridge_.remember(unit); }
void AstraModule::onUnitRenegade(const BWAPI::Unit unit) {
    bridge_.forget(unit);
    bridge_.remember(unit);
}

void AstraModule::updateStrategy() {
    plan_ = strategicDirector_.stabilize(
        strategy_.plan(state_, opponent_.assessment(), openingStyle_),
        state_, opponent_.assessment());
}

void AstraModule::updateMacro() {
    ResourceLedger ledger{state_.self.minerals, state_.self.gas};
    const auto actions = macro_.reconcile(state_, plan_, ledger);
    maintenanceMineralReserve_ = 0;
    maintenanceGasReserve_ = 0;
    for (const auto& action : actions) {
        if (!action.blocksLowerPriority) continue;
        maintenanceMineralReserve_ = std::max(maintenanceMineralReserve_, action.minerals);
        maintenanceGasReserve_ = std::max(maintenanceGasReserve_, action.gas);
        break;
    }
    bridge_.executeMacro(actions, plan_, leasedScouts_);
}

void AstraModule::updateWorkers() {
    auto reserved = bridge_.reservedBuilders();
    reserved.insert(reserved.end(), leasedScouts_.begin(), leasedScouts_.end());
    std::ranges::sort(reserved);
    reserved.erase(std::unique(reserved.begin(), reserved.end()), reserved.end());
    const auto assignments = workers_.assign(state_, plan_, influence_, reserved);
    bridge_.executeWorkers(assignments);
}

void AstraModule::updateScouting() {
    const auto previousLeases = leasedScouts_;
    const auto reservedBuilders = bridge_.reservedBuilders();
    leasedScouts_.clear();
    std::vector<UnitId> available;
    for (const auto& unit : state_.self.units) {
        if (unit.kind == UnitKind::observer && unit.completed) {
            if (std::ranges::find(detectorEscorts_, unit.id) != detectorEscorts_.end()) continue;
            // Combat has already leased the escort. Reserving another first
            // observer here left a two-Observer build with no active scout.
            available.push_back(unit.id);
        } else if (unit.kind == UnitKind::corsair && available.empty()) {
            available.push_back(unit.id);
        }
    }
    if (available.empty()) {
        // Keep one stable worker scout whenever possible. Most importantly,
        // never steal a Probe that macro has ordered to construct a building:
        // a frame-3 scout order used to cancel the opening pylon order issued
        // on frame 1, leaving the economy supply-blocked with a large bank.
        const auto probe = selectOpeningWorkerScout(
            state_, previousLeases, reservedBuilders);
        if (probe >= 0) available.push_back(probe);
    }
    const auto orders = scouts_.assign(state_, available, influence_,
                                       opponent_.assessment());
    for (const auto& order : orders) leasedScouts_.push_back(order.scout);
    bridge_.executeScouts(orders);
}

void AstraModule::updateCombat(
    const bool runSimulation,
    const int navigationInterval,
    const std::size_t commandLimit) {
    const auto friendly = combatUnits(true);
    const auto enemy = combatUnits(false);
    const auto aggressive = plan_.posture == Posture::pressure ||
                            plan_.posture == Posture::attack ||
                            plan_.posture == Posture::harass;
    const auto formed = squads_.form(state_, friendly, enemy, plan_, retreatPoint());
    const auto resetNavigation = advanceWaypoints_.size() != formed.size() ||
                                 navigationSignatures_.size() != formed.size();
    const auto periodicNavigationRefresh = navigationRefresh_ < 0 ||
                                           state_.frame - navigationRefresh_ >=
                                               navigationInterval;
    if (resetNavigation) {
        advanceWaypoints_.assign(formed.size(), {-1, -1});
        navigationSignatures_.assign(formed.size(), 0);
    }
    if (periodicNavigationRefresh) {
        navigationRefresh_ = state_.frame;
    }
    commands_.beginFrame(state_.frame, state_.latencyFrames);
    fight_ = {};
    auto debugSquadSize = std::size_t{0};
    const auto* vanguard = SquadPlanner::selectVanguard(formed, plan_.attackTarget);
    for (std::size_t squadIndex = 0; squadIndex < formed.size(); ++squadIndex) {
        const auto& squad = formed[squadIndex];
        auto requiredRatio = squad.requiredRatio;
        auto objective = squad.objective;
        auto defense = squad.defense;
        if (squad.role == SquadRole::mainArmy) {
            if (!aggressive ||
                (vanguard == &squad &&
                 squad.units.size() <
                     static_cast<std::size_t>(std::max(1, plan_.minimumAttackSize)))) {
                requiredRatio = 0.88;
                objective = plan_.rallyPoint;
                defense = SquadPlanner::defensiveArea(state_, plan_.rallyPoint);
            } else if (vanguard != nullptr && vanguard != &squad &&
                       squad.enemies.empty()) {
                // Detached reinforcements join the strongest mobile component
                // instead of launching a second, usually losing attack wave.
                requiredRatio = 0.88;
                objective = vanguard->center;
            }
        }
        const auto hasGroundUnit = std::ranges::any_of(
            squad.units, [](const UnitSnapshot& unit) { return !unit.flying; });
        auto routeSignature = squad.signature;
        routeSignature ^= static_cast<std::uint32_t>(objective.x);
        routeSignature *= 1099511628211ULL;
        routeSignature ^= static_cast<std::uint32_t>(objective.y);
        routeSignature *= 1099511628211ULL;
        const auto refreshRoute = periodicNavigationRefresh || resetNavigation ||
                                  navigationSignatures_[squadIndex] != routeSignature;
        if (refreshRoute) {
            navigationSignatures_[squadIndex] = routeSignature;
            advanceWaypoints_[squadIndex] = {-1, -1};
        }
        if (refreshRoute && hasGroundUnit) {
            // During uncontested travel, let BWAPI route each unit to the
            // actual destination. A short waypoint from a large squad's
            // centroid can lie behind its front units or on the wrong side
            // of terrain, continually pulling the force back into itself.
            if (!squad.enemies.empty()) {
                advanceWaypoints_[squadIndex] =
                    navigation_.nextWaypoint(squad.center, objective);
            }
        }
        if (hasGroundUnit) {
            if (!squad.enemies.empty() && advanceWaypoints_[squadIndex].valid()) {
                objective = advanceWaypoints_[squadIndex];
            }
        }
        auto estimate = combat_.evaluate(
            squad.units, squad.enemies, requiredRatio,
            squad.enemies.empty() ? opponent_.assessment().uncertainty * 0.25
                                  : opponent_.assessment().uncertainty,
            runSimulation);
        if (!squad.enemies.empty()) {
            estimate.decision = engagements_.stabilize(
                squad.signature, estimate.decision, estimate.ratio,
                requiredRatio, state_.frame);
        }
        // Once a ground threat has crossed the last defensive screen, running
        // the army behind the Nexus only exposes workers and production. Make
        // melee units take the last stand while ranged units still use their
        // range-advantage kiting inside TacticalController.
        if (SquadPlanner::mustHoldDefensiveScreen(squad)) {
            estimate.decision = FightDecision::engage;
        }
        if (SquadPlanner::canCounterattack(squad, estimate, plan_)) {
            // A global defense response must not trap an independently strong
            // reserve army while the allocated defenders protect the base.
            defense = {};
            objective = plan_.attackTarget;
            if (firstCounterattackFrame_ < 0) firstCounterattackFrame_ = state_.frame;
        }
        if (squad.role == SquadRole::mainArmy && squad.units.size() >= debugSquadSize) {
            debugSquadSize = squad.units.size();
            fight_ = estimate;
        }
        const auto targets = SquadPlanner::tacticalTargets(squad, state_.enemy.units);
        for (const auto& order : tactics_.control(
                 squad.units, targets, estimate, objective,
                 squad.retreat, influence_, squad.center, state_.latencyFrames,
                 technologyLevel(state_.self, TechnologyKind::psionicStorm) > 0,
                 defense)) {
            commands_.submit(order);
        }
    }

    for (const auto& order : tactics_.recharge(state_.self.units,
                                              plan_.posture == Posture::defend)) {
        commands_.submit(order);
    }

    detectorEscorts_.clear();
    for (const auto& order : squads_.detectorEscorts(state_, formed, influence_)) {
        detectorEscorts_.push_back(order.actor);
        commands_.submit(order);
    }
    for (const auto& order : transports_.control(
             state_, plan_.attackTarget, retreatPoint(), influence_)) {
        commands_.submit(order);
    }
    // BWAPI calls are capped per combat tick. Priority-aware rotation keeps
    // retreat and detector orders immediate while bounding large-army spikes.
    for (const auto& command : commands_.finalize(commandLimit)) {
        if (bridge_.execute(command)) commands_.markIssued(command);
    }
}

std::vector<UnitSnapshot> AstraModule::combatUnits(const bool ours) const {
    const auto& source = ours ? state_.self.units : state_.enemy.units;
    std::vector<UnitSnapshot> result;
    for (const auto& unit : source) {
        if ((!isCombatUnit(unit.kind) && !isStaticDefense(unit.kind)) ||
            !unit.completed || unit.hallucination || unit.loaded) continue;
        if (!ours && !unit.visible && state_.frame - unit.lastSeen > 24 * 45) continue;
        result.push_back(unit);
    }
    return result;
}

Position AstraModule::retreatPoint() const {
    const auto nexus = std::ranges::find(state_.self.units, UnitKind::nexus, &UnitSnapshot::kind);
    return nexus != state_.self.units.end() ? nexus->position : plan_.rallyPoint;
}

void AstraModule::logDecision() {
    if (!log_) return;
    const auto countUnits = [this](const UnitKind kind, const bool completedOnly) {
        return std::ranges::count_if(state_.self.units, [kind, completedOnly](const auto& unit) {
            return unit.kind == kind && (!completedOnly || unit.completed);
        });
    };
    const auto pylons = countUnits(UnitKind::pylon, false);
    const auto completedPylons = countUnits(UnitKind::pylon, true);
    const auto gateways = countUnits(UnitKind::gateway, false);
    const auto probes = countUnits(UnitKind::probe, false);
    const auto nexuses = countUnits(UnitKind::nexus, false);
    const auto cannons = countUnits(UnitKind::photonCannon, false);
    const auto completedCannons = countUnits(UnitKind::photonCannon, true);
    const auto batteries = countUnits(UnitKind::shieldBattery, false);
    const auto zealots = countUnits(UnitKind::zealot, false);
    const auto completedZealots = countUnits(UnitKind::zealot, true);
    const auto dragoons = countUnits(UnitKind::dragoon, false);
    const auto completedDragoons = countUnits(UnitKind::dragoon, true);
    const auto reavers = countUnits(UnitKind::reaver, false);
    const auto darkTemplar = countUnits(UnitKind::darkTemplar, true);
    const auto highTemplar = countUnits(UnitKind::highTemplar, true);
    const auto stormReady =
        technologyLevel(state_.self, TechnologyKind::psionicStorm) > 0;
    const auto mobileArmy = std::ranges::count_if(
        state_.self.units, [](const UnitSnapshot& unit) {
            return unit.completed && isCombatUnit(unit.kind) && !unit.flying;
        });
    const auto visibleEnemyArmy = std::ranges::count_if(
        state_.enemy.units, [](const UnitSnapshot& unit) {
            return unit.visible && unit.completed && isCombatUnit(unit.kind);
        });
    ResourceLedger diagnosticLedger{state_.self.minerals, state_.self.gas};
    const auto diagnosticActions = macro_.reconcile(state_, plan_, diagnosticLedger);
    log_ << "STATE," << state_.frame << ',' << plan_.name << ','
         << postureName(plan_.posture) << ','
         << enemyPlanName(opponent_.assessment().mostLikely) << ','
         << opponent_.assessment().uncertainty << ',' << fight_.ratio << ','
         << state_.self.minerals << ',' << state_.self.gas << ','
         << state_.self.supplyUsed << ',' << state_.self.supplyTotal << ','
         << frameBudget_.stats().movingAverageMs << ','
         << runtimeLoadName(frameBudget_.load(state_.frame)) << ','
         << bridge_.lastMacroStatus() << ",pylons=" << pylons << '/' << completedPylons
         << ",gateways=" << gateways << ",nexuses=" << nexuses
         << ",probes=" << probes << ",cannons=" << cannons << '/'
         << completedCannons << ",batteries=" << batteries
         << ",zealots=" << zealots << '/' << completedZealots
         << ",dragoons=" << dragoons << '/' << completedDragoons
         << ",reavers=" << reavers << ",dt=" << darkTemplar
         << ",ht=" << highTemplar << ",storm=" << (stormReady ? 1 : 0)
         << ",army=" << mobileArmy
         << ",enemyVisibleArmy=" << visibleEnemyArmy
         << ",minAttack=" << plan_.minimumAttackSize
         << ",gasWorkers=" << std::ranges::count(state_.self.units, true,
                                                  &UnitSnapshot::gatheringGas)
         << ",gasTarget=" << plan_.desiredGasWorkers
         << ",core=" << countUnits(UnitKind::cyberneticsCore, false) << '/'
         << countUnits(UnitKind::cyberneticsCore, true)
         << ",range=" << technologyLevel(state_.self, TechnologyKind::singularityCharge)
         << '/' << technologyInProgress(state_.self, TechnologyKind::singularityCharge)
         << ",minedMinerals=" << BWAPI::Broodwar->self()->gatheredMinerals()
         << ",minedGas=" << BWAPI::Broodwar->self()->gatheredGas()
         << ",counterattackFirst=" << firstCounterattackFrame_
         << ",attackTarget=" << plan_.attackTarget.x << 'x' << plan_.attackTarget.y
         << ",rally=" << plan_.rallyPoint.x << 'x' << plan_.rallyPoint.y
         << ",defense=";
    auto firstDefense = true;
    for (const auto& unit : state_.self.units) {
        char marker{};
        if (unit.kind == UnitKind::nexus) marker = 'N';
        if (unit.kind == UnitKind::pylon) marker = 'P';
        if (unit.kind == UnitKind::photonCannon) marker = 'C';
        if (unit.kind == UnitKind::shieldBattery) marker = 'B';
        if (unit.kind == UnitKind::gateway) marker = 'G';
        if (unit.kind == UnitKind::cyberneticsCore) marker = 'R';
        if (unit.kind == UnitKind::citadelOfAdun) marker = 'T';
        if (unit.kind == UnitKind::templarArchives) marker = 'X';
        if (marker == 0 || !unit.completed || !unit.position.valid()) continue;
        if (!firstDefense) log_ << ';';
        firstDefense = false;
        log_ << marker << '@' << unit.position.x << 'x' << unit.position.y;
    }
    log_ << ",workers=";
    auto workerIndex = 0;
    for (const auto& unit : state_.self.units) {
        if (unit.kind != UnitKind::probe || !unit.position.valid()) continue;
        if (workerIndex++ > 0) log_ << ';';
        log_ << unit.id << '@' << unit.position.x << 'x' << unit.position.y << ':'
             << unit.hitPoints + unit.shields;
    }
    log_ << ",armyPositions=";
    auto armyIndex = 0;
    for (const auto& unit : state_.self.units) {
        if (!unit.completed || !isCombatUnit(unit.kind) ||
            !unit.position.valid()) continue;
        if (armyIndex++ > 0) log_ << ';';
        log_ << unit.id << ':' << unitStats(unit.kind).name << '@'
             << unit.position.x << 'x' << unit.position.y << ':' << unit.durability()
             << ':' << unit.weaponCooldown << ':' << unit.orderTargetId << ':' << unit.ammo;
    }
    log_ << ",knownStructures=";
    auto structureIndex = 0;
    for (const auto& unit : state_.enemy.units) {
        if (!isBuilding(unit.kind) || !unit.position.valid()) continue;
        if (structureIndex++ > 0) log_ << ';';
        log_ << unitStats(unit.kind).name << '@' << unit.position.x << 'x'
             << unit.position.y << ':' << unit.lastSeen;
    }
    log_ << ",visibleCombat=";
    auto enemyIndex = 0;
    for (const auto& unit : state_.enemy.units) {
        if (!unit.visible || !unit.completed || !isCombatUnit(unit.kind) ||
            !unit.position.valid() || enemyIndex >= 16) {
            continue;
        }
        if (enemyIndex++ > 0) log_ << ';';
        log_ << unitStats(unit.kind).name << '@' << unit.position.x << 'x'
             << unit.position.y << ':' << unit.hitPoints + unit.shields;
    }
    log_ << ",actions=";
    for (std::size_t index = 0; index < diagnosticActions.size() && index < 4; ++index) {
        if (index > 0) log_ << ';';
        const auto& action = diagnosticActions[index];
        log_ << static_cast<int>(action.action) << ':' << unitStats(action.target).name << ':'
             << (!action.reserved ? 'W' : (action.executable ? 'R' : 'H')) << ':'
             << action.priority;
    }
    log_ << ",busy=";
    for (std::size_t index = 0; index < state_.self.busyProducers.size(); ++index) {
        if (index > 0) log_ << ';';
        log_ << unitStats(state_.self.busyProducers[index]).name;
    }
    log_ << ",queued=";
    for (std::size_t index = 0; index < state_.self.queuedUnits.size(); ++index) {
        if (index > 0) log_ << ';';
        log_ << unitStats(state_.self.queuedUnits[index]).name;
    }
    log_ << '\n';
    log_.flush();
}

}  // namespace astra::bwapi
