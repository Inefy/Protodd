#include "protodd/Harassment.hpp"
#include "protodd/RouteSafety.hpp"
#include "protodd/Scouting.hpp"
#include "protodd/Squads.hpp"
#include "protodd/ActorMovementRouting.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <vector>

namespace {

using namespace protodd;

UnitSnapshot makeUnit(const UnitId id, const UnitKind kind, const Position position,
                      const bool ours) {
    UnitSnapshot unit;
    unit.id = id;
    unit.kind = kind;
    unit.position = unit.lastPosition = position;
    unit.lastSeen = unit.firstSeen = 6 * 60 * 24;
    unit.ours = ours;
    unit.visible = unit.detected = unit.completed = true;
    unit.powered = true;
    unit.hitPoints = unit.maxHitPoints = 100;
    unit.role = ours ? UnitRole::worker : UnitRole::groundArmy;
    return unit;
}

}  // namespace

int main() {
    using namespace protodd;
    bool passed = true;
    const auto check = [&passed](const bool condition, const char* message) {
        if (!condition) {
            passed = false;
            std::cerr << "FAIL: " << message << '\n';
        }
    };

    constexpr Position start{176, 528};
    constexpr Position target{1456, 528};
    GameState state;
    state.frame = 6 * 60 * 24;
    state.mapWidthPixels = 2400;
    state.mapHeightPixels = 1216;
    state.self.id = 1;
    state.enemy.id = 2;
    state.bases = {
        {1, {176, 528}, {176, 560}, 8000, 5000, 1, state.frame, true, false, 8, 1},
        {2, target, {target.x, target.y + 32}, 8000, 5000, 2,
         state.frame - 4 * 60 * 24, true, false, 8, 1},
    };
    state.self.units.push_back(makeUnit(1, UnitKind::probe, start, true));
    auto cannon = makeUnit(20, UnitKind::photonCannon, {656, 112}, false);
    cannon.groundWeapon = {40, 15, 0, 224, DamageType::normal, false, true};
    state.enemy.units.push_back(cannon);

    constexpr int width = 75;
    constexpr int height = 38;
    std::vector<std::uint8_t> walkable(width * height, 1);
    for (auto y = 0; y < height; ++y) {
        if (y != 3) walkable[static_cast<std::size_t>(y * width + 20)] = 0;
    }
    const NavigationGrid wall(width, height, 32, std::move(walkable));
    {
        Squad advancing; advancing.role = SquadRole::mainArmy;
        CombatEstimate advance; advance.decision = FightDecision::engage;
        DefenseArea noDefense;
        auto actor = makeUnit(10000, UnitKind::dragoon, start, true);
        actor.role = UnitRole::groundArmy;
        actor.dimensionLeft = actor.dimensionRight = actor.dimensionUp = actor.dimensionDown = 8;
        const MovementFootprint body{8, 8, 8, 8};
        check(!wall.lineWalkable(start, target, body) && wall.findPath(start, target, 12000, body).reached(),
              "advance regression has a reachable detour despite its blocked straight chord");
        Command march{actor.id, CommandType::attackMove, -1, target, UnitKind::unknown,
                      50, 0, "squad-objective"};
        int actorSearches = 0;
        bool fullObjectives = true;
        for (int i = 0; i < 32; ++i) {
            actor.id = march.actor = 10000 + i;
            const auto routed = routeActorMovement(march, actor, wall,
                nativeUncontestedGroundTravel(advancing, advance, noDefense, march));
            actorSearches += routed.pathSearched;
            fullObjectives = fullObjectives && routed.order.type == CommandType::attackMove &&
                routed.order.targetPosition == target;
        }
        check(actorSearches == 0 && fullObjectives,
              "uncontested army keeps full engine objectives without per-actor A-star searches");
        advancing.enemies.push_back(makeUnit(20000, UnitKind::zealot, {500,528}, false));
        const auto tactical = routeActorMovement(march, actor, wall,
            nativeUncontestedGroundTravel(advancing, advance, noDefense, march));
        check(tactical.pathSearched && tactical.order.targetPosition != target &&
              wall.lineWalkable(actor.position, tactical.order.targetPosition, body),
              "contested movement retains a checked straight route prefix");
        const auto beforeCrowd = NavigationGrid::diagnosticsForCurrentThread().totalExpansions;
        bool crowdProgress = true;
        for (int i = 0; i < 64; ++i) {
            actor.id = march.actor = 30000 + i;
            actor.position = {start.x, start.y + i % 8};
            const auto bounded = routeActorMovement(march, actor, wall, false,
                maximumActorRouteExpansionsPerUpdate / 64);
            crowdProgress = crowdProgress && bounded.order.type == CommandType::attackMove &&
                bounded.order.targetPosition != actor.position &&
                wall.lineWalkable(actor.position, bounded.order.targetPosition, body);
        }
        const auto crowdWork = NavigationGrid::diagnosticsForCurrentThread().totalExpansions - beforeCrowd;
        check(crowdProgress && crowdWork <= maximumActorRouteExpansionsPerUpdate,
              "a crowded contested army makes body-safe progress within one shared search allowance");
        const auto exhausted = routeActorMovement(march, actor, wall, false, 0);
        check(!exhausted.pathSearched && exhausted.order.type == CommandType::hold,
              "an exhausted search allowance never permits an unchecked terrain crossing");
        auto shortMove = march;
        shortMove.targetPosition = {actor.position.x + 32, actor.position.y};
        const auto directMove = routeActorMovement(shortMove, actor, wall, false, 0);
        check(!directMove.pathSearched && directMove.order == shortMove,
              "direct body-safe movement remains available after the search allowance is spent");
        actor.position = start;
        advancing.enemies.clear();
        advance.decision = FightDecision::retreat;
        check(!nativeUncontestedGroundTravel(advancing, advance, noDefense, march),
              "retreat cannot delegate its tactical route to an offensive advance");
        advance.decision = FightDecision::engage; advance.advanceBlocked = true;
        check(!nativeUncontestedGroundTravel(advancing, advance, noDefense, march),
              "detection-blocked advance retains tactical routing");
        advance.advanceBlocked = false;
        advance.holdScreen = true;
        check(!nativeUncontestedGroundTravel(advancing, advance, noDefense, march),
              "a held interception screen cannot become a native advance");
        advance.holdScreen = false;
        advancing.withdrawing = true;
        check(!nativeUncontestedGroundTravel(advancing, advance, noDefense, march),
              "withdrawal retains its checked movement path");
        advancing.withdrawing = false;
        auto kite = march; kite.source = "combat-kite"; kite.type = CommandType::move;
        check(!nativeUncontestedGroundTravel(advancing, advance, noDefense, kite),
              "tactical kiting is separate from ordinary forward travel");
        auto reload = march; reload.source = "payload-reload-travel"; reload.type = CommandType::move;
        check(nativeUncontestedGroundTravel(advancing, advance, noDefense, reload),
              "uncontested payload reload keeps its engine travel destination");
        DefenseArea screen{start,448,start};
        check(!nativeUncontestedGroundTravel(advancing, advance, screen, march),
              "expansion and defensive screens retain their movement boundary");
        advancing.role = SquadRole::harassment;
        check(!nativeUncontestedGroundTravel(advancing, advance, noDefense, march),
              "raid safety is separate from main-army native travel");
    }
    InfluenceMap influence;
    influence.update(state);

    const auto direct = assessRouteThreat(influence, start, target, false);
    const auto routed = assessRouteThreat(influence, start, target, false, &wall);
    check(direct.reachable && direct.score() < 0.1,
          "straight chord stays outside the Cannon's influence");
    check(routed.reachable && routed.distance > direct.distance + 300.0 &&
              routed.peakThreat > 1.0,
          "ground risk and travel distance follow the long wall-gap route through the Cannon");
    const auto air = assessRouteThreat(influence, start, target, true, &wall);
    check(air.reachable && air.distance == distance(start, target) && air.peakThreat == 0.0,
          "air exposure keeps its independent direct route across the wall");

    ScoutManager scouts;
    ThreatAssessment threat;
    const UnitId scoutIds[]{1};
    const auto scoutOrders = scouts.assign(state, scoutIds, influence, threat, &wall);
    check(scoutOrders.empty() && scouts.informationGap().reason == ScoutGapReason::navigationPending,
          "unfinished wall-gap proof cannot authorize a scout route");
    auto unsafeNeverIssued = scoutOrders.empty();
    auto scoutingBounded = true;
    for (int step = 0; step < 256 &&
         scouts.informationGap().reason == ScoutGapReason::navigationPending; ++step) {
        state.frame += 6;
        static_cast<void>(scouts.controlWorkerScout(state, influence, &wall));
        unsafeNeverIssued = scouts.assign(state, scoutIds, influence, threat, &wall).empty() &&
            unsafeNeverIssued;
        scoutingBounded = scoutingBounded && scouts.navigationDiagnostics().frameWork <=
            ScoutManager::navigationWorkPerFrame;
    }
    check(unsafeNeverIssued && scoutingBounded &&
          scouts.informationGap().reason == ScoutGapReason::unsafeRoute,
          "a Probe is not assigned through a dangerous terrain detour hidden by the straight chord");

    auto raider = makeUnit(30, UnitKind::zealot, start, true);
    raider.role = UnitRole::groundArmy;
    raider.groundWeapon = {16, 22, 0, 32, DamageType::normal, false, true};
    check(harassmentRouteSafe(state, raider, target),
          "the geometric chord itself does not cross the Cannon's ground coverage");
    check(!harassmentRouteSafe(state, raider, target, false, &wall),
          "raid safety rejects the reachable wall-gap route through Cannon coverage");

    GameState detourState;
    detourState.frame = state.frame;
    detourState.mapWidthPixels = 3200;
    detourState.mapHeightPixels = 3200;
    auto centralCannon = makeUnit(40, UnitKind::photonCannon, {1600, 1600}, false);
    centralCannon.groundWeapon = {40, 15, 0, 160, DamageType::normal, false, true};
    detourState.enemy.units.push_back(centralCannon);
    InfluenceMap detourInfluence;
    detourInfluence.update(detourState);
    NavigationGrid openTerrain(100, 100, 32,
        std::vector<std::uint8_t>(100 * 100, 1));
    constexpr Position detourStart{160, 1600};
    constexpr Position detourTarget{3040, 1600};
    const auto baseline = assessRouteThreat(
        detourInfluence, detourStart, detourTarget, false, &openTerrain);
    const auto detour = selectSafeRouteAlternative(
        detourInfluence, detourStart, detourTarget, false, &openTerrain, {}, 0.25);
    check(detourInfluence.maximumGroundThreat(detourStart, detourTarget) > 0.25F,
          "the straight builder corridor crosses weapon coverage before terrain routing");
    check(baseline.reachable && baseline.score() > 0.25,
          "the baseline terrain route exceeds the declared risk ceiling");
    check(detour.profile.reachable && detour.waypoint.valid() &&
              detour.profile.score() <= 0.25 && detour.profile.score() < baseline.score(),
          "bounded route search selects a feasible low-risk detour around weapon coverage");
    const auto stableDetour = selectSafeRouteAlternative(
        detourInfluence, detourStart, detourTarget, false, &openTerrain, {}, 0.25,
        detour.waypoint);
    check(stableDetour.waypoint == detour.waypoint,
          "hysteresis retains the selected detour across equal-score replanning");

    auto builder = makeUnit(41, UnitKind::probe, detourStart, true);
    const auto builderRoute = selectSafeRouteAlternative(
        detourInfluence, builder.position, detourTarget, false, &openTerrain,
        {builder.dimensionLeft, builder.dimensionRight,
         builder.dimensionUp, builder.dimensionDown}, 0.4125, {-1, -1}, 0.25);
    check(builderRoute.waypoint.valid() && builderRoute.profile.peakThreat < 0.25 &&
              builderRoute.profile.score() <= 0.4125,
          "builder screening keeps a footprint-aware safe detour despite a threatening straight chord");

    // A safe hint can cease to equal every regenerated midpoint when either
    // endpoint moves. Retention still requires a current complete route proof.
    constexpr MovementFootprint probeFootprint{11, 11, 11, 11};
    const auto priorBuilderRoute = selectSafeRouteAlternative(
        detourInfluence, detourStart, detourTarget, false, &openTerrain,
        probeFootprint, 0.4125, {-1, -1}, 0.25);
    const auto adjustedEndpointRoute = selectSafeRouteAlternative(
        detourInfluence, detourStart, {3044, 1540}, false, &openTerrain,
        probeFootprint, 0.4125, priorBuilderRoute.waypoint, 0.25);
    check(priorBuilderRoute.waypoint.valid() &&
          adjustedEndpointRoute.waypoint == priorBuilderRoute.waypoint &&
          adjustedEndpointRoute.profile.peakThreat <= 0.25,
          "safe previous detour survives a construction endpoint adjustment");
    const auto movedActorRoute = selectSafeRouteAlternative(
        detourInfluence, {164, 1540}, detourTarget, false, &openTerrain,
        probeFootprint, 0.4125, priorBuilderRoute.waypoint, 0.25);
    check(movedActorRoute.waypoint == priorBuilderRoute.waypoint &&
          movedActorRoute.profile.peakThreat <= 0.25,
          "safe previous detour survives current actor movement");
    auto newThreatState = detourState;
    auto newGuard = centralCannon;
    newGuard.id = 400;
    newGuard.position = newGuard.lastPosition = priorBuilderRoute.waypoint;
    newThreatState.enemy.units.push_back(newGuard);
    InfluenceMap newThreatInfluence;
    newThreatInfluence.update(newThreatState);
    const auto threatenedHintRoute = selectSafeRouteAlternative(
        newThreatInfluence, {164, 1540}, detourTarget, false, &openTerrain,
        probeFootprint, 0.4125, priorBuilderRoute.waypoint, 0.25);
    check(threatenedHintRoute.waypoint != priorBuilderRoute.waypoint,
          "new weapon exposure invalidates the previously safe waypoint");
    GameState clearState = detourState;
    clearState.enemy.units.clear();
    InfluenceMap clearInfluence;
    clearInfluence.update(clearState);
    const auto excessiveHintRoute = selectSafeRouteAlternative(
        clearInfluence, detourStart, detourTarget, false, &openTerrain,
        probeFootprint, 0.4125, {1600, 2368}, 0.25);
    check(excessiveHintRoute.profile.reachable && !excessiveHintRoute.waypoint.valid(),
          "a feasible previous detour outside hysteresis yields to the clear baseline");

    detourState.enemy.id = 2;
    auto firstWorker = makeUnit(42, UnitKind::probe, {2960, 1600}, false);
    auto secondWorker = makeUnit(43, UnitKind::probe, {3000, 1632}, false);
    detourState.enemy.units.push_back(firstWorker);
    detourState.enemy.units.push_back(secondWorker);
    auto safeRaider = makeUnit(44, UnitKind::zealot, detourStart, true);
    safeRaider.groundWeapon = {16, 22, 0, 32, DamageType::normal, false, true};
    const auto raid = harassmentOpportunity(
        detourState, safeRaider, false, &openTerrain);
    check(raid.target.valid() && raid.waypoint.valid() && raid.waypoint != raid.target,
          "raiders choose a bounded safe detour around an observed weapon lane");
    const auto raidRepeat = harassmentOpportunity(
        detourState, safeRaider, false, &openTerrain);
    check(raidRepeat.waypoint == raid.waypoint,
          "raider waypoint selection remains deterministic across repeated planning");

    auto safeProbe = makeUnit(45, UnitKind::probe, detourStart, true);
    detourState.self.id = 1;
    detourState.self.units.push_back(safeProbe);
    detourState.bases = {
        {1, detourStart, {detourStart.x, detourStart.y + 32}, 8000, 5000,
         1, detourState.frame, true, false, 8, 1},
        {2, detourTarget, {detourTarget.x, detourTarget.y + 32}, 8000, 5000,
         2, detourState.frame - 4 * 60 * 24, true, false, 8, 1},
    };
    InfluenceMap scoutInfluence;
    scoutInfluence.update(detourState);
    ScoutManager detourScouts;
    const UnitId safeScoutIds[]{safeProbe.id};
    const auto detourOrders = detourScouts.assign(
        detourState, safeScoutIds, scoutInfluence, {}, &openTerrain);
    check(!detourOrders.empty() && detourOrders.front().routeWaypoint.valid(),
          "scouts issue the selected safe intermediate route waypoint");
    const auto repeatedDetourOrders = detourScouts.assign(
        detourState, safeScoutIds, scoutInfluence, {}, &openTerrain);
    check(!repeatedDetourOrders.empty() &&
              repeatedDetourOrders.front().routeWaypoint == detourOrders.front().routeWaypoint,
          "scout replanning retains an equally rated safe waypoint");

    GameState routeLoad;
    routeLoad.frame = state.frame;
    routeLoad.mapWidthPixels = 4096;
    routeLoad.mapHeightPixels = 3072;
    routeLoad.self.id = 1;
    routeLoad.enemy.id = 2;
    for (int index = 0; index < 96; ++index)
        routeLoad.enemy.units.push_back(makeUnit(1000 + index, UnitKind::probe,
            {3504 + (index % 8) * 16, 432 + (index / 8) * 16}, false));
    auto distantRaider = safeRaider;
    distantRaider.position = {176, 528};
    std::vector<std::uint8_t> separated(128 * 96, 1);
    for (int y = 0; y < 96; ++y) separated[y * 128 + 64] = 0;
    NavigationGrid dividedMap(128, 96, 32, std::move(separated));
    HarassmentRouteBudget raidBudget;
    const auto beforeRaids = NavigationGrid::diagnosticsForCurrentThread().searches;
    const auto unavailableRaid = harassmentOpportunity(
        routeLoad, distantRaider, false, &dividedMap, &raidBudget);
    auto secondRaider = distantRaider;
    secondRaider.id = 999;
    secondRaider.position.y += 128;
    const auto secondUnavailableRaid = harassmentOpportunity(
        routeLoad, secondRaider, false, &dividedMap, &raidBudget);
    check(!unavailableRaid.target.valid() && !secondUnavailableRaid.target.valid() &&
          NavigationGrid::diagnosticsForCurrentThread().searches - beforeRaids <=
              HarassmentRouteBudget::maximumPathSearches && raidBudget.deferredChecks > 0,
          "large remembered worker lines share bounded route work and never authorize an unproved raid");

    auto targetGuard = makeUnit(2000, UnitKind::photonCannon, {3552, 512}, false);
    targetGuard.groundWeapon = {40, 15, 0, 224, DamageType::normal, false, true};
    routeLoad.enemy.units.push_back(targetGuard);
    HarassmentRouteBudget guardedBudget;
    const auto guardedRaid = harassmentOpportunity(
        routeLoad, distantRaider, false, &dividedMap, &guardedBudget);
    check(!guardedRaid.target.valid() && guardedBudget.pathSearches == 0,
          "weapon coverage at a destination rejects it before unnecessary terrain search");
    routeLoad.enemy.units.pop_back();

    std::array<UnitSnapshot, 4> covertRaiders;
    for (std::size_t index = 0; index < covertRaiders.size(); ++index) {
        covertRaiders[index] = distantRaider;
        covertRaiders[index].id = 3000 + static_cast<int>(index);
        covertRaiders[index].kind = UnitKind::darkTemplar;
        covertRaiders[index].cloaked = true;
        covertRaiders[index].position.y = 176 + static_cast<int>(index) * 800;
    }
    StrategicPlan raidPlan;
    raidPlan.posture = Posture::pressure;
    raidPlan.rallyPoint = distantRaider.position;
    const auto beforeFormation = NavigationGrid::diagnosticsForCurrentThread().searches;
    const auto raidGroups = SquadPlanner{}.form(routeLoad, covertRaiders,
        routeLoad.enemy.units, raidPlan, distantRaider.position, &dividedMap);
    check(raidGroups.size() == covertRaiders.size() &&
          std::ranges::all_of(raidGroups, [](const Squad& group) { return group.withdrawing; }) &&
          NavigationGrid::diagnosticsForCurrentThread().searches - beforeFormation <=
              HarassmentRouteBudget::maximumPathSearches,
          "disconnected harassment groups share one formation budget and retain safe fallback missions");

    HarassmentRouteBudget provenBudget;
    const auto rankedRaid = harassmentOpportunity(
        detourState, safeRaider, false, &openTerrain, &provenBudget);
    std::ranges::reverse(detourState.enemy.units);
    const auto reorderedRaid = harassmentOpportunity(
        detourState, safeRaider, false, &openTerrain);
    check(rankedRaid.target == raid.target && rankedRaid.waypoint == raid.waypoint &&
          reorderedRaid.target == rankedRaid.target && reorderedRaid.waypoint == rankedRaid.waypoint,
          "ranked route admission preserves the best safe opportunity under input reversal");


    // A boundary-side corridor must remain a candidate when perpendicular
    // detours extend outside the map. The opposite-side candidates cannot
    // cross this terrain divider; the direct route enters Cannon coverage.
    GameState edgeState;
    edgeState.frame = state.frame;
    edgeState.mapWidthPixels = 1024;
    edgeState.mapHeightPixels = 896;
    auto edgeCannon = makeUnit(5000, UnitKind::photonCannon, {640, 448}, false);
    edgeCannon.groundWeapon = {40, 15, 0, 192, DamageType::normal, false, true};
    edgeState.enemy.units.push_back(edgeCannon);
    InfluenceMap edgeInfluence;
    edgeInfluence.update(edgeState);
    std::vector<std::uint8_t> edgeWalk(128 * 112, 1);
    for (int y = 0; y < 112; ++y) edgeWalk[y * 128 + 104] = 0;
    NavigationGrid edgeTerrain(128, 112, 8, std::move(edgeWalk));
    constexpr Position edgeStart{960, 64}, edgeTarget{896, 832};
    const auto edgeBaseline = assessRouteThreat(edgeInfluence, edgeStart, edgeTarget,
        false, &edgeTerrain, probeFootprint);
    NavigationWorkQueue edgeWork;
    RouteAlternative edgeRoute;
    bool edgeBounded = true;
    int edgeFrame = 0;
    for (; edgeFrame < 256; ++edgeFrame) {
        edgeWork.beginFrame(edgeTerrain, edgeFrame, 257);
        edgeRoute = selectSafeRouteAlternative(edgeInfluence, edgeStart, edgeTarget,
            false, &edgeTerrain, probeFootprint, 0.4125, {-1,-1}, 0.25, &edgeWork, 5001);
        edgeBounded = edgeBounded && edgeWork.diagnostics().frameWork <= 257;
        if (!edgeRoute.profile.searchBudgetExhausted && !edgeRoute.unresolvedAlternatives) break;
    }
    check(edgeBaseline.reachable && edgeBaseline.peakThreat > 0.25,
          "boundary regression baseline enters weapon coverage");
    check(edgeRoute.profile.reachable && edgeRoute.waypoint.valid() &&
          edgeRoute.waypoint.x > edgeStart.x && edgeRoute.profile.peakThreat <= 0.25 &&
          edgeRoute.profile.score() <= 0.4125,
          "out-of-map detours preserve a fully proved safe boundary-side corridor");
    check(edgeFrame < 256 && edgeBounded,
          "boundary detour settles within the shared per-frame navigation allowance");

    return passed ? 0 : 1;
}
