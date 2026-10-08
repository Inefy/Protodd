#include "protodd/ResourceIncome.hpp"

#include <iostream>

int main() {
    using namespace protodd;
    int failures = 0;
    const auto check = [&failures](const bool condition, const char* message) {
        if (condition) return;
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    };

    ResourceIncomeTracker tracker;
    check(!tracker.update(0, 0, 0).valid(),
          "income is unknown before the rolling window has enough samples");
    ResourceIncomeEstimate estimate;
    for (Frame second = 1; second <= 30; ++second) {
        estimate = tracker.update(second * 24, second * 5, second * 2);
    }
    check(estimate.valid() && estimate.mineralsPerMinute == 300 &&
              estimate.gasPerMinute == 120,
          "rolling income rate matches the cumulative engine counters");

    for (Frame second = 31; second <= 42; ++second)
        estimate = tracker.update(second * 24, 150, second * 2);
    check(estimate.valid() && estimate.mineralsPerMinute == 180 &&
              estimate.gasPerMinute == 120,
          "the rolling window smooths a recent mineral-income drop");
    for (Frame second = 43; second <= 60; ++second)
        estimate = tracker.update(second * 24, 150, second * 2);
    check(estimate.valid() && estimate.mineralsPerMinute == 0 &&
              estimate.gasPerMinute == 120,
          "the rolling window forgets mineral income after old samples expire");

    estimate = tracker.update(24, 1, 1);
    check(!estimate.valid(),
          "a new game or reset resource counter clears the old income window");
    tracker.reset();
    check(!tracker.update(100, 0, 0).valid(),
          "explicit game reset returns income to the unknown state");

    ResourceIncomeTracker warmup;
    static_cast<void>(warmup.update(0, 0, 0));
    for (Frame second = 1; second <= 11; ++second)
        estimate = warmup.update(second * 24, second * 5, second * 2);
    check(!estimate.valid(), "short windows do not overreact to individual worker trips");
    estimate = warmup.update(12 * 24, 60, 24);
    check(estimate.valid() && estimate.mineralsPerMinute == 300 &&
              estimate.gasPerMinute == 120,
          "the estimator becomes usable after twelve seconds of observations");
    return failures == 0 ? 0 : 1;
}
