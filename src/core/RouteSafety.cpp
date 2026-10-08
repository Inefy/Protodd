#include "protodd/RouteSafety.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace protodd {
namespace {

RouteThreatProfile assessPathThreat(const InfluenceMap& influence,
                                    const std::vector<Position>& route,
                                    const bool flying,
                                    const NavigationGrid* navigation) {
    if (route.size() < 2) return {};

    RouteThreatProfile profile;
    profile.reachable = true;
    const auto sampleSpacing = std::max(16, navigation != nullptr && !navigation->empty()
        ? navigation->cellSize() : 64);
    auto sampleCount = 0;
    auto threatTotal = 0.0;
    for (std::size_t leg = 1; leg < route.size(); ++leg) {
        const auto start = route[leg - 1];
        const auto finish = route[leg];
        const auto length = distance(start, finish);
        profile.distance += length;
        const auto samples = std::max(1, static_cast<int>(std::ceil(length / sampleSpacing)));
        for (auto step = 1; step <= samples; ++step) {
            const auto ratio = static_cast<double>(step) / samples;
            const Position point{
                start.x + static_cast<int>(std::lround((finish.x - start.x) * ratio)),
                start.y + static_cast<int>(std::lround((finish.y - start.y) * ratio)),
            };
            const auto cell = influence.at(point);
            const auto threat = static_cast<double>(flying ? cell.airThreat : cell.groundThreat);
            threatTotal += threat;
            profile.peakThreat = std::max(profile.peakThreat, threat);
            ++sampleCount;
        }
    }
    if (sampleCount > 0) profile.averageThreat = threatTotal / sampleCount;
    return profile;
}

std::vector<Position> concatenateRoutes(const std::vector<Position>& first,
                                        const std::vector<Position>& second,
                                        const std::vector<Position>& third) {
    std::vector<Position> combined;
    combined.reserve(first.size() + second.size() + third.size());
    const auto append = [&combined](const std::vector<Position>& route) {
        for (const auto point : route)
            if (combined.empty() || combined.back() != point) combined.push_back(point);
    };
    append(first);
    append(second);
    append(third);
    return combined;
}

}  // namespace

double RouteThreatProfile::score() const noexcept {
    return reachable ? averageThreat + peakThreat * 0.65
                     : std::numeric_limits<double>::infinity();
}

std::vector<Position> routePath(
    const Position from, const Position to, const bool flying,
    const NavigationGrid* navigation, const MovementFootprint footprint) {
    if (!from.valid() || !to.valid()) return {};
    if (flying || navigation == nullptr || navigation->empty()) return {from, to};
    if (navigation->lineWalkable(from, to, footprint)) return {from, to};

    const auto path = navigation->findPath(from, to, 12000, footprint);
    if (!path.reached()) return {};
    std::vector<Position> result;
    result.reserve(path.points.size() + 2);
    result.push_back(from);
    for (const auto waypoint : path.points) {
        if (waypoint != result.back()) result.push_back(waypoint);
    }
    if (to != result.back()) result.push_back(to);
    return result;
}

Position nextGroundRouteWaypoint(
    const NavigationGrid* navigation, const Position from, const Position to,
    const MovementFootprint footprint, const int lookaheadCells) {
    if (!from.valid() || !to.valid()) return {-1, -1};
    if (navigation == nullptr || navigation->empty()) return to;
    const auto waypoint = navigation->nextWaypoint(
        from, to, lookaheadCells, 12000, footprint);
    return waypoint.hasUsableWaypoint() ? waypoint.waypoint : Position{-1, -1};
}

Position nearestGroundConstructionAccessPoint(
    const NavigationGrid* navigation, const Position from,
    const Position structureCenter, const MovementFootprint structureFootprint,
    const MovementFootprint moverFootprint, const int maximumCenterDistance) {
    if (!from.valid() || !structureCenter.valid() || maximumCenterDistance < 0)
        return {-1, -1};
    if (navigation == nullptr || navigation->empty()) return structureCenter;

    static constexpr std::array directions{
        Position{0, -1}, Position{1, 0}, Position{0, 1}, Position{-1, 0},
        Position{1, -1}, Position{1, 1}, Position{-1, 1}, Position{-1, -1},
    };
    const auto margin = std::max(8, navigation->cellSize() / 2);
    const auto searchRadius = std::max(64, navigation->cellSize() * 4);
    const auto maximumDistanceSquared = static_cast<std::int64_t>(maximumCenterDistance) *
                                        maximumCenterDistance;
    std::vector<Position> candidates;
    candidates.reserve(directions.size());
    for (const auto direction : directions) {
        const auto dx = direction.x < 0
            ? -(structureFootprint.left + moverFootprint.right + margin)
            : direction.x > 0
                ? structureFootprint.right + moverFootprint.left + margin : 0;
        const auto dy = direction.y < 0
            ? -(structureFootprint.up + moverFootprint.down + margin)
            : direction.y > 0
                ? structureFootprint.down + moverFootprint.up + margin : 0;
        const Position requested{structureCenter.x + dx, structureCenter.y + dy};
        const auto candidate = navigation->nearestWalkable(
            requested, searchRadius, moverFootprint);
        if (!candidate.valid() || std::ranges::find(candidates, candidate) != candidates.end() ||
            distanceSquared(candidate, structureCenter) > maximumDistanceSquared) {
            continue;
        }
        candidates.push_back(candidate);
    }
    std::ranges::stable_sort(candidates, [from](const Position first, const Position second) {
        return distanceSquared(from, first) < distanceSquared(from, second);
    });
    auto best = Position{-1, -1};
    auto bestPathDistance = std::numeric_limits<double>::infinity();
    for (const auto candidate : candidates) {
        // Straight distance is a lower bound on every terrain detour. Once a
        // candidate cannot beat the best validated route, searching it adds
        // no information; clear approaches do not need A* at all.
        auto pathDistance = distance(from, candidate);
        if (pathDistance >= bestPathDistance) break;
        if (!navigation->lineWalkable(from, candidate, moverFootprint)) {
            const auto path = navigation->findPath(from, candidate, 12000, moverFootprint);
            if (!path.reached()) continue;
            pathDistance = 0.0;
            auto previous = from;
            for (const auto point : path.points) {
                pathDistance += distance(previous, point);
                previous = point;
            }
        }
        if (pathDistance < bestPathDistance) {
            best = candidate;
            bestPathDistance = pathDistance;
        }
    }
    return best;
}

