#include "ProtoddModule.hpp"

#include "protodd/Technology.hpp"
#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>

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

int countUnits(
    const std::span<const protodd::UnitSnapshot> units,
    const protodd::UnitKind kind,
    const bool completedOnly = false,
    const bool visibleOnly = false) {
    return static_cast<int>(std::ranges::count_if(
        units, [kind, completedOnly, visibleOnly](const protodd::UnitSnapshot& unit) {
            return unit.kind == kind && (!completedOnly || unit.completed) &&
                   (!visibleOnly || unit.visible);
        }));
}

std::string csvSafe(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        // StarCraft map strings can contain in-band color/control codes. They
        // make terminal output unreadable and can corrupt downstream CSV.
        if (byte < 0x20 || byte == 0x7f) continue;
        result.push_back(character == ',' ? ';' : character);
    }
    return result;
}

std::string composition(
    const std::span<const protodd::UnitSnapshot> units,
    const bool includeVisibility) {
    std::string result;
    for (auto raw = 0; raw < static_cast<int>(protodd::UnitKind::count); ++raw) {
        const auto kind = static_cast<protodd::UnitKind>(raw);
        const auto total = countUnits(units, kind);
        if (total == 0) continue;
        if (!result.empty()) result += ';';
        result += protodd::unitStats(kind).name;
        result += '=' + std::to_string(total);
        result += '/' + std::to_string(countUnits(units, kind, true));
        if (includeVisibility) {
            result += '/' + std::to_string(countUnits(units, kind, false, true));
        }
    }
    return result;
}

std::string goalSummary(const std::vector<protodd::ProductionGoal>& goals) {
    std::string result;
    for (std::size_t index = 0; index < goals.size() && index < 12; ++index) {
        if (!result.empty()) result += ';';
        const auto& goal = goals[index];
        result += std::to_string(static_cast<int>(goal.goal));
        result += ':';
        result += protodd::unitStats(goal.target).name;
        result += ':';
        result += std::to_string(goal.desiredCount);
        result += ':';
        result += std::to_string(goal.priority);
        result += ':';
        result += goal.blocking ? 'B' : 'O';
        result += ':';
        result += csvSafe(goal.reason);
        if (goal.technology != protodd::TechnologyKind::none) {
            result += ':';
            result += std::to_string(static_cast<int>(goal.technology));
        }
    }
    return result;
}

}  // namespace

