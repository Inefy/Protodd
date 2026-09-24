#include "WholeGameSchedule.hpp"

#include <cassert>
#include <stdexcept>
#include <utility>
#include <vector>

int main() {
    using protodd::cpu::Intent;
    using protodd::cpu::PlannedIntent;
    using protodd::cpu::WholeGameSchedule;
    WholeGameSchedule schedule;
    schedule.replace(100, std::vector<PlannedIntent>{
        {5, Intent{.kind = 1}}, {0, Intent{.kind = 2}}, {5, Intent{.kind = 3}}});
    assert(schedule.pending() == 3);
    assert(schedule.hasDue(100));
    auto first = schedule.takeDue(100);
    assert(first.size() == 1 && first[0].slot == 1 && first[0].intent.kind == 2);
    assert(!schedule.hasDue(104) && schedule.hasDue(105));
    assert(schedule.takeDue(104).empty());
    auto sameFrame = schedule.takeDue(105);
    assert(sameFrame.size() == 2 && sameFrame[0].slot == 0 && sameFrame[1].slot == 2);
    assert(schedule.defer(std::move(sameFrame[1]), 106));
    auto deferred = schedule.takeDue(106);
    assert(deferred.size() == 1 && deferred[0].slot == 2);
    assert(schedule.pending() == 0);
    assert(!schedule.hasDue(106));

    schedule.replace(200, std::vector<PlannedIntent>{{23, Intent{.kind = 4}}});
    auto last = schedule.takeDue(223);
    assert(last.size() == 1 && last[0].intent.kind == 4);
    assert(!schedule.defer(std::move(last[0]), 224));
    schedule.replace(300, std::vector<PlannedIntent>{{10, Intent{.kind = 5}}});
    schedule.replace(301, std::vector<PlannedIntent>{{0, Intent{.kind = 6}}});
    assert(schedule.takeDue(301)[0].intent.kind == 6);
    assert(schedule.takeDue(310).empty());
    schedule.replace(400, std::vector<PlannedIntent>{{20, Intent{.kind = 7}}});
    assert(schedule.takeDue(424).empty());
    assert(schedule.pending() == 0);
    schedule.replace(500, std::vector<PlannedIntent>{{5, Intent{.kind = 8}}});
    assert(schedule.takeDue(501).empty());
    assert(schedule.takeDue(499).empty());  // Rewind clears pending decisions.

    bool rejected = false;
    try { schedule.replace(600, std::vector<PlannedIntent>{{24, Intent{}}}); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
}
