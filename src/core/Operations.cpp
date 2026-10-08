#include "protodd/Operations.hpp"
#include "protodd/Combat.hpp"
#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <cmath>

namespace protodd {
namespace {

constexpr Frame kExpansionRetryFrames = 12 * 24;
constexpr Frame kUnsafeRouteHoldFrames = 30 * 24;
constexpr Frame kUnsafeRouteRetryFrames = 60 * 24;
constexpr Frame kUnsafeRouteSafeResetFrames = 5 * 24;
constexpr Frame kSurvivalRunwayFrames = 9 * 60 * 24;

Position safeAlternativeSite(const GameState& state, const Position blockedSite,
                             const std::vector<Position>& recentlyFailedSites,
                             const Frame retryAfter) {
    const BaseSnapshot* best = nullptr;
    for (const auto& base : state.bases) {
        const auto recentlyFailed = state.frame < retryAfter &&
            std::ranges::any_of(recentlyFailedSites, [&base](const Position site) {
                return site.valid() && distanceSquared(base.center, site) <= 96 * 96;
            });
        if (base.ownerId != -1 || base.island || !base.depotFootprintAvailable ||
            !base.center.valid() ||
            !base.mineralLine.valid() || base.mineralPatches < 4 ||
            base.mineralsRemaining < 4000 || base.groundDistanceFromMain < 0 ||
            distanceSquared(base.center, blockedSite) <= 96 * 96 ||
            recentlyFailed) {
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
    failedSites_.clear();
    committedSite_ = {-1, -1};
    unsafeRouteSince_ = -1;
    unsafeRouteLastFrame_ = -1;
    unsafeRouteSite_ = {-1, -1};
    releaseBuilder_ = false;
    reason_ = "No expansion mission";
}

void ExpansionCoordinator::update(StrategicPlan& plan, const GameState& state,
                                  const ExpansionFeedback& feedback) {
    releaseBuilder_ = false;
    if (state.frame >= retryAfter_) {
        retryAfter_ = 0;
        failedSites_.clear();
    }
    const auto rememberFailedSite = [this, &state](const Position site,
                                                   const Frame retryFrames) {
        if (!site.valid()) return;
        if (std::ranges::none_of(failedSites_, [site](const Position known) {
                return distanceSquared(site, known) <= 96 * 96;
            })) failedSites_.push_back(site);
        retryAfter_ = std::max(retryAfter_, state.frame + retryFrames);
    };
    plan.expansionProtectionRequired = false;
    const auto preservingWorkers = plan.name.find("[survival:") != std::string::npos;
    plan.deferExpansion = preservingWorkers;
    const auto completedNexuses = static_cast<int>(std::ranges::count_if(
        state.self.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::nexus && unit.completed;
        }));
    const auto expansionRequested = feedback.pending ||
        plan.desiredBases > completedNexuses;
    const auto feedbackMatchesPlan = feedback.site.valid() &&
        (!plan.expansionTarget.valid() ||
         distanceSquared(feedback.site, plan.expansionTarget) <= 96 * 96);
    const auto recentlyFailed = [this, &state](const Position site) {
        return site.valid() && state.frame < retryAfter_ &&
            std::ranges::any_of(failedSites_, [site](const Position failedSite) {
                return distanceSquared(site, failedSite) <= 96 * 96;
            });
    };
    if (feedback.pending && feedback.site.valid()) {
        plan.expansionTarget = feedback.site;
        plan.rallyPoint = feedback.site;
        committedSite_ = feedback.site;
    } else if (feedback.unsafeRoute && expansionRequested && !plan.deferExpansion &&
               feedbackMatchesPlan && !recentlyFailed(feedback.site)) {
        plan.expansionTarget = feedback.site;
        plan.rallyPoint = feedback.site;
        committedSite_ = feedback.site;
    } else if (!expansionRequested || plan.deferExpansion) {
        committedSite_ = {-1, -1};
    } else if (plan.expansionTarget.valid() && committedSite_.valid()) {
        const auto site = std::ranges::find_if(state.bases, [this](const BaseSnapshot& base) {
            return distanceSquared(base.center, committedSite_) <= 96 * 96 &&
                base.ownerId == -1 && !base.island && base.depotFootprintAvailable &&
                base.center.valid() && base.mineralLine.valid() && base.mineralPatches >= 4 &&
                base.mineralsRemaining >= 4000 && base.groundDistanceFromMain >= 0;
        });
        const auto failed = state.frame < retryAfter_ &&
            std::ranges::any_of(failedSites_, [this](const Position failedSite) {
                return distanceSquared(failedSite, committedSite_) <= 96 * 96;
            });
        const auto occupied = std::ranges::any_of(state.self.units,
            [this](const UnitSnapshot& unit) {
                return unit.kind == UnitKind::nexus && unit.position.valid() &&
                    distanceSquared(unit.position, committedSite_) < 320 * 320;
            });
        if (site == state.bases.end() || failed || occupied) {
            committedSite_ = {-1, -1};
        } else if (distanceSquared(plan.expansionTarget, committedSite_) > 96 * 96) {
            const auto requestedSite = plan.expansionTarget;
            plan.expansionTarget = committedSite_;
            if (distanceSquared(plan.rallyPoint, requestedSite) <= 96 * 96 ||
                !plan.rallyPoint.valid())
                plan.rallyPoint = committedSite_;
        }
    }
    const auto unsafeFeedbackMatchesPlan = feedback.unsafeRoute &&
        feedback.site.valid() &&
        (feedback.pending || !plan.expansionTarget.valid() ||
         distanceSquared(feedback.site, plan.expansionTarget) <= 96 * 96);
    if (unsafeFeedbackMatchesPlan) {
        if (!unsafeRouteSite_.valid() ||
            distanceSquared(unsafeRouteSite_, feedback.site) > 96 * 96) {
            unsafeRouteSite_ = feedback.site;
            unsafeRouteSince_ = state.frame;
        }
        unsafeRouteLastFrame_ = state.frame;
    } else if (unsafeRouteSince_ >= 0 &&
               state.frame - unsafeRouteLastFrame_ >= kUnsafeRouteSafeResetFrames) {
        unsafeRouteSince_ = -1;
        unsafeRouteLastFrame_ = -1;
        unsafeRouteSite_ = {-1, -1};
    }
    const auto unsafeRouteFeedbackGap = unsafeRouteSince_ >= 0 &&
        unsafeRouteSite_.valid() && expansionRequested && !plan.deferExpansion &&
        state.frame - unsafeRouteLastFrame_ < kUnsafeRouteSafeResetFrames &&
        !unsafeFeedbackMatchesPlan;
    if (unsafeRouteFeedbackGap) {
        plan.expansionTarget = unsafeRouteSite_;
        plan.rallyPoint = unsafeRouteSite_;
        committedSite_ = unsafeRouteSite_;
        plan.expansionProtectionRequired = true;
        reason_ = "Secure a safe Probe route to the expansion site";
        return;
    }
    if (!plan.expansionTarget.valid()) {
        reason_ = preservingWorkers ? "No ready replacement base; preserve remaining mining" :
                                     "No expansion mission";
        return;
    }
    const auto constructing = std::ranges::any_of(state.self.units, [&plan](const UnitSnapshot& unit) {
        return unit.kind == UnitKind::nexus && !unit.completed &&
               distanceSquared(unit.position, plan.expansionTarget) < 96 * 96;
    });
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
    const auto feedbackMatchesTarget = feedback.site.valid() &&
        distanceSquared(feedback.site, plan.expansionTarget) <= 96 * 96;
    if (feedback.unsafeRoute && feedbackMatchesTarget && expansionRequested &&
        !plan.deferExpansion) {
        if (recentlyFailed(feedback.site)) {
            expansionBlocked = true;
            blockedReason = "Probe route remains unsafe during site cooldown; choose an alternative";
        } else {
            if (!unsafeRouteSite_.valid() ||
                distanceSquared(unsafeRouteSite_, plan.expansionTarget) > 96 * 96) {
                unsafeRouteSite_ = plan.expansionTarget;
                unsafeRouteSince_ = state.frame;
            }
            unsafeRouteLastFrame_ = state.frame;
            if (state.frame - unsafeRouteSince_ < kUnsafeRouteHoldFrames) {
                // Give the army one bounded window to secure the builder route.
                // If the route stays unsafe, stop defending this one site
                // forever and try another base or preserve the current miners.
                plan.expansionProtectionRequired = true;
                plan.deferExpansion = false;
                committedSite_ = plan.expansionTarget;
                reason_ = "Secure a safe Probe route to the expansion site";
                return;
            }
            rememberFailedSite(feedback.site, kUnsafeRouteRetryFrames);
            unsafeRouteSince_ = -1;
            unsafeRouteLastFrame_ = -1;
            unsafeRouteSite_ = {-1, -1};
            expansionBlocked = true;
            blockedReason = "Probe route stayed unsafe; choose another expansion site";
        }
    }
    if (feedback.rejectedFootprint && feedback.site.valid()) {
        rememberFailedSite(feedback.site, kExpansionRetryFrames);
        if (feedbackMatchesTarget) {
            expansionBlocked = true;
            blockedReason = "Engine rejected Nexus footprint; choose another expansion site";
        }
    }
    if (feedback.noSafeBuilder && feedback.site.valid()) {
        rememberFailedSite(feedback.site, 15 * 24);
        if (feedbackMatchesTarget) {
            expansionBlocked = true;
            blockedReason = "No safe Probe route to Nexus site; choose an alternative";
        }
    }
    if (feedback.noPlacement && feedback.site.valid()) {
        rememberFailedSite(feedback.site, 30 * 24);
        if (feedbackMatchesTarget) {
            expansionBlocked = true;
            blockedReason = "No legal Nexus footprint at site; choose an alternative";
        }
    }
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
    if (constructing) {
        committedSite_ = plan.expansionTarget;
        // Once the Nexus exists, keep its task and funding committed. Continue
        // evaluating the local fight, though, and pull the mobile screen back
        // when the site lacks enough nearby cover.
        plan.deferExpansion = false;
        plan.expansionProtectionRequired = expansionBlocked;
        reason_ = expansionBlocked ? "Reinforce threatened warping Nexus" :
            siteEnemies.empty() ? "Protect Nexus while it warps in" :
                                  "Local cover protects warping Nexus";
        return;
    }
    const auto builderStalled = feedback.pending && feedback.stalledFrames >= 8 * 24;
    // A worker still travelling does not trigger this circuit breaker. Stop
    // the stale order before releasing the bank; otherwise it can spend later.
    if (builderStalled) {
        rememberFailedSite(feedback.site, kExpansionRetryFrames);
        releaseBuilder_ = true;
    }
    const auto waitingToRetry = state.frame < retryAfter_ &&
        std::ranges::any_of(failedSites_, [&plan](const Position site) {
            return distanceSquared(plan.expansionTarget, site) <= 96 * 96;
        });
    if (waitingToRetry && !expansionBlocked) {
        expansionBlocked = true;
        blockedReason = "Builder stalled: reinforce and clear site before retry";
    }

    if (expansionBlocked) {
        const auto alternative = safeAlternativeSite(
            state, plan.expansionTarget, failedSites_, retryAfter_);
        if (alternative.valid()) {
            const auto prior = plan.expansionTarget;
            plan.expansionTarget = alternative;
            committedSite_ = alternative;
            if (distanceSquared(plan.rallyPoint, prior) <= 96 * 96)
                plan.rallyPoint = alternative;
            plan.deferExpansion = false;
            unsafeRouteSince_ = -1;
            unsafeRouteSite_ = {-1, -1};
            plan.name += " [alternate safe expansion]";
            releaseBuilder_ = releaseBuilder_ || feedback.pending;
            reason_ = "Switch to reachable, mineral-rich alternative expansion";
            return;
        }

        plan.deferExpansion = true;
        committedSite_ = {-1, -1};
        unsafeRouteSince_ = -1;
        unsafeRouteSite_ = {-1, -1};
        addWorkerSurvivalPlan(plan, state);
        releaseBuilder_ = releaseBuilder_ || feedback.pending;
        reason_ = plan.name.find("survival: expansion blocked") != std::string::npos
            ? "No safe alternative; preserve the remaining worker income"
            : blockedReason.empty() ? "Expansion blocked; hold current mining lines and retry"
                                    : blockedReason;
        return;
    }

    if (expansionRequested && !plan.deferExpansion && plan.expansionTarget.valid())
        committedSite_ = plan.expansionTarget;
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
    const GameState& state, const Position site, const Position assembly,
    const bool constructionPending) {
    std::vector<Command> result;
    if (!constructionPending || !site.valid() || !assembly.valid()) return result;
    if (std::ranges::any_of(state.self.units, [site](const UnitSnapshot& unit) {
        return unit.kind == UnitKind::nexus && distanceSquared(site, unit.position) < 96 * 96;
    })) return result;
    for (const auto& unit : state.self.units) {
        if (!unit.completed || unit.loaded || unit.flying || unit.disabled || unit.attackFrame || unit.attackWindup ||
            unit.underAttack || !isCombatUnit(unit.kind) || isBuilding(unit.kind) ||
            !unit.position.valid() || std::abs(unit.position.x - site.x) > 104 ||
            std::abs(unit.position.y - site.y) > 88 ||
            distanceSquared(unit.position, assembly) <= 48 * 48) continue;
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
