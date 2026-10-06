#include "protodd/MacroPlanner.hpp"
#include "protodd/Strategy.hpp"
#include "protodd/Technology.hpp"
#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

int failures = 0;

void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

protodd::UnitSnapshot unit(const protodd::UnitId id,
                           const protodd::UnitKind kind,
                           const protodd::Position position,
                           const bool completed = true) {
    protodd::UnitSnapshot result;
    result.id = id;
    result.kind = kind;
    result.position = position;
    result.lastPosition = position;
    result.ours = true;
    result.visible = true;
    result.completed = completed;
    result.buildProgress = completed ? 100 : 40;
    return result;
}

int powerRecoveryGoalCount(const protodd::StrategicPlan& plan) {
    return static_cast<int>(std::ranges::count_if(
        plan.goals, [](const protodd::ProductionGoal& goal) {
            return goal.goal == protodd::GoalKind::build &&
                   goal.target == protodd::UnitKind::pylon &&
                   goal.reason == "restore power to disabled production";
        }));
}

const protodd::ProductionGoal* powerRecoveryGoal(
    const protodd::StrategicPlan& plan) {
    const auto found = std::ranges::find_if(
        plan.goals, [](const protodd::ProductionGoal& goal) {
            return goal.goal == protodd::GoalKind::build &&
                   goal.target == protodd::UnitKind::pylon &&
                   goal.reason == "restore power to disabled production";
        });
    return found == plan.goals.end() ? nullptr : &*found;
}

void testDependencyGraphAndLostCore() {
    using namespace protodd;
    std::array<std::uint8_t, static_cast<std::size_t>(UnitKind::count)> marks{};
    bool acyclic = true;
    const auto visit = [&marks, &acyclic](const auto& self,
                                          const UnitKind kind) -> void {
        const auto index = static_cast<std::size_t>(kind);
        if (index >= marks.size()) {
            acyclic = false;
            return;
        }
        if (marks[index] == 1) {
            acyclic = false;
            return;
        }
        if (marks[index] == 2) return;
        marks[index] = 1;
        for (const auto prerequisite : unitPrerequisites(kind)) {
            self(self, prerequisite);
        }
        marks[index] = 2;
    };
    for (std::size_t index = 0; index < marks.size(); ++index) {
        visit(visit, static_cast<UnitKind>(index));
    }
    expect(acyclic, "unit prerequisite graph is acyclic and in range");

    const auto observatoryRequirements = unitPrerequisites(UnitKind::observatory);
    const auto observerRequirements = unitPrerequisites(UnitKind::observer);
    expect(observatoryRequirements.size() == 1 &&
               observatoryRequirements.front() == UnitKind::roboticsFacility,
           "Observatory building depends only on Robotics Facility");
    expect(observerRequirements.size() == 2 &&
               observerRequirements[0] == UnitKind::roboticsFacility &&
               observerRequirements[1] == UnitKind::observatory,
           "Observer unit depends on Robotics Facility and Observatory");

    GameState state;
    state.self.race = Race::protoss;
    state.self.supplyUsed = 20;
    state.self.supplyTotal = 40;
    state.self.units = {unit(1, UnitKind::pylon, {128, 128}),
                        unit(2, UnitKind::gateway, {192, 128})};
    StrategicPlan dragoonPlan;
    dragoonPlan.goals = {{GoalKind::train, UnitKind::dragoon, 1, 100, true,
                          "replace ranged force"}};
    MacroPlanner planner;
    ResourceLedger firstBank{400, 200};
    const auto recovery = planner.reconcile(state, dragoonPlan, firstBank);
    expect(std::ranges::any_of(recovery, [](const MacroAction& action) {
               return action.action == MacroActionKind::build &&
                      action.target == UnitKind::cyberneticsCore;
           }), "blocking Dragoon demand replaces a lost Cybernetics Core");

    state.frame += 24;
    state.self.units.push_back(unit(3, UnitKind::cyberneticsCore, {256, 128}));
    ResourceLedger secondBank{400, 200};
    const auto restored = planner.reconcile(state, dragoonPlan, secondBank);
    expect(std::ranges::any_of(restored, [](const MacroAction& action) {
               return action.action == MacroActionKind::train &&
                      action.target == UnitKind::dragoon && action.reserved;
           }) && std::ranges::none_of(restored, [](const MacroAction& action) {
               return action.action == MacroActionKind::build &&
                      action.target == UnitKind::cyberneticsCore;
           }), "completed Core unblocks the required Dragoon without repeat construction");
}

