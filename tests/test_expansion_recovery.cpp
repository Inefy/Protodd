#include "protodd/Operations.hpp"
#include "protodd/Workers.hpp"

#include <algorithm>
#include <iostream>
#include <set>

namespace {

protodd::UnitSnapshot unit(const int id, const protodd::UnitKind kind,
                           const protodd::Position position, const bool completed = true) {
    protodd::UnitSnapshot result;
    result.id = id;
    result.kind = kind;
    result.position = position;
    result.completed = completed;
    result.ours = true;
    result.role = kind == protodd::UnitKind::probe ? protodd::UnitRole::worker :
        kind == protodd::UnitKind::nexus ? protodd::UnitRole::resourceDepot :
                                           protodd::UnitRole::groundArmy;
    return result;
}

protodd::BaseSnapshot base(const int id, const int owner, const protodd::Position center,
                           const int minerals, const int route) {
    protodd::BaseSnapshot result;
    result.id = id;
    result.ownerId = owner;
    result.center = center;
    result.mineralLine = {center.x + 32, center.y};
    result.mineralsRemaining = minerals;
    result.mineralPatches = 8;
    result.groundDistanceFromMain = route;
    return result;
}

}  // namespace

int main() {
    using namespace protodd;
    auto errors = 0;
    const auto check = [&errors](const bool condition, const char* message) {
        if (!condition) {
            ++errors;
            std::cerr << message << '\n';
        }
    };

    GameState noDepot;
    noDepot.frame = 9000;
    noDepot.self.id = 1;
    noDepot.self.minerals = 400;
    noDepot.bases = {base(1, -1, {1280, 640}, 12000, 960)};
    for (auto id = 0; id < 5; ++id)
        noDepot.self.units.push_back(unit(id, UnitKind::probe, {320, 320}));
    StrategicPlan rebuild;
    rebuild.expansionTarget = noDepot.bases.front().center;
    WorkerManager workers;
    InfluenceMap influence(64);
    auto assignments = workers.assign(noDepot, rebuild, influence);
    check(assignments.size() == 5 &&
          std::ranges::all_of(assignments, [](const WorkerAssignment& assignment) {
              return assignment.job == WorkerJob::rebuild && assignment.baseId == 1;
          }), "workers without an owned depot did not stage at the funded rebuild site");
    noDepot.self.minerals = 399;
    assignments = workers.assign(noDepot, rebuild, influence);
    check(std::ranges::all_of(assignments, [](const WorkerAssignment& assignment) {
              return assignment.job == WorkerJob::rebuild && assignment.baseId == -1;
          }), "unfunded no-depot workers kept mining without a legal drop-off");

    GameState simultaneous;
    simultaneous.self.id = 1;
    simultaneous.self.units.push_back(unit(100, UnitKind::nexus, {320, 320}));
    simultaneous.self.units.push_back(unit(101, UnitKind::nexus, {1600, 320}));
    simultaneous.bases = {base(1, 1, {320, 320}, 5000, 0),
                          base(2, 1, {1600, 320}, 7000, 1280)};
    for (auto id = 0; id < 16; ++id)
        simultaneous.self.units.push_back(unit(id, UnitKind::probe, {320 + id, 320}));
    StrategicPlan mining;
    const auto multiAssignments = workers.assign(simultaneous, mining, influence);
    std::set<int> assignedBases;
    auto transfers = 0;
    for (const auto& assignment : multiAssignments) {
        assignedBases.insert(assignment.baseId);
        transfers += assignment.job == WorkerJob::transfer;
    }
    check(assignedBases == std::set<int>{1, 2},
          "simultaneous owned bases did not both receive mineral workers");
    check(transfers <= 8, "simultaneous-base balancing exceeded the staged transfer cap");

    GameState blocked;
    blocked.frame = 6000;
    blocked.self.id = 1;
    blocked.self.minerals = 700;
    blocked.self.units.push_back(unit(200, UnitKind::nexus, {320, 320}));
    for (auto id = 0; id < 8; ++id)
        blocked.self.units.push_back(unit(210 + id, UnitKind::probe, {320 + id, 320}));
    const auto owned = base(1, 1, {320, 320}, 2400, 0);
    const auto threatenedSite = base(2, -1, {960, 320}, 12000, 640);
    const auto alternative = base(3, -1, {1920, 320}, 14000, 1600);
    blocked.bases = {owned, threatenedSite, alternative};
    auto marine = unit(300, UnitKind::marine, threatenedSite.center);
    marine.ours = false;
    marine.visible = true;
    marine.groundWeapon.damage = 6;
    marine.groundWeapon.targetsGround = true;
    blocked.enemy.units.push_back(marine);

    StrategicPlan expand;
    expand.name = "income replacement";
    expand.expansionTarget = threatenedSite.center;
    expand.rallyPoint = threatenedSite.center;
    expand.estimatedMiningRunwayFrames = 4 * 60 * 24;
    expand.goals.push_back({GoalKind::expand, UnitKind::nexus, 2, 114, true,
                            "replace mining capacity"});
    ExpansionCoordinator coordinator;
    coordinator.update(expand, blocked, {});
    check(expand.expansionTarget == alternative.center && !expand.deferExpansion &&
          expand.name.find("alternate safe expansion") != std::string::npos,
          "blocked expansion did not switch to a reachable, safe alternative site");

    blocked.bases = {owned, threatenedSite};
    expand.name = "income replacement";
    expand.expansionTarget = threatenedSite.center;
    expand.rallyPoint = threatenedSite.center;
    expand.posture = Posture::hold;
    expand.desiredBases = 2;
    expand.desiredGasWorkers = 3;
    expand.goals.clear();
    coordinator.reset();
    coordinator.update(expand, blocked,
        {threatenedSite.center, true, 8 * 24});
    check(expand.expansionTarget == threatenedSite.center && expand.deferExpansion &&
          expand.posture == Posture::recover && expand.desiredBases == 1 &&
          expand.desiredGasWorkers == 0 && coordinator.releaseBuilder(),
          "blocked one-base expansion did not enter a deferred survival plan");
    check(std::ranges::any_of(expand.goals, [](const ProductionGoal& goal) {
              return goal.goal == GoalKind::train && goal.target == UnitKind::probe &&
                     goal.priority >= 118 && goal.blocking;
          }) && coordinator.reason().find("preserve") != std::string_view::npos,
          "blocked expansion did not preserve workers with an explicit recovery obligation");

    return errors == 0 ? 0 : 1;
}
