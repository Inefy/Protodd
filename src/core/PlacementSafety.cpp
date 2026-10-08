#include "protodd/PlacementSafety.hpp"

#include <algorithm>

namespace protodd {
namespace {

class TemporaryObstacle final {
public:
    TemporaryObstacle(NavigationGrid& navigation, const std::uint64_t id,
                      const NavigationObstacleBounds bounds)
        : navigation_(navigation), id_(id), added_(
              navigation_.updateDynamicObstacle(id_, bounds)) {}
    ~TemporaryObstacle() {
        if (added_) static_cast<void>(navigation_.removeDynamicObstacle(id_));
    }
    [[nodiscard]] bool added() const noexcept { return added_; }
private:
    NavigationGrid& navigation_;
    std::uint64_t id_{};
    bool added_{};
};

bool hasUnchangedBaseline(const NavigationGrid& navigation,
                          const PlacementAccessRequirement& requirement) {
    return std::ranges::any_of(requirement.alternatives,
        [&navigation](const PlacementAccessRoute& route) {
            return route.baseline.reached() &&
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

}  // namespace

void capturePlacementAccessBaselines(
    NavigationGrid& navigation,
    std::vector<PlacementAccessRequirement>& requirements,
    const int maximumExpansions) {
    for (auto& requirement : requirements) {
        requirement.reachableBefore = false;
        for (auto& route : requirement.alternatives) {
            route.capturedObstacleVersion = navigation.obstacleVersion();
            if (requirement.reachableBefore) {
                route.baseline = {};
                continue;
            }
            route.baseline = navigation.findPath(
                route.from, route.to, maximumExpansions, route.footprint);
            requirement.reachableBefore = requirement.reachableBefore ||
                route.baseline.reached();
        }
    }
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
                if (baselineStillWalkable(navigation, route))
                    route.capturedObstacleVersion = navigation.obstacleVersion();
            }
        }
    };
    // Candidate overlays are added and removed repeatedly during tile search.
    // Refresh a still-valid route against the current overlay before adding
    // the next candidate, keeping the route journal useful across that churn.
    refreshBaselines(requirements);
    refreshBaselines(additionalRequirements);
    TemporaryObstacle temporary(navigation, temporaryObstacleId, candidate);
    if (!temporary.added()) return false;

    const auto requirementPreserved = [&navigation, maximumExpansions](
        const PlacementAccessRequirement& requirement) {
        if (!requirement.reachableBefore || requirement.alternatives.empty()) return true;
        if (hasUnchangedBaseline(navigation, requirement)) return true;

        auto routeRemains = false;
        for (const auto& route : requirement.alternatives) {
            if (!route.from.valid() || !route.to.valid()) continue;
            if (navigation.findPath(route.from, route.to, maximumExpansions,
                                   route.footprint).reached()) {
                routeRemains = true;
                break;
            }
        }
        return routeRemains;
    };
    for (const auto& requirement : requirements) {
        if (!requirementPreserved(requirement)) return false;
    }
    for (const auto& requirement : additionalRequirements) {
        if (!requirementPreserved(requirement)) return false;
    }
    return true;
}

}  // namespace protodd
