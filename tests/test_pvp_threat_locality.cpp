#include "protodd/Information.hpp"
#include "protodd/MacroPlanner.hpp"
#include "protodd/Strategy.hpp"
#include "protodd/UnitCatalog.hpp"

#include <iostream>
#include <optional>
#include <string_view>

namespace {

int failures = 0;

void check(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

protodd::UnitSnapshot unit(
    const protodd::UnitId id,
    const protodd::UnitKind kind,
    const bool ours,
    const protodd::Position position) {
    protodd::UnitSnapshot result;
    result.id = id;
    result.kind = kind;
    result.ours = ours;
    result.position = position;
    result.lastPosition = position;
    result.lastSeen = 7 * 60 * 24;
    result.visible = true;
    result.completed = true;
    result.hitPoints = 100;
    result.maxHitPoints = 100;
    result.topSpeed = 4.0;
    return result;
}

protodd::GameState stableMirror() {
    using namespace protodd;
    GameState state;
    state.frame = 7 * 60 * 24;
    state.self.id = 1;
    state.self.race = Race::protoss;
    state.enemy.id = 2;
    state.enemy.race = Race::protoss;

    auto nexus = unit(1, UnitKind::nexus, true, {256, 256});
    nexus.role = UnitRole::resourceDepot;
    state.self.units.push_back(nexus);
    for (int i = 0; i < 2; ++i) {
        auto cannon = unit(10 + i, UnitKind::photonCannon, true,
                           {360 + i * 72, 360});
        cannon.role = UnitRole::staticDefense;
        state.self.units.push_back(cannon);
    }
    for (int i = 0; i < 5; ++i) {
        auto zealot = unit(20 + i, UnitKind::zealot, true,
                           {440 + i * 24, 440});
        zealot.role = UnitRole::groundArmy;
        state.self.units.push_back(zealot);
    }
    auto core = unit(30, UnitKind::cyberneticsCore, true, {400, 256});
    core.role = UnitRole::production;
    state.self.units.push_back(core);
    for (int i = 0; i < 18; ++i) {
        auto probe = unit(40 + i, UnitKind::probe, true,
                          {480 + (i % 4) * 16, 480 + (i / 4) * 16});
        probe.role = UnitRole::worker;
        state.self.units.push_back(probe);
    }

    BaseSnapshot main;
    main.id = 1;
    main.center = {256, 256};
    main.ownerId = state.self.id;
    main.startLocation = true;
    main.mineralsRemaining = 8000;

    BaseSnapshot natural;
    natural.id = 2;
    natural.center = {1500, 256};
    natural.ownerId = -1;
    natural.mineralsRemaining = 8000;
    natural.mineralPatches = 8;
    natural.geysers = 1;

    BaseSnapshot enemyMain;
    enemyMain.id = 3;
    enemyMain.center = {3300, 3400};
    enemyMain.ownerId = state.enemy.id;
    enemyMain.startLocation = true;
    enemyMain.mineralsRemaining = 8000;
    state.bases = {main, natural, enemyMain};

    for (int i = 0; i < 1; ++i) {
        auto gateway = unit(100 + i, UnitKind::gateway, false,
                           {3300 + i * 96, 3400});
        gateway.role = UnitRole::production;
        state.enemy.units.push_back(gateway);
    }
    return state;
}

protodd::GameState quietPvp(const protodd::Frame frame) {
    using namespace protodd;
    GameState state;
    state.frame = frame;
    state.self.id = 1;
    state.enemy.id = 2;
    state.self.race = state.enemy.race = Race::protoss;
    state.self.supplyUsed = 32;
    state.self.supplyTotal = 44;
    auto nexus = unit(1, UnitKind::nexus, true, {256, 256});
    nexus.role = UnitRole::resourceDepot;
    state.self.units = {nexus, unit(2, UnitKind::pylon, true, {160, 160}),
        unit(3, UnitKind::gateway, true, {400, 256}),
        unit(4, UnitKind::assimilator, true, {480, 256}),
        unit(5, UnitKind::zealot, true, {320, 320}),
        unit(6, UnitKind::zealot, true, {340, 320})};
    for (int id = 20; id < 36; ++id) {
        auto probe = unit(id, UnitKind::probe, true, {480 + (id % 4) * 16, 480});
        probe.role = UnitRole::worker;
        state.self.units.push_back(probe);
    }
    for (auto& own : state.self.units) own.lastSeen = frame;

    BaseSnapshot main;
    main.id = 1; main.center = {256, 256}; main.ownerId = state.self.id;
    main.startLocation = true; main.mineralsRemaining = 8000;
    BaseSnapshot natural;
    natural.id = 2; natural.center = {1500, 256}; natural.ownerId = -1;
    natural.mineralsRemaining = 8000; natural.mineralPatches = 8; natural.geysers = 1;
    BaseSnapshot enemyMain;
    enemyMain.id = 3; enemyMain.center = {3300, 3400};
    enemyMain.ownerId = state.enemy.id; enemyMain.startLocation = true;
    enemyMain.mineralsRemaining = 8000;
    state.bases = {main, natural, enemyMain};
    return state;
}

bool hasGoal(const protodd::StrategicPlan& plan, const protodd::GoalKind kind,
             const protodd::UnitKind target, const int minimumPriority = 0,
             const bool requireBlocking = false) {
    return std::ranges::any_of(plan.goals, [=](const protodd::ProductionGoal& goal) {
        return goal.goal == kind && goal.target == target &&
               goal.priority >= minimumPriority && (!requireBlocking || goal.blocking);
    });
}

protodd::StrategicPlan planAt(
    protodd::GameState state,
    const protodd::Position current,
    const protodd::Position previous,
    const std::optional<double> globalAggression = std::nullopt) {
    using namespace protodd;
    for (int i = 0; i < 2; ++i) {
        auto zealot = unit(200 + i, UnitKind::zealot, false, current);
        zealot.lastPosition = previous;
        state.enemy.units.push_back(zealot);
    }
    OpponentModel opponent;
    opponent.update(state);
    auto threat = opponent.assessment();
    if (globalAggression) threat.aggression = *globalAggression;
    return StrategyEngine{}.plan(state, threat);
}

void testRemoteArmyDoesNotCreateHomeEmergency() {
    using namespace protodd;
    const auto plan = planAt(stableMirror(), {3300, 3400}, {3300, 3400});
    check(plan.posture != Posture::defend &&
              plan.name.find("two-gate emergency defense") == std::string::npos,
          "enemy production-area Zealots do not trigger home emergency defense");
    check(plan.desiredBases >= 2,
          "enemy production-area Zealots do not block a safe natural expansion");
}

void testHomeContactRequestsHomeDefense() {
    using namespace protodd;
    const auto plan = planAt(stableMirror(), {300, 256}, {3300, 3400});
    check(plan.posture == Posture::defend &&
              plan.name.find("two-gate emergency defense") != std::string::npos,
          "the same Zealot group requests home defense when it reaches the main");
    check(plan.desiredBases == 1,
          "home contact suppresses the next-base transition");
}

void testGlobalAggressionDoesNotCreateHomeBreach() {
    using namespace protodd;
    const auto plan = planAt(stableMirror(), {3300, 3400}, {3300, 3400}, 0.9);
    check(plan.posture != Posture::defend &&
              plan.name.find("two-gate emergency defense") == std::string::npos,
          "global opponent aggression without local contact does not trigger home defense");
}

void testVisibleHomeContactKeepsDefenseStableDuringRegroup() {
    using namespace protodd;
    auto state = stableMirror();
    state.enemy.units.push_back(unit(300, UnitKind::zealot, false, {700, 256}));
    StrategicPlan candidate;
    candidate.name = "visible local defense";
    candidate.posture = Posture::defend;
    StrategicDirector director;

    const auto defending = director.stabilize(candidate, state, {});
    check(defending.posture == Posture::defend,
          "visible combat contact near the main starts a defensive regroup window");

    state.frame += 24;
    state.enemy.units.back().visible = false;
    candidate.name = "clear candidate";
    candidate.posture = Posture::hold;
    const auto regrouping = director.stabilize(candidate, state, {});
    check(regrouping.posture == Posture::defend &&
              regrouping.name.find("regrouping after defense") != std::string::npos,
          "one fogged contact sample does not send the army back out immediately");

    state.frame += 8 * 24;
    const auto released = director.stabilize(candidate, state, {});
    check(released.posture == Posture::hold,
          "the defensive regroup window releases after sustained clear time");
}

void testExpansionCommitmentSurvivesTransientPlanDropout() {
    using namespace protodd;
    auto state = stableMirror();
    state.self.minerals = 700;
    state.self.supplyUsed = 40;
    state.self.supplyTotal = 50;
    state.bases[1].mineralLine = state.bases[1].center;
    state.bases[1].depotFootprintAvailable = true;
    state.bases[1].groundDistanceFromMain = 1244;
    auto alternateSite = state.bases[1];
    alternateSite.id = 4;
    alternateSite.center = {2500, 256};
    alternateSite.mineralLine = alternateSite.center;
    alternateSite.groundDistanceFromMain = 2244;
    state.bases.push_back(alternateSite);
    const auto initialFrame = state.frame;

    StrategicPlan expanding;
    expanding.name = "safe first expansion";
    expanding.posture = Posture::hold;
    expanding.desiredBases = 2;
    expanding.expansionTarget = state.bases[1].center;
    expanding.rallyPoint = state.bases[1].center;
    expanding.goals.push_back({GoalKind::expand, UnitKind::nexus, 2, 100,
                               true, "match economic phase"});
    StrategicDirector director;
    const auto started = director.stabilize(expanding, state, {});
    check(started.expansionTarget == state.bases[1].center,
          "the safe expansion begins as a strategic commitment");

    state.frame += 24;
    StrategicPlan transientDropout;
    transientDropout.name = "temporary splash checkpoint";
    transientDropout.posture = Posture::hold;
    transientDropout.desiredBases = 1;
    const auto retained = director.stabilize(transientDropout, state, {});
    check(retained.expansionTarget == state.bases[1].center &&
              retained.rallyPoint == state.bases[1].center && retained.desiredBases == 2 &&
              std::ranges::any_of(retained.goals, [](const ProductionGoal& goal) {
                  return goal.goal == GoalKind::expand && goal.target == UnitKind::nexus &&
                      goal.desiredCount >= 2 && goal.blocking;
              }),
          "a transient strategy dropout preserves an actionable expansion and army rally");
    ResourceLedger bank{state.self.minerals, state.self.gas};
    const auto actions = MacroPlanner{}.reconcile(state, retained, bank);
    check(std::ranges::any_of(actions, [](const MacroAction& action) {
              return action.target == UnitKind::nexus && action.reserved;
          }),
          "the retained expansion commitment remains fundable in macro planning");

    state.frame += 24;
    auto changedSite = expanding;
    changedSite.name = "alternate expansion proposal";
    changedSite.expansionTarget = alternateSite.center;
    changedSite.rallyPoint = alternateSite.center;
    const auto sameCommitment = director.stabilize(changedSite, state, {});
    check(sameCommitment.expansionTarget == state.bases[1].center &&
              sameCommitment.rallyPoint == state.bases[1].center,
          "a viable expansion commitment survives a transient switch to another site");

    state.frame = initialFrame + 8 * 24 + 1;
    const auto releasedSite = director.stabilize(changedSite, state, {});
    check(releasedSite.expansionTarget == alternateSite.center,
          "a viable alternative expansion becomes eligible after the commitment window");

    state.frame += 24;
    auto homeContact = unit(300, UnitKind::zealot, false, {700, 256});
    homeContact.groundWeapon.damage = 8;
    homeContact.groundWeapon.targetsGround = true;
    state.enemy.units.push_back(homeContact);
    transientDropout.posture = Posture::defend;
    const auto canceled = director.stabilize(transientDropout, state, {});
    check(!canceled.expansionTarget.valid() && canceled.desiredBases == 1,
          "real home defense cancels the remembered expansion immediately");
}

void testMidfieldRallyDoesNotTrackUnitJitter() {
    using namespace protodd;
    auto state = stableMirror();
    state.enemy.units.push_back(unit(200, UnitKind::zealot, false, {900, 256}));
    state.enemy.units.push_back(unit(201, UnitKind::zealot, false, {900, 280}));
    ThreatAssessment threat;
    StrategyEngine strategy;

    const auto first = strategy.plan(state, threat);
    check(first.name.find("midfield pressure") != std::string::npos &&
              first.rallyPoint.valid(),
          "a visible, contestable midfield force selects the pressure rally");
    const auto committedRally = first.rallyPoint;

    state.frame += 24;
    state.enemy.units[1].position = {912, 264};
    state.enemy.units[1].lastPosition = state.enemy.units[1].position;
    state.enemy.units[1].lastSeen = state.frame;
    state.enemy.units[2].position = {912, 288};
    state.enemy.units[2].lastPosition = state.enemy.units[2].position;
    state.enemy.units[2].lastSeen = state.frame;
    const auto jittered = strategy.plan(state, threat);
    check(jittered.rallyPoint == committedRally,
          "small enemy position changes do not reverse the army rally each strategy tick");

    state.frame += 24;
    for (auto& enemy : state.enemy.units) {
        if (enemy.id != 200 && enemy.id != 201) continue;
        enemy.position = {256, 900 + enemy.id - 200};
        enemy.lastPosition = enemy.position;
        enemy.lastSeen = state.frame;
    }
    const auto shiftedFront = strategy.plan(state, threat);
    check(shiftedFront.rallyPoint != committedRally,
          "a materially displaced midfield front can move the committed rally");
}

void testNaturalApproachIsLocalToTheExpansion() {
    using namespace protodd;
    const auto plan = planAt(stableMirror(), {1600, 256}, {1880, 256});
    check(plan.posture != Posture::defend &&
              plan.name.find("two-gate emergency defense") == std::string::npos,
          "Zealots approaching the natural do not masquerade as a home breach");
    check(plan.desiredBases == 1,
          "Zealots approaching the natural postpone that exposed expansion");
}

void testQuietRangedOpeningAndCannonsStayBounded() {
    using namespace protodd;
    auto state = quietPvp(4 * 60 * 24);
    const auto plan = StrategyEngine{}.plan(state, {});
    check(plan.name.find("PvP ranged economy into Robo") != std::string::npos &&
              plan.desiredGasWorkers == 3 &&
              hasGoal(plan, GoalKind::build, UnitKind::cyberneticsCore),
          "quiet PvP funds its Core and gas before expanding into ranged production");
    check(!hasGoal(plan, GoalKind::build, UnitKind::forge) &&
              !hasGoal(plan, GoalKind::build, UnitKind::photonCannon),
          "quiet PvP does not buy a speculative Cannon screen");
    MacroPlanner macro;
    ResourceLedger coreBank{250, 0};
    const auto coreActions = macro.reconcile(state, plan, coreBank);
    check(std::ranges::any_of(coreActions, [](const MacroAction& action) {
              return action.target == UnitKind::cyberneticsCore &&
                     action.reserved && action.executable;
          }), "the quiet Core checkpoint is an executable funded build");

    state = quietPvp(7 * 60 * 24);
    for (int id = 50; id < 53; ++id) {
        auto cannon = unit(id, UnitKind::photonCannon, true, {360 + id, 360});
        cannon.role = UnitRole::staticDefense;
        cannon.lastSeen = state.frame;
        state.self.units.push_back(cannon);
    }
    const auto quietAfterScreen = StrategyEngine{}.plan(state, {});
    check(!hasGoal(quietAfterScreen, GoalKind::build, UnitKind::photonCannon) &&
              !hasGoal(quietAfterScreen, GoalKind::build, UnitKind::forge),
          "a quiet, already-screened mirror does not accumulate more Cannons or Forge tech");
}

void testTwoGateAndCoreRangedBranches() {
    using namespace protodd;
    auto rush = quietPvp(4 * 60 * 24 + 20 * 24);
    rush.self.units.push_back(unit(7, UnitKind::gateway, true, {420, 256}));
    for (int id = 100; id < 102; ++id) {
        auto gateway = unit(id, UnitKind::gateway, false, {3300 + id, 3400});
        gateway.lastSeen = rush.frame;
        rush.enemy.units.push_back(gateway);
    }
    const auto melee = StrategyEngine{}.plan(rush, {});
    check(melee.name.find("PvP two-gate robotics control") != std::string::npos &&
              hasGoal(melee, GoalKind::build, UnitKind::forge, 118, true) &&
              hasGoal(melee, GoalKind::train, UnitKind::zealot),
          "two scouted enemy Gateways trigger a mobile screen and bounded static anchor");

    rush.frame = 6 * 60 * 24;
    rush.self.units.push_back(unit(8, UnitKind::forge, true, {520, 256}));
    rush.self.units.push_back(unit(9, UnitKind::photonCannon, true, {400, 320}));
    for (int id = 60; id < 63; ++id)
        rush.self.units.push_back(unit(id, UnitKind::zealot, true, {360 + id, 320}));
    const auto recovery = StrategyEngine{}.plan(rush, {});
    check(recovery.desiredGasWorkers >= 3 &&
              hasGoal(recovery, GoalKind::build, UnitKind::cyberneticsCore, 98, true),
          "after the initial screen, two-Gateway pressure restores gas and ranged-tech funding");

    auto ranged = quietPvp(6 * 60 * 24);
    ranged.self.units.push_back(unit(7, UnitKind::gateway, true, {420, 256}));
    ranged.self.units.push_back(unit(8, UnitKind::cyberneticsCore, true, {520, 256}));
    for (int id = 60; id < 66; ++id)
        ranged.self.units.push_back(unit(id, UnitKind::dragoon, true, {360 + id, 320}));
    auto enemyCore = unit(200, UnitKind::cyberneticsCore, false, {3100, 3000});
    enemyCore.lastSeen = ranged.frame;
    ranged.enemy.units.push_back(enemyCore);
    auto mirror = StrategyEngine{}.plan(ranged, {});
    check(mirror.desiredBases == 1 && mirror.posture == Posture::hold &&
              hasGoal(mirror, GoalKind::build, UnitKind::roboticsFacility),
          "Core/ranged evidence keeps the first Reaver checkpoint ahead of the natural");
    ResourceLedger roboBank{500, 400};
    const auto roboActions = MacroPlanner{}.reconcile(ranged, mirror, roboBank);
    check(std::ranges::any_of(roboActions, [](const MacroAction& action) {
              return action.target == UnitKind::roboticsFacility &&
                     action.reserved && action.executable;
          }), "the ranged-tech branch reserves an executable Robotics Facility");
    ranged.self.units.push_back(unit(70, UnitKind::roboticsFacility, true, {600, 256}));
    ranged.self.units.push_back(unit(71, UnitKind::roboticsSupportBay, true, {680, 256}));
    ranged.self.units.push_back(unit(72, UnitKind::reaver, true, {720, 320}));
    mirror = StrategyEngine{}.plan(ranged, {});
    check(mirror.desiredBases >= 2 && mirror.expansionTarget.valid(),
          "the completed ranged and splash checkpoint releases the economic transition");
}

void testContainDefenseDetectionAndEconomicExit() {
    using namespace protodd;
    auto state = quietPvp(5 * 60 * 24 + 30 * 24);
    auto pylon = unit(200, UnitKind::pylon, false, {500, 256});
    auto firstCannon = unit(201, UnitKind::photonCannon, false, {580, 256});
    auto secondCannon = unit(202, UnitKind::photonCannon, false, {620, 256});
    for (auto* enemy : {&pylon, &firstCannon, &secondCannon})
        enemy->lastSeen = state.frame;
    state.enemy.units = {pylon, firstCannon, secondCannon};
    OpponentModel model;
    model.update(state);
    const auto containThreat = model.assessment();
    check(containThreat.staticContain > 0.34,
          "visible powered-area Cannons provide local static-contain evidence");
    const auto defense = StrategyEngine{}.plan(state, containThreat);
    check(defense.posture == Posture::defend && defense.desiredBases == 1 &&
              defense.name.find("break static contain") != std::string::npos &&
              hasGoal(defense, GoalKind::build, UnitKind::gateway, 100, true) &&
              hasGoal(defense, GoalKind::train, UnitKind::zealot, 99, true) &&
              hasGoal(defense, GoalKind::build, UnitKind::shieldBattery, 95),
          "a visible contain replaces expansion with executable minimum-defense production");
    ResourceLedger defenseBank{1000, 600};
    const auto defenseActions = MacroPlanner{}.reconcile(state, defense, defenseBank);
    check(std::ranges::any_of(defenseActions, [](const MacroAction& action) {
              return action.action == MacroActionKind::build &&
                     action.target == UnitKind::gateway && action.reserved && action.executable;
          }) && std::ranges::any_of(defenseActions, [](const MacroAction& action) {
              return action.action == MacroActionKind::train &&
                     action.target == UnitKind::zealot && action.reserved && action.executable;
          }), "proxy defense goals reserve executable Gateway and Zealot production");

    auto proxy = state;
    auto proxyPylon = unit(205, UnitKind::pylon, false, {460, 256});
    auto proxyGateway = unit(206, UnitKind::gateway, false, {560, 256});
    proxyPylon.lastSeen = proxyGateway.lastSeen = proxy.frame;
    proxy.enemy.units = {proxyPylon, proxyGateway};
    model.reset(Race::protoss);
    model.update(proxy);
    const auto proxyThreat = model.assessment();
    const auto proxyDefense = StrategyEngine{}.plan(proxy, proxyThreat);
    check(proxyThreat.proxy > 0.34 && proxyDefense.posture == Posture::defend &&
              proxyDefense.name.find("break proxy") != std::string::npos &&
              proxyDefense.desiredBases == 1 &&
              hasGoal(proxyDefense, GoalKind::build, UnitKind::gateway, 100, true) &&
              hasGoal(proxyDefense, GoalKind::train, UnitKind::zealot, 99, true),
          "visible local proxy production selects the proxy defense branch separately from static contain");

    state.enemy.units.clear();
    state.frame = 8 * 60 * 24;
    for (int id = 70; id < 74; ++id)
        state.self.units.push_back(unit(id, UnitKind::zealot, true, {380 + id, 360}));
    for (int id = 80; id < 82; ++id)
        state.self.units.push_back(unit(id, UnitKind::photonCannon, true, {390 + id, 390}));
    const auto recovery = StrategyEngine{}.plan(state, {});
    check(recovery.desiredBases >= 2 && recovery.posture == Posture::pressure &&
              recovery.name.find("economic counter-window") != std::string::npos,
          "cleared contain evidence releases the stable defense into an economic transition");

    auto cloak = quietPvp(7 * 60 * 24);
    cloak.self.units.push_back(unit(7, UnitKind::gateway, true, {420, 256}));
    cloak.self.units.push_back(unit(8, UnitKind::cyberneticsCore, true, {520, 256}));
    for (int id = 60; id < 66; ++id)
        cloak.self.units.push_back(unit(id, UnitKind::dragoon, true, {360 + id, 320}));
    auto archives = unit(210, UnitKind::templarArchives, false, {3150, 3000});
    archives.lastSeen = cloak.frame;
    auto dt = unit(211, UnitKind::darkTemplar, false, {3120, 3000});
    dt.lastSeen = cloak.frame;
    cloak.enemy.units = {archives, dt};
    model.reset(Race::protoss);
    model.update(cloak);
    const auto techPlan = StrategyEngine{}.plan(cloak, model.assessment());
    check(techPlan.requireMobileDetection && techPlan.desiredGasWorkers >= 3 &&
              hasGoal(techPlan, GoalKind::build, UnitKind::roboticsFacility, 97, true) &&
              hasGoal(techPlan, GoalKind::build, UnitKind::observatory, 96, true) &&
              hasGoal(techPlan, GoalKind::train, UnitKind::observer, 99, true),
          "recent legal DT-tech evidence funds mobile detection before further pressure");

    auto unrelated = quietPvp(7 * 60 * 24);
    unrelated.enemy.units.push_back(unit(220, UnitKind::pylon, false, {3150, 3000}));
    const auto unrelatedPlan = StrategyEngine{}.plan(unrelated, {});
    check(!unrelatedPlan.requireMobileDetection &&
              !hasGoal(unrelatedPlan, GoalKind::train, UnitKind::observer, 124, true),
          "an unrelated remote Pylon does not create a DT detection obligation");
}

void testDestroyedCoreRecoversGasAndRangedProduction() {
    using namespace protodd;
    auto state = quietPvp(6 * 60 * 24);
    state.self.units.push_back(unit(7, UnitKind::gateway, true, {420, 256}));
    state.self.units.push_back(unit(8, UnitKind::dragoon, true, {330, 320}));
    state.enemy.units.push_back(unit(200, UnitKind::zealot, false, {300, 256}));
    ThreatAssessment pressure;
    pressure.combatEnemiesNearMain = 1;
    pressure.mostLikely = EnemyPlan::fastRush;
    const auto plan = StrategyEngine{}.plan(state, pressure);
    check(plan.posture == Posture::defend && plan.desiredGasWorkers >= 3 &&
              hasGoal(plan, GoalKind::build, UnitKind::cyberneticsCore, 110, true) &&
              hasGoal(plan, GoalKind::build, UnitKind::assimilator, 109, true),
          "a destroyed Core cannot strand the ranged army without gas-backed tech recovery");
}

void testExpensiveMirrorTechWaitsForExpansionEconomy() {
    using namespace protodd;
    auto oneBase = quietPvp(9 * 60 * 24);
    oneBase.self.supplyUsed = 80;
    oneBase.self.supplyTotal = 200;
    oneBase.self.units.push_back(unit(7, UnitKind::cyberneticsCore, true, {520, 256}));
    oneBase.self.units.push_back(unit(8, UnitKind::roboticsFacility, true, {560, 320}));
    oneBase.self.units.push_back(unit(9, UnitKind::roboticsSupportBay, true, {600, 320}));
    for (int id = 36; id < 42; ++id) {
        auto probe = unit(id, UnitKind::probe, true, {480 + (id % 4) * 16, 520});
        probe.role = UnitRole::worker;
        oneBase.self.units.push_back(probe);
    }
    for (int id = 60; id < 70; ++id)
        oneBase.self.units.push_back(unit(id, UnitKind::dragoon, true, {600 + id, 440}));
    oneBase.self.units.push_back(unit(70, UnitKind::reaver, true, {640, 480}));
    oneBase.self.units.push_back(unit(71, UnitKind::reaver, true, {680, 480}));
    const auto hasStormGoal = [](const StrategicPlan& plan) {
        return std::ranges::any_of(plan.goals, [](const ProductionGoal& goal) {
            return goal.technology == TechnologyKind::psionicStorm;
        });
    };

    const auto singleBasePlan = StrategyEngine{}.plan(oneBase, {});
    check(!hasGoal(singleBasePlan, GoalKind::build, UnitKind::templarArchives) &&
              !hasStormGoal(singleBasePlan),
          "a one-base 22-Probe mirror does not reserve its army bank for Storm before expanding");

    auto established = oneBase;
    auto secondNexus = unit(72, UnitKind::nexus, true, {1500, 256});
    secondNexus.role = UnitRole::resourceDepot;
    established.self.units.push_back(secondNexus);
    for (int id = 80; id < 86; ++id) {
        auto probe = unit(id, UnitKind::probe, true, {1600 + (id % 4) * 16, 480});
        probe.role = UnitRole::worker;
        established.self.units.push_back(probe);
    }
    const auto establishedPlan = StrategyEngine{}.plan(established, {});
    check(hasGoal(establishedPlan, GoalKind::build, UnitKind::templarArchives, 123, true) &&
              hasStormGoal(establishedPlan),
          "a 2-base 28-Probe mirror unlocks its planned Storm transition");

    auto startedArchives = unit(73, UnitKind::templarArchives, true, {640, 320});
    startedArchives.lastSeen = oneBase.frame;
    oneBase.self.units.push_back(startedArchives);
    const auto continuedTech = StrategyEngine{}.plan(oneBase, {});
    check(hasStormGoal(continuedTech),
          "an already-started Templar transition continues through temporary economy loss");
}

}  // namespace

int main() {
    testRemoteArmyDoesNotCreateHomeEmergency();
    testHomeContactRequestsHomeDefense();
    testGlobalAggressionDoesNotCreateHomeBreach();
    testVisibleHomeContactKeepsDefenseStableDuringRegroup();
    testExpansionCommitmentSurvivesTransientPlanDropout();
    testMidfieldRallyDoesNotTrackUnitJitter();
    testNaturalApproachIsLocalToTheExpansion();
    testQuietRangedOpeningAndCannonsStayBounded();
    testTwoGateAndCoreRangedBranches();
    testContainDefenseDetectionAndEconomicExit();
    testDestroyedCoreRecoversGasAndRangedProduction();
    testExpensiveMirrorTechWaitsForExpansionEconomy();
    if (failures != 0) return 1;
    std::cout << "PvP threat locality checks passed\n";
    return 0;
}
