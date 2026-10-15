#pragma once

#include "protodd/CommandBus.hpp"
#include "protodd/BuildTaskProgress.hpp"
#include "protodd/Navigation.hpp"

#include <array>
#include <algorithm>
#include <span>
#include <vector>

namespace protodd {

// Let a progressing worker keep its route without rearranging nearby traffic.
// Admit assistance before ordinary waypoint retention expires, only after a
// full second without the existing meaningful waypoint-progress observation.
[[nodiscard]] inline bool constructionTrafficNeedsClearance(
    const BuildWaypointProgress& progress, Frame frame, Position waypoint) noexcept {
    return waypoint.valid() && progress.waypoint == waypoint &&
        progress.bestDistancePixels >= 0 && progress.lastProgress >= 0 &&
        frame >= progress.lastProgress && frame - progress.lastProgress >= 24;
}

struct ConstructionTrafficContext {
    std::uint64_t task{};
    UnitId builder{-1};
    Frame firstSeen{-1};
    Position site{-1, -1};
    Position waypoint{-1, -1};
    Frame leaseIssued{-1};
    friend bool operator==(const ConstructionTrafficContext&, const ConstructionTrafficContext&) = default;
};

[[nodiscard]] inline bool constructionTrafficHelper(const UnitSnapshot& unit) noexcept {
    return unit.ours && unit.completed && unit.visible && unit.position.valid() &&
        unit.role == UnitRole::groundArmy && !unit.flying && !unit.loaded &&
        !unit.burrowed && !unit.disabled && !unit.hallucination &&
        !unit.underAttack && !unit.recentlyDamaged && !unit.attackFrame &&
        !unit.attackWindup && !unit.underStorm && unit.incomingDamage <= 0 &&
        unit.healthFraction() >= 0.75 &&
        (unit.kind == UnitKind::zealot || unit.kind == UnitKind::dragoon ||
         unit.kind == UnitKind::archon || unit.kind == UnitKind::darkTemplar);
}

[[nodiscard]] inline bool constructionTrafficProposal(const Command& command) noexcept {
    return command.owner == CommandOwner::combat && command.type == CommandType::move &&
        command.effectiveUrgency() <= 90 &&
        (command.source == "defense-return" || command.source == "combat-retreat");
}

[[nodiscard]] inline NavigationObstacleBounds trafficBody(const UnitSnapshot& unit) noexcept {
    return {unit.position.x - unit.dimensionLeft, unit.position.y - unit.dimensionUp,
            unit.position.x + unit.dimensionRight, unit.position.y + unit.dimensionDown};
}

[[nodiscard]] inline bool trafficSegmentIntersects(
    Position from, Position to, NavigationObstacleBounds box) noexcept {
    double begin = 0, end = 1;
    const auto clip = [&begin, &end](double origin, double delta, double low, double high) {
        if (delta == 0) return origin >= low && origin <= high;
        auto a = (low - origin) / delta, b = (high - origin) / delta;
        if (a > b) std::swap(a, b);
        begin = std::max(begin, a); end = std::min(end, b);
        return begin <= end;
    };
    return clip(from.x, to.x - from.x, box.left, box.right) &&
           clip(from.y, to.y - from.y, box.top, box.bottom);
}

// Include ineligible bodies in connectivity: a worker cannot pass through a
// fighting or foreign-owned blocker just because it cannot be asked to move.
[[nodiscard]] inline std::vector<UnitId> constructionTrafficCluster(
    const UnitSnapshot& builder, Position waypoint, std::span<const UnitSnapshot> units) {
    if (!builder.position.valid() || !waypoint.valid() ||
        distanceSquared(builder.position, waypoint) > 320 * 320) return {};
    std::vector<const UnitSnapshot*> nearby;
    for (const auto& unit : units) {
        if (unit.id == builder.id || unit.flying || unit.loaded ||
            !unit.position.valid() || !unit.ours || !unit.completed ||
            unit.dimensionLeft + unit.dimensionRight <= 0 ||
            distanceSquared(unit.position, builder.position) > 512 * 512) continue;
        nearby.push_back(&unit);
        if (nearby.size() > 64) return {};
    }
    std::vector<std::size_t> selected;
    for (std::size_t i = 0; i < nearby.size(); ++i) {
        auto bounds = trafficBody(*nearby[i]);
        bounds.left -= builder.dimensionRight; bounds.right += builder.dimensionLeft;
        bounds.top -= builder.dimensionDown; bounds.bottom += builder.dimensionUp;
        if (trafficSegmentIntersects(builder.position, waypoint, bounds)) selected.push_back(i);
    }
    for (std::size_t next = 0; next < selected.size(); ++next) {
        if (selected.size() > 7) return {};
        const auto a = trafficBody(*nearby[selected[next]]);
        for (std::size_t i = 0; i < nearby.size(); ++i) {
            if (std::ranges::find(selected, i) != selected.end()) continue;
            const auto b = trafficBody(*nearby[i]);
            const auto gapX = std::max({0, a.left - b.right - 1, b.left - a.right - 1});
            const auto gapY = std::max({0, a.top - b.bottom - 1, b.top - a.bottom - 1});
            if (gapX < builder.dimensionLeft + builder.dimensionRight + 1 &&
                gapY < builder.dimensionUp + builder.dimensionDown + 1) selected.push_back(i);
        }
    }
    std::vector<UnitId> result;
    for (const auto i : selected) result.push_back(nearby[i]->id);
    std::ranges::sort(result);
    return result;
}

struct ConstructionTrafficMember {
    UnitId actor{-1};
    Frame firstSeen{-1};
    Position destination{-1, -1};
    double bestDistance{};
    bool acknowledged{};
    int urgency{};
    Frame routeProved{-1};
};

struct ConstructionTrafficAssistance {
    ConstructionTrafficContext context;
    std::vector<ConstructionTrafficMember> members;
    Frame started{-1};
    Frame lastProgress{-1};
    Frame lastEligible{-1};
    Frame lastHoldDiagnostic{-1};
    std::uint64_t generation{};
    std::uint64_t terrain{}, collision{};
    bool closed{};
    bool motionObserved{};

    void acknowledge(UnitId actor, Position destination, bool accepted) noexcept {
        for (auto& member : members) {
            if (member.actor == actor && member.destination == destination && accepted)
                member.acknowledged = true;
        }
    }
    [[nodiscard]] bool acknowledged() const noexcept {
        return !members.empty() && std::ranges::all_of(members, [](const auto& member) {
            return member.acknowledged;
        });
    }
    [[nodiscard]] bool live(Frame frame, const ConstructionTrafficContext& current) const noexcept {
        return !closed && current == context && started >= 0 && frame >= started &&
            frame - started < 288 && lastEligible >= 0 && frame - lastEligible <= 12 &&
            lastProgress >= 0 && frame - lastProgress <= 48;
    }
    [[nodiscard]] bool canHold(Frame frame, const ConstructionTrafficContext& current) const noexcept {
        return live(frame, current) && acknowledged() && motionObserved;
    }
};

} // namespace protodd
