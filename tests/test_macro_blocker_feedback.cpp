#include "protodd/MacroPlanner.hpp"

#include <algorithm>
#include <iostream>
#include <ranges>
#include <vector>

namespace {

protodd::UnitSnapshot unit(const protodd::UnitId id, const protodd::UnitKind kind,
                           const protodd::Position position = {320, 320}) {
    protodd::UnitSnapshot result;
    result.id = id;
    result.kind = kind;
    result.position = position;
    result.completed = true;
    result.powered = true;
    result.hitPoints = result.maxHitPoints = 100;
    return result;
}

protodd::GameState baseState() {
    using namespace protodd;
    GameState state;
    state.frame = 100;
    state.self.id = 1;
    state.self.race = Race::protoss;
    state.enemy.race = Race::zerg;
    state.self.supplyUsed = 20;
    state.self.supplyTotal = 40;
    state.self.units = {
        unit(1, UnitKind::nexus),
        unit(2, UnitKind::pylon),
        unit(3, UnitKind::forge),
    };
    return state;
}

}  // namespace

int main() {
    using namespace protodd;
    int failures = 0;
    const auto check = [&failures](const bool value, const char* message) {
        if (!value) {
            ++failures;
            std::cerr << "FAIL: " << message << '\n';
        }
    };

    auto state = baseState();
    StrategicPlan plan;
    plan.goals = {
        {GoalKind::build, UnitKind::photonCannon, 1, 120, true,
         "cover the threatened mineral line"},
        {GoalKind::train, UnitKind::probe, 2, 110, true,
         "continue worker production"},
    };
    ResourceLedger ledger{149, 0};
    const std::vector<BuildBlockerFeedback> blockedCannon{{
        UnitKind::photonCannon, {}, BuildBlockerReason::noPlacement, 500}};
    const auto blockedActions = MacroPlanner{}.reconcile(state, plan, ledger, blockedCannon);
    const auto cannon = std::ranges::find_if(blockedActions, [](const MacroAction& action) {
        return action.target == UnitKind::photonCannon;
    });
    check(cannon != blockedActions.end() && !cannon->reserved && !cannon->executable,
          "an actively blocked Cannon does not hold or spend the bank");
    check(std::ranges::any_of(blockedActions, [](const MacroAction& action) {
        return action.target == UnitKind::probe && action.reserved && action.executable;
    }), "released Cannon funds reach an executable worker producer");
    check(ledger.reservedMinerals == 50 && ledger.protectedMinerals == 0,
          "blocked static defense does not protect the whole sub-cost bank");

    state = baseState();
    std::erase_if(state.self.units, [](const UnitSnapshot& existing) {
        return existing.kind == UnitKind::forge;
    });
    plan.goals[0] = {GoalKind::build, UnitKind::forge, 1, 120, true,
                     "viable emergency Forge checkpoint"};
    ResourceLedger forgeLedger{149, 0};
    const auto forgeActions = MacroPlanner{}.reconcile(state, plan, forgeLedger);
    check(std::ranges::any_of(forgeActions, [](const MacroAction& action) {
        return action.target == UnitKind::forge && !action.reserved;
    }), "an unaffordable emergency Forge remains visible as a blocking goal");
    check(forgeLedger.protectedMinerals == 149 && forgeLedger.freeMinerals() == 0,
          "a viable emergency Forge keeps its near-complete cost protected");
    check(std::ranges::none_of(forgeActions, [](const MacroAction& action) {
        return action.target == UnitKind::probe && action.reserved;
    }), "workers cannot consume the viable emergency Forge threshold");

    state = baseState();
    state.self.minerals = 150;
    const ConstructionTaskSite siteA{1, 1, {960, 320}};
    const ConstructionTaskSite siteB{2, 2, {1600, 320}};
    plan.goals = {
        {GoalKind::build, UnitKind::photonCannon, 1, 120, true, "site A", TechnologyKind::none,
         false, false, siteA},
        {GoalKind::build, UnitKind::photonCannon, 1, 120, true, "site B", TechnologyKind::none,
         false, false, siteB},
    };
    ResourceLedger siteLedger{150, 0};
    const std::vector<BuildBlockerFeedback> siteBlocker{{
        UnitKind::photonCannon, siteA, BuildBlockerReason::noBuilder, 500}};
    const auto siteActions = MacroPlanner{}.reconcile(state, plan, siteLedger, siteBlocker);
    const auto actionA = std::ranges::find_if(siteActions, [&siteA](const MacroAction& action) {
        return action.constructionSite.id == siteA.id;
    });
    const auto actionB = std::ranges::find_if(siteActions, [&siteB](const MacroAction& action) {
        return action.constructionSite.id == siteB.id;
    });
    check(actionA != siteActions.end() && !actionA->reserved,
          "builder feedback applies to its exact construction site");
    check(actionB != siteActions.end() && actionB->reserved && actionB->executable,
          "another viable Cannon site can use the released reservation");

    state.frame = 500;
    ResourceLedger retryLedger{150, 0};
    const auto retryActions = MacroPlanner{}.reconcile(state, plan, retryLedger, siteBlocker);
    const auto retryA = std::ranges::find_if(retryActions, [&siteA](const MacroAction& action) {
        return action.constructionSite.id == siteA.id;
    });
    check(retryA != retryActions.end() && retryA->reserved && retryA->executable,
          "the bounded retry deadline re-enables the blocked construction attempt");

    state = baseState();
    std::erase_if(state.self.units, [](const UnitSnapshot& existing) {
        return existing.kind == UnitKind::forge;
    });
    plan.goals = {{GoalKind::build, UnitKind::photonCannon, 1, 120, true,
                   "Cannon waits for its emergency Forge"}};
    const std::vector<BuildBlockerFeedback> missingForgeBlocker{{
        UnitKind::photonCannon, {}, BuildBlockerReason::missingPrerequisite, 500}};
    ResourceLedger missingForgeLedger{149, 0};
    const auto missingForgeActions = MacroPlanner{}.reconcile(
        state, plan, missingForgeLedger, missingForgeBlocker);
    check(std::ranges::any_of(missingForgeActions, [](const MacroAction& action) {
        return action.target == UnitKind::forge && action.blocksLowerPriority;
    }) && missingForgeLedger.protectedMinerals == 149 &&
           missingForgeLedger.freeMinerals() == 0,
           "an active Cannon prerequisite blocker still protects a viable emergency Forge");

    state = baseState();
    plan.goals = {{GoalKind::build, UnitKind::photonCannon, 1, 120, true,
                   "retry after prerequisite recovery"}};
    const std::vector<BuildBlockerFeedback> stalePrerequisiteBlocker{{
        UnitKind::photonCannon, {}, BuildBlockerReason::missingPrerequisite, 500}};
    ResourceLedger prerequisiteRetryLedger{150, 0};
    const auto prerequisiteRetryActions = MacroPlanner{}.reconcile(
        state, plan, prerequisiteRetryLedger, stalePrerequisiteBlocker);
    check(std::ranges::any_of(prerequisiteRetryActions, [](const MacroAction& action) {
        return action.target == UnitKind::photonCannon && action.reserved &&
               action.executable;
    }), "a repaired prerequisite re-enables its target before the generic retry deadline");

    return failures == 0 ? 0 : 1;
}
