#include "protodd/Workers.hpp"

#include <iostream>

int main() {
    using namespace protodd;
    int errors = 0;
    const auto check = [&](const bool value, const char* message) {
        if (!value) { ++errors; std::cerr << message << '\n'; }
    };

    GameState state;
    state.self.minerals = 100;
    state.self.gas = 800;
    StrategicPlan plan;
    plan.desiredGasWorkers = 6;
    plan.goals = {
        {GoalKind::research, UnitKind::unknown, 1, 100, true, "Storm",
         TechnologyKind::psionicStorm},
        {GoalKind::upgrade, UnitKind::unknown, 1, 99, true, "Dragoon range",
         TechnologyKind::singularityCharge},
        {GoalKind::train, UnitKind::observer, 1, 95, true, "first Observer"},
        {GoalKind::train, UnitKind::arbiter, 1, 94, true, "first Arbiter"},
        {GoalKind::train, UnitKind::reaver, 1, 93, true, "first Reaver"},
    };
    GasBankController mixedPlan;
    check(mixedPlan.target(state, plan) == 6,
          "Storm, range, Observer, Arbiter, and Reaver obligations add together");
    state.self.gas = 974;
    ++state.frame;
    check(mixedPlan.target(state, plan) == 6,
          "gas collection continues until the combined reserve plus buffer is funded");
    state.self.gas = 975;
    ++state.frame;
    check(mixedPlan.target(state, plan) == 0,
          "a fully funded combined reserve pauses gas collection");

    GameState upgradeState;
    upgradeState.self.minerals = 100;
    upgradeState.self.gas = 499;
    upgradeState.self.technologies = {{TechnologyKind::protossPlasmaShields, 2, false}};
    StrategicPlan upgradePlan;
    upgradePlan.desiredGasWorkers = 4;
    upgradePlan.goals = {{GoalKind::upgrade, UnitKind::unknown, 3, 100, true,
                          "next shields level", TechnologyKind::protossPlasmaShields}};
    GasBankController upgradePolicy;
    check(upgradePolicy.target(upgradeState, upgradePlan) == 4,
          "an unknown-target upgrade reserves the next level's gas cost");
    upgradeState.self.gas = 500;
    ++upgradeState.frame;
    check(upgradePolicy.target(upgradeState, upgradePlan) == 0,
          "the next-level upgrade reserve uses its exact high-water threshold");

    GameState committedState;
    committedState.self.minerals = 100;
    committedState.self.gas = 299;
    committedState.self.units = {{}};
    committedState.self.units.back().kind = UnitKind::arbiter;
    committedState.self.units.back().ammo = 0;
    committedState.self.queuedUnits = {UnitKind::reaver, UnitKind::observer};
    committedState.self.technologies = {{TechnologyKind::psionicStorm, 0, true}};
    StrategicPlan committedPlan;
    committedPlan.desiredGasWorkers = 3;
    committedPlan.goals = {
        {GoalKind::research, UnitKind::unknown, 1, 100, true, "Storm",
         TechnologyKind::psionicStorm},
        {GoalKind::train, UnitKind::arbiter, 1, 90, true, "Arbiter"},
        {GoalKind::train, UnitKind::reaver, 1, 89, true, "Reaver"},
        {GoalKind::train, UnitKind::observer, 1, 88, true, "Observer"},
    };
    GasBankController committedPolicy;
    check(committedPolicy.target(committedState, committedPlan) == 3,
          "active research and already-issued units are not reserved a second time");
    committedState.self.gas = 300;
    ++committedState.frame;
    check(committedPolicy.target(committedState, committedPlan) == 0,
          "the base reserve governs after the research and unit costs are committed");

    const auto ammoOnlyTarget = [](const int ammo) {
        GameState ammoState;
        ammoState.self.minerals = 100;
        ammoState.self.gas = 350;
        UnitSnapshot reaver;
        reaver.kind = UnitKind::reaver;
        reaver.ammo = ammo;
        ammoState.self.units.push_back(reaver);
        StrategicPlan ammoPlan;
        ammoPlan.desiredGasWorkers = 2;
        ammoPlan.goals = {{GoalKind::train, UnitKind::reaver, 1, 90, true,
                           "paid Reaver; replenish ammunition"}};
        return GasBankController{}.target(ammoState, ammoPlan);
    };
    check(ammoOnlyTarget(0) == 0 && ammoOnlyTarget(5) == 0,
          "mineral-only Reaver ammunition does not create a duplicate gas obligation");

    GameState hysteresisState;
    hysteresisState.self.minerals = 100;
    hysteresisState.self.gas = 350;
    StrategicPlan noObligations;
    noObligations.desiredGasWorkers = 5;
    GasBankController hysteresis;
    check(hysteresis.target(hysteresisState, noObligations) == 0,
          "a mineral-starved bank pauses gas at the base reserve");
    hysteresisState.self.gas = 200;
    ++hysteresisState.frame;
    check(hysteresis.target(hysteresisState, noObligations) == 0,
          "small resource changes do not resume gas collection prematurely");
    hysteresisState.self.gas = 149;
    ++hysteresisState.frame;
    check(hysteresis.target(hysteresisState, noObligations) == 5,
          "gas collection resumes before the protected bank is exhausted");

    if (!errors) std::cout << "Gas-bank controller scenarios passed\n";
    return errors ? 1 : 0;
}
