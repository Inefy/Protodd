#include "protodd/CommandBudget.hpp"

#include <iostream>

int main() {
    using protodd::FrameCommandBudget;
    int failures = 0;
    const auto check = [&failures](const bool condition, const char* message) {
        if (condition) return;
        ++failures;
        std::cerr << message << '\n';
    };

    FrameCommandBudget budget;
    check(budget.consume(false), "calls outside a managed frame remain compatible");
    budget.beginFrame(10, 10, 3);
    for (int i = 0; i < 7; ++i)
        check(budget.consume(false), "direct commands can use only the unreserved share");
    check(!budget.consume(false), "direct commands cannot consume the urgent reserve");
    check(budget.remaining() == 3 && budget.directRemaining() == 0,
          "reserved global capacity remains available to bus dispatch");
    for (int i = 0; i < 3; ++i)
        check(budget.consume(true), "bus commands can consume the reserved global slots");
    check(!budget.consume(true) && budget.remaining() == 0,
          "all issuance paths stop at the same hard frame limit");

    budget.beginFrame(11, 4, 1);
    check(budget.frame() == 11 && budget.limit() == 4 && budget.used() == 0,
          "beginFrame resets the one global allowance");
    for (int i = 0; i < 4; ++i)
        check(budget.consume(true), "bus-only work can use an otherwise idle frame budget");
    check(budget.remaining() == 0, "bus-only issuance also has a hard cap");
    check(protodd::defaultUrgentCommandReserve(96) == 8 &&
          protodd::defaultUrgentCommandReserve(40) == 5 &&
          protodd::defaultUrgentCommandReserve(0) == 0,
          "urgent reserve scales with the active command limit");
    return failures == 0 ? 0 : 1;
}
