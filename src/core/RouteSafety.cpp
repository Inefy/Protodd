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
    profile.navigationStatus = NavigationStatus::reached;
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
    const NavigationGrid* navigation, const MovementFootprint footprint,
    NavigationStatus* navigationStatus, bool* searchBudgetExhausted,
    NavigationWorkQueue* work, const std::uint64_t owner,
    const NavigationOriginPolicy originPolicy) {
    if (navigationStatus != nullptr) *navigationStatus = NavigationStatus::invalidInput;
    if (searchBudgetExhausted != nullptr) *searchBudgetExhausted = false;
    if (!from.valid() || !to.valid()) return {};
    if (flying || navigation == nullptr || navigation->empty() ||
        ((work == nullptr || originPolicy == NavigationOriginPolicy::fixed) &&
         navigation->lineWalkable(from, to, footprint))) {
        if (navigationStatus != nullptr) *navigationStatus = NavigationStatus::reached;
        return {from, to};
    }

    const auto path = work != nullptr ? work->query(owner,*navigation,from,to,footprint,originPolicy)
        : navigation->findPath(from, to, 12000, footprint);
    if (navigationStatus != nullptr) *navigationStatus = path.status;
    if (searchBudgetExhausted != nullptr) *searchBudgetExhausted = path.searchBudgetExhausted;
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
    const MovementFootprint footprint, const int lookaheadCells,
    NavigationWorkQueue* work, const std::uint64_t owner) {
    if (!from.valid() || !to.valid()) return {-1, -1};
    if (navigation == nullptr || navigation->empty()) return to;
    if (work != nullptr) {
        const auto path = work->query(owner,*navigation,from,to,footprint);
        if (!path.reached() || path.points.empty()) return {-1,-1};
        // Match ordinary navigation: native ground movement follows the proved
        // prefix through its corners. Clipping to a directly visible cell can
        // repeatedly target a near corner instead of advancing through it.
        return path.points[navigationWaypointIndex(
            path.points.size(), navigation->cellSize(), lookaheadCells)];
    }
    const auto waypoint = navigation->nextWaypoint(
        from, to, lookaheadCells, 12000, footprint);
    return waypoint.hasUsableWaypoint() ? waypoint.waypoint : Position{-1, -1};
}

