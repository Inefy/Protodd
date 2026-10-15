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

    {
        auto defenseState = baseState();
        defenseState.self.units.push_back(unit(4, UnitKind::probe));
        auto firstCannon = unit(5, UnitKind::photonCannon);
        firstCannon.completed = false;
        defenseState.self.units.push_back(firstCannon);
        StrategicPlan defensePlan;
        defensePlan.posture = Posture::defend;
        defensePlan.goals = {
            {GoalKind::build, UnitKind::photonCannon, 2, 100, true, "complete the defensive pair"},
            {GoalKind::build, UnitKind::gateway, 1, 99, true, "start subsequent army production"},
        };
        auto pendingDefense = std::vector<BuildBlockerFeedback>{{
            UnitKind::photonCannon, {}, BuildBlockerReason::routePending, 103, 100}};
        MacroPlanner defensePlanner;
        ResourceLedger defenseLedger{150, 0};
        const auto defenseActions = defensePlanner.reconcile(
            defenseState, defensePlan, defenseLedger, pendingDefense);
        check(defenseLedger.protectedMinerals == 150 &&
              std::ranges::none_of(defenseActions, [](const MacroAction& action) {
                  return action.target == UnitKind::gateway && action.reserved;
              }), "brief emergency Cannon route work preserves its bank ahead of a Gateway");
        check(std::ranges::any_of(defenseActions, [](const MacroAction& action) {
                  return action.target == UnitKind::photonCannon && !action.reserved &&
                      !action.executable && action.pendingCriticalBankHoldFrames == 120;
              }), "defensive bank protection does not grant unknown-route construction permission");
        defenseState.frame = 219;
        pendingDefense[0].observedAt = 219;
        pendingDefense[0].retryAt = 222;
        ResourceLedger lastDefenseGrace{150, 0};
        (void)defensePlanner.reconcile(defenseState, defensePlan, lastDefenseGrace, pendingDefense);
        check(lastDefenseGrace.protectedMinerals == 150,
              "fresh route feedback preserves only the original emergency grace");
        defenseState.frame = 220;
        pendingDefense[0].observedAt = 220;
        pendingDefense[0].retryAt = 223;
        ResourceLedger expiredDefenseGrace{150, 0};
        const auto afterDefenseGrace = defensePlanner.reconcile(
            defenseState, defensePlan, expiredDefenseGrace, pendingDefense);
        check(expiredDefenseGrace.protectedMinerals == 0 &&
              std::ranges::any_of(afterDefenseGrace, [](const MacroAction& action) {
                  return action.target == UnitKind::gateway && action.reserved;
              }), "continuously pending defense cannot freeze the economy beyond five seconds");
        defenseState.frame = 221;
        ResourceLedger readyDefenseLedger{150, 0};
        const auto readyDefense = defensePlanner.reconcile(defenseState, defensePlan, readyDefenseLedger);
        check(std::ranges::any_of(readyDefense, [](const MacroAction& action) {
                  return action.target == UnitKind::photonCannon && action.reserved && action.executable;
              }), "observed route readiness funds the required second Cannon immediately");
        auto higherPriorityDefensePlan = defensePlan;
        higherPriorityDefensePlan.goals.push_back({GoalKind::train, UnitKind::probe, 2, 130, true,
                                                   "critical worker recovery"});
        ResourceLedger higherPriorityDefenseLedger{150, 0};
        const auto higherPriorityDefenseActions = MacroPlanner{}.reconcile(
            defenseState, higherPriorityDefensePlan, higherPriorityDefenseLedger, pendingDefense);
        check(higherPriorityDefenseLedger.protectedMinerals == 100 &&
              std::ranges::any_of(higherPriorityDefenseActions, [](const MacroAction& action) {
                  return action.target == UnitKind::probe && action.reserved;
              }), "higher-priority recovery spends first while the sorted defense goal protects the remainder");
        auto unsafeDefense = pendingDefense;
        unsafeDefense[0].reason = BuildBlockerReason::unsafeRoute;
        ResourceLedger unsafeDefenseLedger{150, 0};
        (void)MacroPlanner{}.reconcile(defenseState, defensePlan, unsafeDefenseLedger, unsafeDefense);
        check(unsafeDefenseLedger.protectedMinerals == 0,
              "a known unsafe defense route releases the bank without a grace hold");
        auto optionalDefensePlan = defensePlan;
        optionalDefensePlan.goals[0].blocking = false;
        ResourceLedger optionalDefenseLedger{150, 0};
        (void)MacroPlanner{}.reconcile(defenseState, optionalDefensePlan, optionalDefenseLedger, pendingDefense);
        check(optionalDefenseLedger.protectedMinerals == 0,
              "optional static coverage never receives emergency bank protection");
    }

    {
        auto replacementState = baseState();
        replacementState.frame = 6000;
        replacementState.enemy.race = Race::protoss;
        replacementState.self.supplyTotal = 100;
        replacementState.self.units.push_back(unit(4, UnitKind::gateway));
        replacementState.self.units.push_back(unit(5, UnitKind::gateway));
        replacementState.self.units.push_back(unit(6, UnitKind::cyberneticsCore));
        StrategicPlan replacementPlan;
        replacementPlan.posture = Posture::defend;
        replacementPlan.prioritizeReinforcements = true;
        replacementPlan.goals = {{GoalKind::build, UnitKind::photonCannon, 1, 120, true,
                                  "first emergency anchor"}};
        MacroPlanner replacementPlanner;
        const auto funds = [](const std::vector<MacroAction>& actions, const UnitKind kind) {
            return std::ranges::any_of(actions, [kind](const MacroAction& action) {
                return action.target == kind && action.reserved && action.executable;
            });
        };
        ResourceLedger firstBank{150, 100};
        auto replacementActions = replacementPlanner.reconcile(
            replacementState, replacementPlan, firstBank);
        check(funds(replacementActions, UnitKind::photonCannon) &&
              !funds(replacementActions, UnitKind::dragoon),
              "the first emergency Cannon retains its one-time funding boost");
        replacementState.frame += 3;
        ResourceLedger retryBank{150, 100};
        replacementActions = replacementPlanner.reconcile(
            replacementState, replacementPlan, retryBank);
        check(funds(replacementActions, UnitKind::photonCannon),
              "a reservation without observed construction cannot consume the first-Cannon boost");
        auto warpingCannon = unit(7, UnitKind::photonCannon);
        warpingCannon.completed = false;
        replacementState.self.units.push_back(warpingCannon);
        replacementState.frame += 3;
        ResourceLedger warpingBank{150, 100};
        replacementActions = replacementPlanner.reconcile(
            replacementState, replacementPlan, warpingBank);
        check(funds(replacementActions, UnitKind::dragoon),
              "a paid Cannon releases the next bank to available mobile production");
        replacementState.self.units.pop_back();
        replacementState.frame += 231;
        ResourceLedger lossBank{150, 100};
        replacementActions = replacementPlanner.reconcile(
            replacementState, replacementPlan, lossBank);
        check(funds(replacementActions, UnitKind::dragoon) &&
              !funds(replacementActions, UnitKind::photonCannon),
              "a destroyed unfinished Cannon cannot repeatedly take the first-Dragoon bank");
        replacementState.frame += 3;
        ResourceLedger surplusBank{400, 100};
        replacementActions = replacementPlanner.reconcile(
            replacementState, replacementPlan, surplusBank);
        check(funds(replacementActions, UnitKind::photonCannon) &&
              std::ranges::count_if(replacementActions, [](const MacroAction& action) {
                  return action.target == UnitKind::dragoon && action.reserved;
              }) == 2,
              "replacement defense still uses surplus after one cycle per available Gateway");
        replacementState.self.busyProducers = {UnitKind::gateway, UnitKind::gateway};
        replacementState.frame += 3;
        ResourceLedger busyBank{150, 100};
        replacementActions = replacementPlanner.reconcile(
            replacementState, replacementPlan, busyBank);
        check(funds(replacementActions, UnitKind::photonCannon),
              "busy Gateways leave replacement Cannon funding available");
        replacementState.self.busyProducers.clear();
        replacementState.frame = 100;
        ResourceLedger resetBank{150, 100};
        replacementActions = replacementPlanner.reconcile(
            replacementState, replacementPlan, resetBank);
        check(funds(replacementActions, UnitKind::photonCannon) &&
              !funds(replacementActions, UnitKind::dragoon),
              "a new game restores the first-Cannon funding boost");
    }

    plan.goals = {
        {GoalKind::build,UnitKind::nexus,2,120,true,"replacement mining route pending"},
        {GoalKind::train,UnitKind::probe,2,110,true,"keep producing during route work"},
    };
    const std::vector<BuildBlockerFeedback> pendingRoute{{
        UnitKind::nexus,{},BuildBlockerReason::routePending,103}};
    ResourceLedger pendingRouteLedger{350,0};
    const auto pendingRouteActions = MacroPlanner{}.reconcile(state,plan,pendingRouteLedger,pendingRoute);
    check(pendingRouteLedger.protectedMinerals == 0 &&
        std::ranges::any_of(pendingRouteActions, [](const MacroAction& action) {
            return action.target == UnitKind::probe && action.reserved && action.executable;
        }), "unfinished expansion routing releases the sub-cost bank to survival production");
    state.frame = 103;
    ResourceLedger settledRouteLedger{400,0};
    const auto settledRouteActions = MacroPlanner{}.reconcile(state,plan,settledRouteLedger,pendingRoute);
    check(std::ranges::any_of(settledRouteActions, [](const MacroAction& action) {
            return action.target == UnitKind::nexus && action.reserved && action.executable;
        }), "short pending-route feedback retries construction without an unsafe-site cooldown");
    // A retry deadline permits fresh route work, not spending on a route
    // whose latest observed state remains unknown.
    const ConstructionTaskSite routeSite{70,2,{1440,320}};
    const ConstructionTaskSite otherRouteSite{71,3,{1440,1600}};
    auto scopedPlan = plan;
    scopedPlan.goals[0].goal = GoalKind::expand;
    scopedPlan.goals[0].constructionSite = routeSite;
    const std::vector<BuildBlockerFeedback> freshPendingRoute{{
        UnitKind::nexus,routeSite,BuildBlockerReason::routePending,103,100}};
    ResourceLedger freshRouteLedger{400,0};
    const auto freshRouteActions = MacroPlanner{}.reconcile(state,scopedPlan,freshRouteLedger,freshPendingRoute);
    check(std::ranges::any_of(freshRouteActions, [](const MacroAction& action) {
              return action.target == UnitKind::nexus && !action.reserved && !action.executable;
          }), "fresh unknown route cannot claim the expansion bank when its retry becomes due");
    check(std::ranges::any_of(freshRouteActions, [](const MacroAction& action) {
              return action.target == UnitKind::probe && action.reserved && action.executable;
          }) && freshRouteLedger.reservedMinerals == 50 && freshRouteLedger.protectedMinerals == 0,
          "fresh pending admission leaves funds for production across the retry boundary");
    ResourceLedger readyRouteLedger{400,0};
    const auto readyRouteActions = MacroPlanner{}.reconcile(state,scopedPlan,readyRouteLedger);
    check(std::ranges::any_of(readyRouteActions, [](const MacroAction& action) {
              return action.target == UnitKind::nexus && action.reserved && action.executable;
          }), "observed route readiness restores the expansion allocation immediately");
    const std::vector<ConstructionTaskSite> ownedRoute{routeSite};
    ResourceLedger ownedRouteLedger{400,0};
    const auto ownedRouteActions = MacroPlanner{}.reconcile(state,scopedPlan,ownedRouteLedger,freshPendingRoute,ownedRoute);
    check(std::ranges::any_of(ownedRouteActions, [](const MacroAction& action) {
              return action.target == UnitKind::nexus && action.reserved && action.executable;
          }), "a live construction lease retains funding despite expired pre-admission route feedback");
    auto unrelatedPendingRoute = freshPendingRoute;
    unrelatedPendingRoute[0].constructionSite = otherRouteSite;
    ResourceLedger unrelatedRouteLedger{400,0};
    const auto unrelatedRouteActions = MacroPlanner{}.reconcile(state,scopedPlan,unrelatedRouteLedger,unrelatedPendingRoute);
    check(std::ranges::any_of(unrelatedRouteActions, [](const MacroAction& action) {
              return action.target == UnitKind::nexus && action.reserved && action.executable;
          }), "unknown routing at another site does not withhold the requested site's funding");
    // A temporary route delay must not spend the last replacement-base
    // minerals on optional composition while owned mining is running out.
    // This uses the actual pressure repeat's 110-mineral/Zealot cost, while
    // ordinary unknown routing above remains free to fund normal production.
    auto criticalState = baseState();
    criticalState.self.units.push_back(unit(4, UnitKind::probe));
    criticalState.self.units.push_back(unit(5, UnitKind::gateway));
    StrategicPlan criticalPlan;
    criticalPlan.desiredBases = 2;
    criticalPlan.estimatedMiningRunwayFrames = 500;
    criticalPlan.composition = {{UnitKind::zealot, 1.0}};
    criticalPlan.goals = {{GoalKind::expand, UnitKind::nexus, 2, 120, true,
                          "replace dying mining capacity", TechnologyKind::none,
                          false, false, routeSite}};
    auto criticalPending = freshPendingRoute;
    MacroPlanner criticalPlanner;
    ResourceLedger criticalLedger{110, 0};
    const auto criticalActions = criticalPlanner.reconcile(
        criticalState, criticalPlan, criticalLedger, criticalPending);
    check(criticalLedger.protectedMinerals == 60 &&
          std::ranges::none_of(criticalActions, [](const MacroAction& action) {
              return action.target == UnitKind::zealot && action.reserved;
          }), "critical pending route protects the sub-cost bank from optional composition");
    check(std::ranges::any_of(criticalActions, [](const MacroAction& action) {
              return action.target == UnitKind::nexus && !action.reserved && !action.executable;
          }), "critical bank protection grants no unknown-route construction permission");

    criticalState.frame = 103;
    criticalPending[0].observedAt = 103;
    criticalPending[0].retryAt = 106;
    auto workerCriticalPlan = criticalPlan;
    workerCriticalPlan.goals.push_back({GoalKind::train, UnitKind::probe, 2, 93, true,
                                       "continue worker production"});
    ResourceLedger workerCriticalLedger{400, 0};
    const auto workerCriticalActions = criticalPlanner.reconcile(
        criticalState, workerCriticalPlan, workerCriticalLedger, criticalPending);
    check(workerCriticalLedger.protectedMinerals == 350 &&
          std::ranges::any_of(workerCriticalActions, [](const MacroAction& action) {
              return action.target == UnitKind::probe && action.reserved && action.executable;
          }) && std::ranges::none_of(workerCriticalActions, [](const MacroAction& action) {
              return action.target == UnitKind::zealot && action.reserved;
          }), "critical pending route leaves one worker cycle but retains the replacement bank");

    criticalState.frame = 219;
    criticalPending[0].observedAt = 219;
    criticalPending[0].retryAt = 222;
    ResourceLedger renewedCriticalLedger{110, 0};
    (void)criticalPlanner.reconcile(criticalState, criticalPlan, renewedCriticalLedger, criticalPending);
    check(renewedCriticalLedger.protectedMinerals == 60,
          "critical pending bank grace survives until its original boundary");
    criticalState.frame = 220;
    criticalPending[0].observedAt = 220;
    criticalPending[0].retryAt = 223;
    ResourceLedger expiredCriticalLedger{110, 0};
    const auto expiredCriticalActions = criticalPlanner.reconcile(
        criticalState, criticalPlan, expiredCriticalLedger, criticalPending);
    check(expiredCriticalLedger.protectedMinerals == 0 &&
          std::ranges::any_of(expiredCriticalActions, [](const MacroAction& action) {
              return action.target == UnitKind::zealot && action.reserved;
          }), "continuously refreshed unknown routing cannot renew critical bank protection");

    criticalState.frame = 100;
    criticalPending[0].observedAt = 100;
    criticalPending[0].retryAt = 103;
    MacroPlanner switchedCriticalPlanner;
    ResourceLedger beforeSwitchLedger{110, 0};
    (void)switchedCriticalPlanner.reconcile(criticalState, criticalPlan, beforeSwitchLedger, criticalPending);
    auto switchedCriticalPlan = criticalPlan;
    switchedCriticalPlan.goals[0].constructionSite = otherRouteSite;
    auto switchedCriticalPending = criticalPending;
    switchedCriticalPending[0].constructionSite = otherRouteSite;
    criticalState.frame = 104;
    switchedCriticalPending[0].observedAt = 104;
    switchedCriticalPending[0].retryAt = 107;
    ResourceLedger switchCriticalLedger{110, 0};
    (void)switchedCriticalPlanner.reconcile(criticalState, switchedCriticalPlan, switchCriticalLedger, switchedCriticalPending);
    check(switchCriticalLedger.protectedMinerals == 0,
          "changing critical construction site releases its bank on the switch pass");
    criticalState.frame = 105;
    switchedCriticalPending[0].observedAt = 105;
    switchedCriticalPending[0].retryAt = 108;
    ResourceLedger stableSwitchedLedger{110, 0};
    (void)switchedCriticalPlanner.reconcile(criticalState, switchedCriticalPlan, stableSwitchedLedger, switchedCriticalPending);
    check(stableSwitchedLedger.protectedMinerals == 60,
          "a stable replacement site can use the remaining original bank grace");
    criticalState.frame = 220;
    switchedCriticalPending[0].observedAt = 220;
    switchedCriticalPending[0].retryAt = 223;
    ResourceLedger switchedExpiredLedger{110, 0};
    (void)switchedCriticalPlanner.reconcile(criticalState, switchedCriticalPlan, switchedExpiredLedger, switchedCriticalPending);
    check(switchedExpiredLedger.protectedMinerals == 0,
          "site rotation cannot extend the original critical pending-bank boundary");

    criticalState.frame = 100;
    auto emergencyCriticalPlan = criticalPlan;
    emergencyCriticalPlan.goals.push_back({GoalKind::train, UnitKind::zealot, 1, 130, true,
                                          "urgent army reinforcement"});
    ResourceLedger emergencyCriticalLedger{110, 0};
    const auto emergencyCriticalActions = MacroPlanner{}.reconcile(
        criticalState, emergencyCriticalPlan, emergencyCriticalLedger, criticalPending);
    check(emergencyCriticalLedger.protectedMinerals == 0 &&
          std::ranges::any_of(emergencyCriticalActions, [](const MacroAction& action) {
              return action.target == UnitKind::zealot && action.priority == 130 && action.reserved;
          }), "higher-priority emergency production spends ahead of critical pending bank protection");
    auto attackedCriticalState = criticalState;
    attackedCriticalState.self.units.back().underAttack = false;
    attackedCriticalState.self.units[3].underAttack = true;
    ResourceLedger attackedCriticalLedger{110, 0};
    (void)MacroPlanner{}.reconcile(attackedCriticalState, criticalPlan, attackedCriticalLedger, criticalPending);
    check(attackedCriticalLedger.protectedMinerals == 0,
          "a worker breach releases critical pending expansion savings");
    auto deferredCriticalPlan = criticalPlan;
    deferredCriticalPlan.deferExpansion = true;
    ResourceLedger deferredCriticalLedger{110, 0};
    (void)MacroPlanner{}.reconcile(criticalState, deferredCriticalPlan, deferredCriticalLedger, criticalPending);
    check(deferredCriticalLedger.protectedMinerals == 0,
          "explicitly deferred expansion releases critical pending savings");
    auto ordinaryCriticalPlan = criticalPlan;
    ordinaryCriticalPlan.estimatedMiningRunwayFrames = -1;
    ResourceLedger ordinaryCriticalLedger{110, 0};
    (void)MacroPlanner{}.reconcile(criticalState, ordinaryCriticalPlan, ordinaryCriticalLedger, criticalPending);
    check(ordinaryCriticalLedger.protectedMinerals == 0,
          "unknown economic runway does not authorize a pending bank hold");
    auto unsafeCriticalPending = criticalPending;
    unsafeCriticalPending[0].reason = BuildBlockerReason::unsafeRoute;
    ResourceLedger unsafeCriticalLedger{110, 0};
    (void)MacroPlanner{}.reconcile(criticalState, criticalPlan, unsafeCriticalLedger, unsafeCriticalPending);
    check(unsafeCriticalLedger.protectedMinerals == 0,
          "a known unsafe route releases critical pending expansion savings");
    ResourceLedger clearCriticalLedger{400, 0};
    const auto clearCriticalActions = MacroPlanner{}.reconcile(criticalState, criticalPlan, clearCriticalLedger);
    check(std::ranges::any_of(clearCriticalActions, [](const MacroAction& action) {
              return action.target == UnitKind::nexus && action.reserved && action.executable;
          }), "a cleared critical route can spend its actually funded replacement bank");

    state.frame = 220;
    ResourceLedger staleRouteLedger{400,0};
    const auto staleRouteActions = MacroPlanner{}.reconcile(state,scopedPlan,staleRouteLedger,freshPendingRoute);
    check(std::ranges::any_of(staleRouteActions, [](const MacroAction& action) {
              return action.target == UnitKind::nexus && action.reserved && action.executable;
          }), "stale unknown-route observations cannot withhold the bank indefinitely");
    plan.goals = {
        {GoalKind::build,UnitKind::photonCannon,1,120,true,"cover the threatened mineral line"},
        {GoalKind::train,UnitKind::probe,2,110,true,"continue worker production"},
    };

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
