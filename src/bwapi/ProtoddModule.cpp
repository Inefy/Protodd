#include "ProtoddModule.hpp"
#include "protodd/CommandEffectFeedback.hpp"

#include "protodd/FrameSchedule.hpp"
#include "AtomicFile.hpp"
#include "BoundedFile.hpp"

#include "protodd/Technology.hpp"
#include "protodd/ProductionReadiness.hpp"
#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

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
    constexpr auto maximumControlFileBytes = 64 * 1024;
    const auto result = protodd::bwapi::readBoundedFile(path, maximumControlFileBytes);
    return result.withinLimit ? result.contents : std::string{};
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

std::string_view spellCommandName(const protodd::CommandType type) {
    switch (type) {
        case protodd::CommandType::useTech: return "useTech";
        case protodd::CommandType::feedback: return "feedback";
        case protodd::CommandType::mergeArchon: return "mergeArchon";
        default: return "other";
    }
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

#ifdef PROTODD_ENGINE_FAULT_INJECTION
void ProtoddModule::configureAuditFaultInjection(std::string kind, std::string target,
                                                 const std::uint32_t count) {
    if (kind == "phase" && target == "production-observe")
        production_.enableAuditProbe();
    if (kind == "phase" && target == "whole-game-observe")
        wholeGame_.enableAuditProbe();
    auditFaultKind_ = std::move(kind);
    auditFaultTarget_ = std::move(target);
    auditFaultRemaining_ = count;
}

void ProtoddModule::configureAuditSlowObservation(
    const std::uint32_t count, const std::uint32_t delayUs) {
    auditSlowObservationRemaining_ = count;
    auditSlowObservationDelayUs_ = delayUs;
}

void ProtoddModule::maybeInjectAuditFault(const std::string_view kind,
                                          const std::string_view target) {
    if (auditFaultRemaining_ == 0 || kind != auditFaultKind_ ||
        target != auditFaultTarget_) return;
    --auditFaultRemaining_;
    throw std::runtime_error("audit-injected-" + auditFaultKind_ + "-" + auditFaultTarget_);
}
#endif

void ProtoddModule::recordCallbackFailure(const std::string_view callbackName,
                                          const std::string_view message) noexcept {
    ++caughtErrors_;

    try {
        const std::string key(callbackName);
        auto [lastError, inserted] = lastCallbackErrorFrames_.try_emplace(key, -1000);
        if (!inserted && state_.frame - lastError->second < 24) return;
        const auto record = "CALLBACK_ERROR,frame=" + std::to_string(state_.frame) +
            ",callback=" + csvSafe(callbackName) + ",phase=" +
            csvSafe(activePhase_ ? activePhase_ : "unknown") + ",error=" +
            csvSafe(message) + ",total=" + std::to_string(caughtErrors_) + "\n";
        if (log_) {
            log_ << record;
            log_.flush();
            if (log_) {
                lastError->second = state_.frame;
                return;
            }
        }

        std::error_code error;
        std::filesystem::create_directories("bwapi-data/write", error);
        std::ofstream fallback("bwapi-data/write/Protodd.log", std::ios::app);
        if (fallback) {
            fallback << record;
            fallback.flush();
            if (fallback) {
                lastError->second = state_.frame;
                return;
            }
        }
    } catch (...) {
    }
    ++loggingErrors_;
}

void ProtoddModule::onPhaseDisabled(const std::string_view phase,
                                   const std::uint32_t consecutiveFailures) noexcept {
    const char* fallback = "skip-repeatedly-failing-phase";
    if (phase == "observe") {
        fallback = "suspend-frame-work-without-fresh-observation";
    } else if (phase.starts_with("whole-game-")) {
        fallback = "disable-whole-game-and-resume-native-controller";
        hybridControl_ = false;
        try { wholeGame_.end(); } catch (...) { ++loggingErrors_; }
    } else if (phase.starts_with("production-")) {
        fallback = "disable-production-adapter";
        try { production_.disable(log_); } catch (...) { ++loggingErrors_; }
    } else if (phase.starts_with("model-")) {
        fallback = "disable-learned-shadow-model";
        model_.disable(log_, phase);
    }

    try {
        if (log_) {
            log_ << "PHASE_DISABLED,frame=" << state_.frame
                 << ",phase=" << csvSafe(phase)
                 << ",consecutive_failures=" << consecutiveFailures
                 << ",fallback=" << fallback << '\n';
            log_.flush();
        }
    } catch (...) {
        ++loggingErrors_;
    }
}

void ProtoddModule::onStart() {
    callbackBoundary("onStart", [this] { onStartImpl(); });
}

void ProtoddModule::onStartImpl() {
    activePhase_ = "startup";
    if (log_.is_open()) log_.close();
    log_.clear();
    caughtErrors_ = loggingErrors_ = 0;
    lastCallbackErrorFrames_.clear();
    BWAPI::Broodwar->setCommandOptimizationLevel(2);
    BWAPI::Broodwar->setLatCom(true);
    std::error_code error;
    std::filesystem::create_directories("bwapi-data/write", error);
    opponentName_ = BWAPI::Broodwar->enemy() ? BWAPI::Broodwar->enemy()->getName() : "unknown";
    mapName_ = BWAPI::Broodwar->mapName();
    historyMapIdentity_ = mapName_ + "#" + BWAPI::Broodwar->mapHash();
    gameOutcomeId_ = protodd::bwapi::uniqueToken();
    log_.open("bwapi-data/write/Protodd.log", std::ios::app);
    if (log_) {
        log_ << "BOOT," << csvSafe(mapName_) << ',' << csvSafe(opponentName_) << '\n';
        log_.flush();
    }
#ifdef PROTODD_DEVELOPER_PROFILE
    // Interactive commands and overlays are only present in a developer build.
    BWAPI::Broodwar->enableFlag(BWAPI::Flag::UserInput);
#endif
    bridge_.onStart();
    wholeGame_.start();
    hybridProposals_.clear();
    urgentEvents_.clear();
    previousStorms_.clear();
    previousStorms_.reserve(16);
    previousWorkerLineThreats_.clear();
    currentWorkerLineThreats_.clear();
    previousWorkerLineThreats_.reserve(16);
    currentWorkerLineThreats_.reserve(16);
    previousCloakedThreats_.clear();
    currentCloakedThreats_.clear();
    previousCloakedThreats_.reserve(8);
    currentCloakedThreats_.reserve(8);
    hybridReceived_ = hybridTargets_ = hybridSubmitted_ = hybridAccepted_ = 0;
    hybridControl_ = false;
#ifdef PROTODD_WHOLE_GAME_HYBRID
    const auto hybridMode = protodd::bwapi::readBoundedFile(
        "bwapi-data/read/WholeGame-hybrid-mode.txt", 64 * 1024);
    hybridControl_ = wholeGame_.controlling() && hybridMode.withinLimit &&
        !hybridMode.contents.starts_with("shadow");
#endif
    state_ = bridge_.observe();
    if (log_) {
        log_ << "CONTROLLER,whole-game,weights=" << wholeGame_.modelLoaded()
             << ",control=" << wholeGame_.controlling()
#ifdef PROTODD_WHOLE_GAME_HYBRID
             << ",mode=hybrid,hybridControl=" << hybridControl_
#else
             << ",mode=exclusive-or-shadow"
#endif
             << '\n';
    }
    if (log_) {
        log_ << "FEATURE_MANIFEST,profile="
#ifdef PROTODD_TOURNAMENT_PROFILE
             << "tournament"
#elif defined(PROTODD_DEVELOPER_PROFILE)
             << "developer"
#else
             << "unprofiled"
#endif
#ifdef PROTODD_DEVELOPER_PROFILE
             << ",debug_overlay=1,user_input="
#else
             << ",debug_overlay=0,user_input="
#endif
             << BWAPI::Broodwar->isFlagEnabled(BWAPI::Flag::UserInput)
#ifdef PROTODD_WHOLE_GAME_RESEARCH_AUTHORITY
             << ",learned_research_authority=1"
#else
             << ",learned_research_authority=0"
#endif
             << ",whole_game_control=" << wholeGame_.controlling()
#ifdef PROTODD_WHOLE_GAME_HYBRID
             << ",whole_game_hybrid_control=" << hybridControl_
#else
             << ",whole_game_hybrid_control=0"
#endif
             << '\n';
    }
    navigation_ = bridge_.navigationGrid();
    bridge_.initializeNavigationObstacles(navigation_);
    opponent_.reset(state_.enemy.race);
    strategy_.reset();
    strategicDirector_.reset();
    expansion_.reset();
#ifdef PROTODD_PVZ_EARLY_MINERAL_FALLBACK
    macro_ = MacroPlanner{true, true};
#elif defined(PROTODD_PVZ_MINERAL_FALLBACK)
    macro_ = MacroPlanner{true};
#else
    macro_ = MacroPlanner{};
#endif
    workers_ = {};
    plan_ = {};
    fight_ = {};
    phases_.clear();
    phaseFailurePolicy_.reset();
    traceMemory_.clear();
    actionTotals_.clear();
    spellAttemptTotals_.clear();
    spellEffectTotals_.clear();
    pendingSpellEffects_.clear();
    lastActions_.clear();
    damageSamples_.clear();
    incidents_.clear();
    motionSamples_.clear();
    bridge_.actionDiagnostic = [this](const ActionDiagnostic& action) { logAction(action); };
    bridge_.buildLeaseDiagnostic = [this](const BuildLeaseDiagnostic& lease) { logBuildLease(lease); };
    bridge_.buildSelectionDiagnostic = [this](const BuildSelectionDiagnostic& selection) {
        logBuildSelection(selection);
    };
    bridge_.buildRouteDiagnostic = [this](const BuildRouteDiagnostic& route) {
        logBuildRoute(route);
    };
    supplyTightFrames_.reset();
    supplyHardBlockedFrames_.reset();
    supplyUnintendedBlockedFrames_.reset();
    supplyDeliberateOpeningPauseFrames_.reset();
    idleGatewayFrames_.reset();
    idleWorkerFrames_.reset();
    idleTrainingProducerFrames_.reset();
    affordableProducerIdleFrames_.reset();
    supplyCappedFrames_.reset();
    commandsAttempted_ = commandsAccepted_ = macroAttempted_ = macroAccepted_ = 0;
    commandsProposed_ = commandsSuperseded_ = commandsRedundant_ = commandsDeferred_ = 0;
    lastMacroFrame_ = lastSquadLogFrame_ = -1;
    spendingLedger_ = {};
    influence_ = InfluenceMap(64);
    commands_.clear();
    engagements_.reset();
    squads_.reset();
    transports_.reset();
    scouts_.reset();
    frameBudget_.reset();
    debug_ = {};
    detectorEscorts_.clear();
    requiredDetectorCount_ = 0;
    leasedScouts_.clear();
    pendingScoutOrders_.clear();
    scoutingDispatchPending_ = false;
    advanceWaypoints_.clear();
    navigationSignatures_.clear();
    advanceRoutes_.clear();
    stalledAdvances_.clear();
    retreatRouteChecks_.clear();
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
    slowWindowStart_ = slowWindowPeakFrame_ = -1;
    slowWindowPeakUs_ = 0;
    slowWindowLoad_ = RuntimeLoad::normal;

    const auto historyFile = OpponentHistory::filename(opponentName_);
    policy_.start();
    model_.start(log_);
    production_.start(log_);
    tacticalTargetControl_ = false;
#ifdef PROTODD_TACTICAL_LOCAL_EVALUATION
    const auto targetModelLoaded = tacticalTarget_.load(
        "bwapi-data/read/TacticalTarget-weights.bin");
    const auto targetMode = readFile("bwapi-data/read/TacticalTarget-mode.txt");
    tacticalTargetControl_ = targetModelLoaded && targetMode.starts_with("local-target");
    if (log_) log_ << "TACTICAL_TARGET,loaded=" << targetModelLoaded
                   << ",control=" << tacticalTargetControl_ << '\n';
#else
    if (log_) log_ << "TACTICAL_TARGET,unavailable,control=0\n";
#endif
    workerTrainingProfile_ = WorkerTrainingProfile::baseline;
    workerTrainingEnabled_ = false;
    const auto workerMode = readFile("bwapi-data/read/WorkerTraining-mode.txt");
    if (!workerMode.empty()) {
#ifdef PROTODD_PRODUCTION_LOCAL_EVALUATION
        if (workerMode.starts_with("baseline")) workerTrainingEnabled_ = true;
        else if (workerMode.starts_with("plus-one")) {
            workerTrainingProfile_ = WorkerTrainingProfile::plusOne;
            workerTrainingEnabled_ = true;
        } else if (workerMode.starts_with("plus-two")) {
            workerTrainingProfile_ = WorkerTrainingProfile::plusTwo;
            workerTrainingEnabled_ = true;
        }
        log_ << "WORKER_TRAINING_MODE," << (workerTrainingEnabled_ ?
            workerTrainingProfile_ == WorkerTrainingProfile::baseline ? "baseline" :
            workerTrainingProfile_ == WorkerTrainingProfile::plusOne ? "plus-one" : "plus-two" :
            "invalid") << ",enabled=" << workerTrainingEnabled_ << '\n';
#else
        log_ << "WORKER_TRAINING_MODE,unavailable,enabled=0\n";
#endif
    }
    callbackTimes_.clear();
    deferredOptionalPhases_.clear();
    const auto callbackMode = readFile("bwapi-data/read/CallbackAudit-mode.txt");
    callbackAudit_=production_.enabled() || callbackMode == "on";
    if (callbackAudit_) callbackTimes_.reserve(100000);
    bridge_.productionDiagnostic = [this](const BWAPI::UnitCommand& command, bool before, bool accepted) {
        try { return production_.command(command, before, accepted, log_); }
        catch (...) { production_.disable(log_); return !before; }
    };
    bridge_.productionPermission = [this](const BWAPI::UnitCommand& command, std::string_view source) {
        return production_.allows(command,source);
    };
    // Controlled training imports only externally validated results. onEnd
    // alone cannot distinguish a strategic win from an opponent crash.
    const auto learningModeRead = protodd::bwapi::readBoundedFile(
        "bwapi-data/read/Protodd-learning-mode.txt", 64 * 1024);
    const auto& learningMode = learningModeRead.contents;
    const bool frozenLearning = !learningModeRead.withinLimit || learningMode.starts_with("frozen");
    validatedLearning_ = frozenLearning || learningMode.starts_with("validated-train") || policy_.enabled();
    historyPersistenceEnabled_ = learningModeRead.withinLimit && !historyFile.empty();
    if (historyPersistenceEnabled_) {
        const auto readHistory = protodd::bwapi::readBoundedFile(
            std::filesystem::path("bwapi-data/read") / historyFile,
            OpponentHistory::maximumSerializedBytes);
        historyPersistenceEnabled_ = readHistory.withinLimit;
        history_.parse(readHistory.withinLimit ? readHistory.contents : std::string{});
    } else {
        history_.parse({});
    }
    if (!validatedLearning_) {
        if (historyPersistenceEnabled_) {
            const auto writeHistory = protodd::bwapi::readBoundedFile(
                std::filesystem::path("bwapi-data/write") / historyFile,
                OpponentHistory::maximumSerializedBytes);
            historyPersistenceEnabled_ = writeHistory.withinLimit;
            if (writeHistory.withinLimit) history_.merge(writeHistory.contents);
        }
    }
    openingStyle_ = history_.choose(opponentName_, historyMapIdentity_,
        stableSeed(opponentName_ + "|" + historyMapIdentity_), !frozenLearning);
    allIn_.reset(AllInBuild::standard);
#ifdef PROTODD_NATIVE_ALLIN_OPENING
    auto opening = readFile("bwapi-data/read/AllIn-opening.txt");
    if (opening.empty() || opening.starts_with("auto")) {
        // PvP has the strongest comparative evidence. Other matchups remain
        // opt-in experiments until prospective games validate them.
        opening = state_.enemy.race == Race::protoss ? "two-gate-zealot" : "standard";
    }
    allIn_.reset(allInBuild(opening));
#endif
    if (log_) log_ << "ALLIN_SELECTION," << allInBuildName(allIn_.build()) << '\n';
    if (log_) {
        const auto selfStart = BWAPI::Broodwar->self()->getStartLocation();
        log_ << "LEARNING,mode=" << (frozenLearning ? "frozen" :
                    validatedLearning_ ? "validated-train" : "online") << '\n';
        log_ << "START," << csvSafe(BWAPI::Broodwar->mapName()) << ','
             << csvSafe(opponentName_) << ',' << openingStyleName(openingStyle_) << '\n';
        log_ << "MATCH,seed=" << BWAPI::Broodwar->getRandomSeed()
             << ",map_hash=" << BWAPI::Broodwar->mapHash()
             << ",width=" << state_.mapWidthPixels << ",height=" << state_.mapHeightPixels
             << ",self_start_tile_x=" << selfStart.x
             << ",self_start_tile_y=" << selfStart.y << '\n';
        log_ << "DIAGNOSTICS,version=3,sampleFrames=24,entityFrames=24,"
                "beliefFrames=240,orderHeartbeatFrames=120,performanceWindowFrames=24,"
                "damageFrames=1,actionHeartbeatFrames=120,information=legal-observations\n";
        log_.flush();
    }
#ifdef PROTODD_ENGINE_FAULT_INJECTION
    maybeInjectAuditFault("stage", "startup");
#endif
}

void ProtoddModule::onEnd(const bool winner) {
    activePhase_ = "shutdown";
    callbackBoundary("onEnd", [this, winner] { onEndImpl(winner); });
    if (log_.is_open()) {
        log_.flush();
        log_.close();
    }
    log_.clear();
}

void ProtoddModule::onEndImpl(const bool winner) {
    production_.end(log_);
    if (!callbackTimes_.empty()) {
        std::ofstream timing("bwapi-data/write/production-callback-us.bin", std::ios::binary | std::ios::trunc);
        timing.write(reinterpret_cast<const char*>(callbackTimes_.data()),
            static_cast<std::streamsize>(callbackTimes_.size()*sizeof(std::int64_t)));
    }
    wholeGame_.end();
    if (log_) log_ << "HYBRID_SUMMARY,received=" << hybridReceived_
                   << ",targets=" << hybridTargets_ << ",submitted=" << hybridSubmitted_
                   << ",accepted=" << hybridAccepted_ << '\n';
    policy_.end(winner);
    state_.frame = BWAPI::Broodwar->getFrameCount();
    sampleTelemetry();
    logDiagnostics();
    if (!validatedLearning_ && historyPersistenceEnabled_) {
#ifdef PROTODD_ENGINE_FAULT_INJECTION
        maybeInjectAuditFault("stage", "history-output");
#endif
        history_.record(opponentName_, historyMapIdentity_, openingStyle_, winner,
                        gameOutcomeId_);
        const auto historyPath = std::filesystem::path("bwapi-data/write") /
                                 OpponentHistory::filename(opponentName_);
        if (!protodd::bwapi::writeAtomicFile(historyPath, history_.serialize()) && log_)
            log_ << "LEARNING_PERSISTENCE,write=failed,path="
                 << csvSafe(historyPath.filename().string()) << '\n';
    } else if (!validatedLearning_ && log_) {
        log_ << "LEARNING_PERSISTENCE,disabled=invalid-or-oversized-input\n";
    }
    if (log_) {
#ifdef PROTODD_ENGINE_FAULT_INJECTION
        maybeInjectAuditFault("stage", "reporting");
#endif
        flushPerformanceRecord();
        log_ << "TACTICAL_TARGET_SUMMARY,control=" << tacticalTargetControl_
             << ",candidateScores=" << tacticalTarget_.scoreCount()
             << ",comparisons=" << tacticalTarget_.comparisonCount()
             << ",disagreements=" << tacticalTarget_.disagreementCount()
             << ",workerOverCombat=" << tacticalTarget_.workerOverCombatCount()
             << ",buildingOverCombat=" << tacticalTarget_.buildingOverCombatCount()
             << ",combatOverWorker=" << tacticalTarget_.combatOverWorkerCount()
             << ",combatOverBuilding=" << tacticalTarget_.combatOverBuildingCount()
             << ",threatAbandoned=" << tacticalTarget_.threatAbandonedCount() << '\n';
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
             << ",supplyHardBlockedFrames=" << supplyHardBlockedFrames_.total()
             << ",supplyUnintendedBlockedFrames=" << supplyUnintendedBlockedFrames_.total()
             << ",supplyDeliberateOpeningPauseFrames=" << supplyDeliberateOpeningPauseFrames_.total()
             << ",highBankSamples=" << highBankSamples_
             << ",planChanges=" << planChanges_
             << ",postureChanges=" << postureChanges_
             << ",caughtErrors=" << caughtErrors_ << ",loggingErrors=" << loggingErrors_ + bridge_.diagnosticErrors()
             << '\n';
        log_ << "END," << (winner ? "win" : "loss") << ',' << state_.frame << '\n';
        log_.flush();
    }
}

void ProtoddModule::onFrame() {
    callbackBoundary("onFrame", [this] {
        const auto started = std::chrono::steady_clock::now();
        CallbackBudget callbackBudget;
        callbackBoundary("onFrame.runFrame", [this, started, &callbackBudget] {
            runFrame(callbackBudget, started);
        });

        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
        auto frame = state_.frame;
        callbackBoundary("onFrame.frameRead", [&frame] {
            frame = BWAPI::Broodwar->getFrameCount();
        });
        // Keep budget and performance accounting independent from strategy
        // execution. One instrumentation failure must not skip the others.
        callbackBoundary("onFrame.budget", [this, frame, elapsed] {
            frameBudget_.record(frame, elapsed);
        });
        callbackBoundary("onFrame.performance", [this, frame, elapsed] {
            recordPerformance(frame, elapsed);
        });
        callbackBoundary("onFrame.audit", [this, started] {
            if (callbackAudit_ && callbackTimes_.size() < 100000) {
                callbackTimes_.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started).count());
            }
        });
    });
}

