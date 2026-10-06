#include "protodd/Operations.hpp"
#include "protodd/Combat.hpp"
#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <cmath>

namespace protodd {
namespace {

constexpr Frame kExpansionRetryFrames = 12 * 24;
constexpr Frame kSurvivalRunwayFrames = 9 * 60 * 24;

Position safeAlternativeSite(const GameState& state, const Position blockedSite,
                             const Position recentlyFailedSite,
                             const Frame retryAfter) {
    const BaseSnapshot* best = nullptr;
    for (const auto& base : state.bases) {
        if (base.ownerId != -1 || base.island || !base.center.valid() ||
            !base.mineralLine.valid() || base.mineralPatches < 4 ||
            base.mineralsRemaining < 4000 || base.groundDistanceFromMain < 0 ||
            distanceSquared(base.center, blockedSite) <= 96 * 96 ||
            (state.frame < retryAfter &&
             distanceSquared(base.center, recentlyFailedSite) <= 96 * 96)) {
            continue;
        }
        const auto occupied = std::ranges::any_of(state.self.units,
            [&base](const UnitSnapshot& unit) {
                return unit.kind == UnitKind::nexus && unit.position.valid() &&
                       distanceSquared(unit.position, base.center) < 320 * 320;
            });
        if (occupied) continue;
        const auto threatened = std::ranges::any_of(state.enemy.units,
            [&state, &base](const UnitSnapshot& enemy) {
                if (!enemy.completed || enemy.disabled || enemy.loaded ||
                    enemy.hallucination || enemy.invincible ||
                    !enemy.position.valid() || enemy.groundWeapon.damage <= 0 ||
                    (!enemy.visible && (enemy.lastSeen <= 0 ||
                     state.frame - enemy.lastSeen > 5 * 24))) return false;
                const auto radius = std::max(480, enemy.groundWeapon.maxRange + 128);
                return distanceSquared(base.center, enemy.position) <= radius * radius;
            });
        if (threatened) continue;
        if (best == nullptr || base.groundDistanceFromMain < best->groundDistanceFromMain ||
            (base.groundDistanceFromMain == best->groundDistanceFromMain &&
             base.id < best->id)) best = &base;
    }
    return best != nullptr ? best->center : Position{-1, -1};
}

void addWorkerSurvivalPlan(StrategicPlan& plan, const GameState& state) {
    const auto completedNexuses = static_cast<int>(std::ranges::count_if(
        state.self.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::nexus && unit.completed;
        }));
    const auto activeBases = static_cast<int>(std::ranges::count_if(
        state.bases, [&state](const BaseSnapshot& base) {
            return base.ownerId == state.self.id && base.mineralPatches > 0 &&
                   base.mineralsRemaining > 0;
        }));
    if (completedNexuses == 0 || activeBases == 0 ||
        plan.estimatedMiningRunwayFrames < 0 ||
        plan.estimatedMiningRunwayFrames > kSurvivalRunwayFrames) return;

    const auto targetWorkers = std::min(16, activeBases * 12);
    plan.name += " [survival: expansion blocked]";
    if (plan.posture != Posture::defend) plan.posture = Posture::recover;
    plan.desiredBases = completedNexuses;
    plan.desiredWorkers = std::max(plan.desiredWorkers, targetWorkers);
    plan.desiredGasWorkers = 0;
    plan.sustainEconomy = true;
    plan.prioritizeReinforcements = false;
    auto probeGoal = std::ranges::find_if(plan.goals,
        [](const ProductionGoal& goal) {
            return goal.goal == GoalKind::train && goal.target == UnitKind::probe;
        });
    if (probeGoal == plan.goals.end()) {
        plan.goals.push_back({GoalKind::train, UnitKind::probe, targetWorkers,
                              118, true,
                              "preserve mining income while scouting an expansion alternative"});
    } else {
        probeGoal->desiredCount = std::max(probeGoal->desiredCount, targetWorkers);
        probeGoal->priority = std::max(probeGoal->priority, 118);
        probeGoal->blocking = true;
        probeGoal->reason = "preserve mining income while scouting an expansion alternative";
    }
}

}  // namespace

void ExpansionCoordinator::reset() noexcept {
    retryAfter_ = 0;
    failedSite_ = {-1, -1};
    releaseBuilder_ = false;
    reason_ = "No expansion mission";
}

