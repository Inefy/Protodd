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
    for (auto& unit : state.self.units) {
        unit.completed = true;
    }
    state.frame = 4656; plan = baseline(); opening.apply(plan, state, threat);
    check(opening.launchFrame() == 4656 && plan.posture == Posture::attack, "four Zealots launch sticky attack");
    threat.combatEnemiesNearMain = 3;
    for (auto& unit : state.self.units) {
        if (unit.kind == UnitKind::zealot || unit.kind == UnitKind::dragoon ||
            unit.kind == UnitKind::darkTemplar) unit.position = {3000, 3000};
    }
    state.frame += 24; plan = baseline(); opening.apply(plan, state, threat);
    check(plan.posture == Posture::defend && opening.launchFrame() == 4656 &&
          opening.departureFrame() == state.frame && opening.arrivalFrame() == state.frame,
          "departure and arrival are recorded separately from assembly");
    threat = {}; state.frame = opening.arrivalFrame() + 120 * 24;
    plan = baseline(); opening.apply(plan, state, threat);
    check(opening.phase() == AllInPhase::transition && opening.transitionReason() == "pressure-window",
          "bounded pressure window ends commitment");
    check(plan.desiredBases >= 2 && plan.desiredWorkers >= 32 && plan.sustainEconomy,
          "transition funds economic recovery");
    check(plan.posture == Posture::attack && plan.minimumAttackSize == 4,
          "expanding does not recall surviving pressure army");
    plan = baseline(); plan.prioritizeReinforcements = true;
    opening.apply(plan, state, threat);
    check(!plan.prioritizeReinforcements, "safe transition preserves the Nexus priority instead of endless fillers");
    MacroPlanner transitionMacro; ResourceLedger transitionLedger{400, 0};
    const auto transitionActions = transitionMacro.reconcile(state, plan, transitionLedger);
    check(std::ranges::any_of(transitionActions, [](const MacroAction& action) {
        return action.action == MacroActionKind::expand && action.target == UnitKind::nexus &&
               action.priority == 128 && action.reserved && action.executable;
    }), "safe transition reserves the full bank for its second Nexus");
    threat.combatEnemiesNearMain = 3;
    plan = baseline(); plan.prioritizeReinforcements = true;
    opening.apply(plan, state, threat);
    check(plan.prioritizeReinforcements, "transition cannot demote reinforcement mode during a real breach");

    threat = {};
    opening.reset(AllInBuild::twoGateZealot); state = {}; state.frame = 1000;
    state.self.race = Race::protoss; state.enemy.race = Race::protoss;
    add(state, UnitKind::probe, 12); add(state, UnitKind::nexus, 1);
    add(state, UnitKind::gateway, 2); add(state, UnitKind::zealot, 4);
    plan = baseline(); opening.apply(plan, state, threat);
    const auto longRouteLaunch = opening.launchFrame();
    check(longRouteLaunch == 1000 && opening.departureFrame() < 0 && opening.arrivalFrame() < 0,
          "assembly launch is distinct from leaving home and reaching the objective");
    state.frame = longRouteLaunch + 100;
    auto moved = 0;
    for (auto& unit : state.self.units) {
        if (unit.kind == UnitKind::zealot && moved++ < 2) unit.position = {1200, 128};
    }
    plan = baseline(); opening.apply(plan, state, threat);
    check(opening.departureFrame() == state.frame && opening.arrivalFrame() < 0,
          "a distant route records departure before arrival");
    state.frame = longRouteLaunch + 800;
    moved = 0;
    for (auto& unit : state.self.units) {
        if (unit.kind == UnitKind::zealot && moved++ < 2) {
            unit.position = {2600, 2600};
            unit.groundWeapon = {.damage = 8, .maxRange = 80, .targetsGround = true};
        }
    }
    plan = baseline(); opening.apply(plan, state, threat);
    const auto longRouteArrival = opening.arrivalFrame();
    check(longRouteArrival == state.frame && longRouteArrival > opening.departureFrame() &&
          opening.phase() == AllInPhase::pressure,
          "long-distance arrival starts pressure without ending the opening");
    state.frame = longRouteArrival + 100;
    UnitSnapshot contactTarget; contactTarget.kind = UnitKind::zealot;
    contactTarget.position = {2680, 2600}; contactTarget.completed = true;
    contactTarget.visible = true; contactTarget.hitPoints = 100;
    state.enemy.units = {contactTarget};
    plan = baseline(); opening.apply(plan, state, threat);
    const auto firstContact = opening.contactFrame();
    check(firstContact == state.frame && firstContact > longRouteArrival,
          "first meaningful weapon-range contact is recorded separately from arrival");
    state.frame = firstContact + 120 * 24 - 1;
    plan = baseline(); opening.apply(plan, state, threat);
    check(opening.phase() == AllInPhase::pressure,
          "the full pressure window remains available after long travel and contact");
    state.frame += 1; plan = baseline(); opening.apply(plan, state, threat);
    check(opening.phase() == AllInPhase::transition &&
          opening.transitionReason() == "pressure-window",
          "the bounded pressure window ends at its contact-relative deadline");

    opening.reset(AllInBuild::twoGateZealot); state = {}; state.frame = 5000;
    state.self.race = Race::protoss; state.enemy.race = Race::protoss;
    add(state, UnitKind::probe, 12); add(state, UnitKind::nexus, 1);
    add(state, UnitKind::gateway, 2); add(state, UnitKind::zealot, 4);
    BaseSnapshot recovery; recovery.center = {1000, 1000};
    recovery.groundDistanceFromMain = 900; recovery.mineralsRemaining = 10000;
    state.bases.push_back(recovery);
    plan = baseline(); plan.attackTarget = {-1, -1}; plan.expansionTarget = {-1, -1};
    opening.apply(plan, state, threat);
    const auto unreachableLaunch = opening.launchFrame();
    state.frame = unreachableLaunch + 90 * 24 - 1;
    plan = baseline(); plan.attackTarget = {-1, -1}; plan.expansionTarget = {-1, -1};
    opening.apply(plan, state, threat);
    check(opening.phase() == AllInPhase::pressure && opening.arrivalFrame() < 0,
          "unreachable objective keeps the bounded travel interval until its deadline");
    state.frame += 1; plan = baseline(); plan.attackTarget = {-1, -1}; plan.expansionTarget = {-1, -1};
    opening.apply(plan, state, threat);
    check(opening.phase() == AllInPhase::transition &&
          opening.transitionReason() == "travel-timeout" && plan.sustainEconomy &&
          plan.desiredBases >= 2 && plan.desiredWorkers >= 32 &&
          plan.expansionTarget == recovery.center,
          "unreachable all-in releases the one-base commitment into a funded recovery plan");

    threat = {};
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
        check(plan.desiredGasWorkers == 3 &&
              (build == AllInBuild::darkTemplar || requested(plan, UnitKind::gateway) >= 3),
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
    UnitSnapshot enemy; enemy.kind = UnitKind::darkTemplar;
    enemy.visible = true; enemy.lastSeen = state.frame; enemy.position = {128, 128};
    state.enemy.units.push_back(enemy);
    plan = baseline(); opening.apply(plan, state, threat);
    check(plan.requireMobileDetection && requested(plan, UnitKind::observer) == 1,
          "observed cloak threat retains legal detector production");
    check(plan.desiredGasWorkers >= 3 && std::ranges::any_of(plan.goals,
          [](const ProductionGoal& goal) {
              return goal.goal == GoalKind::train && goal.target == UnitKind::observer &&
                  goal.blocking && goal.priority >= 128;
          }) && opening.phase() == AllInPhase::assemble,
          "credible DT evidence funds an executable Observer before first damage");
    state.enemy.units.clear(); enemy = {}; enemy.kind = UnitKind::lurker;
    enemy.visible = true; enemy.lastSeen = state.frame; enemy.position = {128, 128};
    state.enemy.units.push_back(enemy);
    plan = baseline(); opening.apply(plan, state, threat);
    check(plan.requireMobileDetection && requested(plan, UnitKind::observer) == 1,
          "credible Lurker evidence retains detection during an all-in");
    state.enemy.units.clear(); enemy = {}; enemy.kind = UnitKind::observer; enemy.cloaked = true;
    state.enemy.units.push_back(enemy); plan = baseline(); opening.apply(plan, state, threat);
    check(!plan.requireMobileDetection, "harmless cloaked Observer cannot impose a detection tech tax");
    state.enemy.units.clear(); enemy = {}; enemy.kind = UnitKind::wraith;
    enemy.visible = true; enemy.cloaked = true; enemy.lastSeen = state.frame;
    enemy.airWeapon.damage = 5; state.enemy.units.push_back(enemy);
    plan = baseline(); opening.apply(plan, state, threat);
    check(plan.requireMobileDetection && requested(plan, UnitKind::observer) == 1,
          "a credible cloaked-air contact retains its detection deadline through the all-in");
    state.enemy.units.clear();
    std::erase_if(state.self.units, [](const UnitSnapshot& own) {
        return own.kind == UnitKind::assimilator;
    });
    plan = baseline();
    plan.requireMobileDetection = true;
    plan.goals.push_back({GoalKind::detect, UnitKind::observer, 1, 120, true,
                          "native safety obligation"});
    opening.apply(plan, state, threat);
    check(plan.requireMobileDetection && requested(plan, UnitKind::observer) == 1,
          "an all-in retains a pre-existing native detection obligation");
    check(plan.desiredGasWorkers >= 3 && requested(plan, UnitKind::assimilator) >= 1,
          "a carried air/cloak deadline retains gas income and its Assimilator checkpoint");
    ResourceLedger detectorLedger{1000, 500};
    const auto detectorActions = MacroPlanner{}.reconcile(state, plan, detectorLedger);
    check(std::ranges::any_of(detectorActions, [](const MacroAction& action) {
              return action.target == UnitKind::assimilator && action.reserved && action.executable;
          }) && std::ranges::any_of(detectorActions, [](const MacroAction& action) {
              return action.target == UnitKind::roboticsFacility && action.reserved && action.executable;
          }), "the all-in carries a funded gas and Observer prerequisite chain before first damage");
    opening.reset(AllInBuild::twoGateZealot);
    state.frame = 4000;
    UnitSnapshot staleDt; staleDt.kind = UnitKind::darkTemplar;
    staleDt.visible = false; staleDt.lastSeen = state.frame - 16 * 24;
    state.enemy.units = {staleDt};
    auto weakSuspicion = threat; weakSuspicion.cloak = 0.20;
    plan = baseline(); plan.requireMobileDetection = false;
    opening.apply(plan, state, weakSuspicion);
    check(!plan.requireMobileDetection && requested(plan, UnitKind::observer) == 0 &&
          plan.desiredGasWorkers == 0,
          "weak cloak speculation and stale DT memory do not tax the opening with detection");
    opening.reset(AllInBuild::darkTemplar); state = {}; state.frame = 5000;
    add(state, UnitKind::probe, 18); add(state, UnitKind::gateway, 1);
    add(state, UnitKind::cyberneticsCore, 1); add(state, UnitKind::assimilator, 1);
    add(state, UnitKind::dragoon, 2);
    plan = baseline(); opening.apply(plan, state, threat);
    check(requested(plan, UnitKind::dragoon) == 2 && requested(plan, UnitKind::citadelOfAdun) == 1 &&
          requested(plan, UnitKind::gateway) <= 1,
          "DT timing caps the escort and funds tech before extra Gateways");
    check(std::ranges::all_of(plan.composition, [](const CompositionTarget& target) { return target.weight == 0; }),
          "unfinished DT chain cannot spend its gas bank on composition filler");
    check(!plan.prioritizeReinforcements, "opening tech priorities bypass the generic defensive filler mode");
    MacroPlanner macro; ResourceLedger ledger{1000, 500};
    const auto actions = macro.reconcile(state, plan, ledger);
    check(std::ranges::none_of(actions, [](const MacroAction& action) {
              return action.action == MacroActionKind::train && action.target == UnitKind::dragoon;
          }), "macro bridge respects the two-Dragoon DT escort cap");
    add(state, UnitKind::citadelOfAdun, 1); add(state, UnitKind::templarArchives, 1, false);
    plan = baseline(); opening.apply(plan, state, threat);
    check(requested(plan, UnitKind::gateway) == 2, "fund second DT producer after Archives starts");
    add(state, UnitKind::darkTemplar, 1); state.frame = 7800;
    plan = baseline(); opening.apply(plan, state, threat);
    check(opening.phase() == AllInPhase::pressure && plan.minimumAttackSize == 2,
          "first completed DT releases its two-Dragoon screen without waiting for a second DT");
    add(state, UnitKind::nexus, 1);
    enemy = {}; enemy.kind = UnitKind::zealot; enemy.visible = true;
    enemy.completed = true; enemy.position = {300, 128};
    enemy.detected = false;
    enemy.topSpeed = 4.0;
    state.enemy.units = {enemy};
    plan = baseline(); opening.apply(plan, state, threat);
    check(requested(plan, UnitKind::photonCannon) == 0,
          "ordinary visible one-base pressure cannot invent the DT detection anchor");
    state.enemy.units[0].kind = UnitKind::darkTemplar;
    plan = baseline(); opening.apply(plan, state, threat);
    check(requested(plan, UnitKind::photonCannon) == 1,
          "an observed DT at the economy funds one immediate detection anchor");
    UnitSnapshot naturalNexus; naturalNexus.kind = UnitKind::nexus;
    naturalNexus.id = 40; naturalNexus.position = {1800, 128}; naturalNexus.completed = true;
    UnitSnapshot mainCannon; mainCannon.kind = UnitKind::photonCannon;
    mainCannon.id = 41; mainCannon.position = {256, 128}; mainCannon.completed = true;
    mainCannon.sightRange = 224;
    mainCannon.hitPoints = mainCannon.maxHitPoints = 100;
    state.self.units.push_back(naturalNexus);
    state.self.units.push_back(mainCannon);
    state.enemy.units[0].position = {1850, 128};
    plan = baseline(); opening.apply(plan, state, threat);
    const auto naturalCannonGoal = [&naturalNexus](const StrategicPlan& candidate) {
        return std::ranges::find_if(candidate.goals, [&naturalNexus](const ProductionGoal& goal) {
            return goal.goal == GoalKind::build && goal.target == UnitKind::photonCannon &&
                   goal.constructionSite.valid() &&
                   goal.constructionSite.anchor == naturalNexus.position;
        });
    };
    check(naturalCannonGoal(plan) != plan.goals.end() &&
              naturalCannonGoal(plan)->desiredCount == 1,
          "a main Cannon does not satisfy a site-scoped natural-base DT detection demand");

    UnitSnapshot naturalCannon; naturalCannon.kind = UnitKind::photonCannon;
    naturalCannon.id = 42; naturalCannon.position = naturalNexus.position;
    naturalCannon.completed = true; naturalCannon.powered = true;
    naturalCannon.sightRange = 224;
    naturalCannon.hitPoints = naturalCannon.maxHitPoints = 100;
    state.self.units.push_back(naturalCannon);
    plan = baseline(); opening.apply(plan, state, threat);
    check(naturalCannonGoal(plan) == plan.goals.end(),
          "a completed powered Cannon with local detection range satisfies the threatened base");
    state.self.units.back().powered = false;
    plan = baseline(); opening.apply(plan, state, threat);
    check(naturalCannonGoal(plan) != plan.goals.end() &&
              naturalCannonGoal(plan)->desiredCount == 2,
          "an unpowered Cannon cannot satisfy the local emergency and triggers a powered replacement");

    state.self.units.back().powered = true;
    state.self.units.back().completed = false;
    state.self.units.back().buildProgress = 98;
    state.self.units.back().position = {1500, 128};
    state.self.units.back().hitPoints = state.self.units.back().maxHitPoints = 100;
    state.enemy.units[0].position = {1300, 128};
    plan = baseline(); opening.apply(plan, state, threat);
    check(naturalCannonGoal(plan) == plan.goals.end(),
          "an in-progress powered Cannon is usable when its remaining build time beats DT arrival");
    state.self.units.back().buildProgress = 0;
    plan = baseline(); opening.apply(plan, state, threat);
    check(naturalCannonGoal(plan) != plan.goals.end() &&
              naturalCannonGoal(plan)->desiredCount == 2,
          "a local Cannon that finishes after the breach deadline does not count as coverage");

    state.self.units.back().powered = false;
    UnitSnapshot mobileObserver; mobileObserver.kind = UnitKind::observer;
    mobileObserver.id = 43; mobileObserver.position = {1000, 128};
    mobileObserver.completed = true; mobileObserver.topSpeed = 5.0;
    mobileObserver.sightRange = 224; mobileObserver.hitPoints = 100;
    mobileObserver.maxHitPoints = 100; mobileObserver.maxShields = 100;
    state.self.units.push_back(mobileObserver);
    plan = baseline(); opening.apply(plan, state, threat);
    check(naturalCannonGoal(plan) == plan.goals.end(),
          "a healthy Observer arriving before the DT reaches the natural can cover it");
    state.self.units.back().position = {900, 128};
    plan = baseline(); opening.apply(plan, state, threat);
    check(naturalCannonGoal(plan) != plan.goals.end(),
          "an Observer whose travel time misses the breach deadline cannot replace local detection");

    UnitSnapshot forge; forge.id = 44; forge.kind = UnitKind::forge;
    forge.position = naturalNexus.position; forge.completed = true;
    state.self.units.push_back(forge);
    auto macroPlan = baseline(); opening.apply(macroPlan, state, threat);
    ResourceLedger cannonLedger{1000, 500};
    const auto poweredSiteActions = MacroPlanner{}.reconcile(state, macroPlan, cannonLedger);
    check(std::ranges::any_of(poweredSiteActions, [&naturalNexus](const MacroAction& action) {
              return action.action == MacroActionKind::build && action.target == UnitKind::pylon &&
                     action.constructionSite.valid() &&
                     action.constructionSite.anchor == naturalNexus.position;
          }),
          "missing local power creates a Pylon prerequisite anchored at the threatened base");
    UnitSnapshot pylon; pylon.id = 45; pylon.kind = UnitKind::pylon;
    pylon.position = naturalNexus.position; pylon.completed = true;
    pylon.hitPoints = pylon.maxHitPoints = 300;
    state.self.units.push_back(pylon);
    macroPlan = baseline(); opening.apply(macroPlan, state, threat);
    cannonLedger = {1000, 500};
    const auto cannonActions = MacroPlanner{}.reconcile(state, macroPlan, cannonLedger);
    check(std::ranges::any_of(cannonActions, [&naturalNexus](const MacroAction& action) {
              return action.action == MacroActionKind::build &&
                     action.target == UnitKind::photonCannon &&
                     action.constructionSite.valid() &&
                     action.constructionSite.anchor == naturalNexus.position;
          }),
          "the threatened-base construction anchor survives into the macro build action");

    state.enemy.units[0].visible = false;
    plan = baseline(); opening.apply(plan, state, threat);
    check(naturalCannonGoal(plan) == plan.goals.end(),
          "stale DT memory cannot create another base-scoped detection task");
    opening.reset(AllInBuild::standard); plan = baseline(); opening.apply(plan, state, threat);
    check(plan.desiredBases == 3 && plan.goals.size() == 2, "standard profile unchanged");
    return failures ? 1 : 0;
}
