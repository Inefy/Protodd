#pragma once

#include "protodd/Navigation.hpp"
#include "protodd/Squads.hpp"

#include <algorithm>
#include <iterator>

namespace protodd {

[[nodiscard]] inline bool nativeUncontestedGroundTravel(
    const Squad& squad, const CombatEstimate& estimate,
    const DefenseArea& defense, const Command& order) noexcept {
    const auto ordinaryAdvance = order.source == "squad-objective" ||
                                 order.source == "payload-reload-travel";
    return squad.role == SquadRole::mainArmy && squad.enemies.empty() &&
        !squad.withdrawing && !defense.active() && !estimate.advanceBlocked &&
        !estimate.holdScreen && estimate.decision == FightDecision::engage &&
        ordinaryAdvance && order.targetPosition.valid() &&
        (order.type == CommandType::move || order.type == CommandType::attackMove);
}

struct ActorMovementRoutingResult {
    Command order;
    bool pathSearched{};
};

inline constexpr int maximumActorRouteExpansionsPerUpdate = 16'000;

// Keep the actor's checked straight prefix for tactical movement. Ordinary
// uncontested advances can delegate the full destination to engine pathing.
[[nodiscard]] inline ActorMovementRoutingResult routeActorMovement(
    const Command& order, const UnitSnapshot& actor, const NavigationGrid& navigation,
    const bool nativeTravel, const int maximumExpansions = 4000) {
    ActorMovementRoutingResult result{order};
    const auto movement = order.type == CommandType::move || order.type == CommandType::attackMove;
    const MovementFootprint footprint{actor.dimensionLeft, actor.dimensionRight,
                                      actor.dimensionUp, actor.dimensionDown};
    if (!movement || actor.flying || !order.targetPosition.valid() ||
        (nativeTravel && actor.position.valid()) ||
        navigation.lineWalkable(actor.position, order.targetPosition, footprint)) return result;
    std::vector<Position> points;
    const auto route = navigation.nextWaypoint(actor.position, order.targetPosition,
                                               4, std::max(0, maximumExpansions), footprint, &points);
    result.pathSearched = maximumExpansions > 0;
    auto waypoint = route.waypoint;
    if (route.hasUsableWaypoint() && !navigation.lineWalkable(actor.position, waypoint, footprint)) {
        const auto chosen = std::ranges::find(points, waypoint);
        waypoint = {-1, -1};
        if (chosen != points.end()) {
            for (auto point = std::make_reverse_iterator(chosen + 1); point != points.rend(); ++point) {
                if (navigation.lineWalkable(actor.position, *point, footprint)) {
                    waypoint = *point;
                    break;
                }
            }
        }
    }
    if (route.hasUsableWaypoint() && waypoint.valid() && waypoint != actor.position) {
        result.order.targetPosition = waypoint;
    } else {
        result.order = {order.actor, CommandType::hold, -1, {-1, -1}, UnitKind::unknown,
                        order.priority, 0, "navigation-route-unavailable"};
    }
    return result;
}

} // namespace protodd