void ExpansionCoordinator::update(StrategicPlan& plan, const GameState& state,
                                  const ExpansionFeedback& feedback) {
    releaseBuilder_ = false;
    const auto preservingWorkers =
        plan.name.find("[survival: no ready replacement base]") != std::string::npos;
    plan.deferExpansion = preservingWorkers;
    if (!plan.expansionTarget.valid()) {
        reason_ = preservingWorkers ? "No ready replacement base; preserve remaining mining" :
                                     "No expansion mission";
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
    std::vector<UnitSnapshot> siteEnemies;
    for (const auto& enemy : state.enemy.units) {
        const auto radius = std::max(480, enemy.groundWeapon.maxRange + 128);
        if (enemy.completed && !enemy.disabled && !enemy.loaded &&
            !enemy.hallucination && !enemy.invincible && enemy.position.valid() &&
            (enemy.visible || (enemy.lastSeen > 0 && state.frame - enemy.lastSeen <= 5 * 24)) &&
            enemy.groundWeapon.damage > 0 &&
            distanceSquared(enemy.position, plan.expansionTarget) <= radius * radius)
            siteEnemies.push_back(enemy);
    }
    auto expansionBlocked = false;
    auto blockedReason = std::string_view{};
    if (!siteEnemies.empty()) {
        std::vector<UnitSnapshot> cover;
        for (const auto& unit : state.self.units) {
            if (unit.completed && unit.powered && !unit.loaded && !unit.disabled &&
                !unit.hallucination && unit.position.valid() &&
                (isCombatUnit(unit.kind) || isStaticDefense(unit.kind)) &&
                distanceSquared(unit.position, plan.expansionTarget) <= 384 * 384 &&
                std::ranges::any_of(siteEnemies, [&unit](const UnitSnapshot& enemy) {
                    return unit.canAttack(enemy);
                }))
                cover.push_back(unit);
        }
        // A strong army at home does not protect the construction site. Keep
        // its assembly mission, but spend only after a local screen can fight.
        const auto estimate = CombatEvaluator{}.evaluate(cover, siteEnemies, 1.25, 0.15, false);
        if (cover.empty() || estimate.ratio < 1.25) {
            expansionBlocked = true;
            blockedReason = "Clear expansion threats before committing Nexus";
        }
    }
    const auto builderStalled = feedback.pending && feedback.stalledFrames >= 8 * 24;
    // A worker still travelling does not trigger this circuit breaker. Stop
    // the stale order before releasing the bank; otherwise it can spend later.
    if (builderStalled) {
        failedSite_ = feedback.site;
        retryAfter_ = state.frame + kExpansionRetryFrames;
        releaseBuilder_ = true;
    }
    const auto waitingToRetry = state.frame < retryAfter_ &&
        distanceSquared(plan.expansionTarget, failedSite_) <= 96 * 96;
    if (waitingToRetry && !expansionBlocked) {
        expansionBlocked = true;
        blockedReason = "Builder stalled: reinforce and clear site before retry";
    }

    if (expansionBlocked) {
        if (builderStalled) failedSite_ = plan.expansionTarget;
        const auto alternative = safeAlternativeSite(
            state, plan.expansionTarget, failedSite_, retryAfter_);
        if (alternative.valid()) {
            const auto prior = plan.expansionTarget;
            plan.expansionTarget = alternative;
            if (distanceSquared(plan.rallyPoint, prior) <= 96 * 96)
                plan.rallyPoint = alternative;
            plan.deferExpansion = false;
            plan.name += " [alternate safe expansion]";
            releaseBuilder_ = releaseBuilder_ || feedback.pending;
            reason_ = "Switch to reachable, mineral-rich alternative expansion";
            return;
        }

        plan.deferExpansion = true;
        addWorkerSurvivalPlan(plan, state);
        releaseBuilder_ = releaseBuilder_ || feedback.pending;
        reason_ = plan.name.find("survival: expansion blocked") != std::string::npos
            ? "No safe alternative; preserve the remaining worker income"
            : blockedReason.empty() ? "Expansion blocked; hold current mining lines and retry"
                                    : blockedReason;
        return;
    }

    reason_ = feedback.pending ? "Escort builder; keep Nexus footprint clear" :
                                "Assemble beside expansion; fund construction";
}

Position expansionAssemblyPoint(const GameState& state, const Position site,
                                const Position home) noexcept {
    if (!site.valid()) return site;
    for (const auto& base : state.bases) {
        if (distanceSquared(site, base.center) <= 64 * 64 && base.defense.valid() &&
            distanceSquared(site, base.defense.anchor) >= 176 * 176 &&
            distanceSquared(site, base.defense.anchor) <= 320 * 320)
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
        if (!unit.completed || unit.loaded || unit.flying || unit.disabled || unit.attackFrame || unit.attackWindup ||
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