void testLostObservatoryAndInterruptedResearch() {
    using namespace protodd;
    GameState observerState;
    observerState.self.race = Race::protoss;
    observerState.self.supplyUsed = 20;
    observerState.self.supplyTotal = 40;
    observerState.self.units = {
        unit(1, UnitKind::pylon, {128, 128}),
        unit(2, UnitKind::gateway, {192, 128}),
        unit(3, UnitKind::cyberneticsCore, {256, 128}),
        unit(4, UnitKind::roboticsFacility, {320, 128}),
    };
    StrategicPlan observerPlan;
    observerPlan.goals = {{GoalKind::train, UnitKind::observer, 1, 100, true,
                           "restore detection"}};
    ResourceLedger observerBank{400, 200};
    const auto observerActions = MacroPlanner{}.reconcile(
        observerState, observerPlan, observerBank);
    expect(std::ranges::any_of(observerActions, [](const MacroAction& action) {
               return action.action == MacroActionKind::build &&
                      action.target == UnitKind::observatory;
           }), "blocking Observer demand rebuilds the missing Observatory");

    GameState stormState;
    stormState.self.race = Race::protoss;
    stormState.self.supplyUsed = 20;
    stormState.self.supplyTotal = 40;
    stormState.self.units = {
        unit(10, UnitKind::pylon, {128, 128}),
        unit(11, UnitKind::gateway, {192, 128}),
        unit(12, UnitKind::cyberneticsCore, {256, 128}),
        unit(13, UnitKind::citadelOfAdun, {320, 128}),
    };
    stormState.self.technologies = {{TechnologyKind::psionicStorm, 0, true}};
    StrategicPlan stormPlan;
    stormPlan.goals = {{GoalKind::research, UnitKind::unknown, 1, 100, true,
                        "restore Storm research", TechnologyKind::psionicStorm}};
    ResourceLedger stormBank{400, 200};
    const auto stormActions = MacroPlanner{}.reconcile(
        stormState, stormPlan, stormBank);
    expect(std::ranges::any_of(stormActions, [](const MacroAction& action) {
               return action.action == MacroActionKind::build &&
                      action.target == UnitKind::templarArchives;
           }) && std::ranges::none_of(stormActions, [](const MacroAction& action) {
               return action.technology == TechnologyKind::psionicStorm;
           }), "stale Storm progress does not hide the destroyed Archives producer");
}

