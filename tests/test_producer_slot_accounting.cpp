#include "protodd/MacroPlanner.hpp"

#include <algorithm>
#include <iostream>
#include <vector>

namespace {

protodd::UnitSnapshot unit(const protodd::UnitId id, const protodd::UnitKind kind) {
    protodd::UnitSnapshot result;
    result.id = id;
    result.kind = kind;
    result.completed = true;
    result.powered = true;
    return result;
}

int runPlanner(protodd::GameState& state, const protodd::StrategicPlan& plan,
               protodd::ResourceLedger& ledger) {
    const auto actions = protodd::MacroPlanner{}.reconcile(state, plan, ledger);
    return static_cast<int>(std::ranges::count_if(actions, [](const auto& action) {
        return action.action == protodd::MacroActionKind::train &&
               action.target == protodd::UnitKind::zealot && action.reserved &&
               action.executable;
    }));
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

    std::vector<TrainingProducerCandidate> candidates{
        {10, true, true, false, false, false, true, true, 1, 20, 6, false,
         false, false},
        {20, true, true, false, false, false, true, false, 0, 0, 6, false,
         false, false},
    };
    check(selectTrainingProducer(candidates) == 20,
          "a busy low-ID producer does not hide an idle legal producer");
    candidates[0].remainingTrainFrames = 5;
    candidates[0].trainingQueueSize = 1;
    check(selectTrainingProducer(candidates) == 10,
          "a producer finishing inside the command latency may take one successor");
    candidates[0].recentTrainCommand = true;
    check(selectTrainingProducer(candidates) == 20,
          "an unacknowledged accepted train command keeps its producer occupied");
    candidates[0].recentTrainCommand = false;
    candidates[0].remainingTrainFrames = 30;
    candidates[0].legalForTarget = false;
    check(selectTrainingProducer(candidates) == 20,
          "after a producer-level rejection, retry can select the next legal building");
    candidates[1].powered = false;
    check(!selectTrainingProducer(candidates),
          "rejected and unpowered producer candidates are not selected");
    candidates[1].powered = true;
    candidates[1].researching = true;
    check(!selectTrainingProducer(candidates),
          "a producer occupied by research cannot receive a train order");

    GameState state;
    state.frame = 1000;
    state.latencyFrames = 6;
    state.self.minerals = 200;
    state.self.supplyUsed = 4;
    state.self.supplyTotal = 20;
    state.self.units = {unit(1, UnitKind::gateway), unit(2, UnitKind::gateway),
                        unit(3, UnitKind::zealot)};
    state.self.queuedUnits = {UnitKind::zealot};
    // Aggregate compatibility fields deliberately report both an occupied
    // Gateway and a paid waiting unit. Per-building state must avoid counting
    // those commitments against the separate idle Gateway.
    state.self.busyProducers = {UnitKind::gateway};
    state.self.producerSlots = {
        {1, UnitKind::gateway, true, 2, 80, 6, false, false, false},
        {2, UnitKind::gateway, false, 0, 0, 6, false, false, false},
    };
    StrategicPlan composition;
    composition.composition = {{UnitKind::zealot, 1.0}};
    ResourceLedger armyBank{200, 0};
    check(runPlanner(state, composition, armyBank) == 1,
          "one paid queue on a busy Gateway does not suppress the idle Gateway");
    StrategicPlan alreadyPaid;
    alreadyPaid.goals = {{GoalKind::train, UnitKind::zealot, 2, 90, false,
                          "two-unit screen"}};
    ResourceLedger alreadyPaidBank{200, 0};
    check(runPlanner(state, alreadyPaid, alreadyPaidBank) == 0,
          "the active Zealot and its one paid queue entry satisfy a two-unit goal once");

    GameState researchState;
    researchState.frame = 2000;
    researchState.self.minerals = 200;
    researchState.self.gas = 200;
    researchState.self.units = {unit(11, UnitKind::templarArchives)};
    researchState.self.producerSlots = {
        {11, UnitKind::templarArchives, false, 0, 0, 6, false, true, false},
    };
    researchState.self.technologies = {{TechnologyKind::psionicStorm, 0, true}};
    StrategicPlan technology;
    technology.goals = {{GoalKind::upgrade, UnitKind::unknown, 1, 100, false,
                         "amulet", TechnologyKind::khaydarinAmulet}};
    ResourceLedger researchBank{200, 200};
    const auto blockedByResearch = MacroPlanner{}.reconcile(
        researchState, technology, researchBank);
    check(std::ranges::none_of(blockedByResearch, [](const auto& action) {
              return action.action == MacroActionKind::upgrade &&
                     action.technology == TechnologyKind::khaydarinAmulet;
          }), "an actively researching Archives is not double-allocated to an upgrade");
    researchState.self.units.push_back(unit(12, UnitKind::templarArchives));
    researchState.self.producerSlots.push_back(
        {12, UnitKind::templarArchives, false, 0, 0, 6, false, false, false});
    ResourceLedger secondArchiveBank{200, 200};
    const auto usesSecondArchive = MacroPlanner{}.reconcile(
        researchState, technology, secondArchiveBank);
    check(std::ranges::any_of(usesSecondArchive, [](const auto& action) {
              return action.action == MacroActionKind::upgrade &&
                     action.technology == TechnologyKind::khaydarinAmulet &&
                     action.reserved;
          }), "an idle second Archives remains usable while the first is researching");

    if (failures == 0) std::cout << "producer slot accounting scenarios passed\n";
    return failures == 0 ? 0 : 1;
}