Position groundConstructionScreeningDestination(
    const InfluenceMap& influence, const NavigationGrid* navigation,
    const Position from, const Position structureCenter,
    const MovementFootprint structureFootprint, const MovementFootprint moverFootprint) {
    if (!from.valid() || !structureCenter.valid()) return {-1, -1};
    if (influence.at(structureCenter).groundThreat > 0.25F) return {-1, -1};
    if (navigation == nullptr || navigation->empty() ||
        navigation->walkable(structureCenter, moverFootprint)) return structureCenter;
    // A blocked anchor is not a requirement to walk through the building.
    // Execution stages beside its footprint; preselection needs the same
    // complete approach proof before rejecting a worker as unreachable.
    return nearestGroundConstructionAccessPoint(
        navigation, from, structureCenter, structureFootprint, moverFootprint);
}

RouteThreatProfile assessRouteThreat(
    const InfluenceMap& influence, const Position from, const Position to,
    const bool flying, const NavigationGrid* navigation,
    const MovementFootprint footprint) {
    return assessPathThreat(influence,
        routePath(from, to, flying, navigation, footprint), flying, navigation);
}

RouteAlternative selectSafeRouteAlternative(
    const InfluenceMap& influence, const Position from, const Position to,
    const bool flying, const NavigationGrid* navigation,
    const MovementFootprint footprint, const double maximumRisk,
    const Position previousWaypoint, const double maximumPeakThreat) {
    const auto basePath = routePath(from, to, flying, navigation, footprint);
    auto best = RouteAlternative{assessPathThreat(influence, basePath, flying, navigation), { -1, -1 }};
    if (!best.profile.reachable || flying || navigation == nullptr || navigation->empty())
        return best;

    constexpr double hysteresisMargin = 0.08;
    if (best.profile.score() <= maximumRisk &&
        best.profile.peakThreat <= maximumPeakThreat && !previousWaypoint.valid()) return best;
    const auto baselineLength = std::max(distance(from, to), best.profile.distance);
    const auto maximumDistance = baselineLength + std::max(256.0, baselineLength * 0.75);
    auto bestCost = best.profile.score() + std::max(0.0, best.profile.distance - baselineLength) * 0.00035;
    auto previousCandidate = RouteAlternative{};
    auto previousCost = std::numeric_limits<double>::infinity();

    const auto length = std::max(1.0, distance(from, to));
    const auto dx = static_cast<double>(to.x - from.x) / length;
    const auto dy = static_cast<double>(to.y - from.y) / length;
    const auto radius = std::max(384, navigation->cellSize() * 8);
    const std::array<int, 4> offsets{radius, -radius, radius * 2, -radius * 2};
    const auto mapWidth = navigation->width() * navigation->cellSize();
    const auto mapHeight = navigation->height() * navigation->cellSize();
    for (const auto offset : offsets) {
        const Position via{
            (from.x + to.x) / 2 - static_cast<int>(std::lround(dy * offset)),
            (from.y + to.y) / 2 + static_cast<int>(std::lround(dx * offset)),
        };
        if (!via.valid() || via.x >= mapWidth || via.y >= mapHeight ||
            distance(from, via) <= std::max(96, navigation->cellSize() * 3) ||
            distance(via, to) <= std::max(96, navigation->cellSize() * 3) ||
            !navigation->walkable(via, footprint)) continue;

        const auto first = routePath(from, via, false, navigation, footprint);
        const auto second = routePath(via, to, false, navigation, footprint);
        if (first.empty() || second.empty()) continue;
        const auto candidatePath = concatenateRoutes(first, second, {});
        const auto profile = assessPathThreat(influence, candidatePath, false, navigation);
        if (!profile.reachable || profile.distance > maximumDistance ||
            profile.score() > maximumRisk || profile.peakThreat > maximumPeakThreat) continue;
        const auto cost = profile.score() +
            std::max(0.0, profile.distance - baselineLength) * 0.00035;
        const RouteAlternative candidate{profile, via};
        if (via == previousWaypoint) {
            previousCandidate = candidate;
            previousCost = cost;
        }
        if (cost + 1e-9 < bestCost) {
            best = candidate;
            bestCost = cost;
        }
    }

    if (previousCandidate.profile.reachable &&
        previousCandidate.profile.score() <= maximumRisk &&
        previousCandidate.profile.peakThreat <= maximumPeakThreat &&
        previousCost <= bestCost + hysteresisMargin)
        return previousCandidate;
    return best;
}

}  // namespace protodd