namespace protodd::bwapi {

void ProtoddModule::onStart() {
    BWAPI::Broodwar->setCommandOptimizationLevel(2);
    BWAPI::Broodwar->setLatCom(true);
    std::error_code error;
    std::filesystem::create_directories("bwapi-data/write", error);
    opponentName_ = BWAPI::Broodwar->enemy() ? BWAPI::Broodwar->enemy()->getName() : "unknown";
    mapName_ = BWAPI::Broodwar->mapName();
    log_.open("bwapi-data/write/Protodd.log", std::ios::app);
    if (log_) {
        log_ << "BOOT," << csvSafe(mapName_) << ',' << csvSafe(opponentName_) << '\n';
        log_.flush();
    }
    // BWAPI only delivers onSendText and selected-unit information with this
    // flag enabled. Tournament hosts may deny it; the passive overlay still
    // works and reports that its interactive controls are unavailable.
    BWAPI::Broodwar->enableFlag(BWAPI::Flag::UserInput);
    bridge_.onStart();
    state_ = bridge_.observe();
    navigation_ = bridge_.navigationGrid();
    opponent_.reset(state_.enemy.race);
    strategicDirector_.reset();
    expansion_.reset();
    macro_ = {};
    workers_ = {};
    plan_ = {};
    phases_.clear();
    traceMemory_.clear();
    supplyBlockedFrames_.reset();
    idleGatewayFrames_.reset();
    idleWorkerFrames_.reset();
    commandsAttempted_ = commandsAccepted_ = macroAttempted_ = macroAccepted_ = 0;
    commandsProposed_ = commandsSuperseded_ = commandsRedundant_ = commandsDeferred_ = 0;
    lastMacroFrame_ = lastSquadLogFrame_ = -1;
    lastLedger_ = {};
    influence_ = InfluenceMap(64);
    commands_.clear();
    scoutCommands_.clear();
    engagements_.reset();
    squads_.reset();
    transports_.reset();
    scouts_.reset();
    frameBudget_.reset();
    debug_ = {};
    detectorEscorts_.clear();
    leasedScouts_.clear();
    advanceWaypoints_.clear();
    navigationSignatures_.clear();
    navigationRefresh_ = -1;
    firstCounterattackFrame_ = -1;
    firstEnemyContactFrame_ = -1;
    firstBaseBreachFrame_ = -1;
    firstCoreFrame_ = -1;
    firstDragoonFrame_ = -1;
    firstRangeFrame_ = -1;
    firstExpansionFrame_ = -1;
    firstArmyZeroFrame_ = -1;
    firstNexusLossFrame_ = -1;
    firstAttackFrame_ = -1;
    lastTelemetryFrame_ = -1;
    lastEventFrame_ = -1;
    lastArmyCount_ = -1;
    lastProbeCount_ = -1;
    lastNexusCount_ = -1;
    lastCompletedNexusCount_ = -1;
    lastEnemyVisibleArmy_ = -1;
    maxArmyCount_ = 0;
    maxProbeCount_ = 0;
    maxNexusCount_ = 0;
    maxEnemyVisibleArmy_ = 0;
    peakMinerals_ = 0;
    peakGas_ = 0;
    telemetrySamples_ = 0;
    supplyBlockSamples_ = 0;
    highBankSamples_ = 0;
    planChanges_ = 0;
    postureChanges_ = 0;
    lastPlanName_.clear();
    lastPosture_ = Posture::hold;
    maintenanceMineralReserve_ = 0;
    maintenanceGasReserve_ = 0;
    slowWindowStart_ = slowWindowPeakFrame_ = -1;
    slowWindowPeakUs_ = 0;
    slowWindowLoad_ = RuntimeLoad::normal;

    const auto historyFile = OpponentHistory::filename(opponentName_);
    history_.parse(readFile(std::filesystem::path("bwapi-data/read") / historyFile));
    history_.merge(readFile(std::filesystem::path("bwapi-data/write") / historyFile));
    openingStyle_ = history_.choose(opponentName_, mapName_,
                                    stableSeed(opponentName_ + "|" + mapName_));
    if (log_) {
        log_ << "START," << csvSafe(BWAPI::Broodwar->mapName()) << ','
             << csvSafe(opponentName_) << ',' << openingStyleName(openingStyle_) << '\n';
        log_ << "MATCH,seed=" << BWAPI::Broodwar->getRandomSeed()
             << ",map_hash=" << BWAPI::Broodwar->mapHash() << '\n';
        log_ << "DIAGNOSTICS,version=2,sampleFrames=24,entityFrames=240,"
                "beliefFrames=240,orderHeartbeatFrames=120,performanceWindowFrames=24,"
                "information=legal-observations\n";
    }
}

void ProtoddModule::onEnd(const bool winner) {
    sampleTelemetry();
    history_.record(opponentName_, mapName_, openingStyle_, winner);
    std::ofstream historyOutput(std::filesystem::path("bwapi-data/write") /
                                    OpponentHistory::filename(opponentName_),
                                std::ios::binary | std::ios::trunc);
    if (historyOutput) historyOutput << history_.serialize();
    if (log_) {
        flushPerformanceRecord();
        const auto& runtime = frameBudget_.stats();
        log_ << "PERF_SUMMARY," << runtime.samples << ',' << runtime.movingAverageMs << ','
             << runtime.peakMs << ',' << runtime.over42ms << ',' << runtime.over55ms << ','
             << runtime.overOneSecond << ',' << runtime.overTenSeconds << '\n';
        log_ << "SUMMARY,won=" << (winner ? 1 : 0)
             << ",frames=" << state_.frame
             << ",samples=" << telemetrySamples_
             << ",maxArmy=" << maxArmyCount_
             << ",maxProbes=" << maxProbeCount_
             << ",maxNexuses=" << maxNexusCount_
             << ",maxEnemyVisibleArmy=" << maxEnemyVisibleArmy_
             << ",peakMinerals=" << peakMinerals_
             << ",peakGas=" << peakGas_
             << ",firstEnemyContact=" << firstEnemyContactFrame_
             << ",firstBaseBreach=" << firstBaseBreachFrame_
             << ",firstCore=" << firstCoreFrame_
             << ",firstDragoon=" << firstDragoonFrame_
             << ",firstRange=" << firstRangeFrame_
             << ",firstExpansion=" << firstExpansionFrame_
             << ",firstCounterattack=" << firstCounterattackFrame_
             << ",firstAttack=" << firstAttackFrame_
             << ",firstArmyZero=" << firstArmyZeroFrame_
             << ",firstNexusLoss=" << firstNexusLossFrame_
             << ",supplyBlockSamples=" << supplyBlockSamples_
             << ",highBankSamples=" << highBankSamples_
             << ",planChanges=" << planChanges_
             << ",postureChanges=" << postureChanges_
             << '\n';
        log_ << "END," << (winner ? "win" : "loss") << ',' << state_.frame << '\n';
        log_.flush();
    }
}

void ProtoddModule::onFrame() {
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
    recordPerformance(frame, elapsed);
}

void ProtoddModule::onSendText(std::string text) {
    if (text == "/debug") debug_.level = (debug_.level + 1) % 3;
    else if (text == "/debug 0") debug_.level = 0;
    else if (text == "/debug 1") debug_.level = 1;
    else if (text == "/debug 2") debug_.level = 2;
}

void ProtoddModule::runFrame() {
    if (BWAPI::Broodwar->isReplay() || BWAPI::Broodwar->isPaused() ||
        BWAPI::Broodwar->self() == nullptr || BWAPI::Broodwar->enemy() == nullptr) {
        return;
    }
    const auto measure = [this](const char* phase, auto&& operation) {
        const auto start = std::chrono::steady_clock::now();
        operation();
        phases_[phase].record(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count());
    };
    measure("observe", [this] { state_ = bridge_.observe(); });
    if (state_.self.race != Race::protoss) {
        BWAPI::Broodwar->drawTextScreen(8, 8, "Protodd requires Protoss");
        return;
    }

    const auto cadence = frameBudget_.expensiveCadenceMultiplier(state_.frame);
    // Work is staggered to keep frame time predictable under tournament load.
    if (state_.frame % (8 * cadence) == 0) measure("influence", [this] { influence_.update(state_); });
    if (state_.frame % (12 * cadence) == 0) measure("inference", [this] { opponent_.update(state_); });
    if (state_.frame % 24 == 0 || plan_.goals.empty()) measure("strategy", [this] { updateStrategy(); });
    // Macro is cheap and producer idleness is time-sensitive: a Nexus or
    // Gateway should receive its next queue item within a latency-sized
    // window, not after the old quarter-second cadence.  Reconcile every
    // three frames while keeping the more expensive strategy/scouting loops
    // staggered below.
    if (state_.frame % 3 == 1) measure("macro", [this] { updateMacro(); });
    if (state_.frame % 12 == 2) measure("workers", [this] { updateWorkers(); });
    if (state_.frame % (24 * cadence) == 3) measure("scouting", [this] { updateScouting(); });
    const auto combatCadence = std::max(1, state_.latencyFrames) *
                               (frameBudget_.load(state_.frame) == RuntimeLoad::emergency ? 2 : 1);
    if (state_.frame % combatCadence == 0) {
        measure("scout-micro", [this] { updateScoutMicro(); });
        measure("combat", [this] { updateCombat(frameBudget_.allowSimulation(state_.frame),
                     frameBudget_.navigationInterval(state_.frame),
                     frameBudget_.combatCommandLimit(state_.frame)); });
    }
    if (state_.frame % 24 == 5) {
        measure("maintenance", [this] { bridge_.runMaintenance(maintenanceMineralReserve_, maintenanceGasReserve_); });
    }
    if (state_.frame % 24 == 0) measure("diagnostics", [this] { sampleTelemetry(); logDiagnostics(); });
    if (state_.frame % (24 * 5) == 0) measure("state-log", [this] { logDecision(); });

    if (frameBudget_.load(state_.frame) == RuntimeLoad::normal) {
        measure("overlay", [this] { bridge_.drawDebug(state_, plan_, opponent_.assessment(), debug_); });
    }
}

void ProtoddModule::onUnitDiscover(const BWAPI::Unit unit) { bridge_.remember(unit); }
void ProtoddModule::onUnitShow(const BWAPI::Unit unit) { bridge_.remember(unit); }
void ProtoddModule::onUnitDestroy(const BWAPI::Unit unit) {
    if (log_ && unit != nullptr &&
        (unit->getPlayer() == BWAPI::Broodwar->self() || unit->isVisible())) {
        const auto kind = BwapiBridge::toKind(unit->getType());
        log_ << "LOSS," << BWAPI::Broodwar->getFrameCount() << ','
             << (unit->getPlayer() == BWAPI::Broodwar->self() ? "self" : "enemy") << ','
             << unit->getID() << ',' << unitStats(kind).name << ','
             << unit->getPosition().x << ',' << unit->getPosition().y << ','
             << unit->getType().mineralPrice() << ',' << unit->getType().gasPrice() << '\n';
    }
    if (unit != nullptr) {
        traceMemory_.erase("order/" + std::to_string(unit->getID()));
        debug_.orders.erase(unit->getID());
    }
    bridge_.forget(unit);
}
void ProtoddModule::onUnitMorph(const BWAPI::Unit unit) { bridge_.remember(unit); }
void ProtoddModule::onUnitRenegade(const BWAPI::Unit unit) {
    bridge_.forget(unit);
    bridge_.remember(unit);
}

void ProtoddModule::updateStrategy() {
    auto candidate = strategy_.plan(state_, opponent_.assessment(), openingStyle_);
    const auto proposed = candidate.posture;
    plan_ = strategicDirector_.stabilize(std::move(candidate), state_, opponent_.assessment());
    expansion_.update(plan_, state_, bridge_.expansionFeedback());
    if (expansion_.releaseBuilder() && !bridge_.cancelExpansion()) plan_.deferExpansion = false;
    debug_.operation = expansion_.reason();
    trace("strategy", "STRATEGY," + csvSafe(plan_.name) + ',' +
        std::string(postureName(proposed)) + ',' + std::string(postureName(plan_.posture)) + ',' +
        std::string(enemyPlanName(opponent_.assessment().mostLikely)) + ',' + debug_.operation + ',' +
        std::to_string(plan_.deferExpansion));
}

void ProtoddModule::updateMacro() {
    ResourceLedger ledger{state_.self.minerals, state_.self.gas};
    const auto actions = macro_.reconcile(state_, plan_, ledger);
    lastLedger_ = ledger;
    lastMacroFrame_ = state_.frame;
    debug_.macro = actions;
    maintenanceMineralReserve_ = 0;
    maintenanceGasReserve_ = 0;
    for (const auto& action : actions) {
        if (!action.blocksLowerPriority) continue;
        maintenanceMineralReserve_ = std::max(maintenanceMineralReserve_, action.minerals);
        maintenanceGasReserve_ = std::max(maintenanceGasReserve_, action.gas);
        break;
    }
    bridge_.executeMacro(actions, plan_, leasedScouts_);
    for (const auto& execution : bridge_.macroExecutions()) {
        const auto& action = execution.action;
        if (action.reserved && action.executable) {
            ++macroAttempted_;
            if (execution.accepted) ++macroAccepted_;
        }
        const auto key = std::to_string(static_cast<int>(action.action)) + '/' +
            std::to_string(static_cast<int>(action.target)) + '/' +
            std::to_string(static_cast<int>(action.technology)) + '/' + action.reason;
        std::ostringstream entry;
        entry << "MACRO," << static_cast<int>(action.action) << ',' << unitStats(action.target).name
              << ',' << static_cast<int>(action.technology) << ',' << csvSafe(execution.outcome)
              << ',' << execution.accepted << ',' << csvSafe(action.reason) << ','
              << action.minerals << ',' << action.gas << ',' << action.reserved << ',' << action.executable;
        trace("macro/" + key, entry.str());
    }
}

void ProtoddModule::updateWorkers() {
    auto reserved = bridge_.reservedBuilders();
    reserved.insert(reserved.end(), leasedScouts_.begin(), leasedScouts_.end());
    std::ranges::sort(reserved);
    reserved.erase(std::unique(reserved.begin(), reserved.end()), reserved.end());
    const auto assignments = workers_.assign(state_, plan_, influence_, reserved);
    const auto gas = std::ranges::count(assignments, WorkerJob::gas, &WorkerAssignment::job);
    const auto minerals = std::ranges::count(assignments, WorkerJob::minerals, &WorkerAssignment::job);
    trace("workers", "WORKERS,gas=" + std::to_string(gas) + ",minerals=" + std::to_string(minerals) +
        ",leased=" + std::to_string(reserved.size()) + ",gasRequested=" + std::to_string(plan_.desiredGasWorkers));
    bridge_.executeWorkers(assignments);
}

void ProtoddModule::updateScouting() {
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
        const auto probe = scouts_.selectWorkerScout(
            state_, opponent_.assessment(), previousLeases, reservedBuilders);
        if (probe >= 0) available.push_back(probe);
    }
    const auto orders = scouts_.assign(state_, available, influence_,
                                       opponent_.assessment());
    for (const auto& order : orders) leasedScouts_.push_back(order.scout);
    // Keep the opening Probe leased while it evades or returns, even if the
    // strategic scout selector finds no safe new destination this pass.
    const auto openingScout = scouts_.openingScout();
    if (openingScout >= 0 && std::ranges::find(leasedScouts_, openingScout) == leasedScouts_.end())
        leasedScouts_.push_back(openingScout);
    std::vector<ScoutOrder> ordinary;
    for (const auto& order : orders) if (order.scout != openingScout) ordinary.push_back(order);
    bridge_.executeScouts(ordinary);
}