Position nearestGroundConstructionAccessPoint(
    const NavigationGrid* navigation, const Position from,
    const Position structureCenter, const MovementFootprint structureFootprint,
    const MovementFootprint moverFootprint, const int maximumCenterDistance,
    NavigationStatus* navigationStatus, bool* searchBudgetExhausted,
    NavigationWorkQueue* work, const std::uint64_t owner) {
    if (navigationStatus != nullptr) *navigationStatus = NavigationStatus::invalidInput;
    if (searchBudgetExhausted != nullptr) *searchBudgetExhausted = false;
    if (!from.valid() || !structureCenter.valid() || maximumCenterDistance < 0)
        return {-1, -1};
    if (navigation == nullptr || navigation->empty()) {
        if (navigationStatus != nullptr) *navigationStatus = NavigationStatus::reached;
        return structureCenter;
    }

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
    auto unresolved = NavigationStatus::unreachable;
    auto anyBudgetExhausted = false;
    for (const auto candidate : candidates) {
        // Straight distance is a lower bound on every terrain detour. Once a
        // candidate cannot beat the best validated route, searching it adds
        // no information; clear approaches do not need A* at all.
        auto pathDistance = distance(from, candidate);
        if (pathDistance >= bestPathDistance) break;
        if (!navigation->lineWalkable(from, candidate, moverFootprint)) {
            const auto path = work != nullptr ? work->query(owner,*navigation,from,candidate,moverFootprint)
                : navigation->findPath(from, candidate, 12000, moverFootprint);
            if (!path.reached()) {
                anyBudgetExhausted = anyBudgetExhausted || path.searchBudgetExhausted;
                if (work != nullptr && path.searchBudgetExhausted) {
                    if (navigationStatus != nullptr) *navigationStatus = best.valid()
                        ? NavigationStatus::reached : path.status;
                    if (searchBudgetExhausted != nullptr) *searchBudgetExhausted = !best.valid();
                    return best;
                }
                if (path.status == NavigationStatus::partial ||
                    path.status == NavigationStatus::budgetExhausted)
                    unresolved = path.status;
                continue;
            }
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
    if (navigationStatus != nullptr)
        *navigationStatus = best.valid() ? NavigationStatus::reached : unresolved;
    if (searchBudgetExhausted != nullptr)
        *searchBudgetExhausted = !best.valid() && anyBudgetExhausted;
    return best;
}

bool groundConstructionSiteSafe(const InfluenceMap& influence,
                                const Position structureCenter) noexcept {
    return structureCenter.valid() && influence.at(structureCenter).groundThreat <= 0.25F;
}

Position groundConstructionScreeningDestination(
    const InfluenceMap& influence, const NavigationGrid* navigation,
    const Position from, const Position structureCenter,
    const MovementFootprint structureFootprint, const MovementFootprint moverFootprint,
    NavigationStatus* navigationStatus, bool* searchBudgetExhausted,
    NavigationWorkQueue* work, const std::uint64_t owner) {
    if (navigationStatus != nullptr) *navigationStatus = NavigationStatus::invalidInput;
    if (searchBudgetExhausted != nullptr) *searchBudgetExhausted = false;
    if (!from.valid() || !groundConstructionSiteSafe(influence, structureCenter)) return {-1, -1};
    if (navigation == nullptr || navigation->empty() ||
        navigation->walkable(structureCenter, moverFootprint)) {
        if (navigationStatus != nullptr) *navigationStatus = NavigationStatus::reached;
        return structureCenter;
    }
    // A blocked anchor is not a requirement to walk through the building.
    // Execution stages beside its footprint; preselection needs the same
    // complete approach proof before rejecting a worker as unreachable.
    return nearestGroundConstructionAccessPoint(
        navigation, from, structureCenter, structureFootprint, moverFootprint, 96,
        navigationStatus, searchBudgetExhausted, work, owner);
}

RouteThreatProfile assessRouteThreat(
    const InfluenceMap& influence, const Position from, const Position to,
    const bool flying, const NavigationGrid* navigation,
    const MovementFootprint footprint,
    NavigationWorkQueue* work, const std::uint64_t owner) {
    auto status = NavigationStatus::invalidInput;
    auto exhausted = false;
    auto profile = assessPathThreat(influence,
        routePath(from, to, flying, navigation, footprint, &status, &exhausted, work, owner), flying, navigation);
    profile.navigationStatus = status;
    profile.searchBudgetExhausted = exhausted;
    return profile;
}

RouteAlternative selectSafeRouteAlternative(
    const InfluenceMap& influence, const Position from, const Position to,
    const bool flying, const NavigationGrid* navigation,
    const MovementFootprint footprint, const double maximumRisk,
    const Position previousWaypoint, const double maximumPeakThreat,
    NavigationWorkQueue* work, const std::uint64_t owner) {
    auto status = NavigationStatus::invalidInput;
    auto exhausted = false;
    const auto basePath = routePath(from, to, flying, navigation, footprint, &status, &exhausted, work, owner);
    auto best = RouteAlternative{assessPathThreat(influence, basePath, flying, navigation), { -1, -1 }};
    best.profile.navigationStatus = status;
    best.profile.searchBudgetExhausted = exhausted;
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

    // Mining motion must not regenerate every midpoint while its route legs
    // are still pending. The baseline query has already validated the current
    // actor's join; its saved origin only anchors candidate geometry.
    const auto retainedFrom = work != nullptr ? work->retainedOrigin(owner,to,footprint) : Position{-1,-1};
    const auto candidateOrigin = retainedFrom.valid() ? retainedFrom : from;
    const auto length = std::max(1.0, distance(candidateOrigin, to));
    const auto dx = static_cast<double>(to.x - candidateOrigin.x) / length;
    const auto dy = static_cast<double>(to.y - candidateOrigin.y) / length;
    const auto radius = std::max(384, navigation->cellSize() * 8);
    const std::array<int, 4> offsets{radius, -radius, radius * 2, -radius * 2};
    const auto mapWidth = navigation->width() * navigation->cellSize();
    const auto mapHeight = navigation->height() * navigation->cellSize();
    std::array<Position, 5> candidates;
    for (std::size_t index = 0; index < offsets.size(); ++index) {
        const auto offset = offsets[index];
        // Preserve the boundary-side corridor when a perpendicular offset
        // leaves the map. Clipping is only candidate generation: the current
        // footprint, both complete route legs and all threat limits still
        // have to pass before this point can authorize movement.
        candidates[index] = {
            std::clamp((candidateOrigin.x + to.x) / 2 - static_cast<int>(std::lround(dy * offset)),
                footprint.left, std::max(footprint.left, mapWidth - 1 - footprint.right)),
            std::clamp((candidateOrigin.y + to.y) / 2 + static_cast<int>(std::lround(dx * offset)),
                footprint.up, std::max(footprint.up, mapHeight - 1 - footprint.down)),
        };
    }
    // Actor or endpoint motion can move every generated midpoint away from
    // the previous detour. Reprove that hint as one additional candidate;
    // it carries no cached terrain or threat authority. Keep all four new
    // midpoint alternatives and the shared navigation work ceiling.
    candidates.back() = std::find(candidates.begin(), candidates.end() - 1,
        previousWaypoint) == candidates.end() - 1 ? previousWaypoint : Position{-1, -1};
    auto unresolvedAlternatives = false;
    for (const auto via : candidates) {
        if (!via.valid() || via.x >= mapWidth || via.y >= mapHeight ||
            distance(from, via) <= std::max(96, navigation->cellSize() * 3) ||
            distance(via, to) <= std::max(96, navigation->cellSize() * 3) ||
            !navigation->walkable(via, footprint)) continue;

        auto firstStatus = NavigationStatus::invalidInput;
        auto secondStatus = NavigationStatus::invalidInput;
        auto firstExhausted = false;
        auto secondExhausted = false;
        const auto first = routePath(from, via, false, navigation, footprint, &firstStatus, &firstExhausted, work, owner);
        if (work != nullptr && firstExhausted) {
            best.unresolvedAlternatives = true;
            return best;
        }
        const auto second = routePath(via, to, false, navigation, footprint,
            &secondStatus, &secondExhausted, work, owner, NavigationOriginPolicy::fixed);
        if (work != nullptr && secondExhausted) {
            best.unresolvedAlternatives = true;
            return best;
        }
        unresolvedAlternatives = unresolvedAlternatives || firstExhausted || secondExhausted;
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
        {
            previousCandidate.unresolvedAlternatives = unresolvedAlternatives;
            return previousCandidate;
        }
    best.unresolvedAlternatives = unresolvedAlternatives;
    return best;
}

}  // namespace protodd
