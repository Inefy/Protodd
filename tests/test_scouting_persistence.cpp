#include "protodd/Scouting.hpp"

#include <iostream>
#include <string_view>

namespace {

using namespace protodd;

bool check(const bool condition, const std::string_view message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

UnitSnapshot unit(const UnitId id, const UnitKind kind, const Position position,
                  const bool ours, const Frame frame) {
    UnitSnapshot result;
    result.id = id;
    result.kind = kind;
    result.position = result.lastPosition = position;
    result.lastSeen = result.firstSeen = frame;
    result.ours = ours;
    result.visible = result.detected = result.completed = true;
    result.sightRange = kind == UnitKind::observer ? 352 : 224;
    result.hitPoints = result.maxHitPoints = kind == UnitKind::probe ? 40 : 100;
    result.shields = result.maxShields = kind == UnitKind::observer ? 40 : 0;
    result.role = ours ? UnitRole::worker : UnitRole::groundArmy;
    return result;
}

GameState scoutingState() {
    GameState state;
    state.frame = 7 * 60 * 24;
    state.mapWidthPixels = 2048;
    state.mapHeightPixels = 1536;
    state.self.id = 1;
    state.enemy.id = 2;
    state.bases = {
        {1, {192, 192}, {224, 224}, 8000, 5000, 1, state.frame,
         true, false, 8, 1},
        {2, {1216, 192}, {1216, 224}, 8000, 5000, -1, state.frame - 12,
         true, false, 8, 1},
        {3, {1216, 1024}, {1216, 1056}, 8000, 5000, -1, 0,
         true, false, 8, 1},
    };
    state.bases[1].lastConfirmedEmpty = state.bases[1].lastScouted;
    state.self.units.push_back(unit(1, UnitKind::nexus, {192, 192}, true, state.frame));
    state.self.units.push_back(unit(10, UnitKind::probe, {288, 224}, true, state.frame));
    return state;
}

}  // namespace

int main() {
    bool passed = true;
    auto state = scoutingState();
    InfluenceMap influence;
    influence.update(state);
    ThreatAssessment threat;
    ScoutManager scouts;
    const UnitId probe[]{10};

    const auto first = scouts.assign(state, probe, influence, threat);
    passed &= check(first.size() == 1 &&
                        first.front().informationTarget == Position{1216, 1024},
                    "a confirmed empty start cools down while an unvisited start remains useful");

    state.frame += 24;
    const auto continuing = scouts.assign(state, probe, influence, threat);
    passed &= check(continuing.size() == 1 &&
                        continuing.front().informationTarget == Position{1216, 1024} &&
                        continuing.front().leaseGeneration == first.front().leaseGeneration &&
                        scouts.missionMetrics().workerScoutFrames >= 24,
                    "a moving worker keeps the same lease generation and reports its economic scouting cost");

    ScoutManager preempted;
    const auto preemptedOrder = preempted.assign(state, probe, influence, threat);
    if (!preemptedOrder.empty()) {
        const auto releasedGeneration = preempted.releaseLease(
            preemptedOrder.front().scout, state.frame + 1);
        preempted.recordCommandFeedback(
            {preemptedOrder.front().scout, ScoutCommandStatus::rejected,
             preemptedOrder.front().leaseGeneration}, state.frame + 1);
        const auto afterRelease = preempted.assign(state, probe, influence, threat);
        passed &= check(releasedGeneration == preemptedOrder.front().leaseGeneration &&
                            afterRelease.empty() && preempted.missionMetrics().cancelled == 1,
                        "worker preemption releases scout mission state and cools down its displaced target");
    } else {
        passed &= check(false, "scout preemption fixture must begin with an active mission");
    }

    state.frame += 24;
    state.self.units.back().position = continuing.front().target;
    state.bases[2].lastScouted = state.frame;
    state.bases[2].lastConfirmedEmpty = state.frame;
    static_cast<void>(scouts.assign(state, probe, influence, threat));
    passed &= check(scouts.missionMetrics().newInformation == 1 &&
                        scouts.missionMetrics().completed == 1,
                    "a newly observed empty site completes the mission and records information gain");
    passed &= check(scouts.assign(state, probe, influence, threat).empty(),
                    "the scout does not immediately repeat a confirmed-empty location");

    auto dangerState = scoutingState();
    dangerState.bases[1].ownerId = dangerState.enemy.id;
    dangerState.bases[1].lastScouted = dangerState.frame - 90 * 24;
    dangerState.bases[1].lastConfirmedEmpty = -1;
    dangerState.bases[2].lastScouted = dangerState.frame - 60 * 24;
    dangerState.bases.push_back(
        {4, {1664, 1152}, {1664, 1184}, 8000, 5000, -1, 0,
         false, false, 8, 1});
    auto depot = unit(40, UnitKind::commandCenter, dangerState.bases[1].center,
                      false, dangerState.frame - 90 * 24);
    depot.role = UnitRole::resourceDepot;
    depot.lastSeen = dangerState.frame - 90 * 24;
    dangerState.enemy.units.push_back(depot);
    InfluenceMap dangerInfluence;
    dangerInfluence.update(dangerState);
    ScoutManager rejectedMission;
    auto uncertainThreat = threat;
    uncertainThreat.uncertainty = 1.0;
    const auto tech = rejectedMission.assign(
        dangerState, probe, dangerInfluence, uncertainThreat);
    passed &= check(tech.size() == 1 &&
                        tech.front().purpose == ScoutPurpose::checkTech,
                    "stale enemy technology has a revisit value and receives a scout");
    if (!tech.empty()) {
        rejectedMission.recordCommandFeedback(
            {tech.front().scout, ScoutCommandStatus::rejected, tech.front().leaseGeneration},
            dangerState.frame);
        const auto retry = rejectedMission.assign(
            dangerState, probe, dangerInfluence, uncertainThreat);
        passed &= check(!retry.empty() &&
                            retry.front().informationTarget != tech.front().informationTarget &&
                            retry.front().leaseGeneration > tech.front().leaseGeneration &&
                            rejectedMission.missionMetrics().rejected == 1 &&
                            rejectedMission.missionMetrics().cancelled == 1,
                        "a rejected scouting order cools down that route and tries another information target");
        const auto cancelledBeforeStaleFeedback = rejectedMission.missionMetrics().cancelled;
        rejectedMission.recordCommandFeedback(
            {tech.front().scout, ScoutCommandStatus::rejected, tech.front().leaseGeneration},
            dangerState.frame);
        const auto afterStaleFeedback = rejectedMission.assign(
            dangerState, probe, dangerInfluence, uncertainThreat);
        passed &= check(!afterStaleFeedback.empty() && !retry.empty() &&
                            afterStaleFeedback.front().informationTarget == retry.front().informationTarget &&
                            afterStaleFeedback.front().leaseGeneration == retry.front().leaseGeneration &&
                            rejectedMission.missionMetrics().cancelled == cancelledBeforeStaleFeedback,
                        "late rejection feedback from a displaced lease cannot cancel the newer scout mission");
    }

    auto standOffState = scoutingState();
    standOffState.bases[1].ownerId = standOffState.enemy.id;
    standOffState.bases[2].ownerId = standOffState.self.id;
    standOffState.bases[1].lastScouted = 0;
    standOffState.bases[1].lastConfirmedEmpty = -1;
    auto cannon = unit(50, UnitKind::photonCannon, standOffState.bases[1].center,
                       false, standOffState.frame);
    cannon.groundWeapon = {20, 15, 0, 96, DamageType::normal, false, true};
    standOffState.enemy.units.push_back(cannon);
    InfluenceMap standOffInfluence;
    standOffInfluence.update(standOffState);
    ScoutManager standOffScouts;
    const auto approach = standOffScouts.assign(
        standOffState, probe, standOffInfluence, threat);
    passed &= check(approach.size() == 1 &&
                        approach.front().informationTarget == standOffState.bases[1].center &&
                        approach.front().target != approach.front().informationTarget &&
                        distance(approach.front().target, approach.front().informationTarget) <=
                            standOffState.self.units.back().sightRange - 16 &&
                        distance(approach.front().target, cannon.position) > 96 &&
                        approach.front().routeRisk <= 1.0,
                    "a threatened base is approached from a safe stand-off point inside sight range");

    auto abortState = scoutingState();
    abortState.self.units.back() = unit(10, UnitKind::observer, {288, 224}, true,
                                        abortState.frame);
    InfluenceMap abortInfluence;
    abortInfluence.update(abortState);
    ScoutManager abortedScouting;
    const auto observerMission = abortedScouting.assign(
        abortState, probe, abortInfluence, threat);
    passed &= check(observerMission.size() == 1 &&
                        observerMission.front().informationTarget == abortState.bases[2].center,
                    "an Observer begins a safe route to an unknown start");
    if (!observerMission.empty()) {
        auto revealedCannon = unit(60, UnitKind::photonCannon,
            abortState.bases[2].center, false, abortState.frame);
        revealedCannon.airWeapon = {20, 15, 0, 96, DamageType::normal, true, false};
        abortState.enemy.units.push_back(revealedCannon);
        abortInfluence.update(abortState);
        const auto abort = abortedScouting.protectObservers(abortState, abortInfluence);
        passed &= check(abort.size() == 1 &&
                            abort.front().source == "observer-abort-unsafe-route",
                        "newly observed detector-backed anti-air cancels the Observer route");
        abortState.enemy.units.clear();
        abortInfluence.update(abortState);
        passed &= check(abortedScouting.assign(
                            abortState, probe, abortInfluence, threat).empty(),
                        "an aborted lethal corridor stays cooled down after its threat disappears");
    }

    return passed ? 0 : 1;
}
