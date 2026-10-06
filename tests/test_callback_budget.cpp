#include "protodd/CallbackBudget.hpp"
#include "protodd/Diagnostics.hpp"

#include <iostream>

namespace {

bool check(const bool condition, const char* message) {
    if (condition) return true;
    std::cerr << "FAIL: " << message << '\n';
    return false;
}

}  // namespace

int main() {
    const protodd::CallbackBudget budget;
    bool passed = true;
    passed &= check(budget.allowsOptionalWork(0, 8'000),
                    "optional work starts with a full callback budget");
    passed &= check(budget.allowsOptionalWork(26'000, 6'000),
                    "estimated work fits while preserving the emergency reserve");
    passed &= check(!budget.allowsOptionalWork(27'000, 6'000),
                    "optional work is deferred when its estimate consumes safety reserve");
    passed &= check(!budget.allowsOptionalWork(35'000, 0),
                    "slow observation prevents optional work from starting");
    passed &= check(!budget.allowsOptionalWork(-1, 1'000) &&
                        !budget.allowsOptionalWork(1'000, -1),
                    "invalid elapsed or cost measurements fail closed");
    passed &= check(protodd::CallbackBudget::callbackLimitUs -
                        protodd::CallbackBudget::optionalWorkLimitUs ==
                        protodd::CallbackBudget::emergencyReserveUs,
                    "optional phases preserve the documented safety reserve");
    protodd::PhaseTiming timing;
    passed &= check(timing.estimatedUs(4'000) == 4'000,
                    "first phase execution uses its conservative configured estimate");
    timing.record(10'000);
    passed &= check(timing.estimatedUs(4'000) == 15'000 &&
                        !budget.allowsOptionalWork(18'000, timing.estimatedUs(4'000)),
                    "recent phase timing raises estimates before an over-budget start");
    timing.deferForBudget();
    passed &= check(timing.budgetDeferred == 1,
                    "optional deferrals are observable in phase diagnostics");
    return passed ? 0 : 1;
}
