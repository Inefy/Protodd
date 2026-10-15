#include "protodd/PlacementSafety.hpp"

#include <algorithm>

namespace protodd {

struct PlacementAccessProof {
    std::uint64_t terrainIdentity{};
    std::uint64_t collisionIdentity{};
    Position from{-1, -1};
    Position to{-1, -1};
    MovementFootprint footprint{};
    std::vector<Position> points;
};

namespace {

bool sameProvedRoute(const PlacementAccessRoute& route) {
    const auto& proof = route.validatedBaseline;
    return proof && route.baseline.reached() && !route.baseline.points.empty() &&
           proof->from == route.from && proof->to == route.to &&
           proof->footprint == route.footprint && proof->points == route.baseline.points;
}

bool hasUnchangedBaseline(const NavigationGrid& navigation,
                          const PlacementAccessRequirement& requirement) {
    return std::ranges::any_of(requirement.alternatives,
        [&navigation](const PlacementAccessRoute& route) {
            // A failed refresh is not a usable baseline, even when this map's
            // journal happens to have the same revision as the captured map.
            return sameProvedRoute(route) &&
                route.validatedBaseline->terrainIdentity == navigation.terrainIdentity() &&
                !navigation.routeAffectedSince(
                    route.baseline.points, route.from, route.to, route.footprint,
                    route.capturedObstacleVersion);
        });
}

bool baselineStillWalkable(const NavigationGrid& navigation,
                           const PlacementAccessRoute& route) {
    if (!route.baseline.reached() || route.baseline.points.empty() ||
        !navigation.walkable(route.from, route.footprint) ||
        !navigation.walkable(route.to, route.footprint)) return false;
    for (std::size_t index = 0; index < route.baseline.points.size(); ++index) {
        if (!navigation.walkable(route.baseline.points[index], route.footprint)) return false;
        if (index > 0 && !navigation.lineWalkable(route.baseline.points[index - 1],
                                                 route.baseline.points[index],
                                                 route.footprint)) return false;
    }
    return navigation.lineWalkable(route.from, route.baseline.points.front(),
                                   route.footprint) &&
           navigation.lineWalkable(route.baseline.points.back(), route.to,
                                   route.footprint);
}

bool refreshBaseline(const NavigationGrid& navigation, PlacementAccessRoute& route) {
    if (sameProvedRoute(route) &&
        route.validatedBaseline->terrainIdentity == navigation.terrainIdentity() &&
        route.validatedBaseline->collisionIdentity == navigation.collisionIdentity()) {
        route.capturedObstacleVersion = navigation.obstacleVersion();
        // Exact geometry can survive temporary overlays while the cell journal
        // advances. Updating that journal does not require copying the path.
        return true;
    }
    route.validatedBaseline.reset();
    if (!baselineStillWalkable(navigation, route)) return false;
    route.capturedObstacleVersion = navigation.obstacleVersion();
    route.validatedBaseline = std::make_shared<const PlacementAccessProof>(PlacementAccessProof{
        navigation.terrainIdentity(), navigation.collisionIdentity(),
        route.from, route.to, route.footprint, route.baseline.points});
    return true;
}

}  // namespace

void capturePlacementAccessBaselines(
    NavigationGrid& navigation,
    std::vector<PlacementAccessRequirement>& requirements,
    const int maximumExpansions) {
    for (auto& requirement : requirements) {
        requirement.reachableBefore = false;
        for (auto& route : requirement.alternatives) {
            if (requirement.reachableBefore) {
                route.baseline = {};
                route.validatedBaseline.reset();
                continue;
            }
            // Repeated macro passes often ask for identical depot/mineral and
            // producer access. Reuse a complete path only while its immutable
            // proof matches, or after checking every segment in changed
            // geometry. A failed or edited baseline must be searched again.
            if (refreshBaseline(navigation, route)) {
                requirement.reachableBefore = true;
                continue;
            }
            route.validatedBaseline.reset();
            route.capturedObstacleVersion = navigation.obstacleVersion();
            route.baseline = navigation.findPath(
                route.from, route.to, maximumExpansions, route.footprint);
            requirement.reachableBefore = requirement.reachableBefore ||
                route.baseline.reached();
            if (route.baseline.reached())
                static_cast<void>(refreshBaseline(navigation, route));
        }
    }
}

PlacementAccessCaptureStatus capturePlacementAccessBaselinesBounded(
    NavigationGrid& navigation,
    std::vector<PlacementAccessRequirement>& requirements,
    NavigationWorkQueue& work) {
    const auto ownerFor = [](const PlacementAccessRoute& route) {
        auto hash = std::uint64_t{1469598103934665603ULL};
        for (const auto value : {route.from.x, route.from.y, route.to.x, route.to.y,
                                route.footprint.left, route.footprint.right,
                                route.footprint.up, route.footprint.down}) {
            hash ^= static_cast<std::uint32_t>(value);
            hash *= 1099511628211ULL;
        }
        return hash == 0 ? std::uint64_t{1} : hash;
    };
    for (auto& requirement : requirements) {
        requirement.reachableBefore = false;
        for (auto& route : requirement.alternatives) {
            if (refreshBaseline(navigation, route)) {
                requirement.reachableBefore = true;
                break;
            }
            route.validatedBaseline.reset();
            route.capturedObstacleVersion = navigation.obstacleVersion();
            route.baseline = work.query(ownerFor(route), navigation,
                route.from, route.to, route.footprint, NavigationOriginPolicy::fixed);
            if (route.baseline.status == NavigationStatus::budgetExhausted ||
                (route.baseline.status == NavigationStatus::partial &&
                 route.baseline.searchBudgetExhausted)) {
                return PlacementAccessCaptureStatus::deferred;
            }
            if (route.baseline.reached()) {
                requirement.reachableBefore = refreshBaseline(navigation, route);
                if (requirement.reachableBefore) break;
            }
        }
    }
    return PlacementAccessCaptureStatus::complete;
}

bool placementPreservesAccess(
    NavigationGrid& navigation, const std::uint64_t temporaryObstacleId,
    const NavigationObstacleBounds candidate,
    const std::span<PlacementAccessRequirement> requirements,
    const int maximumExpansions,
    const std::span<PlacementAccessRequirement> additionalRequirements) {
    if (!candidate.valid()) return false;
    const auto refreshBaselines = [&navigation](
        const std::span<PlacementAccessRequirement> groups) {
        for (auto& group : groups) {
            for (auto& route : group.alternatives) {
                static_cast<void>(refreshBaseline(navigation, route));
            }
        }
    };
    // Candidate overlays are added and removed repeatedly during tile search.
    // Refresh a still-valid route against the current overlay before adding
    // the next candidate, keeping the route journal useful across that churn.
    refreshBaselines(requirements);
    refreshBaselines(additionalRequirements);
    NavigationObstacleOverlay temporary(navigation, temporaryObstacleId, candidate);
    if (!temporary.added()) return false;

    const auto requirementPreserved = [&navigation, maximumExpansions](
        PlacementAccessRequirement& requirement) {
        if (!requirement.reachableBefore || requirement.alternatives.empty()) return true;
        if (hasUnchangedBaseline(navigation, requirement)) return true;

        auto routeRemains = false;
        for (auto& route : requirement.alternatives) {
            if (!route.from.valid() || !route.to.valid()) continue;
            // The changed-cell journal is deliberately conservative near a
            // footprint. When it intersects a retained detour, validate the
            // actual path in the candidate geometry before starting A* again.
            if (refreshBaseline(navigation, route)) {
                routeRemains = true;
                break;
            }
            auto alternative = navigation.findPath(route.from, route.to, maximumExpansions,
                                                   route.footprint);
            if (alternative.reached()) {
                // A path valid with this extra obstacle is also a valid
                // baseline when the obstacle is removed. Retain that witness
                // so repeated placement retries do not rebuild the same
                // detour; later geometry and candidate checks still apply.
                route.baseline = std::move(alternative);
                static_cast<void>(refreshBaseline(navigation, route));
                routeRemains = true;
                break;
            }
        }
        return routeRemains;
    };
    for (auto& requirement : requirements) {
        if (!requirementPreserved(requirement)) return false;
    }
    for (auto& requirement : additionalRequirements) {
        if (!requirementPreserved(requirement)) return false;
    }
    return true;
}

}  // namespace protodd
