#include "protodd/WholeGameActionPolicy.hpp"

#include <iostream>

int main() {
    using protodd::wholeGameActorEligible;
    using protodd::wholeGameCancelBuildAction;
    const auto check = [](bool condition, const char* message) {
        if (!condition) std::cerr << "FAIL: " << message << '\n';
        return condition;
    };
    bool passed = true;
    passed &= check(wholeGameActorEligible(wholeGameCancelBuildAction, 0, false, true, true),
                    "an unfinished construction can be cancelled");
    passed &= check(!wholeGameActorEligible(1, 2, false, false, false),
                    "an unfinished actor cannot move");
    passed &= check(!wholeGameActorEligible(13, 0, false, false, false),
                    "an unfinished actor cannot train");
    passed &= check(!wholeGameActorEligible(wholeGameCancelBuildAction, 0, false, false, true),
                    "a worker cannot issue building construction cancellation");
    passed &= check(!wholeGameActorEligible(wholeGameCancelBuildAction, 0, false, true, false),
                    "a completed or idle building is not a cancellable construction");
    passed &= check(!wholeGameActorEligible(wholeGameCancelBuildAction, 1, false, true, true),
                    "cancellation does not accept an unrelated target mode");
    passed &= check(wholeGameActorEligible(1, 2, true, false, false),
                    "completed units retain ordinary action eligibility");
    passed &= check(!protodd::wholeGameActionAuthorityAllowed(15, false) &&
                        !protodd::wholeGameActionAuthorityAllowed(16, false) &&
                        !protodd::wholeGameActionAuthorityAllowed(20, false) &&
                        !protodd::wholeGameActionAuthorityAllowed(21, false),
                    "learned research and upgrade authority is off in the default profile");
    passed &= check(!protodd::wholeGameActionAuthorityAllowed(15, false) &&
                        protodd::wholeGameActionAuthorityAllowed(15, true) &&
                        protodd::wholeGameActionAuthorityAllowed(1, false),
                    "explicit developer authority permits tech actions without affecting ordinary actions");
    passed &= check(protodd::wholeGameActionAuthorityAllowed(15) ==
                        protodd::learnedWholeGameResearchAuthorityEnabled,
                    "compiled profile exposes only its configured learned research authority");
    return passed ? 0 : 1;
}
