#include "protodd/Squads.hpp"
#include "protodd/UnitMemory.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <string_view>

namespace {

using namespace protodd;

UnitSnapshot unit(const UnitId id, const UnitKind kind, const Position position,
                  const bool ours) {
    UnitSnapshot result;
    result.id = id;
    result.kind = kind;
    result.position = result.lastPosition = position;
    result.ours = ours;
    result.visible = result.detected = result.completed = true;
    result.hitPoints = result.maxHitPoints = 100;
    result.shields = result.maxShields = 100;
    result.topSpeed = 2.0;
    result.sightRange = kind == UnitKind::observer ? 288 : 0;
    return result;
}

Squad blockedSquad(const int id, const Position center, const Position retreat) {
    Squad result;
    result.id = id;
    result.role = SquadRole::mainArmy;
    result.center = center;
    result.objective = {center.x + 600, center.y};
    result.retreat = retreat;
    result.needsDetection = true;
    result.units = {unit(100 + id, UnitKind::dragoon, center, true)};
    return result;
}

}  // namespace

int main() {
    using namespace protodd;
    bool passed = true;
    const auto check = [&passed](const bool condition, const std::string_view message) {
        if (!condition) {
            passed = false;
            std::cerr << "FAIL: " << message << '\n';
        }
    };

    GameState state;
    state.frame = 2400;
    state.self.id = 1;
    state.enemy.id = 2;
    state.mapWidthPixels = state.mapHeightPixels = 4096;
    auto leftObserver = unit(10, UnitKind::observer, {900, 1000}, true);
    auto rightObserver = unit(11, UnitKind::observer, {2600, 1000}, true);
    state.self.units = {leftObserver, rightObserver};
    const auto left = blockedSquad(1, {1000, 1000}, {800, 1000});
    const auto right = blockedSquad(2, {2500, 1000}, {2300, 1000});
    const std::array squads{left, right};
    InfluenceMap influence;

    // Exercise the real adapter memory normalization, including repeated loss
    // and reacquisition of ordinary enemies at the edge of vision.
    GameState contacts = state;
    contacts.self.units = {unit(100, UnitKind::dragoon, {1000, 1000}, true)};
    auto contact = unit(200, UnitKind::dragoon, {1120, 1000}, false);
    contact.lastSeen = contacts.frame;
    contact.groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
    StrategicPlan contactPlan;
    contactPlan.posture = Posture::attack;
    contactPlan.attackTarget = {2000, 1000};
    for (int sample = 0; sample < 8; ++sample) {
        const auto observed = sample % 2 == 0 ? contact :
            reconcileEnemyMemory(contact, std::nullopt, contacts.frame + 12, false, false).snapshot;
        const auto formed = SquadPlanner{}.form(
            contacts, contacts.self.units, std::array{observed}, contactPlan, {512, 1000});
        const auto army = std::ranges::find_if(formed, [](const Squad& squad) {
            return squad.role == SquadRole::mainArmy;
        });
        const auto allocated = SquadPlanner{}.allocateDetectors(contacts, formed, influence);
        check(army != formed.end() && army->enemies.size() == 1 &&
              !army->needsDetection && SquadPlanner::mobileDetectionReady(contacts, *army) &&
              allocated.requiredObservers == 0 && allocated.unmetDetectionDemands == 0,
              "ordinary vision-edge contacts retain enemy danger without alternating detector demand");
    }
    for (const auto kind : {UnitKind::darkTemplar, UnitKind::lurker, UnitKind::spiderMine}) {
        auto covert = contact;
        covert.kind = kind;
        covert.visible = covert.detected = false;
        const auto formed = SquadPlanner{}.form(
            contacts, contacts.self.units, std::array{covert}, contactPlan, {512, 1000});
        const auto army = std::ranges::find_if(formed, [](const Squad& squad) {
            return squad.role == SquadRole::mainArmy;
        });
        check(army != formed.end() && army->needsDetection &&
              !SquadPlanner::mobileDetectionReady(contacts, *army),
              "known covert fog contacts retain detector waits after ordinary-memory repair");
    }
    contact.detected = false;
    check(contact.requiresDetection(), "a currently visible undetected contact remains a detection threat");
    contact.visible = false;
    contact.cloaked = true;
    check(contact.requiresDetection(), "remembered observed cloak state retains detection demand");
    contact.cloaked = false;
    contact.burrowed = true;
    check(contact.requiresDetection(), "remembered observed burrow state retains detection demand");

    const auto both = SquadPlanner{}.allocateDetectors(
        state, squads, influence, false, true);
    check(both.requiredObservers == 2 && both.assignments.size() == 2 &&
              both.reservedObservers.size() == 2 && both.unmetDetectionDemands == 0 &&
              both.assignments[0].observerId != both.assignments[1].observerId,
          "independent detection demands receive distinct reserved Observers");
    check(std::ranges::all_of(both.assignments, [](const DetectorAssignment& assignment) {
              return assignment.safeToRendezvous && assignment.estimatedArrivalFrames == 50;
          }),
          "allocation records flight-time estimates to safe squad rendezvous anchors");

    state.self.units.pop_back();
    const auto scarce = SquadPlanner{}.allocateDetectors(
        state, squads, influence, false, true);
    check(scarce.requiredObservers == 2 && scarce.assignments.size() == 1 &&
              scarce.reservedObservers.size() == 1 &&
              scarce.unmetDetectionDemands == 1 && scarce.reservedObservers.front() == 10,
          "one Observer cannot cover two distant groups and remains reserved from scouting");
    check(SquadPlanner::mobileDetectionReady(state, left) &&
              !SquadPlanner::mobileDetectionReady(state, right),
          "one local Observer releases only the squad it physically covers");

    StrategicPlan plan;
    plan.goals.push_back({GoalKind::train, UnitKind::observer, 1, 80, false,
                          "optional strategic scout"});
    SquadPlanner::requireDetectorCount(plan, scarce.requiredObservers);
    const auto observerGoal = std::ranges::find_if(plan.goals, [](const ProductionGoal& goal) {
        return goal.goal == GoalKind::train && goal.target == UnitKind::observer;
    });
    check(observerGoal != plan.goals.end() && observerGoal->desiredCount == 2 &&
              observerGoal->priority == 120 && observerGoal->blocking,
          "unmet concurrent coverage raises a blocking replacement and production demand");

    auto surplus = unit(12, UnitKind::observer, {3000, 1000}, true);
    state.self.units = {leftObserver, rightObserver, surplus};
    const auto withScoutReserve = SquadPlanner{}.allocateDetectors(
        state, squads, influence, false, true);
    check(withScoutReserve.assignments.size() == 2 &&
              std::ranges::find(withScoutReserve.reservedObservers, surplus.id) ==
                  withScoutReserve.reservedObservers.end(),
          "only surplus Observers remain available as strategic scouts");

    state.self.units = {unit(13, UnitKind::observer, {700, 1000}, true)};
    state.storms = {{1000, 1000}};
    influence.update(state);
    const std::array oneDemand{left};
    const auto unsafe = SquadPlanner{}.allocateDetectors(
        state, oneDemand, influence, false, true);
    check(unsafe.assignments.size() == 1 &&
              unsafe.reservedObservers == std::vector<UnitId>{13} &&
              !unsafe.assignments.front().safeToRendezvous &&
              unsafe.commands.empty() && unsafe.unmetDetectionDemands == 1,
          "a blocked unsafe rendezvous reserves its detector and reports unmet coverage");

    return passed ? 0 : 1;
}