void ProtoddModule::onSendText(std::string text) {
    callbackBoundary("onSendText", [this, text = std::move(text)] {
#ifdef PROTODD_DEVELOPER_PROFILE
        if (text == "/debug") debug_.level = (debug_.level + 1) % 3;
        else if (text == "/debug 0") debug_.level = 0;
        else if (text == "/debug 1") debug_.level = 1;
        else if (text == "/debug 2") debug_.level = 2;
#else
        static_cast<void>(text);
#endif
    });
}

void ProtoddModule::runFrame(
    CallbackBudget& callbackBudget,
    const CallbackBudget::Clock::time_point callbackStarted) {
    pendingScoutOrders_.clear();
    scoutingDispatchPending_ = false;
    if (BWAPI::Broodwar->isReplay() || BWAPI::Broodwar->isPaused() ||
        BWAPI::Broodwar->self() == nullptr || BWAPI::Broodwar->enemy() == nullptr) {
        return;
    }
    const auto firstRunEstimateUs = [](const std::string_view phase) {
        if (phase == "strategy" || phase == "whole-game-observe") return std::int64_t{8'000};
        if (phase == "inference" || phase == "production-shadow" ||
            phase == "model-shadow" || phase == "scouting") return std::int64_t{6'000};
        if (phase == "diagnostics" || phase == "state-log" || phase == "damage-log")
            return std::int64_t{4'000};
        return std::int64_t{2'000};
    };
    const auto measure = [this, &callbackBudget, callbackStarted, &firstRunEstimateUs](
        const char* phase, auto&& operation, const bool optional = false) {
        if (phaseFailurePolicy_.disabled(phase)) {
            deferredOptionalPhases_.erase(phase);
            return false;
        }
        auto& timing = phases_[phase];
        if (optional) {
            const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                CallbackBudget::Clock::now() - callbackStarted).count();
            if (!callbackBudget.allowsOptionalWork(
                    elapsedUs, timing.estimatedUs(firstRunEstimateUs(phase)))) {
                timing.deferForBudget();
                deferredOptionalPhases_.insert_or_assign(phase, state_.frame);
                return false;
            }
        }
        activePhase_ = phase;
        const auto start = std::chrono::steady_clock::now();
        try {
#ifdef PROTODD_ENGINE_FAULT_INJECTION
            maybeInjectAuditFault("phase", phase);
#endif
            operation();
        }
        catch (...) {
            timing.record(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count());
            const auto update = phaseFailurePolicy_.recordFailure(phase);
            if (update.newlyDisabled) {
                onPhaseDisabled(phase, update.consecutiveFailures);
                return false;
            }
            throw;
        }
        timing.record(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count());
        phaseFailurePolicy_.recordSuccess(phase);
        if (optional) deferredOptionalPhases_.erase(phase);
        return true;
    };
    const auto optional = [&measure](const char* phase, auto&& operation) {
        return measure(phase, std::forward<decltype(operation)>(operation), true);
    };
    const auto optionalDue = [this](const char* phase, const bool scheduled) {
        return scheduled || deferredOptionalPhases_.contains(phase);
    };
    const auto commandBudgetFrame = BWAPI::Broodwar->getFrameCount();
    bridge_.beginCommandBudget(
        commandBudgetFrame, frameBudget_.combatCommandLimit(commandBudgetFrame));
    if (!measure("observe", [this] {
#ifdef PROTODD_ENGINE_FAULT_INJECTION
        const auto slowObservationInjected = auditSlowObservationRemaining_ > 0 &&
            auditSlowObservationDelayUs_ > 0;
        if (slowObservationInjected) {
            auditSlowObservationCombatCallsBefore_ = phases_["combat"].calls;
            --auditSlowObservationRemaining_;
            std::this_thread::sleep_for(std::chrono::microseconds(
                static_cast<std::int64_t>(auditSlowObservationDelayUs_)));
        }
#endif
        state_ = bridge_.observe();
#ifdef PROTODD_ENGINE_FAULT_INJECTION
        if (slowObservationInjected) auditSlowObservationFrame_ = state_.frame;
#endif
    })) return;
    reconcileSpellEffects();
    reconcileCommandEffects();
    if (protodd::hasNewAreaDamageNearFriendlies(
            state_.storms, previousStorms_, state_.self.units)) {
        urgentEvents_.enqueue(protodd::UrgentEvent::areaDamage);
    }
    previousStorms_.assign(state_.storms.begin(), state_.storms.end());
    currentWorkerLineThreats_.clear();
    currentCloakedThreats_.clear();
    for (const auto& enemy : state_.enemy.units) {
        if (!enemy.visible) continue;
        if (protodd::isWorkerLineThreat(enemy, state_.self.units))
            currentWorkerLineThreats_.push_back(enemy.id);
        if (enemy.cloaked || enemy.burrowed) currentCloakedThreats_.push_back(enemy.id);
    }
    std::ranges::sort(currentWorkerLineThreats_);
    std::ranges::sort(currentCloakedThreats_);
    if (protodd::hasNewUrgentUnitIds(currentWorkerLineThreats_, previousWorkerLineThreats_))
        urgentEvents_.enqueue(protodd::UrgentEvent::workerLineBreach);
    if (protodd::hasNewUrgentUnitIds(currentCloakedThreats_, previousCloakedThreats_))
        urgentEvents_.enqueue(protodd::UrgentEvent::cloakedThreat);
    previousWorkerLineThreats_.swap(currentWorkerLineThreats_);
    previousCloakedThreats_.swap(currentCloakedThreats_);
    const auto urgentEventMask = urgentEvents_.consume();
    const auto urgentWork = protodd::urgentWorkFor(urgentEventMask);
    if (urgentEventMask != 0 && log_) {
        log_ << "URGENT_EVENT," << state_.frame << ",mask="
             << static_cast<unsigned>(urgentEventMask)
             << ",macro=" << urgentWork.updateMacro
             << ",workers=" << urgentWork.updateWorkers
             << ",combat=" << urgentWork.updateCombat << '\n';
    }
    spendingLedger_.beginFrame(state_.self.minerals, state_.self.gas);
    bridge_.setSpendingLedger(&spendingLedger_);
    std::vector<LegalWholeGameCommand> learnedCommands;
    [[maybe_unused]] auto wholeGameObserved = !wholeGame_.enabled();
    if (wholeGame_.enabled()) wholeGameObserved = optional("whole-game-observe", [this, &learnedCommands] {
        learnedCommands = wholeGame_.observe();
    });
#ifdef PROTODD_WHOLE_GAME_HYBRID
    std::erase_if(hybridProposals_, [this](const HybridProposal& proposal) {
        return !hybridControl_ || !wholeGame_.controlling() || state_.frame - proposal.frame >= 24;
    });
    if (urgentWork.updateCombat) hybridProposals_.clear();
    if (wholeGame_.controlling()) {
        hybridReceived_ += learnedCommands.size();
        for (const auto& candidate : learnedCommands) {
            const auto& command = candidate.command;
            if (command.getType() != BWAPI::UnitCommandTypes::Attack_Unit ||
                !command.getUnit() || !command.getTarget()) continue;
            ++hybridTargets_;
            if (!hybridControl_ || urgentWork.updateCombat) continue;
            Command proposal;
            proposal.actor = command.getUnit()->getID();
            proposal.type = CommandType::attackUnit;
            proposal.targetUnit = command.getTarget()->getID();
            std::erase_if(hybridProposals_, [&proposal](const HybridProposal& pending) {
                return pending.command.actor == proposal.actor;
            });
            hybridProposals_.push_back({std::move(proposal), state_.frame});
        }
    }
#elif defined(PROTODD_WHOLE_GAME_CONTROL)
    if (state_.self.race == Race::protoss && wholeGame_.controlling() && wholeGameObserved &&
        urgentEventMask == 0) {
        if (!learnedCommands.empty()) measure("whole-game-control", [this, &learnedCommands] {
            for (const auto& candidate : learnedCommands) {
                ++commandsAttempted_;
                const auto accepted = bridge_.executeWholeGame(candidate.command);
                wholeGame_.recordApiResult(
                    candidate, accepted, state_.frame,
                    accepted ? std::string_view{"accepted"}
                             : std::string_view{bridge_.lastIssueError().toString()});
                if (accepted) ++commandsAccepted_;
            }
        });
        if (optionalDue("damage-log", true)) optional("damage-log", [this] { logDamage(); });
        if (optionalDue("diagnostics", protodd::frame_schedule::diagnosticsDue(state_.frame))) optional("diagnostics", [this] {
            sampleTelemetry();
            logDiagnostics();
        });
        if (log_ && optionalDue("state-log", protodd::frame_schedule::stateLogDue(state_.frame))) optional("state-log", [this] {
            log_ << "LEARNED_STATE," << state_.frame << ",controller=whole-game"
                 << ",weights=" << wholeGame_.modelLoaded()
                 << ",control=" << wholeGame_.controlling()
                 << ",minerals=" << state_.self.minerals << ",gas=" << state_.self.gas
                 << ",composition=" << composition(state_.self.units, false) << '\n';
            log_.flush();
        });
        return;
    }
#endif
    if (optionalDue("damage-log", true)) optional("damage-log", [this] { logDamage(); });
    if (state_.self.race != Race::protoss) {
#ifdef PROTODD_DEVELOPER_PROFILE
        BWAPI::Broodwar->drawTextScreen(8, 8, "Protodd requires Protoss");
#endif
        return;
    }

    commands_.beginFrame(state_.frame, state_.latencyFrames);
    bridge_.beginFrameCommands(commands_, state_.frame);

    const auto cadence = frameBudget_.expensiveCadenceMultiplier(state_.frame);
    const auto combatCadence = std::max(1, state_.latencyFrames) *
                               (frameBudget_.load(state_.frame) == RuntimeLoad::emergency ? 2 : 1);
    if (production_.enabled()) {
        const auto productionObserved = optional("production-observe", [this] {
            production_.observe(state_, log_);
        });
        if (productionObserved) optional("production-shadow", [this] {
            production_.infer(state_.frame, frameBudget_, log_);
        });
    }
    if (model_.enabled()) {
        const auto modelObserved = optional("model-observe", [this] { model_.observe(state_); });
        if (modelObserved) optional("model-shadow", [this] {
            model_.infer(state_.frame, frameBudget_, log_);
        });
    }
    // Work is staggered to keep frame time predictable under tournament load.
    if (optionalDue("influence", protodd::frame_schedule::influenceDue(state_.frame, cadence)))
        optional("influence", [this] { influence_.update(state_); });
    if (optionalDue("inference", protodd::frame_schedule::inferenceDue(state_.frame, cadence)))
        optional("inference", [this] { opponent_.update(state_); });
    const auto strategyDue = protodd::frame_schedule::strategyDue(state_.frame, plan_.goals.empty());
    const auto strategyUpdated = optionalDue("strategy", strategyDue) &&
        optional("strategy", [this] { updateStrategy(); });
    const auto scheduledWork = protodd::frameWorkFor(
        urgentEventMask,
        protodd::frame_schedule::macroCadenceDue(state_.frame), strategyUpdated,
        protodd::frame_schedule::workersDue(state_.frame),
        state_.frame % combatCadence == 0);
    // Producer idleness is time-sensitive. Reconcile every three frames and
    // admit construction searches from the remaining callback allowance;
    // training and research retain their disjoint resource allocations.
    if (scheduledWork.updateMacro)
        measure("macro", [this, callbackStarted] {
            const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                CallbackBudget::Clock::now() - callbackStarted).count();
            updateMacro(std::max<std::int64_t>(
                0, CallbackBudget::optionalWorkLimitUs - elapsedUs));
        });
    // Model-controlled production runs after current macro obligations have
    // been reconciled and spends from the same ledger as native commands.
    if (production_.enabled()) {
        optional("production-control", [this] {
            production_.act(state_.frame, [this](const BWAPI::UnitCommand& command) {
                return bridge_.executeProduction(command);
            }, log_);
        });
    }
    if (optionalDue("scouting", protodd::frame_schedule::scoutingDue(state_.frame, cadence)))
        optional("scouting", [this] { updateScouting(); });
    if (scoutingDispatchPending_) {
        for (const auto& feedback : bridge_.submitScouts(pendingScoutOrders_, commands_))
            scouts_.recordCommandFeedback(feedback, state_.frame);
    }
    if (scheduledWork.updateWorkers)
        measure("workers", [this] { updateWorkers(); });
    if (scheduledWork.updateCombat) {
        measure("scout-micro", [this] { updateScoutMicro(); });
        auto runSimulation = !urgentWork.updateCombat &&
                             frameBudget_.allowSimulation(state_.frame);
        if (runSimulation) {
            auto& combatTiming = phases_["combat"];
            const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                CallbackBudget::Clock::now() - callbackStarted).count();
            runSimulation = callbackBudget.allowsOptionalWork(
                elapsedUs, combatTiming.estimatedUs(8'000));
            if (!runSimulation) combatTiming.deferForBudget();
        }
        measure("combat", [this, runSimulation] { updateCombat(runSimulation,
                     frameBudget_.navigationInterval(state_.frame)); });
        measure("observer-safety", [this] {
            for (const auto& command : scouts_.protectObservers(state_, influence_, detectorEscorts_)) {
                auto safetyCommand = command;
                safetyCommand.owner = CommandOwner::observerSafety;
                safetyCommand.urgency = safetyCommand.priority;
                safetyCommand.deadlineFrame = state_.frame;
                safetyCommand.alreadyActive = bridge_.commandActive(safetyCommand);
                commands_.submit(std::move(safetyCommand));
            }
        });
    }
    if (optionalDue("maintenance", protodd::frame_schedule::maintenanceDue(state_.frame)))
        optional("maintenance", [this] { bridge_.runMaintenance(plan_, commands_); });
    measure("command-dispatch", [this] { dispatchFrameCommands(); });
    if (scoutingDispatchPending_)
        measure("scout-command-dispatch", [this] { dispatchScoutingOrders(); });
    if (optionalDue("diagnostics", protodd::frame_schedule::diagnosticsDue(state_.frame)))
        optional("diagnostics", [this] { sampleTelemetry(); logDiagnostics(); });
    if (optionalDue("state-log", protodd::frame_schedule::stateLogDue(state_.frame)))
        optional("state-log", [this] { logDecision(); });

#ifdef PROTODD_DEVELOPER_PROFILE
    if (frameBudget_.load(state_.frame) == RuntimeLoad::normal &&
        optionalDue("overlay", true)) {
        optional("overlay", [this] {
            bridge_.drawDebug(state_, plan_, opponent_.assessment(), debug_);
        });
    }
#endif
#ifdef PROTODD_ENGINE_FAULT_INJECTION
    if (auditSlowObservationFrame_ == state_.frame && log_) {
        const auto combatRan = phases_["combat"].calls >
            auditSlowObservationCombatCallsBefore_;
        log_ << "AUDIT_SLOW_OBSERVATION," << state_.frame
             << ",combatRan=" << combatRan
             << ",influenceDeferred=" << deferredOptionalPhases_.contains("influence")
             << ",inferenceDeferred=" << deferredOptionalPhases_.contains("inference")
             << '\n';
    }
#endif
}

void ProtoddModule::onUnitDiscover(const BWAPI::Unit unit) {
    callbackBoundary("onUnitDiscover", [this, unit] {
        bridge_.remember(unit);
        bridge_.updateNavigationObstacle(navigation_, unit);
        logLifecycle(unit, "discover");
    });
}
void ProtoddModule::onUnitShow(const BWAPI::Unit unit) {
    callbackBoundary("onUnitShow", [this, unit] {
        bridge_.remember(unit);
        bridge_.updateNavigationObstacle(navigation_, unit);
        logLifecycle(unit, "show");
    });
}
void ProtoddModule::onUnitCreate(const BWAPI::Unit unit) {
    callbackBoundary("onUnitCreate", [this, unit] {
        if (unit != nullptr) {
            invalidateActorState(unit->getID());
            bridge_.forget(unit);
            bridge_.updateNavigationObstacle(navigation_, unit);
        }
        logLifecycle(unit, "create");
    });
}
void ProtoddModule::onUnitComplete(const BWAPI::Unit unit) {
    callbackBoundary("onUnitComplete", [this, unit] {
        bridge_.updateNavigationObstacle(navigation_, unit);
        logLifecycle(unit, "complete");
    });
}
void ProtoddModule::onUnitDestroy(const BWAPI::Unit unit) {
    callbackBoundary("onUnitDestroy", [this, unit] { onUnitDestroyImpl(unit); });
}

void ProtoddModule::onUnitDestroyImpl(const BWAPI::Unit unit) {
    bridge_.removeNavigationObstacle(navigation_, unit);
    if (unit != nullptr && unit->getPlayer() == BWAPI::Broodwar->self()) {
        const auto kind = unit->getType();
        if (kind.isWorker()) urgentEvents_.enqueue(UrgentEvent::builderLost);
        if (kind == BWAPI::UnitTypes::Protoss_Pylon)
            urgentEvents_.enqueue(UrgentEvent::powerSourceLost);
    }
    if (log_ && unit != nullptr &&
        (unit->getPlayer() == BWAPI::Broodwar->self() ||
         (unit->getPlayer() == BWAPI::Broodwar->enemy() && unit->isVisible()))) {
        const auto kind = BwapiBridge::toKind(unit->getType());
        log_ << "LOSS," << BWAPI::Broodwar->getFrameCount() << ','
             << (unit->getPlayer() == BWAPI::Broodwar->self() ? "self" : "enemy") << ','
             << unit->getID() << ',' << unitStats(kind).name << ','
             << unit->getPosition().x << ',' << unit->getPosition().y << ','
             << unit->getType().mineralPrice() << ',' << unit->getType().gasPrice()
             << ",costBatchSize=" << (unit->getType().isTwoUnitsInOneEgg() ? 2 : 1);
        const auto prior = damageSamples_.find(unit->getID());
        if (prior != damageSamples_.end()) {
            log_ << ",lastHp=" << prior->second.hitPoints << ",lastShields=" << prior->second.shields
                 << ",lastObserved=" << prior->second.lastSeen;
        }
        const auto action = lastActions_.find(unit->getID());
        if (action != lastActions_.end()) {
            log_ << ",lastAction=" << csvSafe(action->second.source)
                 << ",actionFrame=" << action->second.frame << ",actionTarget=" << action->second.target;
        }
        log_ << ",plan=" << csvSafe(plan_.name) << ",posture=" << postureName(plan_.posture)
             << ",nearbyVisibleEnemies=";
        // Context only: nearby enemies are not identified as the killer.
        for (const auto& enemy : state_.enemy.units) {
            if (enemy.visible && state_.frame - enemy.lastSeen <= 1 &&
                distanceSquared(enemy.position, {unit->getPosition().x, unit->getPosition().y}) <= 640 * 640)
                log_ << enemy.id << ':' << unitStats(enemy.kind).name << ';';
        }
        log_ << '\n';
        log_.flush();
    }
    if (unit != nullptr) invalidateActorState(unit->getID());
    bridge_.forget(unit);
}

void ProtoddModule::invalidateActorState(const UnitId id) {
    if (id < 0) return;
    commands_.forgetUnit(id);
    scouts_.forgetUnit(id);
    squads_.forgetUnit(id);
    transports_.forgetUnit(id);
    engagements_.forgetUnit(id);
    std::erase(detectorEscorts_, id);
    std::erase(leasedScouts_, id);
    std::erase(previousWorkerLineThreats_, id);
    std::erase(currentWorkerLineThreats_, id);
    std::erase(previousCloakedThreats_, id);
    std::erase(currentCloakedThreats_, id);
    std::erase_if(hybridProposals_, [id](const HybridProposal& proposal) {
        return proposal.command.actor == id || proposal.command.targetUnit == id;
    });
    debug_.orders.erase(id);
    lastActions_.erase(id);
    damageSamples_.erase(id);
    motionSamples_.erase(id);
    traceMemory_.erase("order/" + std::to_string(id));
    traceMemory_.erase("action/" + std::to_string(id));
    // Squad signatures include member IDs, so changed squads refresh their own
    // route slots during planning. Keep unrelated routes warm; obstacle
    // revisions invalidate only routes near the changed footprint.
}

void ProtoddModule::onUnitMorph(const BWAPI::Unit unit) {
    callbackBoundary("onUnitMorph", [this, unit] {
        bridge_.remember(unit);
        bridge_.updateNavigationObstacle(navigation_, unit);
        logLifecycle(unit, "morph");
    });
}
void ProtoddModule::onUnitRenegade(const BWAPI::Unit unit) {
    callbackBoundary("onUnitRenegade", [this, unit] {
        logLifecycle(unit, "ownership-change");
        if (unit != nullptr) invalidateActorState(unit->getID());
        bridge_.forget(unit);
        bridge_.remember(unit);
        bridge_.updateNavigationObstacle(navigation_, unit);
    });
}

void ProtoddModule::updateStrategy() {
    const auto action = policy_.decision();
    const auto style = !policy_.enabled() ? openingStyle_ :
        action == PolicyAction::pressure ? OpeningStyle::aggressive :
        action == PolicyAction::economy ? OpeningStyle::economic : OpeningStyle::standard;
    auto candidate = strategy_.plan(state_, opponent_.assessment(), style);
    const auto strategyPosture = candidate.posture;
    auto coveredPressureReleased = false;
    auto containBreak = false;
#ifdef PROTODD_PVT_CONTAIN_BREAK
    if (const auto target = StrategyEngine::pvTContainBreakTarget(state_, candidate);
        target.valid()) {
        candidate.attackTarget = target;
        containBreak = true;
        trace("pvtContainBreak", "EVENT,pvt-contain-break,target=" +
            std::to_string(target.x) + 'x' + std::to_string(target.y), 120);
    }
#endif
    allIn_.apply(candidate, state_, opponent_.assessment());
    SquadPlanner::requireDetectorCount(
        candidate, static_cast<std::size_t>(std::max(0, requiredDetectorCount_)));
    if (allIn_.active()) {
        trace("allin", "ALLIN,build=" + std::string(allInBuildName(allIn_.build())) +
            ",phase=" + std::string(allInPhaseName(allIn_.phase())) + ",launch=" +
            std::to_string(allIn_.launchFrame()) + ",reason=" +
            std::string(allIn_.transitionReason()), 240);
    }
    if (!allIn_.active() && policy_.enabled() && action == PolicyAction::defend &&
        candidate.posture != Posture::defend && candidate.posture != Posture::recover) {
#ifdef PROTODD_COVERED_PRESSURE_RELEASE
        if (!policy_.weightsLoaded())
            coveredPressureReleased = StrategyEngine::coveredPressureRelease(state_, candidate);
#endif
        if (!coveredPressureReleased && !(containBreak && !policy_.weightsLoaded()))
            candidate.posture = Posture::hold;
    }
    const auto proposed = candidate.posture;
    plan_ = strategicDirector_.stabilize(std::move(candidate), state_, opponent_.assessment());
    const auto containBreakCommitted = containBreak &&
        (plan_.posture == Posture::pressure || plan_.posture == Posture::attack);
    if (containBreakCommitted) plan_.breakContainment = true;
    auto naturalRallyOverride = false;
#ifdef PROTODD_THREATENED_NATURAL_RALLY
    if (const auto rally = SquadPlanner::threatenedNaturalRally(state_, plan_)) {
        plan_.rallyPoint = *rally;
        naturalRallyOverride = true;
    }
#endif
    const auto initialExpansionTarget = plan_.expansionTarget;
    expansion_.update(plan_, state_, bridge_.expansionFeedback(plan_.expansionTarget));
    auto releaseExpansionBuilder = expansion_.releaseBuilder();
    // A newly selected alternative can have its own active blocker. Process
    // the chain in this strategic update so failed sites do not become a
    // two-site rally loop across successive planning ticks.
    for (auto attempt = std::size_t{};
         attempt < state_.bases.size() && plan_.expansionTarget.valid() &&
         distanceSquared(plan_.expansionTarget, initialExpansionTarget) > 0;
         ++attempt) {
        const auto currentTarget = plan_.expansionTarget;
        const auto feedback = bridge_.expansionFeedback(currentTarget);
        if (!feedback.rejectedFootprint && !feedback.noSafeBuilder &&
            !feedback.noPlacement && !feedback.unsafeRoute) break;
        expansion_.update(plan_, state_, feedback);
        releaseExpansionBuilder = releaseExpansionBuilder ||
                                  expansion_.releaseBuilder();
        if (!plan_.expansionTarget.valid() ||
            distanceSquared(plan_.expansionTarget, currentTarget) <= 0) break;
    }
    if (releaseExpansionBuilder && bridge_.cancelExpansion() &&
        plan_.expansionTarget.valid() &&
        plan_.name.find("[survival:") == std::string::npos)
        plan_.deferExpansion = false;
    if (plan_.expansionTarget.valid()) {
        auto siteId = (static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(plan_.expansionTarget.x)) << 32U) |
            static_cast<std::uint32_t>(plan_.expansionTarget.y);
        if (siteId == 0) siteId = 1;
        for (auto& goal : plan_.goals) {
            if (goal.goal == GoalKind::expand && goal.target == UnitKind::nexus)
                goal.constructionSite = {siteId, -1, plan_.expansionTarget};
        }
    }
    if (workerTrainingEnabled_) {
        const auto decision = applyWorkerTrainingIntervention(plan_, state_, workerTrainingProfile_);
        if (decision.applied) {
            log_ << "WORKER_TRAINING," << state_.frame << ",committed=" << decision.committed
                 << ",before=" << decision.beforeGoal << ",goal=" << decision.afterGoal
                 << ",beforePriority=" << decision.beforePriority
                 << ",priority=" << decision.afterPriority << '\n';
        }
    }
    debug_.operation = expansion_.reason();
    trace("strategy", "STRATEGY," + csvSafe(plan_.name) + ',' +
        std::string(postureName(proposed)) + ',' + std::string(postureName(plan_.posture)) + ',' +
        std::string(enemyPlanName(opponent_.assessment().mostLikely)) + ',' + debug_.operation + ',' +
        std::to_string(plan_.deferExpansion) + ",strategyPosture=" +
        std::string(postureName(strategyPosture)) + ",policyEnabled=" +
        std::to_string(policy_.enabled()) + ",policyAction=" +
        std::to_string(static_cast<int>(action)) + ",policyWeights=" +
        std::to_string(policy_.weightsLoaded()) + ",naturalRallyOverride=" +
        std::to_string(naturalRallyOverride) + ",coveredPressureRelease=" +
        std::to_string(coveredPressureReleased) + ",containBreak=" +
        std::to_string(containBreak) + ",containBreakCommit=" +
        std::to_string(containBreakCommitted));
}

void ProtoddModule::updateMacro(const std::int64_t planningBudgetUs) {
    const auto profileStarted = std::chrono::steady_clock::now();
    const auto elapsedUs = [](const auto started) {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
    };
    ResourceLedger ledger{state_.self.minerals, state_.self.gas};
    const auto buildBlockers = bridge_.buildBlockerFeedback();
    const auto reconcileStarted = std::chrono::steady_clock::now();
    auto actions = macro_.reconcile(state_, plan_, ledger, buildBlockers);
    const auto reconcileUs = elapsedUs(reconcileStarted);
    for (auto& action : actions) {
        if (!action.reserved || !bridge_.pendingBuildAlreadyPaid(action)) continue;
        if (ledger.releaseCommitted(action.minerals, action.gas)) action.reserved = false;
    }
    spendingLedger_ = ledger;
    bridge_.setSpendingLedger(&spendingLedger_);
    lastMacroFrame_ = state_.frame;
    debug_.macro = actions;
    bridge_.executeMacro(actions, plan_, influence_, leasedScouts_, 8, &navigation_, planningBudgetUs);
    const auto executeUs = elapsedUs(reconcileStarted) - reconcileUs;
    const auto diagnosticStarted = std::chrono::steady_clock::now();
    for (const auto& execution : bridge_.macroExecutions()) {
        const auto& action = execution.action;
        if (log_ && execution.elapsedUs >= 8'000) {
            log_ << "MACRO_ACTION_PROFILE," << state_.frame << ','
                 << unitStats(action.target).name << ',' << execution.elapsedUs << ','
                 << execution.navigationSearches << ',' << csvSafe(execution.outcome) << ','
                 << csvSafe(action.reason) << '\n';
        }
        if (action.reserved && action.executable &&
            execution.outcome != "command-budget-deferred" &&
            execution.outcome != "planning-budget-deferred") {
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
        if (action.timing.active()) {
            auto reason = std::string_view{"unknown"};
            switch (action.timing.reservationReason) {
                case CriticalReservationReason::supply: reason = "supply"; break;
                case CriticalReservationReason::detection: reason = "detection"; break;
                case CriticalReservationReason::range: reason = "range"; break;
                case CriticalReservationReason::none: reason = "none"; break;
            }
            std::ostringstream timing;
            timing << "CRITICAL_GOAL," << reason << ','
                   << action.timing.requiredByFrame << ','
                   << action.timing.expectedReadyFrame << ','
                   << action.timing.slackFrames << ',' << action.timing.feasible << ','
                   << static_cast<int>(action.action) << ','
                   << static_cast<int>(action.target) << ',' << csvSafe(action.reason);
            trace("critical-goal/" + key, timing.str());
        }
    }
    const auto diagnosticUs = elapsedUs(diagnosticStarted);
    const auto profileUs = elapsedUs(profileStarted);
    const auto& raidRouting = squads_.harassmentRoutingStats();
    if (raidRouting.deferredChecks > 0) {
        trace("harassment-route-budget", "HARASSMENT_ROUTE_BUDGET,pathSearches=" +
            std::to_string(raidRouting.pathSearches) + ",deferredChecks=" +
            std::to_string(raidRouting.deferredChecks), 24);
    }
    if (log_ && profileUs >= 8'000) {
        log_ << "MACRO_PROFILE," << state_.frame << ',' << profileUs << ','
             << reconcileUs << ',' << executeUs << ',' << diagnosticUs << ','
             << actions.size() << '\n';
    }
}

void ProtoddModule::updateWorkers() {
    const auto builders = bridge_.reservedBuilders();
    auto reserved = builders;
    reserved.insert(reserved.end(), leasedScouts_.begin(), leasedScouts_.end());
    std::ranges::sort(reserved);
    reserved.erase(std::unique(reserved.begin(), reserved.end()), reserved.end());
#ifdef PROTODD_ABANDONED_BASE_WORKER_EVACUATION
    constexpr auto evacuateAbandonedBase = true;
#else
    constexpr auto evacuateAbandonedBase = false;
#endif
#ifdef PROTODD_SAFE_REMOTE_MINING
    constexpr auto stageExpansionWorkers = true;
#else
    constexpr auto stageExpansionWorkers = false;
#endif
    const auto assignments = workers_.assign(
        state_, plan_, influence_, reserved, evacuateAbandonedBase, stageExpansionWorkers,
        &navigation_);
    const auto& workerRouting = workers_.routingStats();
    if (workerRouting.deferredChecks > 0) {
        trace("worker-route-budget", "WORKER_ROUTE_BUDGET,pathSearches=" +
            std::to_string(workerRouting.economicPathSearches) + ",deferredChecks=" +
            std::to_string(workerRouting.deferredChecks), 24);
    }
    for (const auto& assignment : assignments) {
        if (assignment.job != WorkerJob::evacuate && assignment.job != WorkerJob::defend)
            continue;
        releaseScoutLeaseForPreemption(assignment.worker, "worker",
            assignment.job == WorkerJob::evacuate ? "worker-evacuate" : "worker-defend");
    }
    if (std::ranges::any_of(assignments, [](const WorkerAssignment& assignment) {
            return assignment.job == WorkerJob::transfer && assignment.priority == 60;
        })) {
        trace("stagedExpansionWorkers", "EVENT,staged-expansion-workers", 120);
    }
    const auto gas = std::ranges::count(assignments, WorkerJob::gas, &WorkerAssignment::job);
    const auto minerals = std::ranges::count(assignments, WorkerJob::minerals, &WorkerAssignment::job);
    const auto ids = [](const std::span<const UnitId> units) {
        std::string result;
        for (const auto unit : units) {
            if (!result.empty()) result += ';';
            result += std::to_string(unit);
        }
        return result;
    };
    trace("workers", "WORKERS,gas=" + std::to_string(gas) + ",minerals=" + std::to_string(minerals) +
        ",builders=" + std::to_string(builders.size()) + ",builderIds=" + ids(builders) +
        ",scouts=" + std::to_string(leasedScouts_.size()) + ",scoutIds=" + ids(leasedScouts_) +
        ",leased=" + std::to_string(reserved.size()) + ",gasRequested=" + std::to_string(plan_.desiredGasWorkers));
    static_cast<void>(bridge_.submitWorkerCommands(assignments, commands_));
}

void ProtoddModule::releaseScoutLeaseForPreemption(
    const UnitId actor, const std::string_view newOwner, const std::string_view reason) {
    if (actor < 0) return;
    static_cast<void>(commands_.discardPending(actor, CommandOwner::scouting));
    const auto tracked = std::ranges::find(leasedScouts_, actor) != leasedScouts_.end() ||
        std::ranges::any_of(pendingScoutOrders_, [actor](const ScoutOrder& order) {
            return order.scout == actor;
        }) || scouts_.openingScout() == actor || scouts_.returningScout() == actor;
    if (!tracked) return;
    const auto generation = scouts_.releaseLease(actor, state_.frame);
    std::erase(leasedScouts_, actor);
    std::erase_if(pendingScoutOrders_, [actor](const ScoutOrder& order) {
        return order.scout == actor;
    });
    trace("actor-preemption", "AUTHORITY_PREEMPT,actor=" + std::to_string(actor) +
        ",displaced=scouting,newOwner=" + std::string(newOwner) +
        ",generation=" + std::to_string(generation) + ",reason=" +
        csvSafe(reason), 120);
}

void ProtoddModule::updateScouting() {
    pendingScoutOrders_.clear();
    scoutingDispatchPending_ = true;
    const auto previousLeases = leasedScouts_;
    const auto reservedBuilders = bridge_.reservedBuilders();
    leasedScouts_.clear();
    std::vector<UnitId> available;
    for (const auto& unit : state_.self.units) {
        if (unit.kind == UnitKind::observer && unit.completed) {
            if (std::ranges::find(detectorEscorts_, unit.id) != detectorEscorts_.end()) continue;
            if (ScoutManager::observerInDanger(state_, unit, influence_)) continue;
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
        if (probe >= 0 && probe != scouts_.returningScout()) available.push_back(probe);
    }
    const auto orders = scouts_.assign(state_, available, influence_,
                                       opponent_.assessment(), &navigation_);
    const auto& informationGap = scouts_.informationGap();
    if (informationGap.unresolved) {
        const auto reason = std::string(scoutGapReasonName(informationGap.reason));
        const auto purpose = std::string(scoutPurposeName(informationGap.purpose));
        const auto riskBucket = static_cast<int>(informationGap.risk * 10.0);
        const auto comparison = purpose + '/' +
            std::to_string(informationGap.target.x / 128) + '/' +
            std::to_string(informationGap.target.y / 128) + '/' +
            std::to_string(informationGap.deadlineFrame) + '/' +
            std::to_string(riskBucket) + '/' + reason;
        trace("scout-gap", "SCOUT_GAP,purpose=" + purpose +
            ",x=" + std::to_string(informationGap.target.x) +
            ",y=" + std::to_string(informationGap.target.y) +
            ",deadline=" + std::to_string(informationGap.deadlineFrame) +
            ",risk=" + std::to_string(informationGap.risk) +
            ",reason=" + reason, 120, comparison);
    }
    for (const auto& order : orders) leasedScouts_.push_back(order.scout);
    // Keep the opening Probe leased while it evades or returns, even if the
    // strategic scout selector finds no safe new destination this pass.
    const auto openingScout = scouts_.openingScout();
    if (openingScout >= 0 && std::ranges::find(leasedScouts_, openingScout) == leasedScouts_.end())
        leasedScouts_.push_back(openingScout);
    const auto returningScout = scouts_.returningScout();
    if (returningScout >= 0 && std::ranges::find(leasedScouts_, returningScout) == leasedScouts_.end())
        leasedScouts_.push_back(returningScout);
    std::vector<ScoutOrder> ordinary;
    for (const auto& order : orders) if (order.scout != openingScout) ordinary.push_back(order);
    pendingScoutOrders_ = std::move(ordinary);
}

void ProtoddModule::dispatchScoutingOrders() {
    pendingScoutOrders_.clear();
    scoutingDispatchPending_ = false;
    const auto& metrics = scouts_.missionMetrics();
    trace("scout-metrics", "SCOUT_METRICS,newInformation=" +
        std::to_string(metrics.newInformation) + ",completed=" +
        std::to_string(metrics.completed) + ",cancelled=" +
        std::to_string(metrics.cancelled) + ",lost=" +
        std::to_string(metrics.lost) + ",rejected=" +
        std::to_string(metrics.rejected) + ",workerScoutFrames=" +
        std::to_string(metrics.workerScoutFrames) + ",detectorScoutFrames=" +
        std::to_string(metrics.detectorScoutFrames) + ",routeRiskFrames=" +
        std::to_string(metrics.routeRiskFrames), 120);
}

void ProtoddModule::updateScoutMicro() {
    if (const auto command = scouts_.controlWorkerScout(state_, influence_, &navigation_)) {
        auto scoutingCommand = *command;
        scoutingCommand.owner = CommandOwner::scouting;
        scoutingCommand.urgency = scoutingCommand.priority;
        scoutingCommand.deadlineFrame = state_.frame;
        scoutingCommand.alreadyActive = bridge_.commandActive(scoutingCommand);
        commands_.submit(std::move(scoutingCommand));
    }
    if (scouts_.openingScout() < 0) debug_.scout.clear();
}

void ProtoddModule::updateCombat(
    const bool runSimulation,
    const int navigationInterval) {
    const auto profileStarted = std::chrono::steady_clock::now();
    const auto elapsedUs = [](const auto started) {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
    };
    auto profileStageStarted = std::chrono::steady_clock::now();
    influence_.updateStorms(state_.storms);
    const auto transportOrders = transports_.control(
        state_, plan_.attackTarget, retreatPoint(), influence_,
        plan_.prioritizeReinforcements ? 100 : (state_.enemy.race == Race::protoss ? 2 : 1), true, &navigation_);
    const auto controlPrepUs = elapsedUs(profileStageStarted);
    profileStageStarted = std::chrono::steady_clock::now();
    auto friendly = combatUnits(true);
    std::erase_if(friendly, [this](const UnitSnapshot& unit) { return transports_.ownsReaver(unit.id); });
    debug_.squads.clear();
    const auto enemy = combatUnits(false);
    const auto unitSnapshotUs = elapsedUs(profileStageStarted);
    profileStageStarted = std::chrono::steady_clock::now();
    auto stormReservations = bridge_.reservedStormZones(state_.frame);
    for (const auto center : state_.storms) {
        if (std::ranges::none_of(stormReservations, [center](const Position reserved) {
                return distanceSquared(center, reserved) <=
                    psionicStormReservationDistance * psionicStormReservationDistance;
            })) stormReservations.push_back(center);
    }
    const auto aggressive = plan_.posture == Posture::pressure ||
                            plan_.posture == Posture::attack ||
                            plan_.posture == Posture::harass;
#ifdef PROTODD_BASE_DEFENSE_CONSOLIDATION
    constexpr auto emergencyConsolidation = true;
#else
    constexpr auto emergencyConsolidation = false;
#endif
    const auto formed = squads_.form(state_, friendly, enemy, plan_, retreatPoint(),
                                     &navigation_, emergencyConsolidation);
    const auto formationUs = elapsedUs(profileStageStarted);
    if (std::ranges::any_of(formed, [](const Squad& squad) {
            return squad.emergencyDefense;
        })) {
        trace("emergencyDefenseConsolidation", "EVENT,emergency-defense-consolidation", 120);
    }
    const auto resetNavigation = advanceWaypoints_.size() != formed.size() ||
                                 navigationSignatures_.size() != formed.size() ||
                                 advanceRoutes_.size() != formed.size();
    const auto periodicNavigationRefresh = navigationRefresh_ < 0 ||
                                           state_.frame - navigationRefresh_ >=
                                               navigationInterval;
    if (resetNavigation) {
        advanceWaypoints_.assign(formed.size(), {-1, -1});
        navigationSignatures_.assign(formed.size(), 0);
        advanceRoutes_.assign(formed.size(), {});
    }
    if (periodicNavigationRefresh) {
        navigationRefresh_ = state_.frame;
    }
    const auto submit = [this](Command command) {
        if (command.owner == CommandOwner::unspecified)
            command.owner = CommandOwner::combat;
        if (command.urgency == std::numeric_limits<int>::min())
            command.urgency = command.priority;
        command.alreadyActive = bridge_.commandActive(command);
        commands_.submit(std::move(command));
    };
    fight_ = {};
    auto debugSquadSize = std::size_t{0};
    const auto logSquads = lastSquadLogFrame_ < 0 || state_.frame - lastSquadLogFrame_ >= 24;
    const auto* vanguard = SquadPlanner::selectVanguard(formed, plan_.attackTarget);
    auto maxSquadUs = std::int64_t{0};
    auto maxCombatEstimateUs = std::int64_t{0};
#ifdef PROTODD_FORWARD_THIRD_SCREEN
    const auto coverForwardThird = state_.enemy.race == Race::terran;
#else
    const auto coverForwardThird = false;
#endif
    const auto requestedExpansionCover = SquadPlanner::shouldCoverExpansion(
        state_, plan_, coverForwardThird);
#ifdef PROTODD_BASE_THREAT_EXPANSION_GUARD
    const auto baseThreatBlocksCover = requestedExpansionCover &&
        SquadPlanner::survivingBaseUnderThreat(state_);
    if (baseThreatBlocksCover)
        trace("baseThreatExpansionGuard", "EVENT,base-threat-expansion-guard", 120);
    const auto coverExpansion = requestedExpansionCover && !baseThreatBlocksCover;
#else
    const auto coverExpansion = requestedExpansionCover;
#endif
#ifdef PROTODD_FORWARD_THIRD_SCREEN
    if (coverExpansion && coverForwardThird &&
        std::ranges::count_if(state_.self.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::nexus && unit.completed;
        }) == 2) {
        trace("forwardThirdScreen", "EVENT,forward-third-screen,site=" +
            std::to_string(plan_.expansionTarget.x) + 'x' +
            std::to_string(plan_.expansionTarget.y), 120);
    }
#endif
    const auto squadLoopStarted = std::chrono::steady_clock::now();
    for (std::size_t squadIndex = 0; squadIndex < formed.size(); ++squadIndex) {
        const auto squadStarted = std::chrono::steady_clock::now();
        const auto& squad = formed[squadIndex];
        auto requiredRatio = squad.requiredRatio;
        auto objective = squad.objective;
        auto defense = squad.defense;
        auto travelReason = "squad-mission";
        if (squad.role == SquadRole::mainArmy) {
            travelReason = "attack-target";
            if (coverExpansion)
                objective = expansionAssemblyPoint(state_, plan_.expansionTarget, retreatPoint());
            const auto travelMode = SquadPlanner::mainArmyTravelMode(
                squad, vanguard, aggressive, plan_.minimumAttackSize);
            if (travelMode == MainArmyTravelMode::assemble) {
                travelReason = "assemble-at-rally";
                // A small squad may move toward its rally point, but it must
                // not accept an equal-size fight on the way there. The old
                // 0.88 ratio made a five-Zealot vanguard engage four-to-six
                // enemy Zealots before the next reinforcement arrived.
                const auto undersizedForce = vanguard == nullptr ||
                    vanguard->units.size() <
                        static_cast<std::size_t>(std::max(1, plan_.minimumAttackSize));
                requiredRatio = squad.enemies.empty()
                                    ? 0.88
                                    : (undersizedForce ? 1.18 : 1.05);
                objective = coverExpansion
                    ? expansionAssemblyPoint(state_, plan_.expansionTarget, retreatPoint()) : plan_.rallyPoint;
                defense = SquadPlanner::defensiveArea(state_, plan_.rallyPoint);
            } else if (travelMode == MainArmyTravelMode::joinVanguard) {
                // Detached reinforcements join the strongest mobile component
                // even when they encounter an enemy on the way. That local
                // fight still uses its combat estimate, but the strategic
                // travel objective cannot become a solo assault.
                requiredRatio = squad.enemies.empty()
                    ? 0.88 : std::max(requiredRatio, 1.18);
                objective = SquadPlanner::reinforcementDestination(squad, *vanguard, plan_.attackTarget);
                travelReason = objective == plan_.attackTarget ? "continue-assault" : "join-vanguard";
            }
            if (coverExpansion && vanguard == &squad) {
                const auto assembly = expansionAssemblyPoint(
                    state_, plan_.expansionTarget, retreatPoint());
                objective = assembly;
                travelReason = "cover-expansion";
                // Uncontested travel adopts the new screen immediately so a
                // home leash cannot trap the army. An ongoing fight elsewhere
                // retains its current combat/retreat policy until it resolves.
                defense = SquadPlanner::expansionDefense(
                    squad, assembly, plan_.expansionTarget, defense);
            }
        }
        auto travelGoal = objective;
        const auto hasGroundUnit = std::ranges::any_of(
            squad.units, [](const UnitSnapshot& unit) { return !unit.flying; });
        MovementFootprint squadFootprint;
        for (const auto& member : squad.units) {
            if (member.flying) continue;
            squadFootprint.left = std::max(squadFootprint.left, member.dimensionLeft);
            squadFootprint.right = std::max(squadFootprint.right, member.dimensionRight);
            squadFootprint.up = std::max(squadFootprint.up, member.dimensionUp);
            squadFootprint.down = std::max(squadFootprint.down, member.dimensionDown);
        }
        auto routeSignature = squad.signature;
        routeSignature ^= static_cast<std::uint32_t>(objective.x);
        routeSignature *= 1099511628211ULL;
        routeSignature ^= static_cast<std::uint32_t>(objective.y);
        routeSignature *= 1099511628211ULL;
        for (const auto clearance : {squadFootprint.left, squadFootprint.right,
                                     squadFootprint.up, squadFootprint.down}) {
            routeSignature ^= static_cast<std::uint32_t>(clearance);
            routeSignature *= 1099511628211ULL;
        }
        auto& cachedRoute = advanceRoutes_[squadIndex];
        auto navigationRouteBlocked = false;
        const auto affectedRoute = navigation_.routeAffectedSince(
            cachedRoute.points, cachedRoute.from, cachedRoute.to,
            cachedRoute.footprint, cachedRoute.obstacleVersion);
        const auto refreshRoute = periodicNavigationRefresh || resetNavigation ||
                                  navigationSignatures_[squadIndex] != routeSignature ||
                                  affectedRoute ||
                                  (!squad.enemies.empty() &&
                                   !cachedRoute.computed);
        if (refreshRoute) {
            navigationSignatures_[squadIndex] = routeSignature;
            advanceWaypoints_[squadIndex] = {-1, -1};
        } else {
            cachedRoute.obstacleVersion = navigation_.obstacleVersion();
        }
        if (refreshRoute && hasGroundUnit) {
            // During uncontested travel, let BWAPI route each unit to the
            // actual destination. A short waypoint from a large squad's
            // centroid can lie behind its front units or on the wrong side
            // of terrain, continually pulling the force back into itself.
            if (!squad.enemies.empty()) {
                const auto route = navigation_.nextWaypoint(
                    squad.center, objective, 7, 12000, squadFootprint,
                    &cachedRoute.points);
                cachedRoute.status = route.status;
                if (route.hasUsableWaypoint())
                    advanceWaypoints_[squadIndex] = route.waypoint;
                cachedRoute.computed = true;
            }
            cachedRoute.from = squad.center;
            cachedRoute.to = objective;
            cachedRoute.footprint = squadFootprint;
            cachedRoute.obstacleVersion = navigation_.obstacleVersion();
        } else if (refreshRoute) {
            cachedRoute.points.clear();
            cachedRoute.status = NavigationStatus::invalidInput;
            cachedRoute.computed = false;
            cachedRoute.from = squad.center;
            cachedRoute.to = objective;
            cachedRoute.footprint = squadFootprint;
            cachedRoute.obstacleVersion = navigation_.obstacleVersion();
        }
        if (hasGroundUnit) {
            if (!squad.enemies.empty() && advanceWaypoints_[squadIndex].valid()) {
                objective = advanceWaypoints_[squadIndex];
            } else if (!squad.enemies.empty() &&
                       !navigation_.lineWalkable(squad.center, objective, squadFootprint)) {
                navigationRouteBlocked = true;
            }
        }
        const auto engagementKey = engagements_.identify(squad.units, state_.frame);
#ifdef PROTODD_STALLED_ARMY_ROUTING
        // A distant attack-move can remain accepted while an entire army is
        // motionless behind terrain. Only invoke A* after several members
        // have actually stalled; preserve its waypoint until the group gets
        // there so the next direct order cannot pull it back into the wall.
        if (squad.role == SquadRole::mainArmy && squad.enemies.empty() &&
            hasGroundUnit && travelGoal == plan_.attackTarget &&
            travelGoal.valid() && squad.center.valid()) {
            auto route = stalledAdvances_.find(engagementKey);
            if (route != stalledAdvances_.end() &&
                (state_.frame - route->second.lastSeen > 10 * 24 ||
                 distanceSquared(route->second.destination, travelGoal) > 128 * 128)) {
                stalledAdvances_.erase(route);
                route = stalledAdvances_.end();
            }
            const auto groundCount = std::ranges::count_if(squad.units,
                [](const UnitSnapshot& unit) { return !unit.flying; });
            const auto stalledCount = std::ranges::count_if(squad.units,
                [this](const UnitSnapshot& unit) {
                    if (unit.flying) return false;
                    const auto sample = motionSamples_.find(unit.id);
                    return sample != motionSamples_.end() &&
                        state_.frame - sample->second.since >= 240 &&
                        distanceSquared(sample->second.anchor, unit.position) <= 24 * 24;
                });
            // Preserve the noise floor for full armies, but let a one- or
            // two-unit cleanup force recover when every ground unit is stuck.
            const auto stalledThreshold = std::min(
                groundCount, std::max<std::ptrdiff_t>(3, groundCount / 4));
            if (route == stalledAdvances_.end() &&
                stalledCount >= stalledThreshold) {
                route = stalledAdvances_.emplace(engagementKey,
                    StalledAdvance{travelGoal, {-1, -1}, -1, state_.frame, {}}).first;
            }
            if (route != stalledAdvances_.end()) {
                route->second.lastSeen = state_.frame;
                if (navigation_.lineWalkable(squad.center, travelGoal, squadFootprint)) {
                    stalledAdvances_.erase(route);
                } else {
                    const auto stalledRouteAffected = navigation_.routeAffectedSince(
                        route->second.route.points, route->second.route.from,
                        route->second.route.to, route->second.route.footprint,
                        route->second.route.obstacleVersion);
                    if (stalledRouteAffected ||
                        (!route->second.waypoint.valid() &&
                         route->second.route.status != NavigationStatus::unreachable) ||
                        distanceSquared(squad.center, route->second.waypoint) <= 96 * 96 ||
                        state_.frame - route->second.waypointSince >= 720) {
                        const auto waypoint = navigation_.nextWaypoint(
                            squad.center, travelGoal, 12, 30000, squadFootprint,
                            &route->second.route.points);
                        route->second.route.status = waypoint.status;
                        route->second.route.computed = true;
                        route->second.waypoint = waypoint.hasUsableWaypoint()
                            ? waypoint.waypoint : Position{-1, -1};
                        route->second.waypointSince = state_.frame;
                        route->second.route.from = squad.center;
                        route->second.route.to = travelGoal;
                        route->second.route.footprint = squadFootprint;
                        route->second.route.obstacleVersion = navigation_.obstacleVersion();
                    } else {
                        route->second.route.obstacleVersion = navigation_.obstacleVersion();
                    }
                    if (route->second.waypoint.valid()) {
                        objective = route->second.waypoint;
                        travelReason = "stalled-terrain-route";
                    } else if (route->second.route.status == NavigationStatus::unreachable) {
                        // A proven-disconnected remembered main must not keep
                        // the field army issuing the same futile attack-move.
                        // Sweep other legally known structures and non-owned,
                        // not-recently-cleared base sites that this force can
                        // actually reach. The normal scout loop can refresh
                        // those sites while the army searches them.
                        if (route->second.searchTarget.valid() &&
                            distanceSquared(squad.center, route->second.searchTarget) <=
                                128 * 128) {
                            route->second.searchTarget = {-1, -1};
                            route->second.searchWaypoint = {-1, -1};
                            route->second.searchRoute = {};
                        }
                        if (!route->second.searchTarget.valid()) {
                            std::vector<Position> candidates;
                            candidates.reserve(state_.enemy.units.size() + state_.bases.size());
                            const auto addCandidate = [&](const Position candidate) {
                                if (!candidate.valid() ||
                                    distanceSquared(candidate, travelGoal) <= 128 * 128 ||
                                    distanceSquared(candidate, squad.center) <= 128 * 128 ||
                                    std::ranges::find(candidates, candidate) != candidates.end())
                                    return;
                                candidates.push_back(candidate);
                            };
                            for (const auto& rememberedEnemy : state_.enemy.units) {
                                if (isBuilding(rememberedEnemy.kind) &&
                                    rememberedEnemy.position.valid())
                                    addCandidate(rememberedEnemy.position);
                            }
                            for (const auto& base : state_.bases) {
                                if (base.ownerId == state_.self.id) continue;
                                const auto recentlyCleared = base.ownerId != state_.enemy.id &&
                                    base.lastConfirmedEmpty >= 0 &&
                                    base.lastConfirmedEmpty >= base.lastScouted &&
                                    base.lastConfirmedEmpty <= state_.frame;
                                if (!recentlyCleared) addCandidate(base.center);
                            }
                            std::ranges::sort(candidates, [&squad](const Position left,
                                                                  const Position right) {
                                const auto leftDistance = distanceSquared(squad.center, left);
                                const auto rightDistance = distanceSquared(squad.center, right);
                                return leftDistance != rightDistance
                                    ? leftDistance < rightDistance
                                    : left.x != right.x ? left.x < right.x
                                                       : left.y < right.y;
                            });
                            if (candidates.size() > 24U) candidates.resize(24U);
                            const auto directTarget = std::ranges::find_if(
                                candidates, [this, &squad, &squadFootprint](const Position target) {
                                    return navigation_.lineWalkable(
                                        squad.center, target, squadFootprint);
                                });
                            route->second.searchTarget = directTarget != candidates.end()
                                ? *directTarget
                                : navigation_.nearestReachableTarget(
                                    squad.center, candidates, 12000, squadFootprint);
                            route->second.searchWaypoint = {-1, -1};
                            route->second.searchWaypointSince = -1;
                            route->second.searchRoute = {};
                        }
                        if (route->second.searchTarget.valid()) {
                            const auto affectedSearchRoute = navigation_.routeAffectedSince(
                                route->second.searchRoute.points,
                                route->second.searchRoute.from,
                                route->second.searchRoute.to,
                                route->second.searchRoute.footprint,
                                route->second.searchRoute.obstacleVersion);
                            if (affectedSearchRoute ||
                                (!route->second.searchWaypoint.valid() &&
                                 route->second.searchRoute.status != NavigationStatus::unreachable) ||
                                (route->second.searchWaypoint.valid() &&
                                 distanceSquared(squad.center,
                                     route->second.searchWaypoint) <= 96 * 96) ||
                                state_.frame - route->second.searchWaypointSince >= 720) {
                                const auto search = navigation_.nextWaypoint(
                                    squad.center, route->second.searchTarget, 12, 30000,
                                    squadFootprint, &route->second.searchRoute.points);
                                route->second.searchRoute.status = search.status;
                                route->second.searchRoute.computed = true;
                                route->second.searchWaypoint = search.hasUsableWaypoint()
                                    ? search.waypoint : Position{-1, -1};
                                route->second.searchWaypointSince = state_.frame;
                                route->second.searchRoute.from = squad.center;
                                route->second.searchRoute.to = route->second.searchTarget;
                                route->second.searchRoute.footprint = squadFootprint;
                                route->second.searchRoute.obstacleVersion =
                                    navigation_.obstacleVersion();
                            } else {
                                route->second.searchRoute.obstacleVersion =
                                    navigation_.obstacleVersion();
                            }
                            if (route->second.searchWaypoint.valid()) {
                                objective = route->second.searchWaypoint;
                                travelGoal = route->second.searchTarget;
                                travelReason = "search-after-unreachable-objective";
                            } else {
                                navigationRouteBlocked = true;
                            }
                        } else {
                            navigationRouteBlocked = true;
                            travelReason = "no-reachable-cleanup-objective";
                        }
                    } else {
                        navigationRouteBlocked = true;
                    }
                }
            }
        }
        if (stalledAdvances_.size() > 128U) {
            std::erase_if(stalledAdvances_, [this](const auto& entry) {
                return state_.frame - entry.second.lastSeen > 10 * 24;
            });
        }
#endif
        const auto supportedArmy = SquadPlanner::combatSupport(squad, friendly, &navigation_);
        const auto simulateSquad = runSimulation &&
            protodd::engagementSimulationWithinBudget(
                supportedArmy.size(), squad.enemies.size());
        const auto valuation = valueFightMission(squad.fightMission, requiredRatio);
        requiredRatio = valuation.requiredRatio;
        const auto combatEstimateStarted = std::chrono::steady_clock::now();
        auto estimate = combat_.evaluate(
            supportedArmy, squad.enemies, requiredRatio,
            squad.enemies.empty() ? opponent_.assessment().uncertainty * 0.25
                                  : opponent_.assessment().uncertainty,
            simulateSquad, &navigation_);
        maxCombatEstimateUs = std::max(maxCombatEstimateUs, elapsedUs(combatEstimateStarted));
        const auto proposedDecision = estimate.decision;
        const auto detectionReady = SquadPlanner::mobileDetectionReady(state_, squad);
        const auto retreatDestination = retreatPoint();
        auto retreatRouteFailed = false;
        if (proposedDecision == FightDecision::retreat && hasGroundUnit &&
            squad.center.valid() && retreatDestination.valid() && !navigation_.empty()) {
            const Position startTile{squad.center.x / 32, squad.center.y / 32};
            const Position destinationTile{retreatDestination.x / 32,
                                           retreatDestination.y / 32};
            auto route = retreatRouteChecks_.find(engagementKey);
            const auto affectedRetreatRoute = route != retreatRouteChecks_.end() &&
                navigation_.routeAffectedSince(
                    route->second.route.points, route->second.route.from,
                    route->second.route.to, route->second.route.footprint,
                    route->second.route.obstacleVersion);
            if (route == retreatRouteChecks_.end() ||
                route->second.startTile != startTile ||
                route->second.destinationTile != destinationTile || affectedRetreatRoute) {
                std::vector<Position> routePoints;
                const auto waypoint = navigation_.nextWaypoint(
                    squad.center, retreatDestination, 7, 4000,
                    squadFootprint, &routePoints);
                const auto failed = !waypoint.reached();
                CachedNavigationRoute cached{
                    std::move(routePoints), squad.center, retreatDestination,
                    squadFootprint, navigation_.obstacleVersion(), waypoint.status, true};
                route = retreatRouteChecks_.insert_or_assign(engagementKey,
                    RetreatRouteCheck{startTile, destinationTile, state_.frame,
                                      failed, std::move(cached)}).first;
            } else {
                route->second.route.obstacleVersion = navigation_.obstacleVersion();
            }
            route->second.lastSeen = state_.frame;
            retreatRouteFailed = route->second.failed;
            navigationRouteBlocked = navigationRouteBlocked || retreatRouteFailed;
        }
        if (retreatRouteChecks_.size() > 128U) {
            std::erase_if(retreatRouteChecks_, [this](const auto& entry) {
                return state_.frame - entry.second.lastSeen > 10 * 24;
            });
        }
        estimate.decision = engagements_.stabilize(
            engagementKey, estimate.decision, estimate.ratio,
            requiredRatio, state_.frame, !squad.enemies.empty(),
            EngagementContext{squad.enemies, squad.needsDetection, detectionReady,
                              retreatRouteFailed, squad.fightMission});
        if (squad.enemies.empty() && estimate.decision == FightDecision::kite)
            estimate.decision = FightDecision::retreat;
        if (squad.withdrawing) estimate.decision = FightDecision::retreat;
        estimate.holdScreen = SquadPlanner::mustHoldDefensiveScreen(squad);
        estimate.advanceBlocked = squad.role != SquadRole::baseDefense &&
            !detectionReady;
        if (squad.role == SquadRole::baseDefense &&
            detectionReady) {
            const auto perimeter = SquadPlanner::defensiveEngagementArea(squad, estimate);
            if (perimeter.pursuitRadius > defense.pursuitRadius) {
                defense = perimeter;
                travelReason = "clear-base-perimeter";
            }
        }
        if (!estimate.advanceBlocked && supportedArmy.size() == squad.units.size() &&
            SquadPlanner::canCounterattack(squad, estimate, plan_)) {
            // A global defense response must not trap an independently strong
            // reserve army while the allocated defenders protect the base.
            defense = {};
            objective = plan_.attackTarget;
            travelGoal = objective;
            travelReason = "counterattack";
            if (firstCounterattackFrame_ < 0) firstCounterattackFrame_ = state_.frame;
        }
        if (squad.role == SquadRole::mainArmy && vanguard == &squad &&
            (plan_.posture == Posture::hold || plan_.posture == Posture::pressure ||
             plan_.posture == Posture::defend)) {
            if (const auto target = SquadPlanner::favorableTerranFrontTarget(
                    state_, squad, estimate); target.valid()) {
                defense = {};
                travelGoal = target;
                if (hasGroundUnit && !navigation_.lineWalkable(
                        squad.center, target, squadFootprint)) {
                    const auto route = navigation_.nextWaypoint(
                        squad.center, target, 7, 30000, squadFootprint);
                    if (route.hasUsableWaypoint()) {
                        objective = route.waypoint;
                    } else {
                        objective = squad.center;
                        navigationRouteBlocked = true;
                    }
                } else {
                    objective = target;
                }
                travelReason = "clear-favorable-terran-front";
                trace("favorableTerranFront", "EVENT,favorable-terran-front,target=" +
                    std::to_string(target.x) + 'x' + std::to_string(target.y) +
                    ",field=" + std::to_string(squad.units.size()) +
                    ",ratio=" + std::to_string(estimate.ratio), 120);
            }
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
            coverExpansion
                ? "Cover expansion" : "Assemble / advance";
        if (log_ && logSquads) {
            log_ << "SQUAD," << state_.frame << ',' << squadRoleName(squad.role)
                 << ",key=" << engagementKey << ",units=" << squad.units.size()
                 << ",supportUnits=" << supportedArmy.size() - squad.units.size()
                 << ",enemies=" << squad.enemies.size()
                 << ",center=" << squad.center.x << 'x' << squad.center.y
                 << ",objective=" << objective.x << 'x' << objective.y
                 << ",travelGoal=" << travelGoal.x << 'x' << travelGoal.y
                 << ",travelReason=" << travelReason
                 << ",mission=" << fightMissionName(squad.fightMission)
                 << ",trade=" << valuation.rationale
                 << ",retreat=" << squad.retreat.x << 'x' << squad.retreat.y
                 << ",defenseCenter=" << defense.center.x << 'x' << defense.center.y
                 << ",ratio=" << estimate.ratio << ",required=" << requiredRatio
                 << ",proposed=" << static_cast<int>(proposedDecision)
                 << ",decision=" << static_cast<int>(estimate.decision)
                 << ",confidence=" << estimate.confidence << ",simulation="
                 << (simulateSquad && !estimate.simulationRoutingDeferred)
                 << ",simulationDeferred=" << (runSimulation &&
                     (!simulateSquad || estimate.simulationRoutingDeferred))
                 << ",simulationRoutingDeferred=" << estimate.simulationRoutingDeferred
                 << ",confidenceStaged=" << estimate.confidenceStaged
                 << ",friendlyRemaining=" << estimate.simulatedFriendlyRemaining
                 << ",enemyRemaining=" << estimate.simulatedEnemyRemaining
                 << ",detectionNeeded=" << squad.needsDetection
                 << ",detectionReady=" << detectionReady
                 << ",detectionBlocked=" << estimate.advanceBlocked << ",reason=" << reason << ",members=";
            for (const auto& member : squad.units) log_ << member.id << ';';
            log_ << '\n';
        }
        debug_.squads.push_back({std::string(squadRoleName(squad.role)),
            std::string(valuation.rationale) + "; " + reason, squad.center,
            objective, squad.retreat, estimate.ratio, requiredRatio,
            static_cast<int>(squad.units.size()), static_cast<int>(squad.enemies.size()), estimate.decision});
        if (squad.role == SquadRole::mainArmy && squad.units.size() >= debugSquadSize) {
            debugSquadSize = squad.units.size();
            fight_ = estimate;
        }
        const auto targets = SquadPlanner::tacticalTargets(squad, state_.enemy.units);
        const auto tacticalOrders = tactics_.control(
            squad.units, targets, estimate, objective,
            squad.retreat, influence_, squad.center, state_.latencyFrames,
            technologyLevel(state_.self, TechnologyKind::psionicStorm) > 0,
            defense, squad.withdrawing ? TacticalIntent::withdraw :
                squad.role == SquadRole::harassment ? TacticalIntent::raid : TacticalIntent::battle,
            supportedArmy, &navigation_, state_.self.units,
            tacticalTargetControl_ ? &tacticalTarget_ : nullptr,
            true, stormReservations, travelGoal);
        for (const auto& order : tacticalOrders) {
            if (order.type == CommandType::useTech &&
                order.technology == TechnologyKind::psionicStorm &&
                std::ranges::none_of(stormReservations, [&order](const Position reserved) {
                    return distanceSquared(order.targetPosition, reserved) <=
                        psionicStormReservationDistance * psionicStormReservationDistance;
                })) stormReservations.push_back(order.targetPosition);
            auto safeOrder = order;
            if (navigationRouteBlocked && order.type != CommandType::hold &&
                order.type != CommandType::stop && order.type != CommandType::useTech) {
                const auto actor = std::ranges::find(squad.units, order.actor,
                                                     &UnitSnapshot::id);
                auto target = order.targetPosition;
                if (order.type == CommandType::attackUnit) {
                    const auto enemyTarget = std::ranges::find(
                        squad.enemies, order.targetUnit, &UnitSnapshot::id);
                    if (enemyTarget != squad.enemies.end()) target = enemyTarget->position;
                }
                const auto isMovement = order.type == CommandType::move ||
                                         order.type == CommandType::attackMove;
                if (isMovement && actor != squad.units.end() && !actor->flying &&
                    target.valid() && !navigation_.lineWalkable(actor->position, target,
                        MovementFootprint{actor->dimensionLeft, actor->dimensionRight,
                                          actor->dimensionUp, actor->dimensionDown})) {
                    const auto route = navigation_.nextWaypoint(
                        actor->position, target, 4, 4000,
                        MovementFootprint{actor->dimensionLeft, actor->dimensionRight,
                                          actor->dimensionUp, actor->dimensionDown});
                    if (route.hasUsableWaypoint() && route.waypoint != actor->position) {
                        // Tactical movement (especially retreats) remains useful
                        // when its direct line crosses terrain. Follow a safe
                        // prefix instead of replacing the order with Hold every
                        // combat tick and making the unit oscillate in place.
                        safeOrder.targetPosition = route.waypoint;
                    } else {
                        safeOrder = {order.actor, CommandType::hold, -1, {-1, -1},
                            UnitKind::unknown, order.priority, 0,
                            "navigation-route-unavailable"};
                    }
                }
            }
            submit(std::move(safeOrder));
        }
#ifdef PROTODD_WHOLE_GAME_HYBRID
        for (const auto& pending : hybridProposals_) {
            if (auto proposal = hybridCombatProposal(pending.command, pending.frame,
                    state_.frame, squad, estimate, targets, defense)) {
                submit(std::move(*proposal));
                ++hybridSubmitted_;
            }
        }
#endif
        if (aggressive)
            for (const auto& order : SquadPlanner::supportEscorts(squad, objective)) submit(order);
        maxSquadUs = std::max(maxSquadUs, elapsedUs(squadStarted));
    }
    const auto squadLoopUs = elapsedUs(squadLoopStarted);
    if (logSquads) lastSquadLogFrame_ = state_.frame;
    // A proposal gets one arbitration attempt; never replay it across combat ticks.
    hybridProposals_.clear();

    const auto expansionFeedback = bridge_.expansionFeedback(plan_.expansionTarget);
    const auto activeExpansionBuild = expansionFeedback.pending &&
        distanceSquared(expansionFeedback.site, plan_.expansionTarget) <= 96 * 96;
    for (const auto& order : clearExpansionFootprint(state_, plan_.expansionTarget,
             expansionAssemblyPoint(state_, plan_.expansionTarget, retreatPoint()),
             activeExpansionBuild)) submit(order);

    for (const auto& order : tactics_.recharge(state_.self.units,
                                              plan_.posture == Posture::defend)) {
        submit(order);
    }

#ifdef PROTODD_PVZ_DETECTOR_SURGE
    const auto mobilizeDetectorReserve = state_.enemy.race == Race::zerg;
#else
    const auto mobilizeDetectorReserve = false;
#endif
#if defined(PROTODD_FORWARD_DETECTOR_ESCORT) || defined(PROTODD_DIRECT_DETECTOR_RENDEZVOUS)
    const auto centerBlockedMainEscort = true;
#else
    const auto centerBlockedMainEscort = false;
#endif
#ifdef PROTODD_CONTESTED_DETECTOR_RESERVE
    const auto mobilizeContestedReserve = true;
#else
    const auto mobilizeContestedReserve = false;
#endif
#ifdef PROTODD_DIRECT_DETECTOR_RENDEZVOUS
    constexpr auto directSafeRendezvous = true;
#else
    constexpr auto directSafeRendezvous = false;
#endif
    const auto detectorAllocation = squads_.allocateDetectors(
        state_, formed, influence_, mobilizeDetectorReserve,
        centerBlockedMainEscort, mobilizeContestedReserve,
        directSafeRendezvous);
    requiredDetectorCount_ = static_cast<int>(detectorAllocation.requiredObservers);
    detectorEscorts_ = detectorAllocation.reservedObservers;
    for (const auto& order : detectorAllocation.commands) {
        if (scouts_.observerEvading(order.actor, state_.frame)) continue;
        auto observerCommand = order;
        observerCommand.owner = CommandOwner::observerSafety;
        observerCommand.urgency = observerCommand.priority;
        submit(std::move(observerCommand));
    }
    if (detectorAllocation.requiredObservers > 0 ||
        detectorAllocation.availableObservers > 0) {
        trace("detector-allocation", "DETECTOR_ALLOC,demand=" +
            std::to_string(detectorAllocation.requiredObservers) +
            ",available=" + std::to_string(detectorAllocation.availableObservers) +
            ",assigned=" + std::to_string(detectorAllocation.assignments.size()) +
            ",reserved=" + std::to_string(detectorEscorts_.size()) +
            ",unmet=" + std::to_string(detectorAllocation.unmetDetectionDemands) +
            ",maxArrivalFrames=" +
            std::to_string(detectorAllocation.maximumArrivalFrames), 24);
    }
    for (const auto& order : transportOrders) {
        auto transportCommand = order;
        transportCommand.owner = CommandOwner::transport;
        transportCommand.urgency = transportCommand.priority;
        submit(std::move(transportCommand));
    }
    const auto profileUs = elapsedUs(profileStarted);
    if (log_ && profileUs >= 8'000) {
        log_ << "COMBAT_PROFILE," << state_.frame << ',' << profileUs << ','
             << controlPrepUs << ',' << unitSnapshotUs << ',' << formationUs << ','
             << squadLoopUs << ',' << maxSquadUs << ',' << maxCombatEstimateUs << ','
             << formed.size() << ',' << friendly.size() << ',' << enemy.size() << '\n';
    }
}

void ProtoddModule::dispatchFrameCommands() {
    // BWAPI calls are capped per frame. Priority-aware rotation keeps urgent
    // retreats and detection orders immediate while bounding army spikes.
    const auto selectedCommands = commands_.finalize(bridge_.commandBudgetRemaining());
    std::unordered_set<UnitId> selectedActors;
    const auto ownerName = [](const CommandOwner owner) -> std::string_view {
        switch (owner) {
            case CommandOwner::worker: return "worker";
            case CommandOwner::scouting: return "scouting";
            case CommandOwner::combat: return "combat";
            case CommandOwner::transport: return "transport";
            case CommandOwner::construction: return "construction";
            case CommandOwner::maintenance: return "maintenance";
            case CommandOwner::learned: return "learned";
            case CommandOwner::observerSafety: return "observer-safety";
            case CommandOwner::unspecified: return "unspecified";
        }
        return "unknown";
    };
    for (const auto& command : selectedCommands) {
        selectedActors.insert(command.actor);
        if (command.owner != CommandOwner::scouting)
            releaseScoutLeaseForPreemption(command.actor, ownerName(command.owner), command.source);
    }
    for (const auto& command : commands_.authorityWinners()) {
        if (selectedActors.contains(command.actor) || !command.alreadyActive ||
            command.owner == CommandOwner::scouting) continue;
        releaseScoutLeaseForPreemption(command.actor, ownerName(command.owner),
                                       "active-command-lease");
    }
    std::unordered_set<UnitId> releasedWorkerLeases;
    for (const auto& displaced : commands_.displacedCommands()) {
        const auto& proposal = displaced.proposal;
        const auto& winner = displaced.winner;
        if (proposal.owner != CommandOwner::worker || winner.owner == CommandOwner::worker ||
            (!selectedActors.contains(winner.actor) && !winner.alreadyActive) ||
            !releasedWorkerLeases.insert(proposal.actor).second) continue;
        const auto generation = bridge_.releaseWorkerCommandLease(proposal.actor);
        if (generation > 0) {
            trace("actor-preemption", "AUTHORITY_PREEMPT,actor=" +
                std::to_string(proposal.actor) + ",displaced=worker,newOwner=" +
                std::string(ownerName(winner.owner)) + ",generation=" +
                std::to_string(generation) + ",reason=" + csvSafe(proposal.source), 120);
        }
    }
    const auto& commandStats = commands_.stats();
    commandsProposed_ += commandStats.proposed;
    commandsSuperseded_ += commandStats.superseded;
    commandsRedundant_ += commandStats.redundant;
    commandsDeferred_ += commandStats.budgetDeferred;
    for (const auto& command : commands_.budgetDeferredCommands()) {
        const auto actor = std::to_string(command.actor);
        const auto source = csvSafe(command.source);
        const auto urgency = command.effectiveUrgency();
        trace("command-budget/" + actor,
            "COMMAND_BUDGET_DEFERRED," + actor + ",owner=" +
                std::string(ownerName(command.owner)) + ",urgency=" +
                std::to_string(urgency) + ",ageFrames=" +
                std::to_string(commands_.budgetDeferralAge(command.actor)) +
                ",source=" + source,
            24, source + "/" + std::to_string(urgency), state_.frame);
    }
    for (const auto& command : selectedCommands) {
        ++commandsAttempted_;
        const auto accepted = bridge_.executeFrameCommand(command);
        if (command.type == CommandType::useTech || command.type == CommandType::feedback ||
            command.type == CommandType::mergeArchon) {
            const auto outcome = accepted ? "accepted" : "not-accepted";
            const auto key = std::string(spellCommandName(command.type)) + "," +
                std::to_string(static_cast<int>(command.technology)) + ",issued," + outcome;
            ++spellAttemptTotals_[key];
            if (accepted) observeSpellEffectCommand(command);
        }
        if (accepted && command.owner == CommandOwner::construction &&
            (command.type == CommandType::build || command.type == CommandType::move))
            ++macroAccepted_;
        if (command.owner == CommandOwner::scouting && command.source == "scout-travel") {
            scouts_.recordCommandFeedback({command.actor,
                accepted ? ScoutCommandStatus::accepted : ScoutCommandStatus::rejected,
                command.leaseGeneration}, state_.frame);
        }
        if (accepted) {
            ++commandsAccepted_;
            if (command.source == "hybrid-trained") ++hybridAccepted_;
            commands_.markIssued(command);
            debug_.orders[command.actor] = command.source;
            if (command.owner == CommandOwner::scouting)
                debug_.scout = "Probe " + std::to_string(command.actor) + ": " + command.source;
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
        if (command.owner == CommandOwner::scouting)
            trace("scout", "SCOUT," + std::to_string(command.actor) + ',' + command.source + ',' +
                std::to_string(command.targetUnit) + ',' + std::to_string(accepted));
    }
    for (const auto& command : commands_.authorityWinners()) {
        if (selectedActors.contains(command.actor) || command.owner != CommandOwner::scouting ||
            command.source != "scout-travel") continue;
        scouts_.recordCommandFeedback({command.actor,
            command.alreadyActive ? ScoutCommandStatus::alreadyActive : ScoutCommandStatus::deferred,
            command.leaseGeneration}, state_.frame);
    }
    bridge_.endFrameCommands();
}

void ProtoddModule::reconcileCommandEffects() {
    for (const auto& pending : commands_.pendingEffectFeedback()) {
        const auto& command = pending.command;
        const auto actor = std::to_string(command.actor);
        const auto type = std::to_string(static_cast<int>(command.type));
        const auto source = csvSafe(command.source);
        if (bridge_.commandActive(command)) {
            if (commands_.markEffectObserved(command, state_.frame)) {
                trace("command-effect/" + actor,
                    "COMMAND_EFFECT," + actor + ",type=" + type + ",source=" + source +
                    ",api_accepted=1,effect=observed,delay=" +
                    std::to_string(state_.frame - pending.acceptedFrame),
                    120, "observed/" + type + "/" + source + "/" +
                        std::to_string(pending.acceptedFrame), state_.frame);
            }
            continue;
        }
        const auto window = commandEffectObservationWindow(command.type, state_.latencyFrames);
        if (window > 0 && !pending.timeoutReported &&
            state_.frame - pending.acceptedFrame > window &&
            commands_.markEffectTimeoutReported(command)) {
            trace("command-effect/" + actor,
                "COMMAND_EFFECT," + actor + ",type=" + type + ",source=" + source +
                ",api_accepted=1,effect=unobserved,retryable=1,delay=" +
                std::to_string(state_.frame - pending.acceptedFrame),
                120, "unobserved/" + type + "/" + source + "/" +
                    std::to_string(pending.acceptedFrame), state_.frame);
        }
    }
}

void ProtoddModule::observeSpellEffectCommand(const Command& command) {
    constexpr auto maximumPendingSpellEffects = std::size_t{128};
    const auto typeName = std::string(spellCommandName(command.type));
    const auto technology = std::to_string(static_cast<int>(command.technology));
    const auto addOutcome = [this, &typeName, &technology](const std::string_view outcome) {
        ++spellEffectTotals_[typeName + ',' + technology + ',' + std::string(outcome)];
    };
    if (pendingSpellEffects_.size() >= maximumPendingSpellEffects) {
        addOutcome("censored");
        return;
    }
    PendingSpellEffect pending;
    pending.command = command;
    pending.acceptedFrame = state_.frame;
    pending.observationWindow = command.type == CommandType::feedback ? 24 :
        command.type == CommandType::mergeArchon ? 72 :
        command.technology == TechnologyKind::psionicStorm ? 96 : 48;
    const auto findUnit = [](const std::vector<UnitSnapshot>& units, const UnitId id)
        -> const UnitSnapshot* {
        const auto found = std::ranges::find(units, id, &UnitSnapshot::id);
        return found == units.end() ? nullptr : &*found;
    };
    if (const auto* actor = findUnit(state_.self.units, command.actor)) {
        pending.actorPosition = actor->position;
        pending.baselineActorEnergy = actor->energy;
    } else {
        pending.baselineComplete = false;
    }
    if (command.type == CommandType::useTech && command.targetPosition.valid()) {
        if (command.technology == TechnologyKind::psionicStorm ||
            command.technology == TechnologyKind::stasisField) {
            constexpr auto targetRadius = 128;
            for (const auto& enemy : state_.enemy.units) {
                if (!enemy.visible || !enemy.position.valid() ||
                    distanceSquared(enemy.position, command.targetPosition) >
                        targetRadius * targetRadius) continue;
                pending.subjects.emplace(enemy.id, enemy);
            }
            if (pending.subjects.empty()) pending.baselineComplete = false;
        } else if (command.technology == TechnologyKind::recall) {
            constexpr auto recallRadius = 288;
            constexpr auto minimumRecallTravel = 192;
            for (const auto& ally : state_.self.units) {
                if (!ally.completed || ally.loaded || isBuilding(ally.kind) ||
                    !ally.position.valid() || ally.id == command.actor ||
                    distanceSquared(ally.position, command.targetPosition) >
                        recallRadius * recallRadius ||
                    distanceSquared(ally.position, pending.actorPosition) <=
                        minimumRecallTravel * minimumRecallTravel) continue;
                pending.subjects.emplace(ally.id, ally);
            }
        } else {
            pending.baselineComplete = false;
        }
    } else if (command.type == CommandType::feedback) {
        pending.observationWindow = 24;
        if (const auto* target = findUnit(state_.enemy.units, command.targetUnit);
            target != nullptr && target->visible) {
            pending.subjects.emplace(target->id, *target);
        } else {
            pending.baselineComplete = false;
        }
    } else if (command.type == CommandType::mergeArchon) {
        pending.observationWindow = 72;
        for (const auto id : {command.actor, command.targetUnit}) {
            if (const auto* templar = findUnit(state_.self.units, id);
                templar != nullptr && templar->kind == UnitKind::highTemplar) {
                pending.subjects.emplace(id, *templar);
            }
        }
        for (const auto& ally : state_.self.units)
            if (ally.kind == UnitKind::archon) pending.existingArchons.push_back(ally.id);
        if (pending.subjects.size() != 2) pending.baselineComplete = false;
    } else {
        pending.baselineComplete = false;
    }
    pendingSpellEffects_.push_back(std::move(pending));
}

void ProtoddModule::reconcileSpellEffects() {
    if (pendingSpellEffects_.empty()) return;
    std::unordered_map<UnitId, std::vector<Position>> stormPositionsByCaster;
    if (std::ranges::any_of(pendingSpellEffects_, [](const PendingSpellEffect& pending) {
            return pending.command.type == CommandType::useTech &&
                   pending.command.technology == TechnologyKind::psionicStorm;
        })) {
        for (const auto bullet : BWAPI::Broodwar->getBullets()) {
            if (bullet == nullptr || !bullet->exists() || !bullet->isVisible() ||
                bullet->getType() != BWAPI::BulletTypes::Psionic_Storm ||
                bullet->getPlayer() != BWAPI::Broodwar->self()) continue;
            const auto source = bullet->getSource();
            if (source == nullptr || !source->exists() || !bullet->getPosition().isValid()) continue;
            stormPositionsByCaster[source->getID()].push_back(
                {bullet->getPosition().x, bullet->getPosition().y});
        }
    }
    const auto findUnit = [](const std::vector<UnitSnapshot>& units, const UnitId id)
        -> const UnitSnapshot* {
        const auto found = std::ranges::find(units, id, &UnitSnapshot::id);
        return found == units.end() ? nullptr : &*found;
    };
    for (auto pending = pendingSpellEffects_.begin(); pending != pendingSpellEffects_.end();) {
        const auto& command = pending->command;
        const auto actor = findUnit(state_.self.units, command.actor);
        auto effectObserved = false;
        if (command.type == CommandType::useTech &&
            command.technology == TechnologyKind::psionicStorm) {
            constexpr auto targetRadius = 128;
            const auto storms = stormPositionsByCaster.find(command.actor);
            const auto ownStorm = storms != stormPositionsByCaster.end() &&
                std::ranges::any_of(storms->second, [&command](const Position position) {
                    return distanceSquared(position, command.targetPosition) <=
                        targetRadius * targetRadius;
                });
            auto enemyUnderOwnStorm = false;
            if (ownStorm) {
                enemyUnderOwnStorm = std::ranges::any_of(state_.enemy.units,
                    [&command](const UnitSnapshot& enemy) {
                        return enemy.visible && enemy.underStorm && enemy.position.valid() &&
                            distanceSquared(enemy.position, command.targetPosition) <= 128 * 128;
                    });
            }
            effectObserved = ownStorm && enemyUnderOwnStorm;
            for (const auto& [id, _] : pending->subjects) {
                const auto* enemy = findUnit(state_.enemy.units, id);
                if (enemy == nullptr || !enemy->visible) pending->baselineComplete = false;
            }
        } else if (command.type == CommandType::useTech &&
                   command.technology == TechnologyKind::stasisField) {
            auto targetDisabled = false;
            for (const auto& [id, baseline] : pending->subjects) {
                const auto* enemy = findUnit(state_.enemy.units, id);
                if (enemy == nullptr || !enemy->visible) {
                    pending->baselineComplete = false;
                    continue;
                }
                if (enemy->disabled && !baseline.disabled) targetDisabled = true;
            }
            effectObserved = actor != nullptr &&
                actor->energy < pending->baselineActorEnergy && targetDisabled;
        } else if (command.type == CommandType::useTech &&
                   command.technology == TechnologyKind::recall) {
            auto recalled = false;
            constexpr auto arrivalRadius = 128;
            constexpr auto minimumRecallTravel = 256;
            for (const auto& [id, baseline] : pending->subjects) {
                const auto* ally = findUnit(state_.self.units, id);
                if (ally == nullptr || !ally->completed || ally->loaded) {
                    pending->baselineComplete = false;
                    continue;
                }
                if (pending->actorPosition.valid() && ally->position.valid() &&
                    distanceSquared(ally->position, pending->actorPosition) <=
                        arrivalRadius * arrivalRadius &&
                    distanceSquared(ally->position, baseline.position) >=
                        minimumRecallTravel * minimumRecallTravel) recalled = true;
            }
            effectObserved = actor != nullptr &&
                actor->energy < pending->baselineActorEnergy && recalled;
        } else if (command.type == CommandType::feedback) {
            const auto* target = findUnit(state_.enemy.units, command.targetUnit);
            if (target == nullptr || !target->visible) {
                pending->baselineComplete = false;
            } else if (const auto baseline = pending->subjects.find(command.targetUnit);
                       baseline != pending->subjects.end()) {
                const auto priorDurability = baseline->second.hitPoints + baseline->second.shields;
                const auto currentDurability = target->hitPoints + target->shields;
                effectObserved = target->energy < baseline->second.energy &&
                    currentDurability < priorDurability;
            }
        } else if (command.type == CommandType::mergeArchon) {
            const auto nearMergeSite = [&pending](const UnitSnapshot& unit) {
                for (const auto& [_, templar] : pending->subjects)
                    if (templar.position.valid() && unit.position.valid() &&
                        distanceSquared(unit.position, templar.position) <= 128 * 128) return true;
                return false;
            };
            effectObserved = std::ranges::any_of(state_.self.units,
                [&command, &pending, &nearMergeSite](const UnitSnapshot& ally) {
                    if (ally.kind != UnitKind::archon || !nearMergeSite(ally)) return false;
                    return ally.id == command.actor || ally.id == command.targetUnit ||
                        std::ranges::find(pending->existingArchons, ally.id) ==
                            pending->existingArchons.end();
                });
            if (!effectObserved) {
                for (const auto& [id, _] : pending->subjects) {
                    const auto* templar = findUnit(state_.self.units, id);
                    if (templar == nullptr || templar->kind != UnitKind::highTemplar)
                        pending->baselineComplete = false;
                }
            }
        }
        const auto expired = state_.frame - pending->acceptedFrame >= pending->observationWindow;
        if (effectObserved || expired) {
            const auto outcome = effectObserved ? "observed" :
                pending->baselineComplete ? "ineffective" : "censored";
            const auto key = std::string(spellCommandName(command.type)) + ',' +
                std::to_string(static_cast<int>(command.technology)) + ',' + outcome;
            ++spellEffectTotals_[key];
            pending = pendingSpellEffects_.erase(pending);
        } else {
            ++pending;
        }
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
    std::string comparison,
    Frame frame) {
    if (!log_) return;
    if (frame < 0) frame = state_.frame;
    auto& previous = traceMemory_[key];
    if (comparison.empty()) comparison = value;
    if (comparison == previous.value && frame - previous.frame < heartbeat) return;
    const auto comma = value.find(',');
    if (comma == std::string::npos) return;
    log_ << value.substr(0, comma) << ',' << frame << value.substr(comma) << '\n';
    previous = {std::move(comparison), frame};
}

void ProtoddModule::logAction(const ActionDiagnostic& action) noexcept {
    try {
        const auto frame = BWAPI::Broodwar->getFrameCount();
        const auto stage = action.attempted ? "issued" : "blocked";
        const auto source = csvSafe(action.source);
        const auto outcome = csvSafe(action.outcome);
        ++actionTotals_[source + ',' + stage + ',' + outcome];
        if (action.accepted) {
            lastActions_[action.actor] = {frame, source, action.type, action.target};
            debug_.orders[action.actor] = source;
        }
        std::ostringstream row;
        row << "ACTION," << action.actor << ',' << source << ',' << csvSafe(action.type)
            << ',' << stage << ',' << outcome << ",target=" << action.target
            << ",x=" << action.position.x << ",y=" << action.position.y << ",extra=" << action.extra
            << ",minerals=" << state_.self.minerals << ",gas=" << state_.self.gas;
        const auto actor = BWAPI::Broodwar->getUnit(action.actor);
        if (actor && actor->exists()) row << ",order=" << csvSafe(actor->getOrder().toString())
            << ",hp=" << actor->getHitPoints() << ",shields=" << actor->getShields()
            << ",energy=" << actor->getEnergy();
        const auto comparison = source + '/' + action.type + '/' + stage + '/' + outcome + '/' +
            std::to_string(action.target) + '/' + std::to_string(action.extra) + '/' +
            std::to_string(action.position.x / 64) + '/' + std::to_string(action.position.y / 64);
        trace("action/" + std::to_string(action.actor), row.str(), 120, comparison, frame);
    } catch (...) { ++loggingErrors_; }
}

void ProtoddModule::logBuildLease(const BuildLeaseDiagnostic& lease) noexcept {
    try {
        if (!log_) return;
        log_ << "BUILDLEASE," << lease.frame << ",kind="
             << BwapiBridge::toBwapi(lease.kind).toString()
             << ",builder=" << lease.builder << ",issued=" << lease.issued
             << ",targetX=" << lease.target.x << ",targetY=" << lease.target.y
             << ",builderX=" << lease.builderPosition.x
             << ",builderY=" << lease.builderPosition.y
             << ",lastProgress=" << lease.lastProgress
             << ",travelDeadline=" << lease.travelDeadline
             << ",hardTravelDeadline=" << lease.hardTravelDeadline
             << ",bestDistanceToTarget=" << lease.bestDistanceToTarget
             << ",reason=" << csvSafe(lease.reason)
             << ",order=" << csvSafe(lease.order)
             << ",commandedBuild=" << lease.commandedBuild
             << ",buildTypeMatches=" << lease.buildTypeMatches
             << ",builderCanBuildHere=" << lease.builderCanBuildHere
             << ",mapCanBuildHere=" << lease.mapCanBuildHere
             << ",hasPath=" << lease.hasPath << '\n';
    } catch (...) { ++loggingErrors_; }
}

void ProtoddModule::logBuildSelection(const BuildSelectionDiagnostic& selection) noexcept {
    try {
        if (!log_) return;
        log_ << "BUILDSELECT," << selection.frame
             << ",kind=" << BwapiBridge::toBwapi(selection.kind).toString()
             << ",selected=" << selection.selected
             << ",siteCandidate=" << selection.siteCandidate
             << ",targetX=" << selection.target.x << ",targetY=" << selection.target.y
             << ",anchorX=" << selection.anchor.x << ",anchorY=" << selection.anchor.y
             << ",selectedDistance=" << selection.selectedDistance
             << ",candidateDistance=" << selection.siteCandidateDistance
             << ",candidateCanBuildHere=" << selection.candidateCanBuildHere
             << ",candidateHasPath=" << selection.candidateHasPath << '\n';
    } catch (...) { ++loggingErrors_; }
}

void ProtoddModule::logBuildRoute(const BuildRouteDiagnostic& route) noexcept {
    if (!log_) return;
    try {
        log_ << "BUILDROUTE," << route.frame << ",kind=Protoss_Nexus,builder=" << route.builder
             << ",fromX=" << route.from.x << ",fromY=" << route.from.y
             << ",anchorX=" << route.anchor.x << ",anchorY=" << route.anchor.y
             << ",destinationX=" << route.destination.x << ",destinationY=" << route.destination.y
             << ",stage=" << route.stage << ",reachable=" << route.reachable
             << ",peakThreat=" << route.peakThreat << ",anchorThreat=" << route.anchorThreat
             << ",fromWalkable=" << route.fromWalkable
             << ",destinationWalkable=" << route.destinationWalkable
             << ",nativeHasPath=" << route.nativeHasPath << ",searches=" << route.searches << '\n';
    } catch (...) { ++loggingErrors_; }
}

void ProtoddModule::logLifecycle(const BWAPI::Unit unit, const std::string_view event) {
    if (!log_ || !unit) return;
    const auto ours = unit->getPlayer() == BWAPI::Broodwar->self();
    if (!ours && (unit->getPlayer() != BWAPI::Broodwar->enemy() || !unit->isVisible())) return;
    log_ << "LIFECYCLE," << BWAPI::Broodwar->getFrameCount() << ',' << event << ','
         << (ours ? "self" : "enemy") << ',' << unit->getID() << ','
         << csvSafe(unit->getType().toString()) << ",x=" << unit->getPosition().x
         << ",y=" << unit->getPosition().y << ",completed=" << unit->isCompleted()
         << ",remainingBuild=" << unit->getRemainingBuildTime() << '\n';
}

void ProtoddModule::logDamage() {
    if (!log_) return;
    const auto observe = [this](const PlayerSnapshot& player, const char* side) {
        for (const auto& unit : player.units) {
            if (!unit.ours && !unit.visible) { damageSamples_.erase(unit.id); continue; }
            const auto prior = damageSamples_.find(unit.id);
            if (prior != damageSamples_.end() && prior->second.kind == unit.kind &&
                prior->second.lastSeen == state_.frame - 1) {
                const auto hpLoss = std::max(0, prior->second.hitPoints - unit.hitPoints);
                const auto shieldLoss = std::max(0, prior->second.shields - unit.shields);
                if (hpLoss + shieldLoss > 0) {
                    log_ << "DAMAGE," << state_.frame << ',' << side << ',' << unit.id << ','
                         << unitStats(unit.kind).name << ",hpLoss=" << hpLoss
                         << ",shieldLoss=" << shieldLoss << ",hp=" << unit.hitPoints
                         << ",shields=" << unit.shields << ",x=" << unit.position.x
                         << ",y=" << unit.position.y << ",underAttack=" << unit.underAttack
                         << ",underStorm=" << unit.underStorm << ",cooldown=" << unit.weaponCooldown;
                    const auto action = lastActions_.find(unit.id);
                    if (action != lastActions_.end()) log_ << ",lastAction=" << action->second.source
                        << ",actionFrame=" << action->second.frame << ",actionTarget=" << action->second.target;
                    log_ << '\n';
                }
            }
            damageSamples_[unit.id] = unit;
        }
    };
    observe(state_.self, "self");
    observe(state_.enemy, "enemy");
    std::erase_if(damageSamples_, [this](const auto& entry) { return entry.second.lastSeen < state_.frame; });
}

void ProtoddModule::incident(const std::string_view kind, const UnitId unit, const bool active,
                            const Frame threshold, const std::string_view evidence) {
    const auto key = std::string(kind) + '/' + std::to_string(unit);
    if (!active && !incidents_.contains(key)) return;
    const auto update = incidents_[key].sample(state_.frame, active, threshold);
    if (update && log_) log_ << "INCIDENT," << state_.frame << ',' << kind << ',' << unit
        << ",since=" << update->since << ",duration=" << update->duration
        << ",active=" << update->active << ',' << evidence << '\n';
    if (!active) incidents_.erase(key);
}

void ProtoddModule::logDiagnostics() {
    auto idleGateways = 0;
    auto usableGateways = 0;
    auto idleWorkers = 0;
    auto unpowered = 0;
    auto idleTrainingProducers = 0;
    auto affordableProducerIdle = 0;
    constexpr std::array trainableProtossUnits{
        UnitKind::probe, UnitKind::zealot, UnitKind::dragoon, UnitKind::highTemplar,
        UnitKind::darkTemplar, UnitKind::shuttle, UnitKind::reaver, UnitKind::observer,
        UnitKind::scout, UnitKind::corsair, UnitKind::carrier, UnitKind::arbiter};
    const auto freeMinerals = spendingLedger_.freeMinerals();
    const auto freeGas = spendingLedger_.freeGas();
    const auto freeSupply = std::max(0, state_.self.supplyTotal - state_.self.supplyUsed);
    for (const auto unit : BWAPI::Broodwar->self()->getUnits()) {
        if (unit == nullptr || !unit->exists()) continue;
        const auto id = unit->getID();
        const auto type = unit->getType();
        const auto evidence = "unitKind=" + csvSafe(type.toString()) + ",x=" +
            std::to_string(unit->getPosition().x) + ",y=" + std::to_string(unit->getPosition().y) +
            ",order=" + csvSafe(unit->getOrder().toString()) + ",minerals=" +
            std::to_string(state_.self.minerals) + ",gas=" + std::to_string(state_.self.gas);
        const auto ready = unit->isCompleted() && !unit->isLoaded() &&
            !unit->isLockedDown() && !unit->isStasised() && !unit->isMaelstrommed();
        const auto production = type == BWAPI::UnitTypes::Protoss_Nexus ||
            type == BWAPI::UnitTypes::Protoss_Gateway || type == BWAPI::UnitTypes::Protoss_Stargate ||
            type == BWAPI::UnitTypes::Protoss_Robotics_Facility;
        const auto idleProducer = production && ready && unit->isPowered() &&
            !unit->isTraining() && unit->getRemainingTrainTime() == 0 &&
            unit->getTrainingQueue().empty() && !unit->isResearching() &&
            !unit->isUpgrading() &&
            unit->getLastCommandFrame() + std::max(1, state_.latencyFrames) < state_.frame;
        auto affordableProducer = false;
        if (log_ && idleProducer) {
            affordableProducer = std::ranges::any_of(
                trainableProtossUnits, [unit, type, freeMinerals, freeGas, freeSupply](
                    const UnitKind kind) {
                    const auto product = BwapiBridge::toBwapi(kind);
                    const auto& cost = unitStats(kind);
                    return product != BWAPI::UnitTypes::None &&
                        product.whatBuilds().first == type &&
                        freeMinerals >= cost.minerals && freeGas >= cost.gas &&
                        freeSupply >= product.supplyRequired() && unit->canTrain(product) &&
                        BWAPI::Broodwar->canMake(product, unit);
                });
        }
        incident("idle-worker", id, ready && type.isWorker() && unit->isIdle(), 72, evidence);
        incident("unpowered-building", id, unit->isCompleted() && type.requiresPsi() &&
            !unit->isPowered(), 48, evidence);
        incident("affordable-idle-production", id, affordableProducer, 120, evidence);
        incident("empty-ammunition", id, ready &&
            ((type == BWAPI::UnitTypes::Protoss_Reaver && unit->getScarabCount() == 0) ||
             (type == BWAPI::UnitTypes::Protoss_Carrier && unit->getInterceptorCount() == 0)), 72, evidence);
        auto& motion = motionSamples_[id];
        const Position position{unit->getPosition().x, unit->getPosition().y};
        const auto order = unit->getOrder();
        const auto destination = unit->getOrderTargetPosition();
        const auto travelling = ready && !type.isBuilding() && destination.isValid() &&
            (order == BWAPI::Orders::Move || order == BWAPI::Orders::AttackMove) &&
            unit->getDistance(destination) > 96;
        if (!travelling || !motion.anchor.valid() || distanceSquared(motion.anchor, position) > 24 * 24)
            motion = {position, state_.frame};
        incident("movement-stalled", id, travelling && state_.frame - motion.since >= 144, 0,
            evidence + ",stationarySince=" + std::to_string(motion.since));
        if (!unit->isCompleted()) continue;
        if (unit->getType().requiresPsi() && !unit->isPowered()) ++unpowered;
        if (unit->getType() == BWAPI::UnitTypes::Protoss_Gateway && unit->isPowered()) {
            ++usableGateways;
            if (!unit->isTraining() && unit->getRemainingTrainTime() == 0 &&
                unit->getTrainingQueue().empty() &&
                unit->getLastCommandFrame() + std::max(1, state_.latencyFrames) < state_.frame)
                ++idleGateways;
        }
        if (idleProducer) {
            ++idleTrainingProducers;
            if (affordableProducer) ++affordableProducerIdle;
        }
        if (unit->getType().isWorker() && unit->isIdle()) ++idleWorkers;
    }
    const auto blocked = state_.self.supplyTotal > 0 && state_.self.supplyTotal < 400 &&
        state_.self.supplyTotal - state_.self.supplyUsed < 4;
    // Close conditions for dead, transferred, or otherwise absent own units.
    // Their disappearance must not leave a permanent active incident.
    for (auto it = incidents_.begin(); it != incidents_.end();) {
        auto& entry = *it;
        const auto id = std::stoi(entry.first.substr(entry.first.rfind('/') + 1));
        if (id < 0 || std::ranges::any_of(state_.self.units,
            [id](const UnitSnapshot& unit) { return unit.id == id; })) { ++it; continue; }
        if (const auto update = entry.second.sample(state_.frame, false, 0); update && log_)
            log_ << "INCIDENT," << state_.frame << ',' << entry.first.substr(0, entry.first.rfind('/'))
                 << ',' << id << ",since=" << update->since << ",duration=" << update->duration
                 << ",active=0,reason=unit-unavailable\n";
        it = incidents_.erase(it);
    }
    const auto hardBlocked = state_.self.supplyTotal > 0 && state_.self.supplyTotal < 400 &&
                             state_.self.supplyUsed >= state_.self.supplyTotal;
    // A cap reached while the opening Pylon is already under construction is
    // an observable planned wait. All other hard-cap time is reported as
    // unintended; the split never hides the total hard-blocked duration.
    const auto deliberateOpeningPause = hardBlocked && state_.frame < 4 * 60 * 24 &&
        std::ranges::any_of(state_.self.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::pylon && !unit.completed;
        });
    supplyTightFrames_.sample(state_.frame, blocked ? 1 : 0);
    supplyHardBlockedFrames_.sample(state_.frame, hardBlocked ? 1 : 0);
    supplyDeliberateOpeningPauseFrames_.sample(
        state_.frame, deliberateOpeningPause ? 1 : 0);
    supplyUnintendedBlockedFrames_.sample(
        state_.frame, hardBlocked && !deliberateOpeningPause ? 1 : 0);
    supplyCappedFrames_.sample(
        state_.frame, state_.self.supplyTotal > 0 && state_.self.supplyTotal < 400 ? 1 : 0);
    idleGatewayFrames_.sample(state_.frame, idleGateways);
    idleWorkerFrames_.sample(state_.frame, idleWorkers);
    idleTrainingProducerFrames_.sample(state_.frame, idleTrainingProducers);
    affordableProducerIdleFrames_.sample(state_.frame, affordableProducerIdle);
    debug_.idleGateways = idleGateways;
    debug_.usableGateways = usableGateways;
    debug_.idleWorkers = idleWorkers;
    debug_.unpoweredBuildings = unpowered;
    debug_.health = "Idle Gateways " + std::to_string(idleGateways) + "/" +
        std::to_string(usableGateways) + " | idle Probes " + std::to_string(idleWorkers) +
        " | supply tight " + std::to_string(supplyTightFrames_.total() / 24) + "s";
    if (!log_) return;
    const auto feedback = bridge_.expansionFeedback(plan_.expansionTarget);
    incident("supply-blocked", -1, state_.self.supplyTotal > 0 && state_.self.supplyTotal < 400 &&
        state_.self.supplyUsed >= state_.self.supplyTotal, 48,
        "supply=" + std::to_string(state_.self.supplyUsed) + ",total=" + std::to_string(state_.self.supplyTotal));
    incident("mineral-bank", -1, state_.self.minerals >= 800, 120,
        "minerals=" + std::to_string(state_.self.minerals) + ",macro=" + csvSafe(bridge_.lastMacroStatus()));
    incident("expansion-stalled", -1, feedback.pending && feedback.stalledFrames >= 120, 0,
        "stalledFrames=" + std::to_string(feedback.stalledFrames));
    for (const auto& [key, total] : actionTotals_)
        log_ << "ACTION_TOTAL," << state_.frame << ',' << key << ',' << total << '\n';
    for (const auto& [key, total] : spellAttemptTotals_)
        log_ << "SPELL_TOTAL," << state_.frame << ',' << key << ',' << total << '\n';
    for (const auto& [key, total] : spellEffectTotals_)
        log_ << "SPELL_EFFECT_TOTAL," << state_.frame << ',' << key << ',' << total << '\n';
    log_ << "HEALTH," << state_.frame << ",idleGateways=" << idleGateways
         << ",gateways=" << usableGateways << ",idleWorkers=" << idleWorkers
         << ",idleTrainingProducers=" << idleTrainingProducers
         << ",affordableProducerIdle=" << affordableProducerIdle
         << ",idleTrainingProducerFrames=" << idleTrainingProducerFrames_.total()
         << ",affordableProducerIdleFrames=" << affordableProducerIdleFrames_.total()
         << ",unpowered=" << unpowered << ",supplyTightFrames=" << supplyTightFrames_.total()
         << ",supplyHardBlockedFrames=" << supplyHardBlockedFrames_.total()
         << ",supplyUnintendedBlockedFrames=" << supplyUnintendedBlockedFrames_.total()
         << ",supplyCappedFrames=" << supplyCappedFrames_.total()
         << ",supplyDeliberateOpeningPauseFrames="
         << supplyDeliberateOpeningPauseFrames_.total()
         << ",idleGatewayFrames=" << idleGatewayFrames_.total()
         << ",idleWorkerFrames=" << idleWorkerFrames_.total()
         << ",commandsAttempted=" << commandsAttempted_ << ",commandsAccepted=" << commandsAccepted_
         << ",commandsProposed=" << commandsProposed_ << ",commandsSuperseded=" << commandsSuperseded_
         << ",commandsRedundant=" << commandsRedundant_ << ",commandsDeferred=" << commandsDeferred_
         << ",macroAttempted=" << macroAttempted_ << ",macroAccepted=" << macroAccepted_
         << ",caughtErrors=" << caughtErrors_ << ",loggingErrors=" << loggingErrors_ + bridge_.diagnosticErrors()
         << ",expansionPending=" << feedback.pending << ",expansionStalled=" << feedback.stalledFrames
         << ",expansionDeferred=" << plan_.deferExpansion
         << ",minerals=" << state_.self.minerals << ",gas=" << state_.self.gas
         << ",minedMinerals=" << state_.self.gatheredMinerals << ",minedGas=" << state_.self.gatheredGas
         << ",supply=" << state_.self.supplyUsed << ",supplyTotal=" << state_.self.supplyTotal
         << ",probes=" << countUnits(state_.self.units, UnitKind::probe, true)
         << ",bases=" << countUnits(state_.self.units, UnitKind::nexus, true) << '\n';
    if (protodd::frame_schedule::phaseSummaryDue(state_.frame)) {
        for (const auto& [name, timing] : phases_)
            log_ << "PHASE," << state_.frame << ',' << name << ',' << timing.calls << ','
                 << timing.totalUs << ',' << timing.peakUs << ','
                 << timing.budgetDeferred << '\n';
        for (auto raw = 0; raw < static_cast<int>(EnemyPlan::count); ++raw) {
            const auto kind = static_cast<EnemyPlan>(raw);
            log_ << "BELIEF," << state_.frame << ',' << enemyPlanName(kind) << ','
                 << opponent_.probability(kind) << '\n';
        }
    }
    // Full snapshots have a boundary even if both armies are empty.
    log_ << "SNAPSHOT," << state_.frame << '\n';
    const auto entities = [this](const PlayerSnapshot& player, const char* side) {
            for (const auto& unit : player.units) {
                const auto order = debug_.orders.find(unit.id);
                log_ << "ENTITY," << state_.frame << ',' << side << ',' << unit.id << ','
                     << unitStats(unit.kind).name << ',' << unit.position.x << ',' << unit.position.y
                     << ',' << unit.hitPoints << ',' << unit.shields << ',' << unit.weaponCooldown
                     << ',' << unit.orderTargetId << ',' << unit.lastSeen << ',' << unit.visible
                     << ',' << (unit.ours && order != debug_.orders.end() ? order->second : "")
                     << ",completed=" << unit.completed << ",powered=" << unit.powered
                     << ",energy=" << unit.energy << ",ammo=" << unit.ammo
                     << ",loaded=" << unit.loaded << ",transport=" << unit.transportId
                     << ",underAttack=" << unit.underAttack << ",underStorm=" << unit.underStorm
                     << ",detected=" << unit.detected << ",cloaked=" << unit.cloaked
                     << ",attackFrame=" << unit.attackFrame
                     << ",attackWindup=" << unit.attackWindup
                     << ",groundRange=" << unit.groundWeapon.maxRange
                     << ",groundDamage=" << unit.groundWeapon.damage
                     << ",airRange=" << unit.airWeapon.maxRange
                     << ",airDamage=" << unit.airWeapon.damage
                     << ",armor=" << unit.armor << ",shieldArmor=" << unit.shieldArmor
                     << ",topSpeed=" << unit.topSpeed;
                // Enemy queues and orders in fog are never queried.
                if (unit.ours) {
                    const auto native = BWAPI::Broodwar->getUnit(unit.id);
                    if (native && native->exists()) {
                        const auto hasProductionQueue = isBuilding(unit.kind) ||
                            unit.kind == UnitKind::reaver || unit.kind == UnitKind::carrier;
                        log_ << ",nativeOrder=" << csvSafe(native->getOrder().toString())
                             << ",orderX=" << native->getOrderTargetPosition().x
                             << ",orderY=" << native->getOrderTargetPosition().y
                             << ",lastCommandFrame=" << native->getLastCommandFrame()
                             << ",idle=" << native->isIdle() << ",moving=" << native->isMoving()
                             << ",remainingBuild=" << native->getRemainingBuildTime()
                             << ",remainingTrain=" << (hasProductionQueue ? native->getRemainingTrainTime() : 0)
                             << ",remainingResearch=" << native->getRemainingResearchTime()
                             << ",remainingUpgrade=" << native->getRemainingUpgradeTime()
                             << ",research=" << csvSafe(native->getTech().toString())
                             << ",upgrade=" << csvSafe(native->getUpgrade().toString()) << ",queue=";
                        // BWAPI reuses queue storage on ordinary mobile units;
                        // a Probe can otherwise appear to be training a Pylon.
                        if (hasProductionQueue)
                            for (const auto queued : native->getTrainingQueue()) log_ << csvSafe(queued.toString()) << ';';
                    }
                }
                log_ << '\n';
            }
        };
        entities(state_.self, "self");
        entities(state_.enemy, "enemy");
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
    const auto productionReadiness = assessProductionReadiness(state_.self);
    const auto mobileArmy = std::ranges::count_if(
        state_.self.units, [](const UnitSnapshot& unit) {
            return unit.completed && isCombatUnit(unit.kind) && !unit.flying;
        });
    const auto visibleEnemyArmy = std::ranges::count_if(
        state_.enemy.units, [](const UnitSnapshot& unit) {
            return unit.visible && unit.completed && isCombatUnit(unit.kind);
        });
    // Observability must never invoke the stateful planner a second time.
    const auto& diagnosticLedger = spendingLedger_;
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
         << ",incomePm=" << state_.estimatedMineralIncomePerMinute << '/'
         << state_.estimatedGasIncomePerMinute
         << ",gatewayUtil=" << productionReadiness.occupiedGateways << '/'
         << productionReadiness.usableGateways << '/'
         << productionReadiness.unobservedGateways << '/'
         << productionReadiness.producerSnapshotAvailable
         << ",armyReady=" << productionReadiness.armyReadySoon() << '/'
         << plan_.minimumAttackSize
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
    log_ << ",baseCandidates=";
    for (std::size_t index = 0; index < state_.bases.size(); ++index) {
        if (index > 0) log_ << ';';
        const auto& base = state_.bases[index];
        log_ << base.id << '@' << base.center.x << 'x' << base.center.y
             << ":route=" << base.groundDistanceFromMain
             << ":gas=" << base.geysers
             << ":min=" << base.mineralsRemaining
             << ":owner=" << base.ownerId
             << ":start=" << base.startLocation
             << ":island=" << base.island;
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
