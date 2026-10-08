#include "protodd/Operations.hpp"
#include "protodd/Squads.hpp"
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

    GameState depletedEconomy;
    depletedEconomy.self.id = 1;
    depletedEconomy.self.minerals = 17;
    depletedEconomy.self.units.push_back(unit(150, UnitKind::nexus, {320, 320}));
    for (auto id = 0; id < 12; ++id)
        depletedEconomy.self.units.push_back(unit(160 + id, UnitKind::probe, {320 + id, 320}));
    const auto exhaustedHome = base(1, 1, {320, 320}, 0, 0);
    const auto nearbyNeutral = base(2, -1, {960, 320}, 10000, 640);
    const auto distantNeutral = base(3, -1, {1920, 320}, 12000, 1600);
    depletedEconomy.bases = {exhaustedHome, nearbyNeutral, distantNeutral};
    StrategicPlan recoverMining;
    recoverMining.expansionTarget = nearbyNeutral.center;
    WorkerManager depletedWorkers;
    const auto recoveryAssignments = depletedWorkers.assign(
        depletedEconomy, recoverMining, influence);
    const auto remoteMiners = std::ranges::count_if(
        recoveryAssignments, [&nearbyNeutral](const WorkerAssignment& assignment) {
            return assignment.job == WorkerJob::transfer &&
                   assignment.baseId == nearbyNeutral.id &&
                   assignment.targetPosition == nearbyNeutral.mineralLine;
        });
    check(remoteMiners == 8,
          "idle probes did not bootstrap income from a safe neutral mineral line after depletion");

    GameState rejectedExpansion;
    rejectedExpansion.frame = 9000;
    rejectedExpansion.self.id = 1;
    rejectedExpansion.self.units.push_back(unit(140, UnitKind::nexus, {320, 320}));
    const auto rejectedHome = base(1, 1, {320, 320}, 6000, 0);
    const auto invalidFootprint = base(2, -1, {960, 320}, 9000, 640);
    const auto safeAlternative = base(3, -1, {1920, 320}, 12000, 1600);
    const auto otherAlternative = base(4, -1, {2880, 320}, 11000, 2600);
    rejectedExpansion.bases = {rejectedHome, invalidFootprint, safeAlternative,
                               otherAlternative};
    StrategicPlan retryAnotherSite;
    retryAnotherSite.expansionTarget = invalidFootprint.center;
    retryAnotherSite.rallyPoint = invalidFootprint.center;
    retryAnotherSite.goals.push_back({GoalKind::expand, UnitKind::nexus, 2, 120,
                                      true, "replace mining capacity"});
    ExpansionCoordinator rejectedCoordinator;
    rejectedCoordinator.update(retryAnotherSite, rejectedExpansion,
        {invalidFootprint.center, false, 0, true});
    check(retryAnotherSite.expansionTarget == safeAlternative.center &&
              !retryAnotherSite.deferExpansion,
          "an engine-rejected Nexus footprint kept the planner retrying the same base");

    StrategicPlan retrySafeBuilder;
    retrySafeBuilder.expansionTarget = invalidFootprint.center;
    retrySafeBuilder.rallyPoint = invalidFootprint.center;
    ExpansionCoordinator noBuilderCoordinator;
    noBuilderCoordinator.update(retrySafeBuilder, rejectedExpansion,
        {invalidFootprint.center, false, 0, false, true});
    check(retrySafeBuilder.expansionTarget == safeAlternative.center &&
              !retrySafeBuilder.deferExpansion,
          "a site with no safe Probe route kept blocking a reachable alternative");

    StrategicPlan retryLegalPlacement;
    retryLegalPlacement.expansionTarget = invalidFootprint.center;
    retryLegalPlacement.rallyPoint = invalidFootprint.center;
    ExpansionCoordinator noPlacementCoordinator;
    noPlacementCoordinator.update(retryLegalPlacement, rejectedExpansion,
        {invalidFootprint.center, false, 0, false, false, true});
    check(retryLegalPlacement.expansionTarget == safeAlternative.center &&
              !retryLegalPlacement.deferExpansion,
          "a site without a legal Nexus footprint kept blocking a reachable alternative");

    StrategicPlan retryMultipleSites;
    retryMultipleSites.name = "income replacement";
    retryMultipleSites.expansionTarget = invalidFootprint.center;
    retryMultipleSites.rallyPoint = invalidFootprint.center;
    retryMultipleSites.desiredBases = 2;
    retryMultipleSites.desiredGasWorkers = 3;
    retryMultipleSites.estimatedMiningRunwayFrames = 4 * 60 * 24;
    ExpansionCoordinator multipleSiteCoordinator;
    multipleSiteCoordinator.update(retryMultipleSites, rejectedExpansion,
        {invalidFootprint.center, false, 0, true});
    check(retryMultipleSites.expansionTarget == safeAlternative.center,
          "first rejected Nexus site did not select the nearest safe alternative");
    multipleSiteCoordinator.update(retryMultipleSites, rejectedExpansion,
        {safeAlternative.center, false, 0, false, false, true});
    check(retryMultipleSites.expansionTarget == otherAlternative.center,
          "a second blocked expansion site was not excluded from the alternative search");
    multipleSiteCoordinator.update(retryMultipleSites, rejectedExpansion,
        {otherAlternative.center, false, 0, false, false, true});
    check(retryMultipleSites.deferExpansion &&
              retryMultipleSites.posture == Posture::recover &&
              retryMultipleSites.desiredBases == 1 &&
              retryMultipleSites.desiredGasWorkers == 0,
          "exhausting all legal expansion sites did not enter worker survival mode");

    StrategicPlan rememberUnmatchedBlocker;
    rememberUnmatchedBlocker.name = "income replacement";
    rememberUnmatchedBlocker.expansionTarget = invalidFootprint.center;
    rememberUnmatchedBlocker.rallyPoint = invalidFootprint.center;
    rememberUnmatchedBlocker.desiredBases = 2;
    rememberUnmatchedBlocker.estimatedMiningRunwayFrames = 4 * 60 * 24;
    ExpansionCoordinator unmatchedCoordinator;
    unmatchedCoordinator.update(rememberUnmatchedBlocker, rejectedExpansion,
        {safeAlternative.center, false, 0, false, false, true});
    check(rememberUnmatchedBlocker.expansionTarget == invalidFootprint.center &&
              !rememberUnmatchedBlocker.deferExpansion,
          "a blocker at a different site incorrectly cancelled the current expansion");
    rememberUnmatchedBlocker.expansionTarget = safeAlternative.center;
    rememberUnmatchedBlocker.rallyPoint = safeAlternative.center;
    unmatchedCoordinator.update(rememberUnmatchedBlocker, rejectedExpansion, {});
    check(rememberUnmatchedBlocker.expansionTarget != safeAlternative.center &&
              !rememberUnmatchedBlocker.deferExpansion,
          "a remembered blocker was retried when strategy returned to that site");

    StrategicPlan stableExpansion;
    stableExpansion.name = "steady economic growth";
    stableExpansion.expansionTarget = invalidFootprint.center;
    stableExpansion.rallyPoint = invalidFootprint.center;
    stableExpansion.desiredBases = 2;
    ExpansionCoordinator stableCoordinator;
    stableCoordinator.update(stableExpansion, rejectedExpansion, {});
    stableExpansion.expansionTarget = otherAlternative.center;
    stableExpansion.rallyPoint = otherAlternative.center;
    rejectedExpansion.frame++;
    stableCoordinator.update(stableExpansion, rejectedExpansion, {});
    check(stableExpansion.expansionTarget == invalidFootprint.center &&
              stableExpansion.rallyPoint == invalidFootprint.center,
          "a safe expansion mission and its army rally retargeted every strategy tick");
    stableCoordinator.update(stableExpansion, rejectedExpansion,
        {invalidFootprint.center, false, 0, false, true});
    check(stableExpansion.expansionTarget == safeAlternative.center &&
              stableExpansion.rallyPoint == safeAlternative.center,
          "expansion target hysteresis ignored a newly reported unsafe builder route");

    StrategicPlan escortUnsafeRoute;
    escortUnsafeRoute.name = "mirror expansion route";
    escortUnsafeRoute.posture = Posture::attack;
    escortUnsafeRoute.expansionTarget = invalidFootprint.center;
    escortUnsafeRoute.rallyPoint = invalidFootprint.center;
    escortUnsafeRoute.desiredBases = 2;
    ExpansionCoordinator routeEscortCoordinator;
    routeEscortCoordinator.update(escortUnsafeRoute, rejectedExpansion,
        {invalidFootprint.center, false, 0, false, false, false, true});
    check(escortUnsafeRoute.expansionTarget == invalidFootprint.center &&
              escortUnsafeRoute.expansionProtectionRequired &&
              !escortUnsafeRoute.deferExpansion &&
              SquadPlanner::shouldCoverExpansion(
                  rejectedExpansion, escortUnsafeRoute, false),
          "an unsafe builder route did not hold one expansion site and request army cover");
    escortUnsafeRoute.expansionTarget = otherAlternative.center;
    escortUnsafeRoute.rallyPoint = otherAlternative.center;
    routeEscortCoordinator.update(escortUnsafeRoute, rejectedExpansion, {});
    check(escortUnsafeRoute.expansionTarget == invalidFootprint.center &&
              escortUnsafeRoute.rallyPoint == invalidFootprint.center,
          "an unsafe expansion route caused the army to chase another site before retrying");
    rejectedExpansion.frame += 20 * 24 - 1;
    routeEscortCoordinator.update(escortUnsafeRoute, rejectedExpansion,
        {invalidFootprint.center, true, 0, false, false, false, true});
    check(escortUnsafeRoute.expansionTarget == invalidFootprint.center &&
              escortUnsafeRoute.expansionProtectionRequired,
          "an unsafe route was abandoned before its bounded army-cover window elapsed");
    rejectedExpansion.frame += 10 * 24;
    routeEscortCoordinator.update(escortUnsafeRoute, rejectedExpansion,
        {invalidFootprint.center, true, 0, false, false, false, true});
    check(escortUnsafeRoute.expansionTarget == invalidFootprint.center &&
              escortUnsafeRoute.expansionProtectionRequired,
          "an unsafe route timer survives a still-unsafe pending build retry");
    ++rejectedExpansion.frame;
    routeEscortCoordinator.update(escortUnsafeRoute, rejectedExpansion,
        {invalidFootprint.center, true, 0, false, false, false, true});
    check(escortUnsafeRoute.expansionTarget == safeAlternative.center &&
              !escortUnsafeRoute.deferExpansion &&
              !escortUnsafeRoute.expansionProtectionRequired,
          "a route that stays unsafe beyond the cover window did not switch to a safe alternative");

    auto intermittentlyDeferredState = rejectedExpansion;
    intermittentlyDeferredState.frame = 25000;
    StrategicPlan intermittentlyDeferredPlan;
    intermittentlyDeferredPlan.name = "pressure-sensitive expansion";
    intermittentlyDeferredPlan.expansionTarget = invalidFootprint.center;
    intermittentlyDeferredPlan.rallyPoint = invalidFootprint.center;
    intermittentlyDeferredPlan.desiredBases = 2;
    ExpansionCoordinator intermittentlyDeferredCoordinator;
    intermittentlyDeferredCoordinator.update(intermittentlyDeferredPlan,
        intermittentlyDeferredState,
        {invalidFootprint.center, false, 0, false, false, false, true});
    intermittentlyDeferredPlan.deferExpansion = true;
    intermittentlyDeferredState.frame += 24;
    intermittentlyDeferredCoordinator.update(intermittentlyDeferredPlan,
        intermittentlyDeferredState,
        {invalidFootprint.center, false, 0, false, false, false, true});
    intermittentlyDeferredPlan.deferExpansion = false;
    intermittentlyDeferredState.frame += 30 * 24 - 23;
    intermittentlyDeferredCoordinator.update(intermittentlyDeferredPlan,
        intermittentlyDeferredState,
        {invalidFootprint.center, false, 0, false, false, false, true});
    check(intermittentlyDeferredPlan.expansionTarget == safeAlternative.center &&
              !intermittentlyDeferredPlan.deferExpansion,
          "a brief strategic expansion deferral restarted a continuously unsafe route deadline");

    auto routeFeedbackGapState = rejectedExpansion;
    routeFeedbackGapState.frame = 27000;
    StrategicPlan routeFeedbackGapPlan;
    routeFeedbackGapPlan.name = "intermittent unsafe route";
    routeFeedbackGapPlan.expansionTarget = invalidFootprint.center;
    routeFeedbackGapPlan.rallyPoint = invalidFootprint.center;
    routeFeedbackGapPlan.desiredBases = 2;
    ExpansionCoordinator routeFeedbackGapCoordinator;
    routeFeedbackGapCoordinator.update(routeFeedbackGapPlan, routeFeedbackGapState,
        {invalidFootprint.center, false, 0, false, false, false, true});
    routeFeedbackGapPlan.expansionTarget = otherAlternative.center;
    routeFeedbackGapPlan.rallyPoint = otherAlternative.center;
    routeFeedbackGapState.frame += 24;
    routeFeedbackGapCoordinator.update(routeFeedbackGapPlan, routeFeedbackGapState, {});
    check(routeFeedbackGapPlan.expansionTarget == invalidFootprint.center &&
              routeFeedbackGapPlan.rallyPoint == invalidFootprint.center &&
              routeFeedbackGapPlan.expansionProtectionRequired,
          "a one-tick unsafe-route feedback gap dropped army cover and changed its rally target");
    routeFeedbackGapState.frame += 30 * 24 - 25;
    routeFeedbackGapCoordinator.update(routeFeedbackGapPlan, routeFeedbackGapState,
        {invalidFootprint.center, false, 0, false, false, false, true});
    check(routeFeedbackGapPlan.expansionTarget == invalidFootprint.center &&
              routeFeedbackGapPlan.expansionProtectionRequired,
          "a one-tick unsafe-route feedback gap restarted the bounded army-cover timer");
    ++routeFeedbackGapState.frame;
    routeFeedbackGapCoordinator.update(routeFeedbackGapPlan, routeFeedbackGapState,
        {invalidFootprint.center, false, 0, false, false, false, true});
    check(routeFeedbackGapPlan.expansionTarget == safeAlternative.center &&
              !routeFeedbackGapPlan.expansionProtectionRequired,
          "a route unsafe beyond its original deadline did not switch to a safe alternative");

    ExpansionCoordinator recoveredRouteCoordinator;
    auto recoveredRoute = rejectedExpansion;
    recoveredRoute.frame = 20000;
    StrategicPlan recoveredRoutePlan;
    recoveredRoutePlan.expansionTarget = invalidFootprint.center;
    recoveredRoutePlan.rallyPoint = invalidFootprint.center;
    recoveredRoutePlan.desiredBases = 2;
    recoveredRouteCoordinator.update(recoveredRoutePlan, recoveredRoute,
        {invalidFootprint.center, false, 0, false, false, false, true});
    recoveredRoute.frame += 6 * 24;
    recoveredRouteCoordinator.update(recoveredRoutePlan, recoveredRoute,
        {invalidFootprint.center, true, 0, false, false, false, false});
    recoveredRoute.frame += 6 * 24;
    recoveredRouteCoordinator.update(recoveredRoutePlan, recoveredRoute,
        {invalidFootprint.center, false, 0, false, false, false, true});
    recoveredRoute.frame += 30 * 24 - 1;
    recoveredRouteCoordinator.update(recoveredRoutePlan, recoveredRoute,
        {invalidFootprint.center, false, 0, false, false, false, true});
    check(recoveredRoutePlan.expansionTarget == invalidFootprint.center &&
              recoveredRoutePlan.expansionProtectionRequired,
          "a safe pending route lasting over the recovery grace period restarts the unsafe-route window");

    auto noSafeRouteState = rejectedExpansion;
    noSafeRouteState.frame = 12000;
    noSafeRouteState.bases = {rejectedHome, invalidFootprint};
    StrategicPlan noSafeRoutePlan;
    noSafeRoutePlan.name = "income replacement";
    noSafeRoutePlan.expansionTarget = invalidFootprint.center;
    noSafeRoutePlan.rallyPoint = invalidFootprint.center;
    noSafeRoutePlan.desiredBases = 2;
    noSafeRoutePlan.desiredGasWorkers = 3;
    noSafeRoutePlan.estimatedMiningRunwayFrames = 4 * 60 * 24;
    ExpansionCoordinator noSafeRouteCoordinator;
    noSafeRouteCoordinator.update(noSafeRoutePlan, noSafeRouteState,
        {invalidFootprint.center, false, 0, false, false, false, true});
    noSafeRouteState.frame += 30 * 24;
    noSafeRouteCoordinator.update(noSafeRoutePlan, noSafeRouteState,
        {invalidFootprint.center, false, 0, false, false, false, true});
    check(noSafeRoutePlan.deferExpansion &&
              noSafeRoutePlan.posture == Posture::recover &&
              noSafeRoutePlan.desiredBases == 1 &&
              noSafeRoutePlan.desiredGasWorkers == 0,
          "an unsafe-only expansion map did not enter the worker-survival plan after the route deadline");
    noSafeRouteState.frame += 120;
    StrategicPlan retryUnsafeOnly;
    retryUnsafeOnly.name = "income replacement";
    retryUnsafeOnly.expansionTarget = invalidFootprint.center;
    retryUnsafeOnly.rallyPoint = invalidFootprint.center;
    retryUnsafeOnly.desiredBases = 2;
    retryUnsafeOnly.desiredGasWorkers = 3;
    retryUnsafeOnly.estimatedMiningRunwayFrames = 4 * 60 * 24;
    noSafeRouteCoordinator.update(retryUnsafeOnly, noSafeRouteState,
        {invalidFootprint.center, false, 0, false, false, false, true});
    check(retryUnsafeOnly.deferExpansion && retryUnsafeOnly.desiredBases == 1 &&
              retryUnsafeOnly.desiredGasWorkers == 0,
          "a still-active unsafe-route blocker immediately undid the worker-survival plan");

    StrategicPlan keepDifferentTarget;
    keepDifferentTarget.expansionTarget = otherAlternative.center;
    keepDifferentTarget.rallyPoint = otherAlternative.center;
    keepDifferentTarget.desiredBases = 2;
    ExpansionCoordinator unmatchedUnsafeRouteCoordinator;
    unmatchedUnsafeRouteCoordinator.update(keepDifferentTarget, rejectedExpansion,
        {invalidFootprint.center, false, 0, false, false, false, true});
    check(keepDifferentTarget.expansionTarget == otherAlternative.center &&
              !keepDifferentTarget.expansionProtectionRequired,
          "an unrelated stale unsafe-route blocker overrode the selected expansion site");

    GameState footprintClearing;
    const Position buildSite{1280, 640};
    const Position assembly{1472, 640};
    footprintClearing.self.units.push_back(unit(180, UnitKind::dragoon,
                                                 buildSite));
    footprintClearing.self.units.push_back(unit(181, UnitKind::dragoon,
                                                 assembly));
    footprintClearing.self.units.push_back(unit(182, UnitKind::dragoon,
                                                 {1792, 640}));
    const auto noBuildOrders = clearExpansionFootprint(
        footprintClearing, buildSite, assembly, false);
    const auto activeBuildOrders = clearExpansionFootprint(
        footprintClearing, buildSite, assembly, true);
    check(noBuildOrders.empty(),
          "an unstarted expansion redirected units to clear its footprint");
    check(activeBuildOrders.size() == 1 && activeBuildOrders.front().actor == 180,
          "expansion clearing moved distant units or reissued an order to an assembled unit");

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

    blocked.bases = {owned, threatenedSite};
    expand.name = "income replacement";
    expand.expansionTarget = threatenedSite.center;
    expand.rallyPoint = threatenedSite.center;
    expand.estimatedMiningRunwayFrames = 4 * 60 * 24;
    expand.posture = Posture::hold;
    expand.desiredBases = 2;
    expand.desiredGasWorkers = 3;
    expand.goals.clear();
    noBuilderCoordinator.reset();
    noBuilderCoordinator.update(expand, blocked,
        {threatenedSite.center, false, 0, false, true});
    check(expand.deferExpansion && expand.expansionTarget == threatenedSite.center &&
          expand.posture == Posture::recover && expand.desiredBases == 1 &&
          expand.desiredGasWorkers == 0,
          "no safe builder and no alternative failed to enter the mining-survival plan");

    expand.name = "income replacement";
    expand.expansionTarget = threatenedSite.center;
    expand.rallyPoint = threatenedSite.center;
    expand.posture = Posture::hold;
    expand.desiredBases = 2;
    expand.desiredGasWorkers = 3;
    expand.goals.clear();
    noPlacementCoordinator.reset();
    noPlacementCoordinator.update(expand, blocked,
        {threatenedSite.center, false, 0, false, false, true});
    check(expand.deferExpansion && expand.expansionTarget == threatenedSite.center &&
          expand.posture == Posture::recover && expand.desiredBases == 1 &&
          expand.desiredGasWorkers == 0,
          "no legal Nexus footprint and no alternative failed to enter the mining-survival plan");

    auto unusualGeometry = blocked;
    auto island = base(4, -1, {2464, 320}, 12000, -1);
    island.island = true;
    auto disconnected = base(5, -1, {2784, 320}, 12000, -1);
    auto depleted = base(6, -1, {3104, 320}, 3000, 1280);
    auto blockedFootprint = base(7, -1, {3424, 320}, 9000, 1440);
    blockedFootprint.depotFootprintAvailable = false;
    unusualGeometry.bases = {owned, threatenedSite, island, disconnected,
                             depleted, blockedFootprint};
    StrategicPlan noViableRoute;
    noViableRoute.name = "geometry recovery";
    noViableRoute.expansionTarget = threatenedSite.center;
    noViableRoute.rallyPoint = threatenedSite.center;
    noViableRoute.estimatedMiningRunwayFrames = 4 * 60 * 24;
    coordinator.reset();
    coordinator.update(noViableRoute, unusualGeometry, {});
    check(noViableRoute.deferExpansion &&
              noViableRoute.expansionTarget == threatenedSite.center &&
              noViableRoute.desiredBases == 1,
          "island, disconnected, depleted and blocked-footprint sites do not sustain a false expansion commitment");

    auto reachableMineralOnly = base(8, -1, {3744, 320}, 8000, 1760);
    unusualGeometry.bases.push_back(reachableMineralOnly);
    StrategicPlan mineralOnlyRecovery = noViableRoute;
    mineralOnlyRecovery.deferExpansion = false;
    coordinator.reset();
    coordinator.update(mineralOnlyRecovery, unusualGeometry, {});
    check(!mineralOnlyRecovery.deferExpansion &&
              mineralOnlyRecovery.expansionTarget == reachableMineralOnly.center,
          "a reachable mineral-only base is accepted after excluding unusable geometry");

    GameState warpingSite;
    warpingSite.frame = 11000;
    warpingSite.self.id = 1;
    warpingSite.enemy.id = 2;
    warpingSite.self.units.push_back(unit(400, UnitKind::nexus, {320, 320}));
    warpingSite.self.units.push_back(unit(401, UnitKind::nexus, {960, 320}, false));
    for (int id = 0; id < 12; ++id) {
        auto dragoon = unit(410 + id, UnitKind::dragoon, {320, 320});
        dragoon.hitPoints = dragoon.maxHitPoints = 100;
        dragoon.shields = dragoon.maxShields = 80;
        dragoon.groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
        warpingSite.self.units.push_back(dragoon);
    }
    const auto homeBase = base(1, 1, {320, 320}, 6000, 0);
    const auto forwardSite = base(2, -1, {960, 320}, 12000, 640);
    warpingSite.bases = {homeBase, forwardSite};
    auto siteMarine = unit(500, UnitKind::marine, {forwardSite.center.x + 200,
                                                 forwardSite.center.y});
    siteMarine.ours = false;
    siteMarine.visible = true;
    siteMarine.lastSeen = warpingSite.frame;
    siteMarine.hitPoints = siteMarine.maxHitPoints = 40;
    siteMarine.groundWeapon = {6, 15, 0, 128, DamageType::normal, false, true};
    warpingSite.enemy.units.push_back(siteMarine);
    StrategicPlan warpingPlan;
    warpingPlan.name = "forward economy";
    warpingPlan.posture = Posture::attack;
    warpingPlan.expansionTarget = forwardSite.center;
    warpingPlan.rallyPoint = forwardSite.center;
    warpingPlan.desiredBases = 2;
    ExpansionCoordinator warpingCoordinator;
    warpingCoordinator.update(warpingPlan, warpingSite, {});
    check(warpingPlan.expansionProtectionRequired && !warpingPlan.deferExpansion &&
              !warpingCoordinator.releaseBuilder() &&
              warpingPlan.expansionTarget == forwardSite.center &&
              SquadPlanner::shouldCoverExpansion(warpingSite, warpingPlan, false),
          "a threatened started Nexus keeps its funding and requests mobile cover during attack posture");
    for (int id = 0; id < 4; ++id)
        warpingSite.self.units[static_cast<std::size_t>(id + 2)].position =
            {forwardSite.center.x + 100, forwardSite.center.y};
    warpingCoordinator.update(warpingPlan, warpingSite, {});
    check(!warpingPlan.expansionProtectionRequired && !warpingPlan.deferExpansion &&
              !warpingCoordinator.releaseBuilder(),
          "sufficient local defenders let the army continue its attack while the Nexus warps");

    auto lostSite = warpingSite;
    std::erase_if(lostSite.self.units, [](const UnitSnapshot& own) {
        return own.id == 401;
    });
    for (auto& own : lostSite.self.units) {
        if (own.kind == UnitKind::dragoon) own.position = homeBase.center;
    }
    lostSite.self.minerals = 700;
    StrategicPlan lostExpansion = warpingPlan;
    lostExpansion.name = "income replacement";
    lostExpansion.posture = Posture::attack;
    lostExpansion.expansionProtectionRequired = false;
    lostExpansion.estimatedMiningRunwayFrames = 4 * 60 * 24;
    ExpansionCoordinator lostCoordinator;
    lostCoordinator.update(lostExpansion, lostSite,
        {forwardSite.center, true, 8 * 24});
    check(lostCoordinator.releaseBuilder() && lostExpansion.deferExpansion &&
              lostExpansion.posture == Posture::recover && lostExpansion.sustainEconomy &&
              lostExpansion.desiredBases == 1 && lostExpansion.desiredWorkers >= 12 &&
              !SquadPlanner::shouldCoverExpansion(lostSite, lostExpansion, false),
          "a lost unsafe site releases its stalled builder and army while protecting the last mining base");

    return errors == 0 ? 0 : 1;
}