void testLocalPowerRecovery() {
    using namespace protodd;
    GameState separatedSites;
    separatedSites.frame = 120;
    separatedSites.self.race = Race::protoss;
    separatedSites.enemy.race = Race::terran;
    separatedSites.self.supplyUsed = 24;
    separatedSites.self.supplyTotal = 40;
    auto separatedGateway = unit(30, UnitKind::gateway, {512, 512});
    separatedGateway.powered = false;
    auto separatedForge = unit(31, UnitKind::forge, {720, 512});
    separatedForge.powered = false;
    separatedSites.self.units = {separatedGateway, separatedForge};
    const auto separatedPlan = StrategyEngine{}.plan(separatedSites, {});
    expect(powerRecoveryGoalCount(separatedPlan) == 2,
           "separated unpowered buildings receive independent local Pylon tasks");

    GameState state;
    state.frame = 120;
    state.self.id = 1;
    state.self.race = Race::protoss;
    state.enemy.race = Race::terran;
    state.self.supplyUsed = 24;
    state.self.supplyTotal = 40;
    auto gateway = unit(10, UnitKind::gateway, {512, 512});
    gateway.powered = false;
    auto core = unit(11, UnitKind::cyberneticsCore, {600, 512});
    core.powered = false;
    auto remotePylon = unit(20, UnitKind::pylon, {1500, 1500}, false);
    state.self.units = {gateway, core, remotePylon};

    const auto first = StrategyEngine{}.plan(state, {});
    const auto* firstGoal = powerRecoveryGoal(first);
    expect(powerRecoveryGoalCount(first) == 1 && firstGoal != nullptr &&
               firstGoal->blocking && firstGoal->priority == 98 &&
               firstGoal->constructionSite.valid() &&
               firstGoal->constructionSite.anchor == gateway.position,
           "unpowered nearby buildings share one local blocking Pylon task");
    const auto stableSite = firstGoal != nullptr ? firstGoal->constructionSite
                                                  : ConstructionTaskSite{};

    state.frame += 24;
    const auto second = StrategyEngine{}.plan(state, {});
    const auto* secondGoal = powerRecoveryGoal(second);
    expect(powerRecoveryGoalCount(second) == 1 && secondGoal != nullptr &&
               secondGoal->constructionSite.id == stableSite.id &&
               secondGoal->constructionSite.anchor == stableSite.anchor,
           "power recovery keeps a stable task ID and anchor across frames");

    MacroPlanner macro;
    ResourceLedger waitingBank{0, 0};
    const auto waiting = macro.reconcile(state, second, waitingBank);
    expect(std::ranges::any_of(waiting, [](const MacroAction& action) {
               return action.target == UnitKind::pylon &&
                      action.reason == "restore power to disabled production";
           }), "local power recovery remains pending while unaffordable");

    state.frame += 24;
    state.self.units.push_back(unit(21, UnitKind::pylon, {550, 540}, false));
    const auto underConstruction = StrategyEngine{}.plan(state, {});
    expect(powerRecoveryGoalCount(underConstruction) == 0,
           "a nearby Pylon under construction suppresses duplicate power recovery");
    ResourceLedger constructionBank{0, 0};
    const auto noDuplicate = macro.reconcile(state, underConstruction,
                                              constructionBank);
    expect(std::ranges::none_of(noDuplicate, [](const MacroAction& action) {
               return action.reason == "restore power to disabled production";
           }), "a nearby in-progress Pylon clears remembered duplicate recovery");

    state.frame += 24;
    std::erase_if(state.self.units, [](const UnitSnapshot& candidate) {
        return candidate.id == 21;
    });
    const auto afterLoss = StrategyEngine{}.plan(state, {});
    const auto* recoveredGoal = powerRecoveryGoal(afterLoss);
    expect(recoveredGoal != nullptr &&
               recoveredGoal->constructionSite.id == stableSite.id &&
               recoveredGoal->constructionSite.anchor == stableSite.anchor,
           "lost local Pylon reopens the same stable power-recovery task");
}

void testExplicitOptionalCancellation() {
    using namespace protodd;
    GameState state;
    state.self.race = Race::protoss;
    state.self.supplyUsed = 0;
    state.self.supplyTotal = 200;
    MacroPlanner planner;
    StrategicPlan optional;
    optional.goals = {{GoalKind::build, UnitKind::pylon, 1, 50, true,
                       "optional future Pylon"}};
    ResourceLedger emptyBank{0, 0};
    static_cast<void>(planner.reconcile(state, optional, emptyBank));

    state.frame += 24;
    optional.goals = {{GoalKind::build, UnitKind::pylon, 0, 50, false,
                       "optional future Pylon"}};
    ResourceLedger cancelBank{0, 0};
    const auto cancelled = planner.reconcile(state, optional, cancelBank);
    expect(std::ranges::none_of(cancelled, [](const MacroAction& action) {
               return action.target == UnitKind::pylon;
           }), "an explicit zero nonblocking quota cancels the saved optional task");

    state.frame += 24;
    optional.goals.clear();
    ResourceLedger laterBank{200, 0};
    const auto later = planner.reconcile(state, optional, laterBank);
    expect(std::ranges::none_of(later, [](const MacroAction& action) {
               return action.target == UnitKind::pylon;
           }), "cancelled optional construction is not resurrected from planner memory");
}

}  // namespace

int main() {
    testDependencyGraphAndLostCore();
    testLostObservatoryAndInterruptedResearch();
    testLocalPowerRecovery();
    testExplicitOptionalCancellation();
    return failures == 0 ? 0 : 1;
}
