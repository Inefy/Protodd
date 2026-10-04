#include "protodd/Information.hpp"
#include "protodd/Strategy.hpp"

#include <algorithm>
#include <iostream>

int main() {
    using namespace protodd;
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { ++failures; std::cerr << message << '\n'; }
    };
    const auto unit = [](int id, UnitKind kind, bool completed = true) {
        UnitSnapshot result;
        result.id = id;
        result.kind = kind;
        result.completed = completed;
        result.visible = true;
        result.position = {320, 320};
        result.lastSeen = 8 * 60 * 24;
        result.role = kind == UnitKind::probe ? UnitRole::worker :
            (kind == UnitKind::nexus ? UnitRole::resourceDepot : UnitRole::groundArmy);
        return result;
    };
    const auto hasGoal = [](const StrategicPlan& plan, UnitKind kind,
                            GoalKind goalKind = GoalKind::train) {
        return std::ranges::any_of(plan.goals, [=](const ProductionGoal& goal) {
            return goal.goal == goalKind && goal.target == kind;
        });
    };

    GameState state;
    state.frame = 8 * 60 * 24;
    state.self.race = Race::protoss;
    state.enemy.race = Race::terran;
    state.self.supplyUsed = 60;
    state.self.supplyTotal = 60;
    state.self.units = {unit(1, UnitKind::nexus), unit(2, UnitKind::pylon),
                        unit(3, UnitKind::gateway), unit(4, UnitKind::cyberneticsCore)};
    for (int i = 0; i < 20; ++i) state.self.units.push_back(unit(10 + i, UnitKind::probe));

    // A proxy Factory is not a mech plan by itself. A legally scouted second
    // Command Center confirms expansion without claiming unseen mech absence.
    OpponentModel model;
    auto cc1 = unit(100, UnitKind::commandCenter);
    auto cc2 = unit(101, UnitKind::commandCenter);
    auto factory = unit(102, UnitKind::factory);
    factory.position = {2400, 2400};
    state.enemy.units = {cc1, cc2, factory};
    model.update(state);
    check(!model.assessment().mechanizedPlanActive &&
              std::abs(model.assessment().mechanizedConfidence - 0.22) < 1e-12,
          "a main-base Factory with two Command Centers did not force a mech plan");
    auto proxyFactory = factory;
    proxyFactory.position = {500, 320};
    state.enemy.units = {cc1, cc2, proxyFactory};
    ++state.frame;
    OpponentModel proxyModel;
    proxyModel.update(state);
    check(proxyModel.assessment().mechanizedPlanActive &&
              std::abs(proxyModel.assessment().mechanizedConfidence - 0.42) < 1e-12,
          "proxied Factory enters at the exact 0.42 evidence threshold despite two Command Centers");
    auto corroboratingVulture = unit(107, UnitKind::vulture);
    corroboratingVulture.position = {2400, 2400};
    state.enemy.units = {factory, corroboratingVulture};
    ++state.frame;
    OpponentModel aboveThreshold;
    aboveThreshold.update(state);
    check(aboveThreshold.assessment().mechanizedPlanActive &&
              std::abs(aboveThreshold.assessment().mechanizedConfidence - 0.50) < 1e-12,
          "Factory plus Vulture reaches the stated 0.50 above-threshold confidence");

    // A fleeting Vulture is too weak to flip the plan, while a first observed
    // Tank or Mine is decisive and immediately requests mobile detection.
    auto vulture = unit(103, UnitKind::vulture);
    vulture.position = {2400, 2400};
    state.enemy.units = {vulture};
    ++state.frame;
    model.update(state);
    check(!model.assessment().mechanizedPlanActive,
          "one fleeting Vulture did not trigger a mech transition");
    check(model.mostLikelyPlan() == EnemyPlan::unknown,
          "one weak Vulture cue does not replace the current enemy plan label");
    for (const auto kind : {UnitKind::siegeTank, UnitKind::spiderMine}) {
        OpponentModel directCue;
        auto directUnit = unit(104, kind);
        directUnit.position = {2400, 2400};
        state.enemy.units = {directUnit};
        ++state.frame;
        directCue.update(state);
        check(directCue.assessment().mechanizedPlanActive &&
                  directCue.assessment().mechanizedEvidenceFresh &&
                  directCue.mostLikelyPlan() == EnemyPlan::mechanized,
              "the first directly observed Tank or Mine establishes a fresh mech plan");
        auto threat = directCue.assessment();
        auto plan = StrategyEngine{}.plan(state, threat);
        check(plan.requireMobileDetection && hasGoal(plan, UnitKind::observer) &&
                  hasGoal(plan, UnitKind::roboticsFacility, GoalKind::build),
              "a first Tank or Mine reserves the Observer prerequisite chain");
        check(plan.minimumAttackSize >= 20 && plan.posture == Posture::hold,
              "an undetected mech line holds the field army behind its opening");
    }

    // A fresh Observer permits a cautious pressure posture. A home breach
    // overrides that transition immediately and preserves emergency defense.
    auto tank = unit(105, UnitKind::siegeTank);
    tank.position = {2400, 2400};
    state.enemy.units = {tank};
    state.self.units.push_back(unit(80, UnitKind::observer));
    ThreatAssessment detectedMech;
    detectedMech.mechanizedPlanActive = true;
    detectedMech.mechanizedConfidence = 0.62;
    detectedMech.mostLikely = EnemyPlan::mechanized;
    detectedMech.uncertainty = 1.0;
    auto detectedPlan = StrategyEngine{}.plan(state, detectedMech);
    check(detectedPlan.requireMobileDetection && detectedPlan.posture == Posture::pressure,
          "a completed late Observer releases the army into cautious pressure");
    detectedMech.combatEnemiesNearMain = 1;
    const auto emergencyPlan = StrategyEngine{}.plan(state, detectedMech);
    check(emergencyPlan.posture == Posture::defend,
          "current local combat exits the mech hold into emergency defense");
    auto approachingMech = detectedMech;
    approachingMech.combatEnemiesNearMain = 0;
    approachingMech.approachingArmyValue = 2.0;
    check(StrategyEngine{}.plan(state, approachingMech).posture == Posture::defend,
          "an approaching enemy force triggers immediate mech defense");
    auto pressureMech = detectedMech;
    pressureMech.combatEnemiesNearMain = 0;
    pressureMech.immediateGround = 0.56;
    check(StrategyEngine{}.plan(state, pressureMech).posture == Posture::defend,
          "high immediate ground pressure triggers emergency mech defense");
    StrategicDirector director;
    auto emergencyContact = unit(106, UnitKind::siegeTank);
    emergencyContact.position = {320, 320};
    state.enemy.units = {emergencyContact};
    detectedMech.combatEnemiesNearMain = 1;
    auto directed = director.stabilize(
        StrategyEngine{}.plan(state, detectedMech), state, detectedMech);
    check(directed.posture == Posture::defend,
          "StrategicDirector enters defense immediately on a visible base breach");
    state.enemy.units.clear();
    detectedMech.combatEnemiesNearMain = 0;
    state.frame += 4;
    directed = director.stabilize(StrategyEngine{}.plan(state, detectedMech),
                                  state, detectedMech);
    check(directed.posture == Posture::defend,
          "emergency posture persists while the existing clear window is running");
    state.frame += 8 * 24;
    directed = director.stabilize(StrategyEngine{}.plan(state, detectedMech),
                                  state, detectedMech);
    check(directed.posture == Posture::pressure,
          "army pressure resumes only after the clear window expires");

    // Hysteresis preserves a previously confirmed plan beyond the ordinary
    // 90-second unit memory, then releases it after a longer quiet interval.
    OpponentModel staleModel;
    state.self.units.erase(std::remove_if(state.self.units.begin(), state.self.units.end(),
        [](const UnitSnapshot& u) { return u.kind == UnitKind::observer; }), state.self.units.end());
    state.enemy.units = {tank};
    tank.visible = true;
    tank.lastSeen = state.frame;
    state.enemy.units = {tank};
    staleModel.update(state);
    tank.visible = false;
    state.enemy.units = {tank};
    state.frame += 90 * 24;
    staleModel.update(state);
    check(staleModel.assessment().mechanizedPlanActive &&
              staleModel.assessment().mechanizedEvidenceFresh,
          "a cue remains fresh and latched at exactly 90 seconds");
    ++state.frame;
    staleModel.update(state);
    check(staleModel.assessment().mechanizedPlanActive &&
              !staleModel.assessment().mechanizedEvidenceFresh,
          "freshness expires immediately after 90 seconds while hysteresis remains latched");
    state.frame += 60 * 24 - 1;
    staleModel.update(state);
    check(staleModel.assessment().mechanizedPlanActive,
          "the plan remains latched at exactly 150 seconds");
    ++state.frame;
    staleModel.update(state);
    check(!staleModel.assessment().mechanizedPlanActive &&
              staleModel.assessment().mostLikely != EnemyPlan::mechanized,
          "the plan clears immediately after 150 seconds without inferring a bio opening");
    auto weakFactory = unit(108, UnitKind::factory);
    weakFactory.position = {2400, 2400};
    weakFactory.lastSeen = state.frame;
    state.enemy.units = {weakFactory};
    ++state.frame;
    staleModel.update(state);
    check(!staleModel.assessment().mechanizedPlanActive,
          "one fresh weak Factory cue cannot re-enter after the strong clue expires");
    auto freshTank = unit(109, UnitKind::siegeTank);
    freshTank.lastSeen = state.frame;
    staleModel.reset(Race::terran);
    state.enemy.units = {freshTank};
    ++state.frame;
    staleModel.update(state);
    check(staleModel.assessment().mechanizedPlanActive,
          "a direct Tank clue can establish the plan before the reset check");
    state.enemy.race = Race::protoss;
    state.enemy.units.clear();
    ++state.frame;
    staleModel.update(state);
    check(!staleModel.assessment().mechanizedPlanActive &&
              staleModel.mostLikelyPlan() != EnemyPlan::mechanized,
          "race reset clears the latched mech state and restores ordinary beliefs");

    // Releasing growth depends on our own completed screen and safe local
    // conditions. The two-Gateway arm identity and in-progress buildings stay.
    auto robo = unit(90, UnitKind::roboticsFacility, false);
    robo.visible = false;
    state.self.units.push_back(robo);
    state.self.units.push_back(unit(91, UnitKind::observer));
    for (int i = 0; i < 4; ++i) state.self.units.push_back(unit(100 + i, UnitKind::zealot));
    for (int i = 0; i < 4; ++i) state.self.units.push_back(unit(110 + i, UnitKind::dragoon));
    state.frame = 8 * 60 * 24;
    state.enemy.race = Race::terran;
    detectedMech.combatEnemiesNearMain = 0;
    auto safePlan = StrategyEngine{}.plan(state, detectedMech);
    check(safePlan.desiredBases >= 2 && hasGoal(safePlan, UnitKind::nexus, GoalKind::expand),
          "a stable detected defense releases a natural without hidden-state assumptions");
    check(std::ranges::count_if(safePlan.goals, [](const ProductionGoal& goal) {
              return goal.goal == GoalKind::build && goal.target == UnitKind::roboticsFacility;
          }) == 1 && std::ranges::any_of(safePlan.goals, [](const ProductionGoal& goal) {
              return goal.goal == GoalKind::build && goal.target == UnitKind::roboticsFacility &&
                     goal.desiredCount == 1;
          }),
          "the in-progress Robotics Facility commitment remains a single count-one goal");
    StrategyEngine fixedArm;
    fixedArm.setPvTStrategy(PvTStrategyId::safeTwoGatewayRangeObserver);
    safePlan = fixedArm.plan(state, detectedMech);
    const auto requestsSecondBase = [](const StrategicPlan& plan) {
        return std::ranges::any_of(plan.goals, [](const ProductionGoal& goal) {
            return goal.goal == GoalKind::expand && goal.target == UnitKind::nexus &&
                   goal.desiredCount >= 2;
        });
    };
    check(safePlan.desiredBases < 2 && !requestsSecondBase(safePlan),
          "the mech economy release does not bypass the safe 2-Gateway range-Observer checkpoint");
    state.self.units.push_back(unit(92, UnitKind::gateway));
    state.self.technologies.push_back({TechnologyKind::singularityCharge, 1, false});
    safePlan = fixedArm.plan(state, detectedMech);
    check(safePlan.desiredBases >= 2,
          "the safe opening checkpoint permits growth once its range and production requirements are complete");
    check(safePlan.pvtStrategy == PvTStrategyId::safeTwoGatewayRangeObserver,
          "within-match transitions preserve the selected PvT opening arm ID");

    return failures == 0 ? 0 : 1;
}
