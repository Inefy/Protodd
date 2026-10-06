#include "protodd/PhaseFailurePolicy.hpp"

#include <stdexcept>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main() {
    protodd::PhaseFailurePolicy policy(3);

    auto update = policy.recordFailure("strategy");
    require(update.consecutiveFailures == 1 && !update.disabled && !update.newlyDisabled,
            "first failure disabled the phase too early");
    policy.recordSuccess("strategy");
    require(policy.consecutiveFailures("strategy") == 0,
            "success did not reset consecutive failures");

    update = policy.recordFailure("strategy");
    require(update.consecutiveFailures == 1 && !update.disabled,
            "failure count did not restart after success");
    update = policy.recordFailure("strategy");
    require(update.consecutiveFailures == 2 && !update.disabled,
            "second consecutive failure disabled the phase too early");
    update = policy.recordFailure("strategy");
    require(update.consecutiveFailures == 3 && update.disabled && update.newlyDisabled,
            "third consecutive failure did not trip the fallback");
    require(policy.disabled("strategy"), "disabled phase was not latched");

    update = policy.recordFailure("diagnostics");
    require(update.consecutiveFailures == 1 && !update.disabled,
            "one phase's errors contaminated another phase");
    update = policy.recordFailure("strategy");
    require(update.disabled && !update.newlyDisabled,
            "latched phase reported a second disable transition");

    protodd::PhaseFailurePolicy immediate(0);
    update = immediate.recordFailure("observe");
    require(update.disabled && update.newlyDisabled,
            "zero failure limit was not normalized to fail closed");
    return 0;
}
