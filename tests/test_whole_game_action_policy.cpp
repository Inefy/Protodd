#include "protodd/WholeGameActionPolicy.hpp"

#include <array>
#include <iostream>

int main() {
    using protodd::wholeGameActorEligible;
    using protodd::wholeGameCancelBuildAction;
    using protodd::WholeGameEntityTargetStatus;
    using protodd::wholeGameEntityTargetStatus;
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
    constexpr std::array<std::size_t, 27> supportedActions{
        0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13, 15, 16, 17, 18, 20, 21,
        23, 24, 30, 31, 32, 33, 34};
    for (const auto action : supportedActions) {
        passed &= check(wholeGameActorEligible(action, 0, true, false, false),
                        "every supported action accepts a completed actor");
        if (action == wholeGameCancelBuildAction) {
            passed &= check(wholeGameActorEligible(action, 0, false, true, true),
                            "cancel_build accepts an unfinished building");
            passed &= check(!wholeGameActorEligible(action, 0, false, false, false),
                            "cancel_build rejects an unfinished worker");
            passed &= check(!wholeGameActorEligible(action, 0, false, true, false),
                            "cancel_build rejects an idle incomplete building");
        } else {
            passed &= check(!wholeGameActorEligible(action, 0, false, false, false),
                            "every other supported action rejects an unfinished unit");
            passed &= check(!wholeGameActorEligible(action, 0, false, true, true),
                            "every other supported action rejects an unfinished building");
        }
    }
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
    passed &= check(wholeGameEntityTargetStatus(true, false, false, false, false) ==
                        WholeGameEntityTargetStatus::missingId,
                    "entity-target intent without a target ID is rejected explicitly");
    passed &= check(wholeGameEntityTargetStatus(true, true, true, false, true) ==
                        WholeGameEntityTargetStatus::notVisible,
                    "a remembered but hidden target is not legal at delayed dispatch");
    passed &= check(wholeGameEntityTargetStatus(true, true, true, true, false) ==
                        WholeGameEntityTargetStatus::noLongerExists,
                    "a target destroyed between proposal and dispatch is rejected");
    passed &= check(wholeGameEntityTargetStatus(true, true, true, true, true) ==
                        WholeGameEntityTargetStatus::available,
                    "a visible live target remains dispatchable");
    return passed ? 0 : 1;
}