void ProtoddModule::updateScoutMicro() {
    scoutCommands_.beginFrame(state_.frame, state_.latencyFrames);
    if (const auto command = scouts_.controlWorkerScout(state_, influence_, &navigation_)) scoutCommands_.submit(*command);
    for (const auto& command : scoutCommands_.finalize(1)) {
        const auto accepted = bridge_.execute(command);
        if (accepted) {
            scoutCommands_.markIssued(command);
            debug_.orders[command.actor] = command.source;
            debug_.scout = "Probe " + std::to_string(command.actor) + ": " + command.source;
        }
        trace("scout", "SCOUT," + std::to_string(command.actor) + ',' + command.source + ',' +
            std::to_string(command.targetUnit) + ',' + std::to_string(accepted));
    }
    if (scouts_.openingScout() < 0) debug_.scout.clear();
}

void ProtoddModule::updateCombat(
    const bool runSimulation,
    const int navigationInterval,
    const std::size_t commandLimit) {
    const auto transportOrders = transports_.control(
        state_, plan_.attackTarget, retreatPoint(), influence_,
        plan_.prioritizeReinforcements ? 100 : (state_.enemy.race == Race::protoss ? 2 : 1), true, &navigation_);
    auto friendly = combatUnits(true);
    std::erase_if(friendly, [this](const UnitSnapshot& unit) { return transports_.ownsReaver(unit.id); });
    debug_.squads.clear();
    const auto enemy = combatUnits(false);
    const auto aggressive = plan_.posture == Posture::pressure ||
                            plan_.posture == Posture::attack ||
                            plan_.posture == Posture::harass;
    const auto formed = squads_.form(state_, friendly, enemy, plan_, retreatPoint(), &navigation_);
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
    const auto logSquads = lastSquadLogFrame_ < 0 || state_.frame - lastSquadLogFrame_ >= 24;
    const auto* vanguard = SquadPlanner::selectVanguard(formed, plan_.attackTarget);
    for (std::size_t squadIndex = 0; squadIndex < formed.size(); ++squadIndex) {
        const auto& squad = formed[squadIndex];
        auto requiredRatio = squad.requiredRatio;
        auto objective = squad.objective;
        auto defense = squad.defense;
        // Protecting an economy does not make a losing outward chase safe.
        // A breached screen permits nearby interception, never a ratio override.
        if (squad.role == SquadRole::baseDefense) {
            requiredRatio = std::max(requiredRatio,
                                     plan_.posture == Posture::defend ? 1.25 : 1.05);
        }
        if (squad.role == SquadRole::mainArmy) {
            if (plan_.expansionTarget.valid() && plan_.posture != Posture::attack)
                objective = expansionAssemblyPoint(state_, plan_.expansionTarget, retreatPoint());
            const auto undersizedVanguard =
                vanguard == &squad &&
                squad.units.size() <
                    static_cast<std::size_t>(std::max(1, plan_.minimumAttackSize));
            if (!aggressive || undersizedVanguard) {
                // A small squad may move toward its rally point, but it must
                // not accept an equal-size fight on the way there. The old
                // 0.88 ratio made a five-Zealot vanguard engage four-to-six
                // enemy Zealots before the next reinforcement arrived.
                requiredRatio = squad.enemies.empty()
                                    ? 0.88
                                    : (undersizedVanguard ? 1.18 : 1.05);
                objective = plan_.expansionTarget.valid()
                    ? expansionAssemblyPoint(state_, plan_.expansionTarget, retreatPoint()) : plan_.rallyPoint;
                defense = SquadPlanner::defensiveArea(state_, plan_.rallyPoint);
            } else if (vanguard != nullptr && vanguard != &squad &&
                       squad.enemies.empty()) {
                // Detached reinforcements join the strongest mobile component
                // instead of launching a second, usually losing attack wave.
                requiredRatio = 0.88;
                objective = vanguard->center;
            }
            if (vanguard == &squad && aggressive)
                objective = SquadPlanner::supportRendezvous(state_, squad, objective);
            if (plan_.expansionTarget.valid() && plan_.posture != Posture::attack &&
                vanguard == &squad &&
                distanceSquared(squad.center, plan_.expansionTarget) <= 448 * 448)
                defense = {plan_.expansionTarget, 448, plan_.expansionTarget};
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
        const auto proposedDecision = estimate.decision;
        const auto engagementKey = engagements_.identify(squad.units, state_.frame);
        estimate.decision = engagements_.stabilize(
            engagementKey, estimate.decision, estimate.ratio,
            requiredRatio, state_.frame, !squad.enemies.empty());
        if (squad.enemies.empty() && estimate.decision == FightDecision::kite)
            estimate.decision = FightDecision::retreat;
        if (squad.withdrawing) estimate.decision = FightDecision::retreat;
        estimate.holdScreen = SquadPlanner::mustHoldDefensiveScreen(squad);
        estimate.advanceBlocked = squad.role != SquadRole::baseDefense &&
            !SquadPlanner::mobileDetectionReady(state_, squad);
        if (!estimate.advanceBlocked && SquadPlanner::canCounterattack(squad, estimate, plan_)) {
            // A global defense response must not trap an independently strong
            // reserve army while the allocated defenders protect the base.
            defense = {};
            objective = plan_.attackTarget;
            if (firstCounterattackFrame_ < 0) firstCounterattackFrame_ = state_.frame;
        }
        const auto reason = estimate.advanceBlocked ? "Wait for mobile detection" :
            estimate.holdScreen ? "Protect economy: intercept within reach" :
            !squad.missionReason.empty() ? squad.missionReason.c_str() :
            estimate.decision != proposedDecision ?
                (estimate.decision == FightDecision::engage ? "Commit: awaiting sustained contrary evidence" :
                                                            "Regroup: await stable advantage") :
            estimate.decision == FightDecision::retreat ?
                (defense.front.valid() ? "Hold terrain: unfavorable fight" : "Retreat: unfavorable fight") :
            estimate.decision == FightDecision::kite ? "Fire and reposition" :
            !squad.enemies.empty() ? "Local fight accepted" :
            defense.front.valid() ? "Occupy defensive terrain" :
            plan_.expansionTarget.valid() && plan_.posture != Posture::attack
                ? "Cover expansion" : "Assemble / advance";
        if (log_ && logSquads) {
            log_ << "SQUAD," << state_.frame << ',' << squadRoleName(squad.role)
                 << ",key=" << engagementKey << ",units=" << squad.units.size()
                 << ",enemies=" << squad.enemies.size()
                 << ",center=" << squad.center.x << 'x' << squad.center.y
                 << ",objective=" << objective.x << 'x' << objective.y
                 << ",ratio=" << estimate.ratio << ",required=" << requiredRatio
                 << ",proposed=" << static_cast<int>(proposedDecision)
                 << ",decision=" << static_cast<int>(estimate.decision)
                 << ",confidence=" << estimate.confidence << ",simulation=" << runSimulation
                 << ",friendlyRemaining=" << estimate.simulatedFriendlyRemaining
                 << ",enemyRemaining=" << estimate.simulatedEnemyRemaining
                 << ",detectionBlocked=" << estimate.advanceBlocked << ",reason=" << reason << ",members=";
            for (const auto& member : squad.units) log_ << member.id << ';';
            log_ << '\n';
        }
        debug_.squads.push_back({std::string(squadRoleName(squad.role)), reason, squad.center,
            objective, squad.retreat, estimate.ratio, requiredRatio,
            static_cast<int>(squad.units.size()), static_cast<int>(squad.enemies.size()), estimate.decision});
        if (squad.role == SquadRole::mainArmy && squad.units.size() >= debugSquadSize) {
            debugSquadSize = squad.units.size();
            fight_ = estimate;
        }
        const auto targets = SquadPlanner::tacticalTargets(squad, state_.enemy.units);
        for (const auto& order : tactics_.control(
                 squad.units, targets, estimate, objective,
                 squad.retreat, influence_, squad.center, state_.latencyFrames,
                 technologyLevel(state_.self, TechnologyKind::psionicStorm) > 0,
                 defense, squad.withdrawing ? TacticalIntent::withdraw :
                     squad.role == SquadRole::harassment ? TacticalIntent::raid : TacticalIntent::battle)) {
            commands_.submit(order);
        }
    }
    if (logSquads) lastSquadLogFrame_ = state_.frame;

    for (const auto& order : clearExpansionFootprint(state_, plan_.expansionTarget,
             expansionAssemblyPoint(state_, plan_.expansionTarget, retreatPoint()))) commands_.submit(order);

    for (const auto& order : tactics_.recharge(state_.self.units,
                                              plan_.posture == Posture::defend)) {
        commands_.submit(order);
    }

    detectorEscorts_.clear();
    for (const auto& order : squads_.detectorEscorts(state_, formed, influence_)) {
        detectorEscorts_.push_back(order.actor);
        commands_.submit(order);
    }
    for (const auto& order : transportOrders) {
        commands_.submit(order);
    }
    // BWAPI calls are capped per combat tick. Priority-aware rotation keeps
    // retreat and detector orders immediate while bounding large-army spikes.
    const auto selectedCommands = commands_.finalize(commandLimit);
    const auto& commandStats = commands_.stats();
    commandsProposed_ += commandStats.proposed;
    commandsSuperseded_ += commandStats.superseded;
    commandsRedundant_ += commandStats.redundant;
    commandsDeferred_ += commandStats.budgetDeferred;
    for (const auto& command : selectedCommands) {
        ++commandsAttempted_;
        const auto accepted = bridge_.execute(command);
        if (accepted) {
            ++commandsAccepted_;
            commands_.markIssued(command);
            debug_.orders[command.actor] = command.source;
        }
        std::ostringstream entry;
        entry << "ORDER," << command.actor << ',' << static_cast<int>(command.type) << ','
              << command.targetUnit << ',' << command.targetPosition.x << ',' << command.targetPosition.y
              << ',' << command.source << ',' << accepted;
        // Moving orders naturally change by a few pixels on almost every
        // combat tick. Deduplicate on intent while retaining exact coordinates
        // in the sampled row.
        const auto comparison = std::to_string(static_cast<int>(command.type)) + '/' +
            command.source + '/' + std::to_string(accepted);
        trace("order/" + std::to_string(command.actor), entry.str(), 120, comparison);
    }
}

std::vector<UnitSnapshot> ProtoddModule::combatUnits(const bool ours) const {
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

Position ProtoddModule::retreatPoint() const {
    const auto nexus = std::ranges::find(state_.self.units, UnitKind::nexus, &UnitSnapshot::kind);
    return nexus != state_.self.units.end() ? nexus->position : plan_.rallyPoint;
}

void ProtoddModule::sampleTelemetry() {
    if (!log_ || state_.frame == lastTelemetryFrame_) return;
    lastTelemetryFrame_ = state_.frame;

    const auto army = static_cast<int>(std::ranges::count_if(
        state_.self.units, [](const UnitSnapshot& unit) {
            return unit.completed && isCombatUnit(unit.kind) && !unit.flying;
        }));
    const auto probes = countUnits(state_.self.units, UnitKind::probe);
    const auto nexuses = countUnits(state_.self.units, UnitKind::nexus);
    const auto completedNexuses = countUnits(state_.self.units, UnitKind::nexus, true);
    const auto visibleEnemyArmy = static_cast<int>(std::ranges::count_if(
        state_.enemy.units, [](const UnitSnapshot& unit) {
            return unit.visible && unit.completed && isCombatUnit(unit.kind);
        }));
    const auto& threat = opponent_.assessment();
    const auto emit = [this](const std::string_view kind, const std::string_view value) {
        if (log_) log_ << "EVENT," << state_.frame << ',' << kind << ',' << csvSafe(value) << '\n';
    };
    const auto emitFrame = [&emit](const std::string_view kind, const Frame frame) {
        if (frame >= 0) emit(kind, std::to_string(frame));
    };

    ++telemetrySamples_;
    maxArmyCount_ = std::max(maxArmyCount_, army);
    maxProbeCount_ = std::max(maxProbeCount_, probes);
    maxNexusCount_ = std::max(maxNexusCount_, nexuses);
    maxEnemyVisibleArmy_ = std::max(maxEnemyVisibleArmy_, visibleEnemyArmy);
    peakMinerals_ = std::max(peakMinerals_, state_.self.minerals);
    peakGas_ = std::max(peakGas_, state_.self.gas);
    if (state_.self.supplyTotal > 0 && state_.self.supplyUsed >= state_.self.supplyTotal) {
        ++supplyBlockSamples_;
    }
    if (state_.frame >= 7200 && state_.self.minerals >= 800) ++highBankSamples_;

    if (visibleEnemyArmy > 0 && firstEnemyContactFrame_ < 0) {
        firstEnemyContactFrame_ = state_.frame;
        emitFrame("enemy-contact", firstEnemyContactFrame_);
    }
    if (threat.combatEnemiesNearMain > 0 && firstBaseBreachFrame_ < 0) {
        firstBaseBreachFrame_ = state_.frame;
        emitFrame("base-breach", firstBaseBreachFrame_);
    }
    if (countUnits(state_.self.units, UnitKind::cyberneticsCore, true) > 0 &&
        firstCoreFrame_ < 0) {
        firstCoreFrame_ = state_.frame;
        emitFrame("core-complete", firstCoreFrame_);
    }
    if (countUnits(state_.self.units, UnitKind::dragoon, true) > 0 && firstDragoonFrame_ < 0) {
        firstDragoonFrame_ = state_.frame;
        emitFrame("dragoon-complete", firstDragoonFrame_);
    }
    if (technologyLevel(state_.self, TechnologyKind::singularityCharge) > 0 &&
        firstRangeFrame_ < 0) {
        firstRangeFrame_ = state_.frame;
        emitFrame("range-complete", firstRangeFrame_);
    }
    if (nexuses >= 2 && firstExpansionFrame_ < 0) {
        firstExpansionFrame_ = state_.frame;
        emitFrame("second-nexus", firstExpansionFrame_);
    }
    if (firstAttackFrame_ < 0 &&
        (plan_.posture == Posture::pressure || plan_.posture == Posture::attack ||
         plan_.posture == Posture::harass)) {
        firstAttackFrame_ = state_.frame;
        emitFrame("attack-posture", firstAttackFrame_);
    }
    if (army == 0 && maxArmyCount_ >= 4 && firstArmyZeroFrame_ < 0) {
        firstArmyZeroFrame_ = state_.frame;
        emitFrame("army-zero", firstArmyZeroFrame_);
    }
    if (lastCompletedNexusCount_ > 0 && completedNexuses == 0 && firstNexusLossFrame_ < 0) {
        firstNexusLossFrame_ = state_.frame;
        emitFrame("nexus-loss", firstNexusLossFrame_);
    }
    if (firstCounterattackFrame_ >= 0 && lastEventFrame_ != firstCounterattackFrame_) {
        lastEventFrame_ = firstCounterattackFrame_;
        emitFrame("counterattack", firstCounterattackFrame_);
    }
    if (lastPlanName_.empty()) {
        lastPlanName_ = plan_.name;
        lastPosture_ = plan_.posture;
    } else {
        if (plan_.name != lastPlanName_) {
            ++planChanges_;
            emit("plan-change", plan_.name);
            lastPlanName_ = plan_.name;
        }
        if (plan_.posture != lastPosture_) {
            ++postureChanges_;
            emit("posture-change", postureName(plan_.posture));
            lastPosture_ = plan_.posture;
        }
    }

    lastArmyCount_ = army;
    lastProbeCount_ = probes;
    lastNexusCount_ = nexuses;
    lastCompletedNexusCount_ = completedNexuses;
    lastEnemyVisibleArmy_ = visibleEnemyArmy;
}

void ProtoddModule::recordPerformance(const Frame frame, const std::int64_t elapsedUs) {
    if (!log_) return;
    if (slowWindowStart_ >= 0 && frame - slowWindowStart_ >= 24) {
        flushPerformanceRecord();
    }
    if (elapsedUs < 28000) return;
    if (slowWindowStart_ < 0) slowWindowStart_ = frame;
    if (elapsedUs > slowWindowPeakUs_) {
        slowWindowPeakUs_ = elapsedUs;
        slowWindowPeakFrame_ = frame;
        slowWindowLoad_ = frameBudget_.load(frame);
    }
}

void ProtoddModule::flushPerformanceRecord() {
    if (!log_ || slowWindowPeakFrame_ < 0) return;
    log_ << "PERF," << slowWindowPeakFrame_ << ',' << slowWindowPeakUs_ << ','
         << runtimeLoadName(slowWindowLoad_) << '\n';
    slowWindowStart_ = slowWindowPeakFrame_ = -1;
    slowWindowPeakUs_ = 0;
    slowWindowLoad_ = RuntimeLoad::normal;
}

void ProtoddModule::trace(
    std::string key,
    std::string value,
    const Frame heartbeat,
    std::string comparison) {
    if (!log_) return;
    auto& previous = traceMemory_[key];
    if (comparison.empty()) comparison = value;
    if (comparison == previous.value && state_.frame - previous.frame < heartbeat) return;
    const auto comma = value.find(',');
    if (comma == std::string::npos) return;
    log_ << value.substr(0, comma) << ',' << state_.frame << value.substr(comma) << '\n';
    previous = {std::move(comparison), state_.frame};
}

void ProtoddModule::logDiagnostics() {
    auto idleGateways = 0;
    auto usableGateways = 0;
    auto idleWorkers = 0;
    auto unpowered = 0;
    for (const auto unit : BWAPI::Broodwar->self()->getUnits()) {
        if (unit == nullptr || !unit->exists() || !unit->isCompleted()) continue;
        if (unit->getType().requiresPsi() && !unit->isPowered()) ++unpowered;
        if (unit->getType() == BWAPI::UnitTypes::Protoss_Gateway && unit->isPowered()) {
            ++usableGateways;
            if (!unit->isTraining() && unit->getRemainingTrainTime() == 0 &&
                unit->getTrainingQueue().empty() &&
                unit->getLastCommandFrame() + std::max(1, state_.latencyFrames) < state_.frame)
                ++idleGateways;
        }
        if (unit->getType().isWorker() && unit->isIdle()) ++idleWorkers;
    }
    const auto blocked = state_.self.supplyTotal > 0 && state_.self.supplyTotal < 400 &&
        state_.self.supplyTotal - state_.self.supplyUsed < 4;
    supplyBlockedFrames_.sample(state_.frame, blocked ? 1 : 0);
    idleGatewayFrames_.sample(state_.frame, idleGateways);
    idleWorkerFrames_.sample(state_.frame, idleWorkers);
    debug_.health = "Idle Gateways " + std::to_string(idleGateways) + "/" +
        std::to_string(usableGateways) + " | idle Probes " + std::to_string(idleWorkers) +
        " | supply tight " + std::to_string(supplyBlockedFrames_.total() / 24) + "s";
    if (!log_) return;
    const auto feedback = bridge_.expansionFeedback();
    log_ << "HEALTH," << state_.frame << ",idleGateways=" << idleGateways
         << ",gateways=" << usableGateways << ",idleWorkers=" << idleWorkers
         << ",unpowered=" << unpowered << ",supplyTightFrames=" << supplyBlockedFrames_.total()
         << ",idleGatewayFrames=" << idleGatewayFrames_.total()
         << ",idleWorkerFrames=" << idleWorkerFrames_.total()
         << ",commandsAttempted=" << commandsAttempted_ << ",commandsAccepted=" << commandsAccepted_
         << ",commandsProposed=" << commandsProposed_ << ",commandsSuperseded=" << commandsSuperseded_
         << ",commandsRedundant=" << commandsRedundant_ << ",commandsDeferred=" << commandsDeferred_
         << ",macroAttempted=" << macroAttempted_ << ",macroAccepted=" << macroAccepted_
         << ",expansionPending=" << feedback.pending << ",expansionStalled=" << feedback.stalledFrames
         << ",expansionDeferred=" << plan_.deferExpansion
         << ",minerals=" << state_.self.minerals << ",gas=" << state_.self.gas
         << ",minedMinerals=" << state_.self.gatheredMinerals << ",minedGas=" << state_.self.gatheredGas
         << ",supply=" << state_.self.supplyUsed << ",supplyTotal=" << state_.self.supplyTotal
         << ",probes=" << countUnits(state_.self.units, UnitKind::probe, true)
         << ",bases=" << countUnits(state_.self.units, UnitKind::nexus, true) << '\n';
    if (state_.frame % 240 == 0) {
        for (const auto& [name, timing] : phases_)
            log_ << "PHASE," << state_.frame << ',' << name << ',' << timing.calls << ','
                 << timing.totalUs << ',' << timing.peakUs << '\n';
        for (auto raw = 0; raw < static_cast<int>(EnemyPlan::count); ++raw) {
            const auto kind = static_cast<EnemyPlan>(raw);
            log_ << "BELIEF," << state_.frame << ',' << enemyPlanName(kind) << ','
                 << opponent_.probability(kind) << '\n';
        }
        const auto entities = [this](const PlayerSnapshot& player, const char* side) {
            for (const auto& unit : player.units) {
                const auto order = debug_.orders.find(unit.id);
                log_ << "ENTITY," << state_.frame << ',' << side << ',' << unit.id << ','
                     << unitStats(unit.kind).name << ',' << unit.position.x << ',' << unit.position.y
                     << ',' << unit.hitPoints << ',' << unit.shields << ',' << unit.weaponCooldown
                     << ',' << unit.orderTargetId << ',' << unit.lastSeen << ',' << unit.visible
                     << ',' << (unit.ours && order != debug_.orders.end() ? order->second : "") << '\n';
            }
        };
        entities(state_.self, "self");
        entities(state_.enemy, "enemy");
    }
    log_.flush();
}

void ProtoddModule::logDecision() {
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
    const auto robotics = countUnits(UnitKind::roboticsFacility, false);
    const auto completedRobotics = countUnits(UnitKind::roboticsFacility, true);
    const auto supportBays = countUnits(UnitKind::roboticsSupportBay, false);
    const auto completedSupportBays = countUnits(UnitKind::roboticsSupportBay, true);
    const auto observatories = countUnits(UnitKind::observatory, false);
    const auto completedObservatories = countUnits(UnitKind::observatory, true);
    const auto completedNexuses = countUnits(UnitKind::nexus, true);
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
    // Observability must never invoke the stateful planner a second time.
    const auto& diagnosticLedger = lastLedger_;
    const auto& diagnosticActions = debug_.macro;
    const auto& threat = opponent_.assessment();
    const auto workerGas = std::ranges::count_if(
        state_.self.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::probe && unit.gatheringGas;
        });
    const auto workerCarrying = std::ranges::count_if(
        state_.self.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::probe && unit.carryingResources;
        });
    const auto workerUnderAttack = std::ranges::count_if(
        state_.self.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::probe && unit.underAttack;
        });
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
         << ",reavers=" << reavers
         << ",robotics=" << robotics << '/' << completedRobotics
         << ",support=" << supportBays << '/' << completedSupportBays
         << ",observatory=" << observatories << '/' << completedObservatories
         << ",dt=" << darkTemplar
         << ",ht=" << highTemplar << ",storm=" << (stormReady ? 1 : 0)
         << ",army=" << mobileArmy
         << ",enemyVisibleArmy=" << visibleEnemyArmy
         << ",minAttack=" << plan_.minimumAttackSize
         << ",sustainEconomy=" << plan_.sustainEconomy
         << ",breakContainment=" << plan_.breakContainment
         << ",expansionTarget=" << plan_.expansionTarget.x << 'x' << plan_.expansionTarget.y
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
    log_ << ",bases=" << nexuses << '/' << completedNexuses
         << ",desiredBases=" << plan_.desiredBases
         << ",desiredWorkers=" << plan_.desiredWorkers
         << ",desiredGas=" << plan_.desiredGasWorkers
         << ",attackRatio=" << plan_.attackThreshold
         << ",threat=" << threat.immediateGround << '/' << threat.combatEnemiesNearMain
         << '/' << threat.approachingCombatEnemies << '/' << threat.estimatedArmyValue
         << '/' << threat.approachingArmyValue << '/' << threat.enemyProductionCapacity
         << ",enemyComp=" << composition(state_.enemy.units, true)
         << ",selfComp=" << composition(state_.self.units, false)
         << ",workersStatus=" << workerGas << '/' << workerCarrying << '/' << workerUnderAttack
         << ",macroFrame=" << lastMacroFrame_
         << ",ledger=" << diagnosticLedger.freeMinerals() << '/'
         << diagnosticLedger.freeGas() << '/' << diagnosticLedger.reservedMinerals << '/'
         << diagnosticLedger.reservedGas
         << ",goals=" << goalSummary(plan_.goals)
         << ",actionDetail=";
    for (std::size_t index = 0; index < diagnosticActions.size() && index < 12; ++index) {
        if (index > 0) log_ << ';';
        const auto& action = diagnosticActions[index];
        log_ << static_cast<int>(action.action) << ':' << unitStats(action.target).name << ':'
             << action.priority << ':' << action.minerals << ':' << action.gas << ':'
             << (!action.reserved ? 'W' : (action.executable ? 'R' : 'H')) << ':'
             << csvSafe(action.reason);
    }
    log_ << '\n';
    log_.flush();
}

}  // namespace protodd::bwapi
