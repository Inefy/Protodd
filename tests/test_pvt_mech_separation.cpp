#include "protodd/MacroPlanner.hpp"
#include "protodd/Strategy.hpp"

#include <algorithm>
#include <iostream>
#include <ranges>

namespace {

using namespace protodd;

int failures = 0;

void check(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

UnitSnapshot unit(const UnitId id, const UnitKind kind, const Position position,
                  const bool ours) {
    UnitSnapshot result;
    result.id = id;
    result.kind = kind;
    result.position = position;
    result.lastPosition = position;
    result.lastSeen = 8 * 60 * 24;
    result.visible = true;
    result.completed = true;
    result.ours = ours;
    result.hitPoints = result.maxHitPoints = 100;
    result.topSpeed = 4.0;
    return result;
}

GameState establishedPvT(const Frame frame) {
    GameState state;
    state.frame = frame;
    state.self.id = 1;
    state.self.race = Race::protoss;
    state.self.supplyUsed = 96;
    state.self.supplyTotal = 120;
    state.self.minerals = 1000;
    state.self.gas = 1000;
    state.enemy.id = 2;
    state.enemy.race = Race::terran;

    auto main = unit(1, UnitKind::nexus, {256, 256}, true);
    main.role = UnitRole::resourceDepot;
    auto natural = unit(2, UnitKind::nexus, {1500, 256}, true);
    natural.role = UnitRole::resourceDepot;
    state.self.units = {main, natural,
        unit(3, UnitKind::pylon, {160, 160}, true),
        unit(4, UnitKind::gateway, {420, 256}, true),
        unit(5, UnitKind::gateway, {500, 256}, true),
        unit(6, UnitKind::assimilator, {480, 320}, true),
        unit(7, UnitKind::cyberneticsCore, {520, 320}, true),
        unit(8, UnitKind::roboticsFacility, {600, 320}, true),
        unit(9, UnitKind::observatory, {680, 320}, true)};
    for (int id = 20; id < 24; ++id)
        state.self.units.push_back(unit(id, UnitKind::dragoon,
                                       {360 + id * 4, 350}, true));
    for (int id = 40; id < 64; ++id) {
        auto probe = unit(id, UnitKind::probe, {460 + (id % 5) * 16, 470}, true);
        probe.role = UnitRole::worker;
        state.self.units.push_back(probe);
    }

    BaseSnapshot ownedMain;
    ownedMain.id = 1;
    ownedMain.center = {256, 256};
    ownedMain.ownerId = state.self.id;
    ownedMain.startLocation = true;
    ownedMain.mineralsRemaining = 8000;
    BaseSnapshot ownedNatural;
    ownedNatural.id = 2;
    ownedNatural.center = {1500, 256};
    ownedNatural.ownerId = state.self.id;
    ownedNatural.mineralsRemaining = 8000;
    ownedNatural.mineralPatches = 8;
    BaseSnapshot enemyMain;
    enemyMain.id = 3;
    enemyMain.center = {3300, 3200};
    enemyMain.ownerId = state.enemy.id;
    enemyMain.startLocation = true;
    enemyMain.mineralsRemaining = 8000;
    state.bases = {ownedMain, ownedNatural, enemyMain};
    return state;
}

void testMechContactUsesRangeAndDetectionInsteadOfBioFortification() {
    auto state = establishedPvT(8 * 60 * 24);
    for (int id = 100; id < 102; ++id) {
        auto factory = unit(id, UnitKind::factory, {3100 + id * 8, 3000}, false);
        factory.lastSeen = state.frame;
        state.enemy.units.push_back(factory);
    }
    for (int id = 110; id < 112; ++id) {
        auto tank = unit(id, UnitKind::siegeTank, {700 + (id - 110) * 96, 256}, false);
        tank.lastPosition = {980 + (id - 110) * 96, 256};
        tank.lastSeen = state.frame;
        tank.groundWeapon = {70, 75, 64, 384, DamageType::explosive, false, true};
        state.enemy.units.push_back(tank);
    }

    ThreatAssessment threat;
    threat.uncertainty = 0.2;
    threat.combatEnemiesNearMain = 2;
    threat.approachingArmyValue = 4.0;
    threat.immediateGround = 0.7;
    const auto plan = StrategyEngine{}.plan(state, threat);

    check(plan.posture == Posture::defend &&
              plan.name.find("anti-mech response") != std::string::npos &&
              plan.desiredWorkers > 14,
          "observed Tanks at home trigger a local mech defense posture");
    check(plan.requireMobileDetection && plan.desiredGasWorkers >= 3 &&
              std::ranges::any_of(plan.goals, [](const ProductionGoal& goal) {
                  return goal.target == UnitKind::observer && goal.blocking;
              }),
          "Factories and Tanks keep the powered Observer path funded");
    check(std::ranges::none_of(plan.goals, [](const ProductionGoal& goal) {
              return goal.target == UnitKind::forge ||
                     goal.target == UnitKind::photonCannon;
          }),
          "mech-only contact does not reuse the Forge and Cannon bio-intercept queue");
    check(std::ranges::none_of(plan.goals, [](const ProductionGoal& goal) {
              return goal.target == UnitKind::zealot &&
                     goal.reason.find("field bodies before vulnerable dragoon tech") !=
                         std::string::npos;
          }),
          "mech-only contact does not reuse the sustained-bio Zealot reservation");
    check(std::ranges::any_of(plan.goals, [](const ProductionGoal& goal) {
              return goal.technology == TechnologyKind::singularityCharge &&
                     goal.blocking;
          }),
          "the ranged counter reserves Dragoon range against siege weapons");

    ResourceLedger ledger{1000, 1000};
    const auto actions = MacroPlanner{}.reconcile(state, plan, ledger);
    check(std::ranges::any_of(actions, [](const MacroAction& action) {
              return action.action == MacroActionKind::train &&
                     action.target == UnitKind::observer && action.reserved &&
                     action.executable;
          }),
          "the mech detection demand is executable with completed Robotics prerequisites");
    check(std::ranges::any_of(actions, [](const MacroAction& action) {
              return action.action == MacroActionKind::upgrade &&
                     action.technology == TechnologyKind::singularityCharge &&
                     action.reserved && action.executable;
          }),
          "the Dragoon range upgrade is executable with the completed Core");
}

void testObservedMineOpensMobileDetectionWithoutBioStaticSpend() {
    auto state = establishedPvT(8 * 60 * 24);
    auto factory = unit(100, UnitKind::factory, {3100, 3000}, false);
    factory.lastSeen = state.frame;
    auto mine = unit(101, UnitKind::spiderMine, {760, 300}, false);
    mine.lastSeen = state.frame;
    state.enemy.units = {factory, mine};

    ThreatAssessment threat;
    threat.uncertainty = 0.2;
    threat.combatEnemiesNearMain = 1;
    threat.immediateGround = 0.5;
    const auto plan = StrategyEngine{}.plan(state, threat);
    check(plan.requireMobileDetection && plan.desiredGasWorkers >= 3 &&
              std::ranges::any_of(plan.goals, [](const ProductionGoal& goal) {
                  return goal.target == UnitKind::observer && goal.blocking;
              }),
          "an observed mine independently makes the Observer path a funded requirement");
    check(std::ranges::none_of(plan.goals, [](const ProductionGoal& goal) {
              return goal.target == UnitKind::photonCannon ||
                     goal.target == UnitKind::forge;
          }),
          "mine evidence triggers detection rather than bio static defense");
}

void testRemoteMechEvidenceKeepsEconomicRecoveryOpen() {
    auto state = establishedPvT(9 * 60 * 24);
    for (int id = 100; id < 102; ++id) {
        auto factory = unit(id, UnitKind::factory, {3100 + id * 8, 3000}, false);
        factory.lastSeen = state.frame;
        state.enemy.units.push_back(factory);
    }
    ThreatAssessment uncertainty;
    uncertainty.uncertainty = 0.2;
    const auto plan = StrategyEngine{}.plan(state, uncertainty);
    check(plan.posture != Posture::defend && plan.desiredBases >= 2 &&
              plan.desiredWorkers > 14,
          "scouted distant Factory production prepares counters without freezing worker growth or expansion");
    check(std::ranges::none_of(plan.goals, [](const ProductionGoal& goal) {
              return goal.target == UnitKind::forge ||
                     goal.target == UnitKind::photonCannon;
          }),
          "remote Factory evidence does not buy static defenses for an absent bio rush");
}

void testMarineInterceptAndPostScreenWorkerRecovery() {
    auto bio = establishedPvT(6 * 60 * 24);
    std::erase_if(bio.self.units, [](const UnitSnapshot& own) {
        return own.kind == UnitKind::nexus && own.id == 2;
    });
    for (int id = 100; id < 102; ++id) {
        auto barracks = unit(id, UnitKind::barracks, {3000 + id * 8, 3000}, false);
        barracks.lastSeen = bio.frame;
        bio.enemy.units.push_back(barracks);
    }
    for (int id = 110; id < 114; ++id) {
        auto marine = unit(id, UnitKind::marine, {650 + id * 8, 256}, false);
        marine.firstSeen = 2200;
        marine.lastSeen = bio.frame;
        marine.groundWeapon = {6, 15, 0, 128, DamageType::normal, false, true};
        bio.enemy.units.push_back(marine);
    }
    const auto intercept = StrategyEngine{}.plan(bio, {});
    check(intercept.posture == Posture::defend && intercept.desiredWorkers <= 14 &&
              std::ranges::any_of(intercept.goals, [](const ProductionGoal& goal) {
                  return goal.target == UnitKind::photonCannon && goal.blocking;
              }) &&
              std::ranges::any_of(intercept.goals, [](const ProductionGoal& goal) {
                  return goal.target == UnitKind::zealot && goal.blocking;
              }),
          "a real early Marine rush still funds a static intercept and mobile screen");

    auto stabilized = establishedPvT(8 * 60 * 24);
    stabilized.self.units.push_back(unit(70, UnitKind::observer, {700, 300}, true));
    for (int id = 80; id < 83; ++id)
        stabilized.self.units.push_back(unit(id, UnitKind::photonCannon,
                                              {320 + id * 8, 320}, true));
    ThreatAssessment stalePressure;
    stalePressure.aggression = 0.9;
    const auto recovery = StrategyEngine{}.plan(stabilized, stalePressure);
    check(recovery.desiredBases >= 2 && recovery.desiredWorkers > 14,
          "a completed anti-bio screen lets worker growth and expansion recover after contact clears");
}

}  // namespace

int main() {
    testMechContactUsesRangeAndDetectionInsteadOfBioFortification();
    testObservedMineOpensMobileDetectionWithoutBioStaticSpend();
    testRemoteMechEvidenceKeepsEconomicRecoveryOpen();
    testMarineInterceptAndPostScreenWorkerRecovery();
    if (failures != 0) return 1;
    std::cout << "PvT mech separation checks passed\n";
    return 0;
}
