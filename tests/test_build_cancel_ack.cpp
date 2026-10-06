#include "protodd/BuildCancellation.hpp"

#include <iostream>

int main() {
    using protodd::BuildCancellation;
    bool passed = true;
    const auto check = [&passed](bool condition, const char* message) {
        if (!condition) { std::cerr << "FAIL: " << message << '\n'; passed = false; }
    };
    BuildCancellation cancellation;
    check(!cancellation.request(100, 6, false) && !cancellation.awaiting(),
          "a rejected Stop does not release or await the lease");
    check(!cancellation.requestDue(111) && cancellation.requestDue(112),
          "a rejected Stop waits through the command retry interval");
    check(cancellation.request(112, 6, true) && cancellation.awaiting(),
          "an accepted Stop enters the awaiting-acknowledgment state");
    check(!cancellation.retryDue(123), "the old build order remains leased during command latency");
    check(!cancellation.acknowledged(true) && cancellation.awaiting(),
          "an actionable old order cannot be acknowledged by API acceptance alone");
    check(cancellation.retryDue(124), "an unacknowledged Stop can be retried after its latency window");
    check(!cancellation.request(124, 6, false) && cancellation.awaiting(),
          "a rejected retry leaves the original cancellation pending");
    check(!cancellation.retryDue(135) && cancellation.retryDue(136),
          "a rejected retry starts a fresh latency-bounded interval");
    check(cancellation.request(136, 6, true), "an accepted retry refreshes the acknowledgment deadline");
    check(!cancellation.acknowledged(true) && cancellation.awaiting(),
          "a replacement construction remains blocked while the old command is actionable");
    check(cancellation.acknowledged(false) && !cancellation.awaiting(),
          "an interrupted or stopped builder releases ownership only after the order clears");
    return passed ? 0 : 1;
}
