#include "ProtoddModule.hpp"

#include <BWAPI.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "protodd/FrameSchedule.hpp"
#include "protodd/Learning.hpp"

#if !defined(PROTODD_ENGINE_FAULT_INJECTION)
#error "The fault scenario must use the isolated test-only injection build."
#endif

using protodd::bwapi::ProtoddModule;

namespace {

std::string rowStartingWith(const std::string_view text, const std::string_view prefix) {
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        const auto length = end == std::string_view::npos ? text.size() - start : end - start;
        const auto row = text.substr(start, length);
        if (row.starts_with(prefix)) return std::string(row);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return {};
}

std::uint64_t csvUnsignedColumn(const std::string_view row, const std::size_t column) {
    std::size_t start = 0;
    for (std::size_t index = 0; index < column; ++index) {
        const auto comma = row.find(',', start);
        if (comma == std::string_view::npos) return 0;
        start = comma + 1;
    }
    const auto end = row.find(',', start);
    const auto field = row.substr(start, end == std::string_view::npos
        ? row.size() - start : end - start);
    try { return std::stoull(std::string(field)); }
    catch (...) { return 0; }
}

std::size_t countOccurrences(const std::string_view text, const std::string_view needle) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string_view::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

}  // namespace

class FaultScenario final : public BWAPI::AIModule {
    ProtoddModule module_;
    std::ofstream result_;
    std::string scenario_;
    std::string lifecycleHistoryAlias_;
    std::string lifecycleHistoryFile_;
    int lifecyclePriorHistoryGames_{};
    bool finished_{};
    bool lifecycleFaultSent_{};
    bool lifecycleResetSent_{};
    bool lifecycleEarlyExitSent_{};
    int failures_{};
    std::vector<std::int64_t> callbackTimesUs_;

