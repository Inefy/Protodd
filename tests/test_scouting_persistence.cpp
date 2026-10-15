#include "protodd/Scouting.hpp"

#include <array>
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

void openingPendingRouteControls(bool& passed) {
    constexpr int side=512,cell=8;
    std::vector<std::uint8_t> cells(side*side,1);
    for(int y=0;y<480;++y) cells[y*side+256]=0;
    NavigationGrid terrain(side,side,cell,cells);
    GameState state;
    state.frame=1000; state.mapWidthPixels=state.mapHeightPixels=side*cell;
    state.self.id=1;state.enemy.id=2;
    const Position origin{3200,400},enemyStart{288,3120},expansion{3500,1000};
    state.self.units={unit(1,UnitKind::nexus,origin,true,state.frame),
        unit(2,UnitKind::pylon,{3240,400},true,state.frame),
        unit(10,UnitKind::probe,origin,true,state.frame)};
    state.bases={{1,origin,origin,8000,5000,1,state.frame,true,false,8,1},
                 {2,enemyStart,enemyStart,8000,5000,-1,0,true,false,8,1},
                 {3,expansion,expansion,8000,5000,-1,0,false,false,8,1}};
    InfluenceMap influence; influence.update(state);
    ThreatAssessment threat;
    ScoutManager manager;
    const UnitId scout=manager.selectWorkerScout(state,threat);
    const std::array available{scout};
    const auto initial=manager.assign(state,available,influence,threat,&terrain);
    passed &= check(scout==10 && initial.empty() &&
                    manager.activeMissionScouts()==std::vector<UnitId>{10} &&
                    manager.navigationDiagnostics().queued>0,
                    "an opening scout reserves its pending enemy-start mission instead of a cheap expansion");
    const auto waiting=manager.controlWorkerScout(state,influence,&terrain);
    passed &= check(waiting && waiting->type==CommandType::hold &&
                    waiting->source=="probe-scout-route-pending",
                    "pending enemy-search proof holds the current position instead of sending the scout home");
    std::vector<ScoutOrder> proved;
    for(int step=0;step<256 && proved.empty();++step){
        state.frame+=6;
        proved=manager.assign(state,available,influence,threat,&terrain);
        passed &= check(manager.navigationDiagnostics().frameWork<=ScoutManager::navigationWorkPerFrame,
                        "retaining the opening mission preserves the shared navigation work allowance");
    }
    passed &= check(proved.size()==1 && proved.front().purpose==ScoutPurpose::findEnemy &&
                    proved.front().informationTarget==enemyStart,
                    "the resumable enemy-start route eventually becomes the issued mission");
    const auto travelling=manager.controlWorkerScout(state,influence,&terrain);
    passed &= check(travelling && travelling->type==CommandType::move &&
                    travelling->targetPosition==enemyStart,
                    "only a proved enemy-search mission releases the opening scout to travel");

    const auto originalLease=proved.front().leaseGeneration;
    const auto cancellations=manager.missionMetrics().cancelled;
    state.frame+=6;state.self.units.back().position={3100,600};
    NavigationGrid refreshed(side,side,cell,cells);
    const auto reproving=manager.assign(state,available,influence,threat,&refreshed);
    const auto reproofHold=manager.controlWorkerScout(state,influence,&refreshed);
    passed &= check(reproving.empty() && reproofHold && reproofHold->type==CommandType::hold &&
                    manager.missionMetrics().cancelled==cancellations,
                    "a previously owned enemy-search mission survives temporary reproof without cancellation cooldown");
    std::vector<ScoutOrder> reproved;
    for(int step=0;step<256 && reproved.empty();++step){
        state.frame+=6;reproved=manager.assign(state,available,influence,threat,&refreshed);
    }
    passed &= check(reproved.size()==1 && reproved.front().informationTarget==enemyStart &&
                    reproved.front().leaseGeneration==originalLease,
                    "reproved enemy-start routing retains the original mission lease");

    auto multipleStarts=state;multipleStarts.bases[2].startLocation=true;
    ScoutManager alternatives;(void)alternatives.selectWorkerScout(multipleStarts,threat);
    const auto nearerStart=alternatives.assign(multipleStarts,available,influence,threat,&terrain);
    passed &= check(nearerStart.size()==1 && nearerStart.front().purpose==ScoutPurpose::findEnemy &&
                    nearerStart.front().informationTarget==expansion,
                    "a proved alternate enemy start can be searched before an unresolved distant start");

    // Route waiting must never suppress immediate worker-chaser escape.
    ScoutManager threatened;
    passed &= check(threatened.selectWorkerScout(state,threat)==10,
                    "the pending-route danger fixture owns an opening worker scout");
    (void)threatened.assign(state,available,influence,threat,&terrain);
    const auto scoutPosition=state.self.units.back().position;
    auto chaser=unit(50,UnitKind::probe,{scoutPosition.x+32,scoutPosition.y},false,state.frame);
    chaser.orderTargetId=10;chaser.groundWeapon={5,22,0,15,DamageType::normal,false,true};
    state.enemy.units.push_back(chaser);state.self.units.back().underAttack=true;
    influence.update(state);
    const auto escape=threatened.controlWorkerScout(state,influence,&terrain);
    passed &= check(escape && escape->type==CommandType::move &&
                    escape->source=="probe-harass-evade-chaser",
                    "an immediate worker chaser overrides pending enemy-search waiting");

    // A completely disproved start can release an otherwise useful safe task.
    auto unreachableState=state;
    unreachableState.enemy.units.clear();unreachableState.self.units.back().underAttack=false;
    unreachableState.mapWidthPixels=unreachableState.mapHeightPixels=256;
    unreachableState.self.units[0].position={200,200};
    unreachableState.self.units[1].position={216,200};
    unreachableState.self.units.back().position={200,200};
    unreachableState.self.units.back().sightRange=64;
    unreachableState.bases[0].center={200,200};
    unreachableState.bases[1].center={16,16};
    unreachableState.bases[2].center={200,224};
    std::vector<std::uint8_t> closedCells(32*32,1);
    for(int y=0;y<16;++y)for(int x=0;x<16;++x)closedCells[y*32+x]=0;
    NavigationGrid closed(32,32,8,closedCells);
    ScoutManager fallback;
    (void)fallback.selectWorkerScout(unreachableState,threat);
    influence.update(unreachableState);
    const auto fallbackOrders=fallback.assign(unreachableState,available,influence,threat,&closed);
    passed &= check(fallbackOrders.size()==1 &&
                    fallbackOrders.front().purpose==ScoutPurpose::checkExpansion,
                    "a disproved enemy-start route permits a proved expansion fallback");
}

}  // namespace

int main() {
    bool passed = true;
    openingPendingRouteControls(passed);
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
        passed &= check(releasedGeneration > preemptedOrder.front().leaseGeneration &&
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
