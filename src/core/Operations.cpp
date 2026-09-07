#include "protodd/Operations.hpp"
#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <cmath>

namespace protodd {

void ExpansionCoordinator::reset() noexcept {
    retryAfter_ = 0;
    failedSite_ = {-1, -1};
    releaseBuilder_ = false;
    reason_ = "No expansion mission";
}

void ExpansionCoordinator::update(StrategicPlan& plan, const GameState& state,
                                  const ExpansionFeedback& feedback) {
    releaseBuilder_ = false;
    plan.deferExpansion = false;
    if (!plan.expansionTarget.valid()) {
        reason_ = "No expansion mission";
        return;
    }
    const auto constructing = std::ranges::any_of(state.self.units, [&plan](const UnitSnapshot& unit) {
        return unit.kind == UnitKind::nexus && !unit.completed &&
               distanceSquared(unit.position, plan.expansionTarget) < 96 * 96;
    });
    if (constructing) {
        reason_ = "Protect Nexus while it warps in";
        return;
    }
    // A worker still travelling does not trigger this circuit breaker. Stop
    // the stale order before releasing the bank; otherwise it can spend later.
    if (feedback.pending && feedback.stalledFrames >= 8 * 24) {
        failedSite_ = feedback.site;
        retryAfter_ = state.frame + 12 * 24;
        releaseBuilder_ = true;
    }
    if (state.frame < retryAfter_ &&
        distanceSquared(plan.expansionTarget, failedSite_) <= 96 * 96) {
        plan.deferExpansion = true;
        reason_ = "Builder stalled: reinforce and clear site before retry";
    } else {
        reason_ = feedback.pending ? "Escort builder; keep Nexus footprint clear" :
                                    "Assemble beside expansion; fund construction";
    }
}

Position expansionAssemblyPoint(const GameState& state, const Position site,
                                const Position home) noexcept {
    if (!site.valid()) return site;
    for (const auto& base : state.bases) {
        if (distanceSquared(site, base.center) <= 64 * 64 && base.defense.valid() &&
            distanceSquared(site, base.defense.anchor) >= 176 * 176)
            return base.defense.anchor;
    }
    // Stay on the reachable home side, outside the 128x96 Nexus footprint.
    if (home.valid() && distanceSquared(home, site) >= 192 * 192)
        return moveToward(site, home, 192.0);
    return {std::max(0, site.x - 192), site.y};
}

std::vector<Command> clearExpansionFootprint(
    const GameState& state, const Position site, const Position assembly) {
    std::vector<Command> result;
    if (!site.valid() || !assembly.valid()) return result;
    if (std::ranges::any_of(state.self.units, [site](const UnitSnapshot& unit) {
        return unit.kind == UnitKind::nexus && distanceSquared(site, unit.position) < 96 * 96;
    })) return result;
    for (const auto& unit : state.self.units) {
        if (!unit.completed || unit.loaded || unit.flying || unit.disabled || unit.attackFrame ||
            unit.underAttack || !isCombatUnit(unit.kind) || isBuilding(unit.kind) ||
            !unit.position.valid() || std::abs(unit.position.x - site.x) > 104 ||
            std::abs(unit.position.y - site.y) > 88) continue;
        // Never interrupt a ready defensive shot to make room for construction.
        const auto fighting = std::ranges::any_of(state.enemy.units, [&unit](const UnitSnapshot& enemy) {
            const auto& weapon = enemy.flying ? unit.airWeapon : unit.groundWeapon;
            return enemy.visible && enemy.position.valid() && unit.canAttack(enemy) &&
                   distanceSquared(unit.position, enemy.position) <=
                       (weapon.maxRange + 64) * (weapon.maxRange + 64);
        });
        if (!fighting) result.push_back({unit.id, CommandType::move, -1, assembly,
            UnitKind::unknown, 92, 0, "clear-nexus-footprint"});
    }
    return result;
}

}  // namespace protodd