    void check(const char* name, const bool passed) {
        result_ << "CHECK," << name << ',' << passed << '\n';
        result_.flush();
        if (!passed) ++failures_;
    }

public:
    void onStart() override {
        std::ifstream scenarioFile("bwapi-data/read/scenario.txt");
        scenarioFile >> scenario_;
        std::error_code ignored;
        std::filesystem::remove("bwapi-data/write/Protodd.log", ignored);
        const auto resultPath = scenario_ == "lifecycle-output-blocked" ||
                                scenario_ == "lifecycle-write-denied"
            ? "bwapi-data/lifecycle-result.csv" : "bwapi-data/write/scenario.csv";
        result_.open(resultPath, std::ios::trunc);
        result_ << "START," << scenario_ << ',' << BWAPI::Broodwar->mapFileName() << '\n';
        if (scenario_ == "fault-end-history" ||
            scenario_ == "lifecycle-truncated-history") {
            std::error_code ignoredLearning;
            std::filesystem::create_directories("bwapi-data/read", ignoredLearning);
            std::ofstream learningMode("bwapi-data/read/Protodd-learning-mode.txt",
                                       std::ios::trunc);
            learningMode << "online\n";
            learningMode.flush();
        }
        if (scenario_ == "lifecycle-truncated-history") {
            const auto enemy = BWAPI::Broodwar->enemy();
            lifecycleHistoryAlias_ = enemy ? enemy->getName() : std::string{};
            lifecycleHistoryFile_ = protodd::OpponentHistory::filename(
                lifecycleHistoryAlias_);
            if (lifecycleHistoryFile_.empty()) {
                ++failures_;
                result_ << "CHECK,history-fixture-alias-is-bounded,0\n";
            } else {
                const auto mapIdentity = std::string(BWAPI::Broodwar->mapName()) + "#" +
                                         BWAPI::Broodwar->mapHash();
                const auto priorOutputPath = std::filesystem::path("bwapi-data/write") /
                                             lifecycleHistoryFile_;
                std::ifstream priorOutput(priorOutputPath, std::ios::binary);
                const std::string priorOutputText(
                    (std::istreambuf_iterator<char>(priorOutput)),
                    std::istreambuf_iterator<char>());
                protodd::OpponentHistory parsedPriorOutput;
                parsedPriorOutput.parse(priorOutputText);
                for (auto raw = 0; raw < static_cast<int>(protodd::OpeningStyle::count); ++raw) {
                    lifecyclePriorHistoryGames_ += parsedPriorOutput.lookup(
                        lifecycleHistoryAlias_, mapIdentity,
                        static_cast<protodd::OpeningStyle>(raw)).games();
                }
                std::ofstream truncatedHistory(
                    std::filesystem::path("bwapi-data/read") / lifecycleHistoryFile_,
                    std::ios::binary | std::ios::trunc);
                truncatedHistory << "# protodd-history-v3\n"
                                    "# outcome,id,opponent,map,style,result\n"
                                    "outcome,deadbeef,unfinished";
                truncatedHistory.flush();
                if (!truncatedHistory) {
                    ++failures_;
                    result_ << "CHECK,truncated-history-fixture-written,0\n";
                }
            }
        }
        if (scenario_ == "fault-callback") {
            module_.configureAuditFaultInjection("callback", "onFrame.runFrame", 1);
        } else if (scenario_ == "fault-lifecycle") {
            module_.configureAuditFaultInjection("callback", "onUnitCreate", 1);
        } else if (scenario_ == "fault-startup") {
            module_.configureAuditFaultInjection("stage", "startup", 1);
        } else if (scenario_ == "fault-diagnostics") {
            module_.configureAuditFaultInjection("phase", "diagnostics", 1);
        } else if (scenario_ == "fault-diagnostics-evacuation") {
            module_.configureAuditFaultInjection("phase", "diagnostics", 3);
        } else if (scenario_ == "fault-phase") {
            module_.configureAuditFaultInjection("phase", "strategy", 3);
        } else if (scenario_ == "fault-observe") {
            module_.configureAuditFaultInjection("phase", "observe", 3);
        } else if (scenario_ == "fault-production") {
            module_.configureAuditFaultInjection("phase", "production-observe", 3);
        } else if (scenario_ == "fault-production-evacuation") {
            module_.configureAuditFaultInjection("phase", "production-observe", 3);
        } else if (scenario_ == "fault-whole-game") {
            module_.configureAuditFaultInjection("phase", "whole-game-observe", 3);
        } else if (scenario_ == "slow-observation" ||
                   scenario_ == "slow-observation-model") {
            module_.configureAuditSlowObservation(10, 33'000);
        } else if (scenario_ != "fault-end-history" &&
                   scenario_ != "lifecycle-truncated-history" &&
                   scenario_ != "lifecycle-reset" &&
                   scenario_ != "lifecycle-early-exit" &&
                   scenario_ != "lifecycle-write-denied" &&
                   scenario_ != "lifecycle-output-blocked" &&
                   scenario_ != "fault-end-reporting") {
            ++failures_;
            result_ << "CHECK,known-fault-scenario,0\n";
        }
        module_.onStart();
        // The audit toggles are applied after start so the test-only adapter
        // state survives the production runtime's normal initialization.
        if (scenario_ == "fault-production" ||
            scenario_ == "fault-production-evacuation")
            module_.configureAuditFaultInjection("phase", "production-observe", 3);
        else if (scenario_ == "fault-whole-game")
            module_.configureAuditFaultInjection("phase", "whole-game-observe", 3);
        result_ << "READY," << BWAPI::Broodwar->isPaused() << ','
                << BWAPI::Broodwar->isInGame() << '\n';
        result_.flush();
    }

    void onFrame() override {
        if (finished_) return;
        if (scenario_ == "fault-lifecycle" && !lifecycleFaultSent_) {
            const auto units = BWAPI::Broodwar->self()->getUnits();
            if (!units.empty()) {
                lifecycleFaultSent_ = true;
                module_.onUnitCreate(*units.begin());
            }
        }
        if (scenario_ == "slow-observation" ||
            scenario_ == "slow-observation-model") {
            const auto callbackStarted = std::chrono::steady_clock::now();
            module_.onFrame();
            callbackTimesUs_.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - callbackStarted).count());
        } else module_.onFrame();
        const auto frame = BWAPI::Broodwar->getFrameCount();
        if (scenario_ == "lifecycle-early-exit" && !lifecycleEarlyExitSent_ &&
            frame >= 24) {
            lifecycleEarlyExitSent_ = true;
            result_ << "EARLY_EXIT_REQUEST," << frame << '\n';
            result_.flush();
            finished_ = true;
            BWAPI::Broodwar->leaveGame();
            return;
        }
        if (scenario_ == "lifecycle-reset" && !lifecycleResetSent_ && frame >= 24) {
            lifecycleResetSent_ = true;
            module_.configureAuditFaultInjection("stage", "startup", 1);
            module_.onEnd(false);
            module_.onStart();
            module_.onFrame();
            std::ifstream restartedLog("bwapi-data/write/Protodd.log", std::ios::binary);
            const std::string restartedText((std::istreambuf_iterator<char>(restartedLog)),
                                            std::istreambuf_iterator<char>());
            const auto firstBoot = restartedText.find("BOOT,");
            const auto secondBoot = firstBoot == std::string::npos ? firstBoot
                : restartedText.find("BOOT,", firstBoot + 1);
            const auto firstManifest = restartedText.find("FEATURE_MANIFEST,");
            const auto secondManifest = firstManifest == std::string::npos ? firstManifest
                : restartedText.find("FEATURE_MANIFEST,", firstManifest + 1);
            const auto firstStart = restartedText.find("START,");
            const auto secondStart = firstStart == std::string::npos ? firstStart
                : restartedText.find("START,", firstStart + 1);
            check("same-module-onEnd-then-onStart-reopens-log",
                secondBoot != std::string::npos && secondManifest != std::string::npos &&
                secondStart != std::string::npos);
            check("same-module-restart-attributes-startup-fault-to-fresh-session",
                countOccurrences(restartedText, "CALLBACK_ERROR,") == 1 &&
                restartedText.find("callback=onStart,phase=startup,error=audit-injected-stage-startup") !=
                    std::string::npos &&
                restartedText.find("LOG_ERROR") == std::string::npos);
            result_ << "RESET_SESSION," << frame << ",boot=" << (secondBoot != std::string::npos)
                    << ",manifest=" << (secondManifest != std::string::npos)
                    << ",start=" << (secondStart != std::string::npos) << '\n';
            result_.flush();
            return;
        }
        // The injected stalls keep FrameBudget reduced for up to 15 seconds.
        // Observe the model beyond that recovery window before requiring a
        // prediction; the ordinary containment cases still finish at 240.
        const auto completionFrame = scenario_ == "slow-observation-model" ? 1200 : 240;
        if (frame < completionFrame) return;

