#include "protodd/MacroPlanner.hpp"
#include "protodd/Strategy.hpp"

#include <algorithm>
#include <iostream>
#include <ranges>

namespace {

using namespace protodd;

int failures = 0;

void check(const bool condition, const char* message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

UnitSnapshot unit(const UnitId id, const UnitKind kind, const bool ours,
                  const Position position) {
    UnitSnapshot result;
    result.id = id;
    result.kind = kind;
    result.ours = ours;
    result.position = position;
    result.lastPosition = position;
    result.visible = true;
    result.lastSeen = 8 * 60 * 24;
    result.completed = true;
    result.powered = true;
    result.hitPoints = result.maxHitPoints = 100;
    return result;
}

GameState establishedPvZ() {
    GameState state;
    state.frame = 8 * 60 * 24;
    state.self.id = 1;
    state.self.race = Race::protoss;
    state.self.supplyUsed = 100;
    state.self.supplyTotal = 160;
    state.self.minerals = 1000;
    state.self.gas = 1000;
    state.enemy.id = 2;
    state.enemy.race = Race::zerg;
    state.self.units = {
        unit(1, UnitKind::nexus, true, {256, 256}),
        unit(2, UnitKind::nexus, true, {1500, 256}),
        unit(3, UnitKind::pylon, true, {256, 400}),
        unit(4, UnitKind::pylon, true, {1500, 400}),
        unit(16, UnitKind::pylon, true, {700, 400}),
        unit(17, UnitKind::pylon, true, {1100, 500}),
        unit(5, UnitKind::forge, true, {350, 400}),
        unit(6, UnitKind::photonCannon, true, {400, 400}),
        unit(7, UnitKind::photonCannon, true, {1500, 500}),
        unit(8, UnitKind::gateway, true, {500, 400}),
        unit(9, UnitKind::gateway, true, {600, 500}),
        unit(10, UnitKind::assimilator, true, {700, 500}),
        unit(11, UnitKind::cyberneticsCore, true, {600, 400}),
        unit(12, UnitKind::roboticsFacility, true, {800, 400}),
        unit(13, UnitKind::roboticsSupportBay, true, {900, 500}),
        unit(14, UnitKind::observatory, true, {1050, 400}),
        unit(15, UnitKind::stargate, true, {1200, 400}),
    };
    for (int id = 20; id < 24; ++id)
        state.self.units.push_back(unit(id, UnitKind::zealot, true, {480, 500}));
    for (int id = 30; id < 34; ++id)
        state.self.units.push_back(unit(id, UnitKind::dragoon, true, {560, 500}));
    for (int id = 40; id < 70; ++id) {
        auto probe = unit(id, UnitKind::probe, true, {320, 500});
        probe.role = UnitRole::worker;
        state.self.units.push_back(probe);
    }
    BaseSnapshot main;
    main.id = 1;
    main.center = {256, 256};
    main.ownerId = state.self.id;
    main.mineralsRemaining = 8000;
    main.lastScouted = state.frame;
    main.startLocation = true;
    main.mineralPatches = 8;
    main.geysers = 1;
    BaseSnapshot natural;
    natural.id = 2;
    natural.center = {1500, 256};
    natural.ownerId = state.self.id;
    natural.mineralsRemaining = 8000;
    natural.lastScouted = state.frame;
    natural.mineralPatches = 8;
    natural.geysers = 1;
    state.bases = {main, natural};
    return state;
}

bool hasGoal(const StrategicPlan& plan, const UnitKind target,
             const GoalKind kind = GoalKind::train) {
    return std::ranges::any_of(plan.goals, [target, kind](const ProductionGoal& goal) {
        return goal.target == target && goal.goal == kind;
    });
}

bool hasReservedAction(const std::vector<MacroAction>& actions,
                       const UnitKind target, const MacroActionKind kind) {
    return std::ranges::any_of(actions, [target, kind](const MacroAction& action) {
        return action.target == target && action.action == kind && action.reserved &&
               action.executable;
    });
}

void testHydraDeadlineExcludesLurkerDetectorSpend() {
    auto state = establishedPvZ();
    for (int id = 100; id < 107; ++id) {
        auto hydra = unit(id, UnitKind::hydralisk, false, {2600 + id, 2500});
        hydra.role = UnitRole::groundArmy;
        state.enemy.units.push_back(hydra);
    }

    const auto plan = StrategyEngine{}.plan(state, {});
    check(hasGoal(plan, UnitKind::reaver) &&
              std::ranges::any_of(plan.goals, [](const ProductionGoal& goal) {
                  return goal.target == UnitKind::reaver && goal.desiredCount >= 2;
              }),
          "Hydra mass opens the Reaver splash deadline");
    check(!plan.requireMobileDetection && !hasGoal(plan, UnitKind::observer),
          "Hydra-only pressure does not spend the separate Lurker detector budget");
    ResourceLedger bank{300, 150};
    const auto actions = MacroPlanner{}.reconcile(state, plan, bank);
    check(hasReservedAction(actions, UnitKind::reaver, MacroActionKind::train),
          "the funded Hydra splash chain reaches an executable Reaver");
}

void testHydraChainAdvancesThroughPoweredPrerequisites() {
    auto state = establishedPvZ();
    std::erase_if(state.self.units, [](const UnitSnapshot& friendly) {
        return friendly.kind == UnitKind::roboticsFacility ||
               friendly.kind == UnitKind::roboticsSupportBay ||
               friendly.kind == UnitKind::observatory;
    });
    for (int id = 100; id < 107; ++id)
        state.enemy.units.push_back(unit(id, UnitKind::hydralisk, false,
                                         {2600 + id, 2500}));

    MacroPlanner macro;
    auto plan = StrategyEngine{}.plan(state, {});
    ResourceLedger firstBank{350, 250};
    const auto first = macro.reconcile(state, plan, firstBank);
    check(hasReservedAction(first, UnitKind::roboticsFacility, MacroActionKind::build),
          "Hydra pressure spends first on the powered Robotics Facility prerequisite");

    state.frame++;
    state.self.units.push_back(unit(110, UnitKind::roboticsFacility, true, {800, 400}));
    plan = StrategyEngine{}.plan(state, {});
    ResourceLedger secondBank{500, 300};
    const auto second = macro.reconcile(state, plan, secondBank);
    check(hasReservedAction(second, UnitKind::roboticsSupportBay, MacroActionKind::build) &&
              std::ranges::none_of(second, [](const MacroAction& action) {
                  return action.target == UnitKind::reaver &&
                         action.action == MacroActionKind::train;
              }),
          "completed Robotics advances to the powered Support Bay before Reaver training");

    state.frame++;
    state.self.units.push_back(unit(111, UnitKind::roboticsSupportBay, true, {900, 500}));
    plan = StrategyEngine{}.plan(state, {});
    ResourceLedger finalBank{300, 150};
    const auto final = macro.reconcile(state, plan, finalBank);
    check(hasReservedAction(final, UnitKind::reaver, MacroActionKind::train),
          "completed Support Bay unlocks executable Reaver production");
}

void testLurkerDeadlineFundsMobileDetection() {
    auto state = establishedPvZ();
    std::erase_if(state.self.units, [](const UnitSnapshot& friendly) {
        return friendly.kind == UnitKind::assimilator ||
               friendly.kind == UnitKind::roboticsFacility ||
               friendly.kind == UnitKind::observatory;
    });
    auto lurker = unit(100, UnitKind::lurker, false, {2600, 2500});
    lurker.role = UnitRole::groundArmy;
    state.enemy.units.push_back(lurker);

    auto plan = StrategyEngine{}.plan(state, {});
    check(plan.requireMobileDetection && hasGoal(plan, UnitKind::observer) &&
              hasGoal(plan, UnitKind::assimilator, GoalKind::build),
          "observed Lurker evidence independently opens mobile detection and gas");
    MacroPlanner macro;
    ResourceLedger firstBank{500, 200};
    const auto first = macro.reconcile(state, plan, firstBank);
    check(hasReservedAction(first, UnitKind::roboticsFacility, MacroActionKind::build) &&
              hasReservedAction(first, UnitKind::assimilator, MacroActionKind::build),
          "Lurker response funds powered Robotics and gas prerequisites first");

    state.frame++;
    state.self.units.push_back(unit(110, UnitKind::roboticsFacility, true, {800, 400}));
    plan = StrategyEngine{}.plan(state, {});
    ResourceLedger secondBank{300, 200};
    const auto second = macro.reconcile(state, plan, secondBank);
    check(hasReservedAction(second, UnitKind::observatory, MacroActionKind::build),
          "completed Robotics unlocks the powered Observatory before Observer training");

    state.frame++;
    state.self.units.push_back(unit(111, UnitKind::assimilator, true, {700, 500}));
    state.self.units.push_back(unit(112, UnitKind::observatory, true, {1050, 400}));
    plan = StrategyEngine{}.plan(state, {});
    ResourceLedger finalBank{200, 200};
    const auto final = macro.reconcile(state, plan, finalBank);
    check(hasReservedAction(final, UnitKind::observer, MacroActionKind::train),
          "powered detector infrastructure reaches executable Observer production");
}

void testMutaliskDeadlineDoesNotOpenGroundSplash() {
    auto state = establishedPvZ();
    for (int id = 100; id < 104; ++id) {
        auto mutalisk = unit(id, UnitKind::mutalisk, false, {2600 + id, 2500});
        mutalisk.role = UnitRole::airArmy;
        mutalisk.flying = true;
        state.enemy.units.push_back(mutalisk);
    }

    const auto plan = StrategyEngine{}.plan(state, {});
    check(std::ranges::any_of(plan.goals, [](const ProductionGoal& goal) {
              return goal.target == UnitKind::corsair && goal.desiredCount >= 6;
          }) && hasGoal(plan, UnitKind::dragoon),
          "Mutalisk pressure opens Corsair and Dragoon air-control goals");
    check(!hasGoal(plan, UnitKind::reaver) && !hasGoal(plan, UnitKind::observer) &&
              std::ranges::none_of(plan.goals, [](const ProductionGoal& goal) {
                  return goal.target == UnitKind::roboticsSupportBay;
              }),
          "Mutalisk-only pressure does not stack unrelated ground splash or detector tech");
    ResourceLedger bank{500, 300};
    const auto actions = MacroPlanner{}.reconcile(state, plan, bank);
    check(hasReservedAction(actions, UnitKind::corsair, MacroActionKind::train) &&
              hasReservedAction(actions, UnitKind::dragoon, MacroActionKind::train),
          "the powered Stargate and Gateway execute the Mutalisk response");

    ResourceLedger shortBank{100, 50};
    const auto shortBankActions = MacroPlanner{}.reconcile(
        state, plan, shortBank);
    check(hasReservedAction(shortBankActions, UnitKind::probe, MacroActionKind::train) &&
              std::ranges::none_of(shortBankActions, [](const MacroAction& action) {
                  return (action.target == UnitKind::corsair ||
                          action.target == UnitKind::dragoon) && action.reserved;
              }),
          "an unaffordable flyer package leaves the current Probe cycle available");
}

void testEarlyPoolDefenseAndEconomicRelease() {
    auto state = establishedPvZ();
    state.frame = 3 * 60 * 24;
    state.self.units.erase(std::remove_if(state.self.units.begin(), state.self.units.end(),
        [](const UnitSnapshot& friendly) {
            return (friendly.kind == UnitKind::nexus && friendly.id == 2) ||
                   friendly.kind == UnitKind::photonCannon ||
                   friendly.kind == UnitKind::roboticsFacility ||
                   friendly.kind == UnitKind::roboticsSupportBay ||
                   friendly.kind == UnitKind::observatory ||
                   friendly.kind == UnitKind::stargate ||
                   friendly.kind == UnitKind::dragoon ||
                   friendly.kind == UnitKind::zealot;
        }), state.self.units.end());
    for (int id = 110; id < 118; ++id) {
        auto ling = unit(id, UnitKind::zergling, false, {300 + id % 3 * 12, 300});
        ling.role = UnitRole::groundArmy;
        ling.groundWeapon = {5, 8, 0, 32, DamageType::normal, false, true};
        state.enemy.units.push_back(ling);
    }
    ThreatAssessment rush;
    rush.uncertainty = 0.2;
    rush.mostLikely = EnemyPlan::fastRush;
    rush.combatEnemiesNearMain = 8;
    rush.immediateGround = 0.8;
    const auto defense = StrategyEngine{}.plan(state, rush);
    check(defense.posture == Posture::defend && defense.desiredWorkers <= 12 &&
              std::ranges::any_of(defense.goals, [](const ProductionGoal& goal) {
                  return goal.target == UnitKind::zealot && goal.blocking;
              }),
          "early Pool contact preserves the capped worker line and blocking Zealot screen");
    ResourceLedger defenseBank{600, 100};
    const auto defenseActions = MacroPlanner{}.reconcile(state, defense, defenseBank);
    check(hasReservedAction(defenseActions, UnitKind::photonCannon, MacroActionKind::build) ||
              hasReservedAction(defenseActions, UnitKind::zealot, MacroActionKind::train),
          "early Pool defense starts a funded static or mobile survival action");

    auto stable = state;
    stable.frame = 4 * 60 * 24;
    stable.enemy.units.clear();
    stable.self.units.push_back(unit(2, UnitKind::nexus, true, {1500, 256}));
    stable.self.units.push_back(unit(70, UnitKind::photonCannon, true, {400, 400}));
    stable.self.units.push_back(unit(71, UnitKind::photonCannon, true, {1500, 500}));
    for (int id = 80; id < 84; ++id)
        stable.self.units.push_back(unit(id, UnitKind::zealot, true, {480, 500}));
    const auto recovery = StrategyEngine{}.plan(stable, {});
    check(recovery.desiredWorkers > 12 && recovery.desiredBases >= 2 &&
              std::ranges::any_of(recovery.goals, [](const ProductionGoal& goal) {
                  return goal.goal == GoalKind::expand && goal.target == UnitKind::nexus;
              }),
          "completed Cannon and Zealot screens release worker growth and expansion");
    ResourceLedger workerBank{100, 0};
    const auto workerActions = MacroPlanner{}.reconcile(stable, recovery, workerBank);
    check(hasReservedAction(workerActions, UnitKind::probe, MacroActionKind::train),
          "the recovered economy resumes an executable Probe cycle");
}

void testExpiredTechEvidenceDoesNotReopenExperimentalChain() {
    auto state = establishedPvZ();
    state.frame = 6 * 60 * 24;
    auto den = unit(100, UnitKind::hydraliskDen, false, {2600, 2500});
    den.visible = false;
    den.lastSeen = state.frame - 91 * 24;
    state.enemy.units.push_back(den);
    auto spire = unit(101, UnitKind::spire, false, {2600, 2500});
    spire.visible = false;
    spire.lastSeen = state.frame - 91 * 24;
    state.enemy.units.push_back(spire);

    const auto plan = StrategyEngine{false, true}.plan(state, {});
    check(std::ranges::none_of(plan.goals, [](const ProductionGoal& goal) {
              return goal.target == UnitKind::roboticsFacility ||
                     goal.target == UnitKind::roboticsSupportBay ||
                     (goal.target == UnitKind::reaver && goal.blocking);
          }),
          "aged-out Hydra and air tech sightings do not restart the optional splash chain");
}

}  // namespace

int main() {
    testHydraDeadlineExcludesLurkerDetectorSpend();
    testHydraChainAdvancesThroughPoweredPrerequisites();
    testLurkerDeadlineFundsMobileDetection();
    testMutaliskDeadlineDoesNotOpenGroundSplash();
    testEarlyPoolDefenseAndEconomicRelease();
    testExpiredTechEvidenceDoesNotReopenExperimentalChain();
    return failures == 0 ? 0 : 1;
}
