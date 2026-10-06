#include "protodd/FrameSchedule.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void check(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

}  // namespace

int main() {
    using namespace protodd::frame_schedule;
    constexpr protodd::Frame window = 2400;
    std::array<int, 9> calls{};
    auto oldCollisionPeak = 0;
    auto newCollisionPeak = 0;
    for (protodd::Frame frame = 0; frame < window; ++frame) {
        const auto combat = dueEvery(frame, 6);
        const auto influence = influenceDue(frame, 1);
        const auto inference = inferenceDue(frame, 1);
        const auto periodicInference = dueEvery(frame, scaledPeriod(inferencePeriod, 1), inferenceOffset);
        const auto strategy = strategyDue(frame, false);
        const auto macro = macroCadenceDue(frame) || strategy;
        const auto workers = workersDue(frame);
        const auto scouting = scoutingDue(frame, 1);
        const auto maintenance = maintenanceDue(frame);
        const auto diagnostics = diagnosticsDue(frame);
        const auto stateLog = stateLogDue(frame);
        calls[0] += influence;
        calls[1] += inference;
        calls[2] += strategy;
        calls[3] += macro;
        calls[4] += workers;
        calls[5] += scouting;
        calls[6] += maintenance;
        calls[7] += diagnostics;
        calls[8] += stateLog;

        const auto oldStrategy = dueEvery(frame, 24);
        const auto oldMacro = dueEvery(frame, 3, 1) || oldStrategy;
        const auto oldDiagnostics = dueEvery(frame, 24);
        const auto oldCount = static_cast<int>(combat) + dueEvery(frame, 8) +
            dueEvery(frame, 12) + oldStrategy + oldMacro + workers + scouting +
            maintenance + oldDiagnostics + dueEvery(frame, 120);
        const auto newCount = static_cast<int>(combat) + influence + periodicInference +
            strategy + macro + workers + scouting + maintenance + diagnostics + stateLog;
        if (oldCount > oldCollisionPeak) oldCollisionPeak = oldCount;
        if (newCount > newCollisionPeak) newCollisionPeak = newCount;
    }

    check(calls == std::array<int, 9>{300, 201, 100, 800, 200, 100, 100, 100, 20},
          "sustained phase frequencies hold, with inference also updated at startup");
    check(oldCollisionPeak == 7 && newCollisionPeak == 2,
          "explicit offsets separate steady periodic work from the combat callback cluster");
    const auto oldStartupCount = static_cast<int>(dueEvery(0, 6)) + dueEvery(0, 8) +
        dueEvery(0, 12) + dueEvery(0, 24) +
        (dueEvery(0, 3, 1) || dueEvery(0, 24)) + dueEvery(0, 24) + dueEvery(0, 120);
    const auto startupStrategy = strategyDue(0, true);
    const auto newStartupCount = static_cast<int>(dueEvery(0, 6)) + influenceDue(0, 1) +
        inferenceDue(0, 1) + startupStrategy +
        (macroCadenceDue(0) || startupStrategy);
    check(oldStartupCount == 7 && newStartupCount == 5,
          "startup retains immediate opponent inference while cutting initial phase overlap");
    check(inferenceDue(0, 1) && !inferenceDue(1, 1) && inferenceDue(8, 1),
          "opponent inference runs immediately and then joins its staggered cadence");
    check(!strategyDue(24, false) && strategyDue(22, false),
          "strategy refresh uses its measured noncombat offset");
    check(workersDue(2) && !workersDue(12),
          "worker scheduling retains its exact twelve-frame frequency");
    check(latestDueAtOrBefore(240, diagnosticsPeriod, diagnosticsOffset) == 236 &&
              latestDueAtOrBefore(19, diagnosticsPeriod, diagnosticsOffset) == -1,
          "diagnostic consumers locate the latest scheduled sample without assuming frame zero");
    check(phaseSummaryDue(236) && !phaseSummaryDue(240) && phaseSummaryDue(476),
          "phase summaries follow the last diagnostics tick before each 240-frame boundary");

    int reducedInfluence = 0;
    int reducedInference = 0;
    int reducedScouting = 0;
    for (protodd::Frame frame = 0; frame < window; ++frame) {
        reducedInfluence += influenceDue(frame, 2);
        reducedInference += inferenceDue(frame, 2);
        reducedScouting += scoutingDue(frame, 2);
    }
    check(reducedInfluence == 150 && reducedInference == 101 && reducedScouting == 50,
          "reduced-load cadence scales sustained phase frequencies and keeps startup inference");

    if (failures != 0) return EXIT_FAILURE;
    std::cout << "Periodic phase schedule tests passed\n";
    return EXIT_SUCCESS;
}