        std::ifstream moduleLog("bwapi-data/write/Protodd.log", std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(moduleLog)),
                               std::istreambuf_iterator<char>());
        const auto callbackFailure = text.find("CALLBACK_ERROR") != std::string::npos &&
            text.find("callback=onFrame.runFrame") != std::string::npos &&
            text.find("audit-injected-callback-onFrame.runFrame") != std::string::npos;
        const auto lifecycleFailure = text.find("CALLBACK_ERROR") != std::string::npos &&
            text.find("callback=onUnitCreate") != std::string::npos &&
            text.find("audit-injected-callback-onUnitCreate") != std::string::npos;
        const auto startupFailure = text.find("CALLBACK_ERROR") != std::string::npos &&
            text.find("callback=onStart") != std::string::npos &&
            text.find("audit-injected-stage-startup") != std::string::npos;
        const auto diagnosticsFailure = text.find("CALLBACK_ERROR") != std::string::npos &&
            text.find("callback=onFrame.runFrame") != std::string::npos &&
            text.find("audit-injected-phase-diagnostics") != std::string::npos;
        const auto phaseCallbackFailure = text.find("CALLBACK_ERROR") != std::string::npos &&
            text.find("callback=onFrame.runFrame") != std::string::npos &&
            text.find("audit-injected-phase-strategy") != std::string::npos;
        const auto diagnosticsFrame = protodd::frame_schedule::latestDueAtOrBefore(
            frame, protodd::frame_schedule::diagnosticsPeriod,
            protodd::frame_schedule::diagnosticsOffset);
        const auto phaseRowPrefix = "PHASE," + std::to_string(diagnosticsFrame) + ',';
        const auto healthRowPrefix = "HEALTH," + std::to_string(diagnosticsFrame) + ',';
        const auto healthContinued = text.find(healthRowPrefix) != std::string::npos;
        if (scenario_ == "fault-end-history" || scenario_ == "fault-end-reporting") {
            finished_ = true;
            BWAPI::Broodwar->leaveGame();
            return;
        }
        if (scenario_ == "lifecycle-output-blocked") {
            check("write-path-is-blocked-file",
                std::filesystem::is_regular_file("bwapi-data/write"));
            check("module-log-was-not-created",
                !std::filesystem::exists("bwapi-data/write/Protodd.log"));
            check("module-ran-while-output-was-unavailable",
                frame >= 240 && BWAPI::Broodwar->isInGame());
        } else if (scenario_ == "lifecycle-write-denied") {
            check("write-path-remained-a-directory",
                std::filesystem::is_directory("bwapi-data/write"));
            check("module-log-was-not-created-under-denied-acl",
                !std::filesystem::exists("bwapi-data/write/Protodd.log"));
            check("module-ran-while-write-access-was-denied",
                frame >= 240 && BWAPI::Broodwar->isInGame());
        } else if (scenario_ == "lifecycle-truncated-history") {
            check("truncated-history-does-not-stop-startup-or-frame-work",
                text.find("LEARNING,mode=online") != std::string::npos &&
                text.find("START,") != std::string::npos && healthContinued &&
                text.find("CALLBACK_ERROR") == std::string::npos);
        } else if (scenario_ == "lifecycle-reset") {
            const auto firstStart = text.find("START,");
            const auto secondStart = firstStart == std::string::npos ? firstStart
                : text.find("START,", firstStart + 1);
            const auto secondHealth = secondStart == std::string::npos
                ? std::string::npos : text.find("HEALTH,", secondStart);
            check("lifecycle-reset-second-session-started", lifecycleResetSent_ &&
                countOccurrences(text, "BOOT,") == 2 &&
                countOccurrences(text, "FEATURE_MANIFEST,") == 2 &&
                countOccurrences(text, "START,") == 2);
            check("lifecycle-reset-second-session-produced-diagnostics",
                secondHealth != std::string::npos);
            check("lifecycle-reset-first-session-ended",
                countOccurrences(text, "SUMMARY,won=") == 1 &&
                countOccurrences(text, "END,loss,") == 1);
        } else if (scenario_ == "fault-callback") {
            check("production-callback-boundary-caught-injected-error", callbackFailure);
            check("callback-work-continued-after-injection", healthContinued);
            check("runtime-reported-one-caught-error",
                healthContinued && text.find(",caughtErrors=1,") != std::string::npos);
        } else if (scenario_ == "fault-lifecycle") {
            check("production-lifecycle-callback-boundary-caught-injected-error",
                lifecycleFaultSent_ && lifecycleFailure);
            check("lifecycle-callback-work-continued-after-injection", healthContinued);
            check("lifecycle-error-reached-health-counter",
                text.find(",caughtErrors=1,") != std::string::npos);
        } else if (scenario_ == "fault-startup") {
            check("startup-stage-error-was-caught-and-identified", startupFailure);
            check("frame-work-continued-after-startup-error", healthContinued);
            check("startup-error-reached-health-counter",
                healthContinued && text.find(",caughtErrors=1,") != std::string::npos);
        } else if (scenario_ == "fault-diagnostics") {
            check("diagnostics-error-was-caught-and-identified", diagnosticsFailure);
            check("frame-work-continued-after-diagnostics-error", healthContinued);
            check("diagnostics-error-reached-health-counter",
                healthContinued && text.find(",caughtErrors=1,") != std::string::npos);
        } else if (scenario_ == "fault-diagnostics-evacuation") {
            const auto disabled = text.find("PHASE_DISABLED") != std::string::npos &&
                text.find("phase=diagnostics") != std::string::npos &&
                text.find("consecutive_failures=3") != std::string::npos &&
                text.find("fallback=skip-repeatedly-failing-phase") != std::string::npos;
            const auto diagnosticsStoppedAtThree = text.find(
                "error=audit-injected-phase-diagnostics,total=2") != std::string::npos;
            const auto disabledAt = text.find("PHASE_DISABLED,frame=");
            const auto emergencyMoveAt = disabledAt == std::string::npos
                ? std::string::npos
                : text.find(",worker-evacuate,Move,issued,accepted,", disabledAt);
            check("diagnostics-disabled-after-three-phase-failures", disabled);
            check("diagnostics-phase-stopped-at-three-failures", diagnosticsStoppedAtThree);
            check("emergency-worker-escape-continued-after-logger-phase-failure",
                disabledAt != std::string::npos && emergencyMoveAt != std::string::npos &&
                disabledAt < emergencyMoveAt);
        } else if (scenario_ == "fault-phase") {
            const auto disabled = text.find("PHASE_DISABLED") != std::string::npos &&
                text.find("phase=strategy") != std::string::npos &&
                text.find("consecutive_failures=3") != std::string::npos &&
                text.find("fallback=skip-repeatedly-failing-phase") != std::string::npos;
            const auto strategyStoppedAtThree =
                text.find(phaseRowPrefix + "strategy,3,") != std::string::npos;
            const auto macroContinued = text.find(phaseRowPrefix + "macro,") != std::string::npos;
            check("production-phase-failure-reached-circuit-breaker", disabled);
            check("disabled-strategy-phase-stopped-after-third-failure",
                strategyStoppedAtThree);
            check("independent-macro-phase-continued", macroContinued && healthContinued);
            check("phase-errors-stayed-inside-callback-boundary", phaseCallbackFailure);
        } else if (scenario_ == "fault-observe") {
            const auto disabled = text.find("PHASE_DISABLED") != std::string::npos &&
                text.find("phase=observe") != std::string::npos &&
                text.find("consecutive_failures=3") != std::string::npos &&
                text.find("fallback=suspend-frame-work-without-fresh-observation") !=
                    std::string::npos;
            const auto observationStoppedAtThree =
                text.find("consecutive_failures=3") != std::string::npos &&
                text.find("phase=observe") != std::string::npos &&
                text.find("audit-injected-phase-observe") != std::string::npos;
            const auto frameWorkSuspended = text.find(phaseRowPrefix + "macro,") ==
                    std::string::npos && !healthContinued;
            check("observation-failure-disabled-frame-work", disabled);
            check("observation-stopped-after-third-failure", observationStoppedAtThree);
            check("no-stale-snapshot-work-ran-after-observation-failed",
                frameWorkSuspended);
        } else if (scenario_ == "fault-production" ||
                   scenario_ == "fault-production-evacuation") {
            const auto disabled = text.find("PHASE_DISABLED") != std::string::npos &&
                text.find("phase=production-observe") != std::string::npos &&
                text.find("consecutive_failures=3") != std::string::npos &&
                text.find("fallback=disable-production-adapter") != std::string::npos;
            const auto phaseStoppedAtThree =
                text.find(phaseRowPrefix + "production-observe,3,") != std::string::npos;
            const auto nativeWorkContinued = text.find(phaseRowPrefix + "macro,") !=
                std::string::npos && healthContinued;
            check("production-adapter-disabled-after-three-phase-failures", disabled);
            check("production-observation-stopped-at-three-failures", phaseStoppedAtThree);
            check("native-macro-and-health-work-continued", nativeWorkContinued);
            if (scenario_ == "fault-production-evacuation") {
                const auto disabledAt = text.find(
                    "PHASE_DISABLED,frame=", text.find("phase=production-observe"));
                const auto emergencyMoveAt = text.find(
                    ",worker-evacuate,Move,issued,accepted,");
                check("emergency-worker-escape-continued-after-adapter-disable",
                    disabledAt != std::string::npos && emergencyMoveAt != std::string::npos &&
                    disabledAt < emergencyMoveAt);
            }
        } else if (scenario_ == "fault-whole-game") {
            const auto disabled = text.find("PHASE_DISABLED") != std::string::npos &&
                text.find("phase=whole-game-observe") != std::string::npos &&
                text.find("consecutive_failures=3") != std::string::npos &&
                text.find("fallback=disable-whole-game-and-resume-native-controller") !=
                    std::string::npos;
            const auto phaseStoppedAtThree =
                text.find(phaseRowPrefix + "whole-game-observe,3,") != std::string::npos;
            const auto nativeWorkContinued = text.find(phaseRowPrefix + "macro,") !=
                std::string::npos && healthContinued;
            check("whole-game-disabled-after-three-phase-failures", disabled);
            check("whole-game-observation-stopped-at-three-failures", phaseStoppedAtThree);
            check("native-controller-work-continued-after-fallback", nativeWorkContinued);
        } else if (scenario_ == "slow-observation" ||
                   scenario_ == "slow-observation-model") {
            const auto observeRow = rowStartingWith(text, phaseRowPrefix + "observe,");
            const auto influenceRow = rowStartingWith(text, phaseRowPrefix + "influence,");
            const auto inferenceRow = rowStartingWith(text, phaseRowPrefix + "inference,");
            const auto combatRow = rowStartingWith(text, phaseRowPrefix + "combat,");
            const auto startupSafetyRow = rowStartingWith(text, "AUDIT_SLOW_OBSERVATION,0,");
            const auto delayedObservation = csvUnsignedColumn(observeRow, 5) >= 30'000;
            const auto optionalDeferred = csvUnsignedColumn(influenceRow, 6) > 0 &&
                csvUnsignedColumn(inferenceRow, 6) > 0;
            check("slow-observation-was-measured-inside-observe-phase", delayedObservation);
            check("optional-phases-were-deferred-after-slow-observation", optionalDeferred);
            check("combat-remained-executable-on-the-over-budget-callback",
                startupSafetyRow.find(",combatRan=1,") != std::string::npos &&
                startupSafetyRow.find(",influenceDeferred=1,") != std::string::npos &&
                startupSafetyRow.find(",inferenceDeferred=1") != std::string::npos);
            check("combat-remained-scheduled-through-the-load-run",
                csvUnsignedColumn(combatRow, 3) > 0);

            if (scenario_ == "slow-observation-model") {
                const auto modelLoaded = text.find(
                    "MODEL,status=shadow,schema=protodd-macro-v2,") != std::string::npos;
                const auto modelPredicted = text.find("MODEL_SHADOW,") != std::string::npos;
                const auto modelDisabled = text.find(
                    "MODEL,status=disabled,reason=inference-budget") != std::string::npos;
                check("learned-model-shadow-loaded", modelLoaded);
                check("learned-model-produced-shadow-inference", modelPredicted);
                const auto skippedAt = text.find("MODEL_SHADOW_SKIPPED,");
                const auto predictedAt = text.find("MODEL_SHADOW,");
                check("learned-model-deferred-under-load-then-resumed",
                    skippedAt != std::string::npos && predictedAt != std::string::npos &&
                    predictedAt > skippedAt);
                check("learned-model-stayed-within-inference-budget",
                    modelLoaded && !modelDisabled);
            }

            auto sortedTimes = callbackTimesUs_;
            std::sort(sortedTimes.begin(), sortedTimes.end());
            const auto percentile = [&sortedTimes](const double p) {
                if (sortedTimes.empty()) return std::int64_t{0};
                const auto index = static_cast<std::size_t>(
                    p * static_cast<double>(sortedTimes.size() - 1));
                return sortedTimes[index];
            };
            result_ << "CALLBACK_SUMMARY," << callbackTimesUs_.size() << ','
                    << percentile(0.50) << ',' << percentile(0.95) << ','
                    << percentile(0.99) << ','
                    << (sortedTimes.empty() ? 0 : sortedTimes.back()) << '\n';
            check("slow-observation-run-recorded-full-callback-distribution",
                callbackTimesUs_.size() >= 200);
            for (std::size_t index = 0; index < callbackTimesUs_.size(); ++index)
                result_ << "CALLBACK_US," << index << ',' << callbackTimesUs_[index] << '\n';
        }
        const auto includeModelTrace = scenario_ == "slow-observation-model";
        for (const auto& line : [&text, &phaseRowPrefix, &healthRowPrefix,
                                 includeModelTrace] {
                 std::vector<std::string> selected;
                 std::size_t start = 0;
                 while (start < text.size()) {
                     const auto end = text.find('\n', start);
                     const auto row = text.substr(start, end == std::string::npos
                         ? std::string::npos : end - start);
                     if (row.starts_with("CALLBACK_ERROR") || row.starts_with("PHASE_DISABLED") ||
                         row.starts_with("AUDIT_SLOW_OBSERVATION,") ||
                         row.starts_with(healthRowPrefix) ||
                        row.starts_with(phaseRowPrefix) ||
                        (includeModelTrace && (row.starts_with("MODEL,status=") ||
                         row.starts_with("MODEL_SHADOW,") ||
                         row.starts_with("MODEL_SHADOW_SKIPPED,")))) selected.push_back(row);
                     if (end == std::string::npos) break;
                     start = end + 1;
                 }
                 return selected;
             }()) {
            result_ << "MODULE," << line << '\n';
        }
        check("engine-fault-postcondition", failures_ == 0);
        result_ << "DONE," << frame << ',' << failures_ << '\n';
        result_.flush();
        finished_ = true;
        BWAPI::Broodwar->leaveGame();
    }

    void onEnd(const bool winner) override {
        if (scenario_ == "fault-end-history")
            module_.configureAuditFaultInjection("stage", "history-output", 1);
        else if (scenario_ == "fault-end-reporting")
            module_.configureAuditFaultInjection("stage", "reporting", 1);
        module_.onEnd(winner);
        if (scenario_ == "lifecycle-early-exit") {
            std::ifstream moduleLog("bwapi-data/write/Protodd.log", std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(moduleLog)),
                                   std::istreambuf_iterator<char>());
            const auto frame = BWAPI::Broodwar->getFrameCount();
            check("engine-delivered-early-end-to-production-module",
                lifecycleEarlyExitSent_ && frame < 240 &&
                countOccurrences(text, "SUMMARY,won=") == 1 &&
                countOccurrences(text, "END,") == 1 &&
                text.find("CALLBACK_ERROR") == std::string::npos);
            result_ << "ENGINE_EARLY_END," << frame << ',' << winner << '\n';
            result_ << "DONE," << frame << ',' << failures_ << '\n';
            result_.flush();
        }
        if (scenario_ == "lifecycle-truncated-history") {
            const auto inputPath = std::filesystem::path("bwapi-data/read") /
                                   lifecycleHistoryFile_;
            const auto outputPath = std::filesystem::path("bwapi-data/write") /
                                    lifecycleHistoryFile_;
            std::ifstream truncatedInput(inputPath, std::ios::binary);
            const std::string inputText((std::istreambuf_iterator<char>(truncatedInput)),
                                        std::istreambuf_iterator<char>());
            std::ifstream recoveredOutput(outputPath, std::ios::binary);
            const std::string outputText((std::istreambuf_iterator<char>(recoveredOutput)),
                                         std::istreambuf_iterator<char>());
            protodd::OpponentHistory parsedOutput;
            parsedOutput.parse(outputText);
            const auto mapIdentity = std::string(BWAPI::Broodwar->mapName()) + "#" +
                                     BWAPI::Broodwar->mapHash();
            auto recoveredGames = 0;
            for (auto raw = 0; raw < static_cast<int>(protodd::OpeningStyle::count); ++raw) {
                recoveredGames += parsedOutput.lookup(
                    lifecycleHistoryAlias_, mapIdentity,
                    static_cast<protodd::OpeningStyle>(raw)).games();
            }
            check("truncated-history-input-was-present",
                inputText.starts_with("# protodd-history-v3\n") &&
                inputText.ends_with("outcome,deadbeef,unfinished"));
            check("end-of-game-wrote-valid-snapshot-with-current-outcome",
                outputText.starts_with("# protodd-history-v3\n") &&
                outputText.find("unfinished") == std::string::npos &&
                parsedOutput.serialize() == outputText &&
                recoveredGames == lifecyclePriorHistoryGames_ + 1);
            result_ << "HISTORY_RECOVERY," << lifecycleHistoryFile_
                    << ",input_bytes=" << inputText.size()
                    << ",output_bytes=" << outputText.size()
                    << ",prior_games=" << lifecyclePriorHistoryGames_
                    << ",current_games=" << recoveredGames << '\n';
            result_.flush();
        }
        if (scenario_ == "lifecycle-reset") {
            std::ifstream moduleLog("bwapi-data/write/Protodd.log", std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(moduleLog)),
                                   std::istreambuf_iterator<char>());
            check("lifecycle-reset-both-session-summaries-ended",
                countOccurrences(text, "SUMMARY,won=") == 2 &&
                countOccurrences(text, "END,") == 2);
        }
        if (scenario_ == "fault-end-history" || scenario_ == "fault-end-reporting") {
            std::ifstream moduleLog("bwapi-data/write/Protodd.log", std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(moduleLog)),
                                   std::istreambuf_iterator<char>());
            const auto target = scenario_ == "fault-end-history"
                ? "audit-injected-stage-history-output" : "audit-injected-stage-reporting";
            check("shutdown-stage-error-was-caught-and-identified",
                text.find("CALLBACK_ERROR") != std::string::npos &&
                text.find("callback=onEnd") != std::string::npos &&
                text.find("phase=shutdown") != std::string::npos &&
                text.find(target) != std::string::npos);
            check("outer-engine-callback-continued-after-shutdown-error", true);
            result_ << "DONE," << BWAPI::Broodwar->getFrameCount() << ',' << failures_ << '\n';
            result_.flush();
        }
        if (result_) {
            if (scenario_ == "slow-observation") {
                std::ifstream moduleLog("bwapi-data/write/Protodd.log", std::ios::binary);
                const std::string text((std::istreambuf_iterator<char>(moduleLog)),
                                       std::istreambuf_iterator<char>());
                const auto performance = rowStartingWith(text, "PERF_SUMMARY,");
                if (!performance.empty()) result_ << "MODULE," << performance << '\n';
            }
            result_ << "END," << BWAPI::Broodwar->getFrameCount() << ',' << winner << '\n';
            result_.flush();
        }
    }

    void onSendText(std::string text) override { module_.onSendText(std::move(text)); }
    void onUnitDiscover(BWAPI::Unit unit) override { module_.onUnitDiscover(unit); }
    void onUnitShow(BWAPI::Unit unit) override { module_.onUnitShow(unit); }
    void onUnitDestroy(BWAPI::Unit unit) override { module_.onUnitDestroy(unit); }
    void onUnitMorph(BWAPI::Unit unit) override { module_.onUnitMorph(unit); }
    void onUnitRenegade(BWAPI::Unit unit) override { module_.onUnitRenegade(unit); }
    void onUnitCreate(BWAPI::Unit unit) override { module_.onUnitCreate(unit); }
    void onUnitComplete(BWAPI::Unit unit) override { module_.onUnitComplete(unit); }
};

extern "C" __declspec(dllexport) void gameInit(BWAPI::Game* game) {
    BWAPI::BroodwarPtr = game;
}
extern "C" __declspec(dllexport) BWAPI::AIModule* newAIModule() {
    return new FaultScenario;
}
BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
