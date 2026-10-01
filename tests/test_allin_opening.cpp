#include "protodd/AllInOpening.hpp"
#include "protodd/MacroPlanner.hpp"
#include "protodd/Squads.hpp"

#include <algorithm>
#include <iostream>

using namespace protodd;
namespace {
int failures{};
void check(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
void add(GameState& state, UnitKind kind, int amount, bool complete = true) {
    for (int i = 0; i < amount; ++i) {
        UnitSnapshot unit; unit.id = static_cast<int>(state.self.units.size()) + 1;
        unit.kind = kind; unit.ours = true; unit.completed = complete;
        unit.position = {128, 128}; unit.hitPoints = 100;
        state.self.units.push_back(unit);
    }
}
StrategicPlan baseline() {
    StrategicPlan plan;
    plan.name = "native"; plan.expansionTarget = {1024, 1024};
    plan.attackTarget = {3000, 3000}; plan.desiredBases = 3; plan.desiredWorkers = 50;
    plan.goals = {{GoalKind::build, UnitKind::photonCannon, 4, 120, true, "optional"},
                  {GoalKind::build, UnitKind::roboticsFacility, 1, 118, true, "optional"}};
    return plan;
}
int requested(const StrategicPlan& plan, UnitKind kind) {
    int result = 0;
    for (const auto& goal : plan.goals) if (goal.target == kind) result = std::max(result, goal.desiredCount);
    return result;
}
}
int main() {
    ThreatAssessment threat;
    GameState state; state.self.race = Race::protoss; state.enemy.race = Race::protoss;
    add(state, UnitKind::probe, 12); add(state, UnitKind::nexus, 1); add(state, UnitKind::gateway, 1);
    AllInOpeningPlanner opening; opening.reset(AllInBuild::twoGateZealot);
    auto plan = baseline(); opening.apply(plan, state, threat);
    check(plan.maximumBases == 1 && plan.desiredWorkers == 14 && plan.desiredGasWorkers == 0,
          "gasless commitment caps workers, expansion, and gas");
    check(requested(plan, UnitKind::gateway) == 2 && requested(plan, UnitKind::photonCannon) == 0,
          "fund second Gateway instead of speculative static defense");
    check(requested(plan, UnitKind::roboticsFacility) == 0, "no speculative detection tax");
    check(!SquadPlanner::shouldCoverExpansion(state, plan), "one-base army never guards an unrequested natural");
    add(state, UnitKind::zealot, 4, false);
    state.frame = 4000; plan = baseline(); opening.apply(plan, state, threat);
    check(opening.phase() == AllInPhase::assemble, "unfinished army cannot launch");
    for (auto& unit : state.self.units) unit.completed = true;
    state.frame = 4656; plan = baseline(); opening.apply(plan, state, threat);
    check(opening.launchFrame() == 4656 && plan.posture == Posture::attack, "four Zealots launch sticky attack");
    threat.combatEnemiesNearMain = 3;
    state.frame += 24; plan = baseline(); opening.apply(plan, state, threat);
    check(plan.posture == Posture::defend && opening.launchFrame() == 4656, "real breach interrupts without reroll");
    threat = {}; state.frame = 4656 + 120 * 24;
    plan = baseline(); opening.apply(plan, state, threat);
    check(opening.phase() == AllInPhase::transition && opening.transitionReason() == "pressure-window",
          "bounded pressure window ends commitment");
    check(plan.desiredBases >= 2 && plan.desiredWorkers >= 32 && plan.sustainEconomy,
          "transition funds economic recovery");
    check(plan.posture == Posture::attack && plan.minimumAttackSize == 4,
          "expanding does not recall surviving pressure army");
    state.bases = {{}};
    state.bases[0].center = {1000, 1000}; state.bases[0].groundDistanceFromMain = 900;
    state.bases[0].mineralsRemaining = 10000;
    plan = baseline(); plan.expansionTarget = {-1, -1}; threat.proxy = 1;
    opening.apply(plan, state, threat);
    check(plan.expansionTarget == Position{1000, 1000} && requested(plan, UnitKind::nexus) >= 2,
          "perimeter proxy belief alone cannot leave economic recovery without a site");
    threat = {};
    state.frame += 24; plan = baseline(); opening.apply(plan, state, threat);
    check(opening.phase() == AllInPhase::transition, "transition cannot revert to a new all-in");
    opening.reset(AllInBuild::twoGateZealot);
    state.frame = 9000; plan = baseline(); opening.apply(plan, state, threat);
    check(opening.transitionReason() == "deadline", "hard deadline aborts stalled opening even without launch");
    for (const auto build : {AllInBuild::threeGateDragoon, AllInBuild::fourGateDragoon, AllInBuild::darkTemplar}) {
        opening.reset(build); state = {}; state.frame = 7200;
        add(state, UnitKind::probe, 20); add(state, UnitKind::gateway, 2);
        add(state, UnitKind::cyberneticsCore, 1); add(state, UnitKind::assimilator, 1);
        add(state, UnitKind::dragoon, 6);
        if (build == AllInBuild::darkTemplar) add(state, UnitKind::darkTemplar, 2);
        plan = baseline(); opening.apply(plan, state, threat);
        check(opening.phase() == AllInPhase::pressure && plan.posture == Posture::attack,
              "each alternative launches on its completed army milestone");
        check(plan.desiredGasWorkers == 3 && requested(plan, UnitKind::gateway) >= 3,
              "alternative funds gas and explicit Gateway ceiling");
        std::erase_if(state.self.units, [](const UnitSnapshot& u) {
            return u.kind == UnitKind::dragoon || u.kind == UnitKind::darkTemplar;
        });
        state.frame += 12 * 24; plan = baseline(); opening.apply(plan, state, threat);
        check(opening.transitionReason() == "army-loss", "failed push resumes economy instead of endless recommitment");
    }
    opening.reset(AllInBuild::twoGateZealot); state = {}; add(state, UnitKind::probe, 12);
    add(state, UnitKind::zealot, 3); add(state, UnitKind::gateway, 2);
    plan = baseline(); opening.apply(plan, state, threat);
    check(requested(plan, UnitKind::assimilator) == 1, "gas begins behind the first three completed Zealots");
    add(state, UnitKind::assimilator, 1);
    plan = baseline(); opening.apply(plan, state, threat);
    check(plan.desiredGasWorkers == 3 && requested(plan, UnitKind::cyberneticsCore) == 1,
          "two-Gate pressure develops ranged access before the expansion transition");
    add(state, UnitKind::cyberneticsCore, 1);
    plan = baseline(); opening.apply(plan, state, threat);
    check(requested(plan, UnitKind::dragoon) == 2, "completed Core shifts reinforcement priority to Dragoons");
    UnitSnapshot enemy; enemy.kind = UnitKind::darkTemplar; state.enemy.units.push_back(enemy);
    plan = baseline(); opening.apply(plan, state, threat);
    check(plan.requireMobileDetection && requested(plan, UnitKind::observer) == 1,
          "observed cloak threat retains legal detector production");
    opening.reset(AllInBuild::standard); plan = baseline(); opening.apply(plan, state, threat);
    check(plan.desiredBases == 3 && plan.goals.size() == 2, "standard profile unchanged");
    return failures ? 1 : 0;
}
