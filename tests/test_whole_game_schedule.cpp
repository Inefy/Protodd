#include "WholeGameSchedule.hpp"
#include "protodd/WholeGameActionPolicy.hpp"

#include <iostream>
#include <set>
#include <utility>
#include <vector>

int main() {
    using protodd::cpu::Intent;
    using protodd::cpu::PlannedIntent;
    using protodd::cpu::WholeGameSchedule;
    const auto check = [](const bool condition, const char* message) {
        if (!condition) std::cerr << "FAIL: " << message << '\n';
        return condition;
    };
    bool passed = true;
    WholeGameSchedule schedule;
    schedule.replace(100, std::vector<PlannedIntent>{
        {5, Intent{.kind = 1, .actorIds = {7, 8}}, 41, 0},
        // Slot 1 emitted no intent; preserve the model slot ordinal in audit rows.
        {5, Intent{.kind = 2, .actorIds = {7}}, 42, 2},
        {10, Intent{.kind = protodd::wholeGameCancelBuildAction, .actorIds = {9}}, 43, 5},
    });
    passed &= check(schedule.takeDue(104).empty(),
                    "delayed actions are not dispatched before their due frame");
    auto repeatedSlots = schedule.takeDue(105);
    passed &= check(repeatedSlots.size() == 2 && repeatedSlots[0].slot == 0 &&
                        repeatedSlots[1].slot == 2,
                    "same-frame repeated slots retain original model ordinals and order");

    std::set<int> claimedActors;
    std::vector<int> deferred;
    for (const auto& item : repeatedSlots) {
        for (const auto actor : item.intent.actorIds) {
            if (!claimedActors.insert(actor).second) deferred.push_back(actor);
        }
    }
    passed &= check(claimedActors == std::set<int>{7, 8} && deferred == std::vector<int>{7},
                    "group actors are retained while a repeated actor is deferred");
    repeatedSlots[1].intent.actorIds = deferred;
    passed &= check(schedule.defer(std::move(repeatedSlots[1]), 106),
                    "a repeated actor can retry on the next frame");
    const auto retry = schedule.takeDue(106);
    passed &= check(retry.size() == 1 && retry[0].attemptId == 42 &&
                        retry[0].proposalFrame == 100 && retry[0].slot == 2 &&
                        retry[0].intent.actorIds == std::vector<int>{7},
                    "retry preserves proposal identity, slot, and only the deferred actor");

    const auto cancellation = schedule.takeDue(110);
    passed &= check(cancellation.size() == 1 && cancellation[0].attemptId == 43 &&
                        cancellation[0].slot == 5 &&
                        protodd::wholeGameActorEligible(
                            cancellation[0].intent.kind, 0, false, true, true),
                    "delayed cancellation remains eligible for an unfinished building");
    passed &= check(!protodd::wholeGameActorEligible(
                        cancellation[0].intent.kind, 0, false, true, false),
                    "a cancellation is rejected after construction has already ended");

    WholeGameSchedule implicitOrdinals;
    implicitOrdinals.replace(200, std::vector<PlannedIntent>{
        {0, Intent{.kind = 1, .actorIds = {1}}, 51},
        {0, Intent{.kind = 2, .actorIds = {2}}, 52},
    });
    const auto implicitSlots = implicitOrdinals.takeDue(200);
    passed &= check(implicitSlots.size() == 2 && implicitSlots[0].slot == 0 &&
                        implicitSlots[1].slot == 1,
                    "callers without explicit model ordinals retain plan-index slots");
    return passed ? 0 : 1;
}
