#include "protodd/MacroPlanner.hpp"
#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <iostream>

namespace {

protodd::UnitSnapshot unit(const protodd::UnitId id, const protodd::UnitKind kind,
                           const bool completed = true) {
    protodd::UnitSnapshot result;
    result.id = id;
    result.kind = kind;
    result.completed = completed;
    result.powered = true;
    return result;
}

bool reservesPylon(protodd::GameState state, const protodd::StrategicPlan& plan) {
    protodd::ResourceLedger ledger{100, 0};
    const auto actions = protodd::MacroPlanner{}.reconcile(state, plan, ledger);
    return std::ranges::any_of(actions, [](const protodd::MacroAction& action) {
        return action.action == protodd::MacroActionKind::build &&
               action.target == protodd::UnitKind::pylon && action.reserved;
    });
}

protodd::GameState baseState() {
    protodd::GameState state;
    state.self.race = protodd::Race::protoss;
    state.self.supplyUsed = 27;
    state.self.supplyTotal = 34;
    state.self.units = {
        unit(1, protodd::UnitKind::nexus),
        unit(2, protodd::UnitKind::pylon),
        unit(3, protodd::UnitKind::gateway),
    };
    state.self.producerSlots = {
        {1, protodd::UnitKind::nexus, false, 0, 0, 6, false, false, false, {}},
        {3, protodd::UnitKind::gateway, false, 0, 0, 6, false, false, false, {}},
    };
    return state;
}

protodd::StrategicPlan zealotPlan() {
    protodd::StrategicPlan plan;
    plan.desiredWorkers = 0;
    plan.composition = {{protodd::UnitKind::zealot, 1.0}};
    return plan;
}

}  // namespace

int main() {
    using namespace protodd;
    int failures = 0;
    const auto check = [&failures](const bool condition, const char* message) {
        if (!condition) {
            ++failures;
            std::cerr << "FAIL: " << message << '\n';
        }
    };

    auto state = baseState();
    auto plan = zealotPlan();
    state.pylonBuilderTravelFrames = 0;
    check(!reservesPylon(state, plan),
          "one idle producer does not trigger supply before its next cycle reaches the deadline");
    state.self.units.push_back(unit(4, UnitKind::gateway));
    state.self.producerSlots.push_back(
        {4, UnitKind::gateway, false, 0, 0, 6, false, false, false, {}});
    check(reservesPylon(state, plan),
          "concurrent idle producers forecast their combined supply before the cap");

    auto loneNexus = baseState();
    loneNexus.self.supplyUsed = 28;
    loneNexus.pylonBuilderTravelFrames = 0;
    std::erase_if(loneNexus.self.units, [](const UnitSnapshot& value) {
        return value.kind == UnitKind::gateway;
    });
    std::erase_if(loneNexus.self.producerSlots, [](const ProducerSlotSnapshot& slot) {
        return slot.kind == UnitKind::gateway;
    });
    auto noTrainingDemand = zealotPlan();
    noTrainingDemand.composition.clear();
    check(reservesPylon(loneNexus, noTrainingDemand),
          "a lone Nexus protects a Pylon with six supply of headroom for resource lead time");

    auto queued = baseState();
    queued.pylonBuilderTravelFrames = 0;
    queued.self.units.back().kind = UnitKind::roboticsFacility;
    queued.self.producerSlots.back().kind = UnitKind::roboticsFacility;
    queued.self.producerSlots.back().trainingQueueSize = 1;
    queued.self.producerSlots.back().queuedUnits = {UnitKind::reaver};
    queued.self.queuedUnits = {UnitKind::reaver};
    auto observerPlan = zealotPlan();
    observerPlan.composition = {{UnitKind::observer, 1.0}};
    check(reservesPylon(queued, observerPlan),
          "a waiting high-supply queue item raises the producer forecast");

    auto active = baseState();
    active.pylonBuilderTravelFrames = 0;
    active.self.units.back().kind = UnitKind::roboticsFacility;
    active.self.units.back().remainingTrainFrames = 500;
    active.self.producerSlots.back().kind = UnitKind::roboticsFacility;
    active.self.producerSlots.back().activeTraining = true;
    active.self.producerSlots.back().trainingQueueSize = 2;
    active.self.producerSlots.back().remainingTrainFrames = 500;
    active.self.producerSlots.back().queuedUnits = {UnitKind::observer};
    active.self.queuedUnits = {UnitKind::observer};
    check(!reservesPylon(active, observerPlan),
          "already-used active supply is not counted again when its queue finishes after the deadline");

    auto distantBuilder = baseState();
    distantBuilder.pylonBuilderTravelFrames = 500;
    check(reservesPylon(distantBuilder, plan),
          "a longer builder route expands the forecast window and starts supply earlier");

    auto pending = baseState();
    pending.pylonBuilderTravelFrames = 0;
    auto unfinished = unit(5, UnitKind::pylon, false);
    unfinished.buildProgress = 0;
    pending.self.units.push_back(unfinished);
    check(!reservesPylon(pending, plan),
          "a live Pylon finishing before the replacement deadline supplies its capacity once");

    auto lost = baseState();
    lost.self.supplyUsed = 16;
    lost.self.supplyTotal = 18;
    lost.self.units.erase(std::ranges::find_if(lost.self.units, [](const UnitSnapshot& value) {
        return value.kind == UnitKind::pylon;
    }));
    lost.self.producerSlots.clear();
    check(reservesPylon(lost, plan),
          "a lost Pylon is not credited from the former unit count or a stale forecast");

    auto capped = baseState();
    capped.self.supplyUsed = 390;
    capped.self.supplyTotal = 392;
    auto capPylon = unit(5, UnitKind::pylon, false);
    capPylon.buildProgress = 0;
    capped.self.units.push_back(capPylon);
    check(!reservesPylon(capped, plan),
          "timely Pylon capacity is clamped at 400 internal supply");

    if (failures == 0) std::cout << "supply forecast scenarios passed\n";
    return failures == 0 ? 0 : 1;
}
