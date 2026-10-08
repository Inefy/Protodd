#include "protodd/CommandEffectFeedback.hpp"
#include "protodd/ProductionEffectFeedback.hpp"

#include <iostream>

int main() {
    using protodd::loadCommandStillConfirmed;
    int failures = 0;
    const auto check = [&failures](const bool condition, const char* message) {
        if (condition) return;
        ++failures;
        std::cerr << message << '\n';
    };

    check(loadCommandStillConfirmed(false, true, 132, 100, 20),
          "pickup order should remain confirmed through latency window");
    check(!loadCommandStillConfirmed(false, true, 133, 100, 20),
          "stale pickup order should become retryable after latency window");
    check(!loadCommandStillConfirmed(false, false, 105, 100, 20),
          "unrelated engine order must not acknowledge pickup");
    check(!loadCommandStillConfirmed(false, true, 99, 100, 20),
          "future command timestamp must not acknowledge pickup");
    check(!loadCommandStillConfirmed(false, true, 1, -1, 20),
          "missing command timestamp must not acknowledge pickup");
    check(loadCommandStillConfirmed(true, false, 1000, 100, 20),
          "observed passenger load must remain confirmed after the order expires");
    check(protodd::loadEffectAcknowledgmentWindow(-4) == 12,
          "invalid negative latency must use the minimum acknowledgment window");
    check(protodd::loadEffectAcknowledgmentWindow(2147483647) == 2147483647,
          "latency window must saturate instead of overflowing");
    check(protodd::commandEffectObservationWindow(protodd::CommandType::move, 6) == 24,
          "move effect timeout should follow bounded movement suppression");
    check(protodd::commandEffectObservationWindow(protodd::CommandType::attackUnit, 6) == 18,
          "attack effect timeout should retain the firing window");
    check(protodd::commandEffectObservationWindow(protodd::CommandType::load, 6) == 18,
          "load effect timeout should follow its latency acknowledgment window");
    check(protodd::trainingEffectObserved(0, 0, false, true, true),
          "matching newly active training should acknowledge production");
    check(protodd::trainingEffectObserved(1, 2, true, true, false),
          "new waiting queue entry should acknowledge production");
    check(!protodd::trainingEffectObserved(0, 0, true, true, true),
          "preexisting same-type training must not acknowledge a new command");
    check(!protodd::trainingEffectObserved(0, 0, false, true, false),
          "different active training type must not acknowledge production");
    return failures == 0 ? 0 : 1;
}
