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
    BuildCancellation handedOff;
    check(!handedOff.handoff(200, false) && !handedOff.awaiting(),
          "a rejected or deferred replacement cannot release construction authority");
    check(handedOff.handoff(201, true) && handedOff.awaiting() && handedOff.handoffPending(),
          "a confirmed replacement closes construction authority while retaining its reservation");
    check(!handedOff.requestDue(201) && !handedOff.retryDue(10000),
          "handoff cannot retry Stop and cancel the replacement owner's emergency movement");
    check(!handedOff.acknowledged(false, 201) && handedOff.awaiting(),
          "same-frame API acceptance is not a native acknowledgment");
    check(!handedOff.acknowledged(true, 202) && handedOff.awaiting(),
          "a later observation must still retain an actionable old build order");
    check(!handedOff.handoff(203, true),
          "repeated confirmation cannot move the original handoff acknowledgment boundary");
    check(handedOff.acknowledged(false, 204) && !handedOff.awaiting() &&
              !handedOff.handoffPending() && handedOff.requestDue(204),
          "a later non-actionable observation releases the old lease and resets cancellation state");
    BuildCancellation stopThenHandoff;
    check(stopThenHandoff.request(300, 6, true) && stopThenHandoff.handoff(301, true) &&
              !stopThenHandoff.retryDue(10000) && stopThenHandoff.acknowledged(false, 302),
          "emergency handoff supersedes a pending Stop without issuing another Stop");
    return passed ? 0 : 1;
}
