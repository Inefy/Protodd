#include "protodd/Harassment.hpp"
#include "protodd/RouteSafety.hpp"
#include "protodd/Scouting.hpp"
#include "protodd/Squads.hpp"

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
    check(scoutOrders.empty() && scouts.informationGap().reason == ScoutGapReason::unsafeRoute,
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

    return passed ? 0 : 1;
}
