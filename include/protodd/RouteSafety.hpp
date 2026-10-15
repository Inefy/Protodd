#pragma once

#include "protodd/InfluenceMap.hpp"
#include "protodd/Navigation.hpp"

#include <limits>

namespace protodd {

struct RouteThreatProfile {
    bool reachable{};
    double distance{};
    double averageThreat{};
    double peakThreat{};
    NavigationStatus navigationStatus{NavigationStatus::invalidInput};
    bool searchBudgetExhausted{};

    [[nodiscard]] double score() const noexcept;
};

struct RouteAlternative {
    RouteThreatProfile profile{};
    // A stable intermediate command target for the selected safe detour.
    // Invalid means the baseline route was retained.
    Position waypoint{-1, -1};
    // The selected path remains fully proved; this reports whether any
    // alternative leg remained unknown after the bounded search.
    bool unresolvedAlternatives{};
};

// Returns the terrain route that ground units can take, or a straight route
// for air units and when terrain is unavailable. An empty result means a
// completed ground route was not proved. The optional status distinguishes
// an unfinished bounded search from proven unreachability.
[[nodiscard]] std::vector<Position> routePath(
    Position from, Position to, bool flying,
    const NavigationGrid* navigation = nullptr,
    MovementFootprint footprint = {},
    NavigationStatus* navigationStatus = nullptr,
    bool* searchBudgetExhausted = nullptr,
    NavigationWorkQueue* work = nullptr, std::uint64_t owner = 0,
    NavigationOriginPolicy originPolicy = NavigationOriginPolicy::movingActor);

// Return a command target along the current ground path. Long construction
// routes use this instead of asking BWAPI's Build command to pathfind the
// entire distance through terrain at once.
[[nodiscard]] Position nextGroundRouteWaypoint(
    const NavigationGrid* navigation, Position from, Position to,
    MovementFootprint footprint = {}, int lookaheadCells = 7,
    NavigationWorkQueue* work = nullptr, std::uint64_t owner = 0);

// Find a walkable, path-connected staging point beside a construction
// footprint. The returned point stays within the final command radius instead
// of stopping at NavigationGrid's wider partial-path snap.
[[nodiscard]] Position nearestGroundConstructionAccessPoint(
    const NavigationGrid* navigation, Position from, Position structureCenter,
    MovementFootprint structureFootprint, MovementFootprint moverFootprint = {},
    int maximumCenterDistance = 96,
    NavigationStatus* navigationStatus = nullptr,
    bool* searchBudgetExhausted = nullptr,
    NavigationWorkQueue* work = nullptr, std::uint64_t owner = 0);

// Check the structure site independently of the builder's route or distance.
// Recheck before an unstarted Nexus consumes its construction budget.
[[nodiscard]] bool groundConstructionSiteSafe(
    const InfluenceMap& influence, Position structureCenter) noexcept;

// Match builder preselection to an executable construction approach when the
// anchor's center is occupied. A walkable center keeps the original screening
// endpoint; a blocked one requires a proved nearby access point. Preserve the
// original rejection of weapon or spell exposure at the structure center.
[[nodiscard]] Position groundConstructionScreeningDestination(
    const InfluenceMap& influence, const NavigationGrid* navigation,
    Position from, Position structureCenter,
    MovementFootprint structureFootprint, MovementFootprint moverFootprint = {},
    NavigationStatus* navigationStatus = nullptr,
    bool* searchBudgetExhausted = nullptr,
    NavigationWorkQueue* work = nullptr, std::uint64_t owner = 0);

// Samples threat along the route rather than across the straight chord between
// its endpoints. Air units retain independent straight-line air exposure.
[[nodiscard]] RouteThreatProfile assessRouteThreat(
    const InfluenceMap& influence, Position from, Position to, bool flying,
    const NavigationGrid* navigation = nullptr,
    MovementFootprint footprint = {},
    NavigationWorkQueue* work = nullptr, std::uint64_t owner = 0);

// Compare the shortest terrain route with at most four deterministic, feasible
// midpoint detours clipped to footprint-safe map bounds and, when distinct,
// one previous waypoint revalidated with
// current inputs. All candidates share the navigation work ceiling.
// Alternatives are considered when the baseline exceeds the
// risk limit, or when a previous detour can be retained within the hysteresis
// margin. The route length is bounded to baseline + max(256 px, 75%).
[[nodiscard]] RouteAlternative selectSafeRouteAlternative(
    const InfluenceMap& influence, Position from, Position to, bool flying,
    const NavigationGrid* navigation = nullptr,
    MovementFootprint footprint = {}, double maximumRisk = 0.25,
    Position previousWaypoint = {-1, -1},
    double maximumPeakThreat = std::numeric_limits<double>::infinity(),
    NavigationWorkQueue* work = nullptr, std::uint64_t owner = 0);

}  // namespace protodd
