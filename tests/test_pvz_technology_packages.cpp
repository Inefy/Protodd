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
    result.completed = true;
    result.powered = true;
    result.hitPoints = result.maxHitPoints = 100;
    return result;
}

GameState establishedPvZ(const int minute = 8) {
    GameState state;
    state.frame = minute * 60 * 24;
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
        unit(5, UnitKind::pylon, true, {700, 400}),
        unit(6, UnitKind::pylon, true, {1100, 500}),
        unit(7, UnitKind::forge, true, {350, 400}),
        unit(8, UnitKind::photonCannon, true, {400, 400}),
        unit(9, UnitKind::photonCannon, true, {1500, 500}),
        unit(10, UnitKind::gateway, true, {500, 400}),
        unit(11, UnitKind::gateway, true, {600, 500}),
        unit(12, UnitKind::assimilator, true, {700, 500}),
        unit(13, UnitKind::cyberneticsCore, true, {600, 400}),
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
             const GoalKind kind) {
    return std::ranges::any_of(plan.goals, [target, kind](const ProductionGoal& goal) {
        return goal.target == target && goal.goal == kind;
    });
}

bool hasTechnology(const StrategicPlan& plan, const TechnologyKind technology) {
    return std::ranges::any_of(plan.goals, [technology](const ProductionGoal& goal) {
        return goal.technology == technology;
    });
}

bool hasAction(const std::vector<MacroAction>& actions, const UnitKind target,
               const MacroActionKind kind) {
    return std::ranges::any_of(actions, [target, kind](const MacroAction& action) {
        return action.target == target && action.action == kind && action.reserved &&
               action.executable;
    });
}

bool hasResearchAction(const std::vector<MacroAction>& actions,
                       const TechnologyKind technology) {
    return std::ranges::any_of(actions, [technology](const MacroAction& action) {
        return action.action == MacroActionKind::research &&
               action.technology == technology && action.reserved && action.executable;
    });
}

bool hasUpgradeAction(const std::vector<MacroAction>& actions,
                      const TechnologyKind technology) {
    return std::ranges::any_of(actions, [technology](const MacroAction& action) {
        return action.action == MacroActionKind::upgrade &&
               action.technology == technology && action.reserved && action.executable;
    });
}

void testLateClockAloneDoesNotOpenPvZTechPackages() {
    const auto state = establishedPvZ(12);
    const auto plan = StrategyEngine{}.plan(state, {});
    check(std::ranges::none_of(plan.goals, [](const ProductionGoal& goal) {
              return goal.target == UnitKind::stargate ||
                     goal.target == UnitKind::citadelOfAdun ||
                     goal.target == UnitKind::templarArchives ||
                     goal.target == UnitKind::corsair ||
                     goal.target == UnitKind::highTemplar ||
                     goal.technology == TechnologyKind::legEnhancements ||
                     goal.technology == TechnologyKind::psionicStorm ||
                     goal.technology == TechnologyKind::khaydarinAmulet ||
                     goal.technology == TechnologyKind::protossAirWeapons;
          }),
          "PvZ clocks alone do not start air, speed, Storm or Templar packages");
    ResourceLedger bank{1000, 1000};
    const auto actions = MacroPlanner{}.reconcile(state, plan, bank);
    check(std::ranges::none_of(actions, [](const MacroAction& action) {
              return action.target == UnitKind::stargate ||
                     action.target == UnitKind::citadelOfAdun ||
                     action.target == UnitKind::templarArchives ||
                     action.target == UnitKind::corsair ||
                     action.target == UnitKind::highTemplar ||
                     action.technology == TechnologyKind::psionicStorm ||
                     action.technology == TechnologyKind::legEnhancements;
          }),
          "the unscouted baseline does not spend on a speculative tech chain");
}

void testSpireAndMutaliskEvidenceStageTheBoundedAirPackage() {
    auto state = establishedPvZ();
    auto spire = unit(100, UnitKind::spire, false, {2600, 2500});
    spire.lastSeen = state.frame;
    state.enemy.units.push_back(spire);
    auto plan = StrategyEngine{}.plan(state, {});
    check(std::ranges::any_of(plan.goals, [](const ProductionGoal& goal) {
              return goal.goal == GoalKind::build && goal.target == UnitKind::stargate &&
                     goal.blocking && goal.desiredCount == 1;
          }) && std::ranges::any_of(plan.goals, [](const ProductionGoal& goal) {
              return goal.goal == GoalKind::train && goal.target == UnitKind::corsair &&
                     goal.desiredCount >= 3 && goal.desiredCount <= 4;
          }),
          "a recent Spire opens a bounded powered Stargate and Corsair package");
    check(!hasTechnology(plan, TechnologyKind::psionicStorm) &&
              !hasTechnology(plan, TechnologyKind::legEnhancements) &&
              !hasGoal(plan, UnitKind::highTemplar, GoalKind::train),
          "air-only evidence does not open the Storm or speed package");

    MacroPlanner macro;
    ResourceLedger stargateBank{600, 400};
    auto actions = macro.reconcile(state, plan, stargateBank);
    check(hasAction(actions, UnitKind::stargate, MacroActionKind::build),
          "the observed-air package first funds a legal Stargate");

    state.frame++;
    state.self.units.push_back(unit(110, UnitKind::stargate, true, {1200, 400}));
    plan = StrategyEngine{}.plan(state, {});
    ResourceLedger corsairBank{600, 400};
    actions = macro.reconcile(state, plan, corsairBank);
    check(hasAction(actions, UnitKind::corsair, MacroActionKind::train),
          "a powered Stargate reaches executable Corsair production");

    for (int id = 120; id < 124; ++id) {
        auto mutalisk = unit(id, UnitKind::mutalisk, false, {2600 + id, 2500});
        mutalisk.flying = true;
        mutalisk.lastSeen = state.frame;
        state.enemy.units.push_back(mutalisk);
    }
    for (int id = 130; id < 133; ++id)
        state.self.units.push_back(unit(id, UnitKind::corsair, true, {1200, 500}));
    plan = StrategyEngine{}.plan(state, {});
    check(std::ranges::any_of(plan.goals, [](const ProductionGoal& goal) {
              return goal.target == UnitKind::corsair && goal.desiredCount <= 8;
          }) && hasTechnology(plan, TechnologyKind::protossAirWeapons),
          "fielded Corsairs and confirmed air mass unlock a bounded air-weapon upgrade");
    ResourceLedger upgradeBank{1000, 1000};
    actions = macro.reconcile(state, plan, upgradeBank);
    check(hasUpgradeAction(actions, TechnologyKind::protossAirWeapons),
          "the evidence-gated air weapon upgrade is executable at the Cybernetics Core");
}

