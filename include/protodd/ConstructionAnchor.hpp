#pragma once

#include "protodd/GameState.hpp"

#include <algorithm>
#include <span>
#include <string_view>

namespace protodd {

[[nodiscard]] constexpr bool usesHomeConstructionAnchor(UnitKind kind) noexcept {
    switch (kind) {
    case UnitKind::pylon:
    case UnitKind::gateway:
    case UnitKind::forge:
    case UnitKind::cyberneticsCore:
    case UnitKind::roboticsFacility:
    case UnitKind::observatory:
    case UnitKind::roboticsSupportBay:
    case UnitKind::stargate:
    case UnitKind::citadelOfAdun:
    case UnitKind::templarArchives:
    case UnitKind::fleetBeacon:
    case UnitKind::arbiterTribunal:
    case UnitKind::assimilator:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] constexpr Position constructionBuilderAnchor(
    UnitKind kind, Position home, Position rally, Position expansion,
    bool defenseDirectionKnown = true, bool groundPressure = false) noexcept {
    if (kind == UnitKind::nexus && expansion.valid()) return expansion;
    if (usesHomeConstructionAnchor(kind) && home.valid()) return home;
    if ((kind == UnitKind::photonCannon || kind == UnitKind::shieldBattery) && home.valid()) {
        // Static placement uses the powered defended base unless its forward
        // intercept is compact. Worker screening must prove this same journey,
        // rather than an unrelated distant army rally.
        const auto forwardDistanceSquared = distanceSquared(home, rally);
        if (!defenseDirectionKnown || !rally.valid() ||
            forwardDistanceSquared < 64 * 64 || forwardDistanceSquared > 256 * 256)
            return home;
        if (groundPressure) return {(home.x + rally.x) / 2, (home.y + rally.y) / 2};
    }
    return rally.valid() ? rally : home;
}

[[nodiscard]] constexpr bool homeAnchoredSupplyPylon(
    const UnitKind kind, const std::string_view reason,
    const bool hasConstructionSite) noexcept {
    if (kind != UnitKind::pylon || hasConstructionSite) return false;
    return reason == "operational supply invariant" ||
           reason == "maintain a supply buffer" ||
           reason == "restore power to disabled production";
}

[[nodiscard]] constexpr bool relocatableSupplyPylon(
    const UnitKind kind, const std::string_view reason,
    const bool hasConstructionSite) noexcept {
    return homeAnchoredSupplyPylon(kind, reason, hasConstructionSite) &&
           reason != "restore power to disabled production";
}

struct SupplyConstructionBase {
    UnitId id{-1};
    Position center{-1, -1};
    bool completed{};
    bool threatened{};
};

struct RefineryConstructionSite {
    UnitId geyser{-1};
    Position center{-1, -1};
    bool completedDepot{};
    bool threatened{};
};

struct CannonConstructionSupport {
    UnitId id{-1};
    Position center{-1, -1};
    int groundRange{};
    bool available{};
    bool powered{};
    bool completed{};
    Frame remainingBuild{-1};
};

struct OpeningCannonPairSupport {
    UnitId id{-1};
    Position center{-1, -1};
    int groundRange{};

    [[nodiscard]] constexpr bool active() const noexcept {
        return center.valid() && groundRange > 0;
    }
};

// The second opening Cannon needs the first one's firing cover while it
// warps. A paid, powered first Cannon with no more work left than a new Cannon
// is projected to finish first. Later defenses, expansions and explicit
// forward sites keep their ordinary placement policy.
[[nodiscard]] inline OpeningCannonPairSupport selectOpeningCannonPairSupport(
    const UnitKind kind, const Frame frame, const int nexusCount,
    const Position defendedBase, const bool explicitSite,
    const Frame newCannonBuildTime,
    const std::span<const CannonConstructionSupport> cannons) noexcept {
    if (kind != UnitKind::photonCannon || frame < 0 || frame >= 8 * 60 * 24 ||
        nexusCount != 1 || explicitSite || !defendedBase.valid() || cannons.size() != 1)
        return {};
    const auto& first = cannons.front();
    if (!first.available || !first.powered || !first.center.valid() ||
        first.groundRange <= 0 ||
        distanceSquared(first.center, defendedBase) > 416 * 416 ||
        (!first.completed && (newCannonBuildTime <= 0 || first.remainingBuild < 0 ||
                              first.remainingBuild > newCannonBuildTime)))
        return {};
    return {first.id, first.center, first.groundRange};
}

// This only admits supporting geometry. Power, occupancy, mining lanes and
// current builder/producer access must still pass their independent proofs.
[[nodiscard]] constexpr bool openingCannonPlacementAllowed(
    const Position candidate, const OpeningCannonPairSupport support) noexcept {
    return candidate.valid() && (!support.active() ||
        distanceSquared(candidate, support.center) <= support.groundRange * support.groundRange);
}

// Static partners can use adjacent build tiles. Their collision bodies and
// surrounding producer/mineral routes still receive the ordinary access
// proofs. Only the selected first Cannon gets this exception; other structures
// and every producer keep their required movement gap.
[[nodiscard]] constexpr int openingCannonStructureGap(
    const UnitKind kind, const UnitKind otherKind, const Position otherCenter,
    const int ordinaryGap, const OpeningCannonPairSupport support) noexcept {
    return kind == UnitKind::photonCannon && otherKind == UnitKind::photonCannon &&
        support.active() && otherCenter == support.center ? 0 : ordinaryGap;
}

// A refinery belongs to an available resource at a completed economy. Use
// that resource as the builder/placement anchor, independently of army rally.
// Current terrain and route safety are still proved by the construction path.
[[nodiscard]] inline Position selectRefineryConstructionAnchor(
    const Position home, const std::span<const RefineryConstructionSite> sites) noexcept {
    const RefineryConstructionSite* best = nullptr;
    for (const auto& site : sites) {
        if (!site.completedDepot || site.threatened || !site.center.valid()) continue;
        if (best == nullptr ||
            (home.valid() && distanceSquared(home, site.center) < distanceSquared(home, best->center)) ||
            ((!home.valid() || distanceSquared(home, site.center) == distanceSquared(home, best->center)) &&
             site.geyser < best->geyser)) best = &site;
    }
    return best != nullptr ? best->center : Position{-1, -1};
}

// Keep ordinary supply near home until it is threatened. A completed safe
// base may then supply the whole army without sending its builder back into
// the evacuation lane. This selects an anchor, never a route-safety proof.
[[nodiscard]] inline Position selectSupplyConstructionAnchor(
    const Position home, const std::span<const SupplyConstructionBase> bases) noexcept {
    if (!home.valid() || std::ranges::none_of(bases, [home](const auto& base) {
            return base.completed && base.center == home && base.threatened;
        })) return home;
    const SupplyConstructionBase* best = nullptr;
    for (const auto& base : bases) {
        if (!base.completed || base.threatened || !base.center.valid()) continue;
        if (best == nullptr || distanceSquared(home, base.center) < distanceSquared(home, best->center) ||
            (distanceSquared(home, base.center) == distanceSquared(home, best->center) &&
             base.id < best->id)) best = &base;
    }
    return best != nullptr ? best->center : home;
}

}  // namespace protodd