void testMediumLingEvidenceStagesSpeedWithoutStorm() {
    auto state = establishedPvZ();
    for (int id = 100; id < 108; ++id) {
        auto ling = unit(id, UnitKind::zergling, false, {2600 + id, 2500});
        ling.lastSeen = state.frame;
        state.enemy.units.push_back(ling);
    }
    const auto plan = StrategyEngine{}.plan(state, {});
    check(hasTechnology(plan, TechnologyKind::legEnhancements) &&
              hasGoal(plan, UnitKind::zealot, GoalKind::train) &&
              !hasTechnology(plan, TechnologyKind::psionicStorm) &&
              !hasGoal(plan, UnitKind::highTemplar, GoalKind::train),
          "a screened medium Ling count chooses bounded speed Zealots without Storm");
    check(std::ranges::none_of(plan.goals, [](const ProductionGoal& goal) {
              return goal.target == UnitKind::corsair || goal.target == UnitKind::reaver;
          }),
          "the speed package does not add unrelated air or splash tech");
    ResourceLedger bank{500, 300};
    MacroPlanner macro;
    auto actions = macro.reconcile(state, plan, bank);
    check(hasAction(actions, UnitKind::citadelOfAdun, MacroActionKind::build),
          "the speed package stages its Citadel prerequisite from own Core access");

    state.frame++;
    state.self.units.push_back(unit(120, UnitKind::citadelOfAdun, true, {700, 550}));
    const auto completedPlan = StrategyEngine{}.plan(state, {});
    ResourceLedger completionBank{1000, 1000};
    actions = macro.reconcile(state, completedPlan, completionBank);
    check(hasUpgradeAction(actions, TechnologyKind::legEnhancements) &&
              hasAction(actions, UnitKind::zealot, MacroActionKind::train),
          "the powered speed package researches Leg Enhancements and fields its Zealot support");
}

void testLargeLingEvidenceStagesStormAndTemplars() {
    auto state = establishedPvZ();
    for (int id = 100; id < 114; ++id) {
        auto ling = unit(id, UnitKind::zergling, false, {2600 + id, 2500});
        ling.lastSeen = state.frame;
        state.enemy.units.push_back(ling);
    }
    MacroPlanner macro;
    auto plan = StrategyEngine{}.plan(state, {});
    check(hasTechnology(plan, TechnologyKind::psionicStorm) &&
              !hasTechnology(plan, TechnologyKind::legEnhancements) &&
              !hasGoal(plan, UnitKind::reaver, GoalKind::train) &&
              !hasGoal(plan, UnitKind::highTemplar, GoalKind::train),
          "a large Ling swarm chooses Storm alone until Storm itself is researched");
    ResourceLedger citadelBank{600, 400};
    auto actions = macro.reconcile(state, plan, citadelBank);
    check(hasAction(actions, UnitKind::citadelOfAdun, MacroActionKind::build),
          "Storm research stages the powered Citadel before the Archives");

    state.frame++;
    state.self.units.push_back(unit(120, UnitKind::citadelOfAdun, true, {700, 550}));
    plan = StrategyEngine{}.plan(state, {});
    ResourceLedger archivesBank{600, 400};
    actions = macro.reconcile(state, plan, archivesBank);
    check(hasAction(actions, UnitKind::templarArchives, MacroActionKind::build),
          "the completed Citadel unlocks the powered Archives");

    state.frame++;
    state.self.units.push_back(unit(121, UnitKind::templarArchives, true, {800, 500}));
    plan = StrategyEngine{}.plan(state, {});
    ResourceLedger stormBank{600, 400};
    actions = macro.reconcile(state, plan, stormBank);
    check(hasResearchAction(actions, TechnologyKind::psionicStorm),
          "the completed Archives research Storm before funding spellcasters");

    state.frame++;
    state.self.technologies.push_back({TechnologyKind::psionicStorm, 1, false});
    plan = StrategyEngine{}.plan(state, {});
    check(std::ranges::any_of(plan.goals, [](const ProductionGoal& goal) {
              return goal.target == UnitKind::highTemplar &&
                     goal.desiredCount >= 2 && goal.desiredCount <= 4;
          }),
          "researched Storm opens a bounded two-to-four Templar support group");
    ResourceLedger templarBank{500, 400};
    actions = macro.reconcile(state, plan, templarBank);
    check(hasAction(actions, UnitKind::highTemplar, MacroActionKind::train),
          "the researched Storm package fields its first powered High Templar");
}

}  // namespace

int main() {
    testLateClockAloneDoesNotOpenPvZTechPackages();
    testSpireAndMutaliskEvidenceStageTheBoundedAirPackage();
    testMediumLingEvidenceStagesSpeedWithoutStorm();
    testLargeLingEvidenceStagesStormAndTemplars();
    return failures == 0 ? 0 : 1;
}
