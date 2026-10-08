#include "protodd/Strategy.hpp"
#include "protodd/Technology.hpp"
#include "protodd/Combat.hpp"
#include "protodd/Harassment.hpp"
#include "protodd/LocalThreatQueries.hpp"

#include "protodd/ProductionReadiness.hpp"
#include "protodd/UnitCatalog.hpp"
#include "StrategyDetail.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>

namespace protodd {
using namespace strategy_detail;

int criticalDeadlinePriorityAdjustment(
    const CriticalGoalTiming& timing, const Frame currentFrame) noexcept {
    constexpr Frame urgencyHorizon = framesForSeconds(90);
    if (!timing.active() || !timing.feasible || timing.slackFrames < 0 ||
        timing.requiredByFrame < currentFrame || timing.slackFrames >= urgencyHorizon) {
        return 0;
    }
    return static_cast<int>(20 * (urgencyHorizon - timing.slackFrames) /
                            urgencyHorizon);
}

CriticalGoalTiming assessCriticalGoalTiming(
    const GameState& state,
    const ProductionGoal& goal,
    const Frame requiredByFrame,
    const CriticalReservationReason reason) {
    CriticalGoalTiming timing;
    if (reason == CriticalReservationReason::none || requiredByFrame < 0) return timing;
    timing.requiredByFrame = requiredByFrame;
    timing.reservationReason = reason;
    timing.expectedReadyFrame = estimatedGoalReadyFrame(state, goal);
    if (timing.expectedReadyFrame < 0) return timing;
    timing.slackFrames = requiredByFrame - timing.expectedReadyFrame;

    auto needsBuilder = goal.goal == GoalKind::build || goal.goal == GoalKind::expand;
    if (goal.goal == GoalKind::train) {
        needsBuilder = std::ranges::any_of(unitPrerequisites(goal.target),
            [&state](const UnitKind prerequisite) {
                return effectiveUnitCount(state, prerequisite, UnitCountBasis::completed) == 0;
            });
    } else if (goal.technology != TechnologyKind::none) {
        const auto& technology = technologyStats(goal.technology);
        const auto requirement = technologyPrerequisite(
            goal.technology, technologyLevel(state.self, goal.technology) + 1);
        needsBuilder = effectiveUnitCount(state, technology.producer, UnitCountBasis::completed) == 0 ||
            (requirement != UnitKind::unknown && effectiveUnitCount(state, requirement, UnitCountBasis::completed) == 0);
    }
    const auto builderAvailable = !needsBuilder || effectiveUnitCount(state, UnitKind::probe, UnitCountBasis::completed) > 0;
    timing.feasible = requiredByFrame >= state.frame && builderAvailable &&
                      timing.expectedReadyFrame <= requiredByFrame;
    return timing;
}

bool StrategyEngine::coveredPressureRelease(
    const GameState& state, const StrategicPlan& plan) noexcept {
    if (state.enemy.race != Race::terran || state.frame < framesForSeconds(400) ||
        plan.posture != Posture::pressure || plan.prioritizeReinforcements ||
        !plan.attackTarget.valid() || effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::completed) < 2 ||
        effectiveUnitCount(state, UnitKind::probe, UnitCountBasis::completed) < 20) return false;
    const auto army = std::ranges::count_if(state.self.units, [](const UnitSnapshot& unit) {
        return unit.completed && !unit.disabled && !unit.loaded &&
            !unit.hallucination && !isBuilding(unit.kind) &&
            !isWorker(unit.kind) && isCombatUnit(unit.kind);
    });
    // A small army can win the immediate perimeter estimate yet still feed
    // unseen tanks as it advances. The first 14-unit release lost its repeat
    // screen; this variant waits for a substantial connected field force.
    if (army < 30) return false;
    if (std::ranges::any_of(state.self.units, [](const UnitSnapshot& unit) {
            return isWorker(unit.kind) && unit.underAttack;
        })) return false;

    auto nearby = 0;
    for (const auto& enemy : state.enemy.units) {
        if (!enemy.visible || !enemy.completed || enemy.hallucination ||
            !enemy.position.valid() || !isCombatUnit(enemy.kind)) continue;
        for (const auto& base : state.bases) {
            if (base.ownerId != state.self.id || !base.center.valid()) continue;
            const auto separation = distanceSquared(enemy.position, base.center);
            // Immediate contact still belongs to the emergency defense.
            if (separation <= PixelRadius{320}.squared()) return false;
            if (separation <= PixelRadius{640}.squared()) { ++nearby; break; }
        }
    }
    // Keep local base-defense allocation for perimeter contact, but do not
    // let a few Vultures hold a near-max army at its rally indefinitely.
    return nearby <= 8 && army >= nearby * 5;
}

Position StrategyEngine::pvTContainBreakTarget(
    const GameState& state, const StrategicPlan& plan) noexcept {
    if (state.enemy.race != Race::terran || state.frame < framesForMinutes(12) ||
        plan.posture != Posture::pressure || !plan.attackTarget.valid()) return {-1, -1};
    auto bases = 0;
    for (const auto& base : state.bases) {
        if (base.ownerId != state.self.id || !base.center.valid()) continue;
        ++bases;
        for (const auto& enemy : state.enemy.units) {
            if (!enemy.visible || !enemy.completed || enemy.disabled ||
                enemy.hallucination || !enemy.position.valid() ||
                !isCombatUnit(enemy.kind) ||
                outsidePixelRadius(enemy.position, base.center, PixelRadius{320})) continue;
            return {-1, -1};
        }
    }
    if (bases < 2 || std::ranges::any_of(state.self.units, [](const UnitSnapshot& unit) {
            return isWorker(unit.kind) && unit.underAttack;
        })) return {-1, -1};

    std::vector<const UnitSnapshot*> army;
    for (const auto& unit : state.self.units) {
        if (unit.completed && !unit.disabled && !unit.loaded &&
            !unit.hallucination && unit.position.valid() &&
            !isBuilding(unit.kind) && !isWorker(unit.kind) &&
            isCombatUnit(unit.kind) && !unit.flying) army.push_back(&unit);
    }
    if (army.size() < 16) return {-1, -1};
    // A connected field force must own the push. Supply scattered among home
    // guards, air units, and reinforcements is not a breakout opportunity.
    const UnitSnapshot* anchor = nullptr;
    auto nearbyMax = std::size_t{0};
    for (const auto* candidate : army) {
        const auto nearby = static_cast<std::size_t>(std::ranges::count_if(army, [candidate](const UnitSnapshot* unit) {
            return withinPixelRadius(unit->position, candidate->position, PixelRadius{640});
        }));
        if (nearby > nearbyMax) { nearbyMax = nearby; anchor = candidate; }
    }
    if (anchor == nullptr || nearbyMax < 16) return {-1, -1};
    const auto detectorReady = std::ranges::any_of(state.self.units, [anchor](const UnitSnapshot& unit) {
        return unit.kind == UnitKind::observer && unit.completed && !unit.disabled &&
            !unit.loaded && !unit.hallucination && unit.healthFraction() >= 0.25 &&
            unit.position.valid() &&
            withinPixelRadius(unit.position, anchor->position, PixelRadius{512});
    });
    if (!detectorReady) return {-1, -1};

    auto fieldPower = 0.0;
    auto opposition = 0.0;
    for (const auto* unit : army) {
        if (withinPixelRadius(unit->position, anchor->position, PixelRadius{640}))
            fieldPower += unitStats(unit->kind).combatValue *
                std::clamp(unit->healthFraction(), 0.15, 1.0);
    }
    for (const auto& enemy : state.enemy.units) {
        if (!enemy.completed || enemy.disabled || enemy.hallucination ||
            !enemy.position.valid() || !isCombatUnit(enemy.kind) ||
            (!enemy.visible && state.frame - enemy.lastSeen > framesForSeconds(8)) ||
            outsidePixelRadius(enemy.position, anchor->position, PixelRadius{960})) continue;
        opposition += unitStats(enemy.kind).combatValue *
            std::clamp(enemy.healthFraction(), 0.15, 1.0);
    }
    if (fieldPower < std::max(4.0, opposition) * 1.6) return {-1, -1};

    auto best = Position{-1, -1};
    auto bestScore = std::numeric_limits<double>::infinity();
    auto knownDepots = 0;
    for (const auto& depot : state.enemy.units) {
        if (depot.role != UnitRole::resourceDepot || depot.hallucination ||
            !depot.position.valid()) continue;
        ++knownDepots;
        // An unfinished outer Command Center is the best timing to interrupt
        // Terran's map control; it need not finish before becoming a target.
        auto score = distance(anchor->position, depot.position) -
                     (depot.completed ? 0.0 : 256.0);
        for (const auto& defender : state.enemy.units) {
            if (!defender.completed || defender.disabled || !defender.position.valid() ||
                defender.groundWeapon.damage <= 0 ||
                (!defender.visible && !isBuilding(defender.kind) &&
                 state.frame - defender.lastSeen > framesForSeconds(30)) ||
                outsidePixelRadius(defender.position, depot.position, PixelRadius{800})) continue;
            score += unitStats(defender.kind).combatValue * 128.0;
        }
        if (score < bestScore) { bestScore = score; best = depot.position; }
    }
    // A lone remembered main is not a breakout destination through an
    // established minefield. Wait until scouting finds an outer economy.
    return knownDepots >= 2 ? best : Position{-1, -1};
}

StrategicPlan StrategyEngine::plan(
    const GameState& state,
    const ThreatAssessment& threat,
    const OpeningStyle style) const {
    StrategicPlan result;
    switch (state.enemy.race) {
        case Race::terran: result = planPvT(state, threat); break;
        case Race::zerg: result = planPvZ(state, threat); break;
        case Race::protoss: result = planPvP(state, threat); break;
        case Race::unknown:
        case Race::random:
            result = planPvP(state, threat);
            result.name = "Safe one-gate core versus unknown";
            break;
    }

    const auto home = ourMain(state);
    const auto mapAttackTarget = enemyMain(state);
    // Matchup planners can choose a nearer perimeter rally. Preserve that
    // decision; only fill in the map-level defaults when no specialized point
    // was requested. Previously this unconditional assignment erased every
    // PvP forward-intercept rally before combat could use it.
    if (!result.attackTarget.valid()) result.attackTarget = mapAttackTarget;
    if (!result.rallyPoint.valid()) {
        result.rallyPoint = home.valid() && result.attackTarget.valid()
                                ? moveToward(home, result.attackTarget, 160.0)
                                : home;
    }
    applyOpeningStyle(result, state, style);
    addAdaptiveCounters(result, state);
    addEconomicRecovery(result, state, lateEconomyRecovery_);
    // Safety runs last so an opponent-specific economic style cannot override
    // direct evidence of an all-in at our main.
    addSafetyReactions(result, threat);

    const auto visibleGroundContact = hasVisibleGroundCombatContact(
        state, home.valid() ? std::optional<Position>{home} : std::nullopt,
        PixelRadius{800});
    const auto visibleProtossContact = state.enemy.race == Race::protoss &&
        std::ranges::any_of(state.enemy.units, [](const UnitSnapshot& enemy) {
            return enemy.visible && enemy.completed && !enemy.flying &&
                   isCombatUnit(enemy.kind) && enemy.position.valid();
        });
    const auto forwardCounter = visibleProtossContact &&
        state.frame >= framesForMinutes(7) && state.frame < framesForMinutes(11) &&
        effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed) >= 6 &&
        effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed) >= 2 &&
        threat.combatEnemiesNearMain == 0 &&
        !visibleGroundContact &&
        !hardBreachAtMain(state) &&
        result.posture != Posture::recover &&
        threat.workerRush <= 0.30;
    if (forwardCounter) {
        // The safety pass may have converted a perimeter sighting back to
        // Defend. If the army is still outside the hard-breach radius, keep
        // the mobile screen fighting in the midfield instead of donating it
        // to a mineral-line surround.
        result.posture = Posture::pressure;
        result.attackThreshold = std::min(result.attackThreshold, 1.30);
        result.minimumAttackSize = std::min(result.minimumAttackSize, 8);
        if (mapAttackTarget.valid()) {
            result.attackTarget = mapAttackTarget;
            result.rallyPoint = mapAttackTarget;
        }
        result.name += " [post-safety forward counter]";
    }

    addPostPressureTransition(result, state, threat);
    addMapControlEconomy(result, state, threat);

    // A later PvZ base can be requested again by the generic economy pass.
    // Hold that request while the two-base ground screen is thin, and turn a
    // large mineral surplus into defenders before financing more Probes.
    if (pvzArmyFloor_ && state.enemy.race == Race::zerg &&
        state.frame >= framesForSeconds(400) && state.frame < framesForSeconds(1100) &&
        effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::completed) >= 2 &&
        effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::completed) >= 1 &&
        effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::completed) >= 2 &&
        effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::completed) >= 2 &&
        effectiveUnitCount(state, UnitKind::probe, UnitCountBasis::completed) >= 26 &&
        threat.air <= 0.42 && recentEnemyCount(state, UnitKind::mutalisk) == 0) {
        const auto workers = effectiveUnitCount(state, UnitKind::probe, UnitCountBasis::observed);
        const auto groundArmy = static_cast<int>(std::ranges::count_if(
            state.self.units, [](const UnitSnapshot& unit) {
                return unit.completed && !unit.disabled && !unit.loaded &&
                       !unit.hallucination && !unit.flying &&
                       !isBuilding(unit.kind) && !isWorker(unit.kind) &&
                       isCombatUnit(unit.kind);
            }));
        const auto floor = std::clamp(workers / 3, 12, 22);
        if (groundArmy < floor) {
            const auto committedBases = effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed);
            const auto baseCap = std::max(2, committedBases);
            result.desiredBases = std::min(result.desiredBases, baseCap);
            result.maximumBases = std::min(result.maximumBases, baseCap);
            result.desiredWorkers = std::min(result.desiredWorkers,
                                             std::max(workers, 44));
            result.name += " [two-base ground army floor]";
            for (auto& objective : result.goals) {
                if (objective.goal == GoalKind::expand &&
                    objective.target == UnitKind::nexus)
                    objective.desiredCount = std::min(objective.desiredCount, baseCap);
            }
            if (state.self.minerals >= 600) {
                goal(result, GoalKind::train, UnitKind::zealot,
                     effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::observed) + std::min(4, floor - groundArmy),
                     103, "spend two-base mineral surplus on ground defenders", true);
            }
        }
    }

    // The generic saturated-economy pass can reintroduce a Nexus after the
    // PvZ timing is chosen. That immediately changes the squad mission to
    // cover-expansion and recalls the force. Reconcile the short timing here,
    // after all generic economy styles have made their requests.
    if (pvzGatewayOpening_ && state.enemy.race == Race::zerg &&
        pvzEarlyPressureEligible(state, threat) &&
        result.posture == Posture::pressure && result.attackTarget.valid()) {
        result.desiredBases = 1;
        result.expansionTarget = {-1, -1};
        result.sustainEconomy = false;
        for (auto& objective : result.goals) {
            if (objective.goal != GoalKind::expand ||
                objective.target != UnitKind::nexus) continue;
            objective.desiredCount = 1;
            objective.blocking = false;
            objective.reason = "finish the six-Zealot pressure before the natural";
        }
    }

    const auto safeToClose = threat.combatEnemiesNearMain == 0 &&
                             threat.immediateGround <= 0.45 &&
                             !hardBreachAtMain(state);
    auto ownMobilePower = 0.0;
    auto ownMobileCount = 0;
    for (const auto& unit : state.self.units) {
        if (!unit.completed || unit.disabled || unit.loaded || unit.hallucination ||
            isBuilding(unit.kind) || isWorker(unit.kind) || !isCombatUnit(unit.kind)) continue;
        ownMobilePower += unitStats(unit.kind).combatValue *
                          std::clamp(unit.healthFraction(), 0.15, 1.0);
        ++ownMobileCount;
    }
    auto recentEnemyMobilePower = 0.0;
    auto knownEnemyWorkers = 0;
    for (const auto& unit : state.enemy.units) {
        if (!unit.completed || unit.disabled || unit.hallucination) continue;
        if (isWorker(unit.kind)) {
            ++knownEnemyWorkers;
            continue;
        }
        if (isBuilding(unit.kind) || !isCombatUnit(unit.kind) ||
            (!unit.visible && state.frame - unit.lastSeen > framesForSeconds(60))) continue;
        recentEnemyMobilePower += unitStats(unit.kind).combatValue *
                                  std::clamp(unit.healthFraction(), 0.15, 1.0);
    }
    const auto ownedBases = static_cast<int>(std::ranges::count_if(
        state.bases, [&state](const BaseSnapshot& base) {
            return base.ownerId == state.self.id;
        }));
    const auto knownEnemyBases = static_cast<int>(std::ranges::count_if(
        state.bases, [&state](const BaseSnapshot& base) {
            return state.enemy.id >= 0 && base.ownerId == state.enemy.id;
        }));
    const auto decisiveLeadCloseout = safeToClose && state.self.supplyUsed >= 220 &&
        ownMobileCount >= 20 &&
        ((ownedBases >= knownEnemyBases + 2 &&
          ownMobilePower >= std::max(12.0, recentEnemyMobilePower) * 1.65) ||
         (knownEnemyBases <= 1 && knownEnemyWorkers <= 12 &&
          ownMobilePower >= std::max(12.0, recentEnemyMobilePower) * 1.35));

    // A maxed army is already the largest possible economic conversion. Do
    // not park it beside another speculative Nexus while the opponent rebuilds
    // from a collapsed position. Keep a small margin below the hard cap so one
    // lost Probe or Observer cannot immediately reverse the map-level order.
    const auto supplyCapCloseout = state.self.supplyTotal >= 400 &&
                                   state.self.supplyUsed >= 392 &&
                                   safeToClose;
    const auto commitCloseout = [&](const std::string_view reason) {
        const auto target = enemyMain(state);
        if (target.valid()) {
            result.name += " [";
            result.name += reason;
            result.name += ']';
            result.posture = Posture::attack;
            result.attackThreshold = std::min(result.attackThreshold, 1.10);
            result.minimumAttackSize = std::min(result.minimumAttackSize, 8);
            result.attackTarget = target;
            result.rallyPoint = target;
            result.expansionTarget = {-1, -1};
            result.sustainEconomy = false;
            const auto committedBases = effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed);
            result.desiredBases = std::min(result.desiredBases, committedBases);
            result.maximumBases = std::min(result.maximumBases, committedBases);
            for (auto& objective : result.goals) {
                if (objective.goal != GoalKind::expand) continue;
                objective.desiredCount = committedBases;
                objective.blocking = false;
                objective.reason = "convert the army lead before further expansion";
            }
        }
    };
    if (supplyCapCloseout) {
        commitCloseout("supply-cap closeout");
    } else if (decisiveLeadCloseout) {
        commitCloseout("decisive-lead closeout");
    }
    result.desiredBases = std::min(result.desiredBases, result.maximumBases);

    // Every planner can request economic growth. Resolve that request to a
    // concrete natural before infrastructure reconciliation so the expansion
    // reservation, builder routing, placement, and cover all share one
    // location.
    const auto existingNexuses = effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed);
    const auto continueCoveredNatural = pvpCoveredRangedNaturalActive_ &&
        state.frame >= pvpCoveredRangedNaturalLastFrame_;
    auto rangedNaturalWindow = pvpCoveredRangedNatural_ && coveredRangedNatural(
        state, threat, continueCoveredNatural ? 1.10 : 1.50);
    const auto rangedMirrorExpansion = state.enemy.race == Race::protoss &&
        (std::ranges::any_of(state.enemy.units, [](const UnitSnapshot& enemy) {
             return enemy.kind == UnitKind::cyberneticsCore && enemy.position.valid();
         }) ||
         recentEnemyCount(state, UnitKind::dragoon) > 0 ||
         recentEnemyCount(state, UnitKind::reaver) > 0);
    const auto establishedRangedLead = effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed) >= 8 &&
        effectiveUnitCount(state, UnitKind::observer, UnitCountBasis::completed) > 0 && recentEnemyMobilePower > 0.0 &&
        ownMobilePower >= recentEnemyMobilePower * 3.0;
    const auto splashScreenTarget = recentEnemyCount(state, UnitKind::dragoon) >= 5 ||
        recentEnemyCount(state, UnitKind::reaver) > 0 ? 2 : 1;
    if (existingNexuses == 1 && result.desiredBases > 1 && rangedMirrorExpansion &&
        effectiveUnitCount(state, UnitKind::reaver, UnitCountBasis::completed) < splashScreenTarget &&
        !establishedRangedLead && !rangedNaturalWindow && minute(state) < 12) {
        // A six-Dragoon screen is not yet the planned combined army. Buying
        // the natural first outranks Robotics and moves those Dragoons away
        // from home while the first Reaver is still several production steps
        // away. Finish that defensive checkpoint before committing the Nexus.
        result.desiredBases = 1;
        result.expansionTarget = {-1, -1};
        result.sustainEconomy = false;
        result.breakContainment = false;
        result.posture = Posture::hold;
        result.rallyPoint = home;
        result.name += " [splash before natural]";
        if (effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed) >= 4) {
            goal(result, GoalKind::build, UnitKind::roboticsFacility, 1, 113,
                 "splash screen before the natural", true);
            goal(result, GoalKind::build, UnitKind::roboticsSupportBay, 1, 113,
                 "complete splash support before the natural", true);
            goal(result, GoalKind::train, UnitKind::reaver, splashScreenTarget, 114,
                 "complete the splash screen before expansion", true);
        }
    }
    if (existingNexuses == 1 && result.desiredBases > 1) {
        // The first expansion is positional infrastructure, not a greed score.
        // Always take the nearest resource base so matchup-specific danger or
        // richness scoring cannot skip the natural for a third/fourth location.
        result.expansionTarget = nearestExpansionSite(state);
    }
    auto exposedEconomy = false;
    for (const auto& base : state.bases) {
        if (base.ownerId != state.self.id) continue;
        auto attackers = 0.0;
        auto defenders = 0.0;
        auto breached = false;
        for (const auto& enemy : state.enemy.units) {
            if (!enemy.visible || !enemy.completed || enemy.disabled || enemy.hallucination ||
                !enemy.position.valid() || enemy.groundWeapon.damage <= 0 || isWorker(enemy.kind) ||
                outsidePixelRadius(enemy.position, base.center, PixelRadius{640})) continue;
            attackers += unitStats(enemy.kind).combatValue * std::clamp(enemy.healthFraction(), 0.25, 1.0);
            breached = breached || withinPixelRadius(enemy.position, base.center, PixelRadius{320});
        }
        if (attackers <= 0.0) continue;
        for (const auto& ally : state.self.units) {
            if (!ally.completed || ally.disabled || ally.loaded || ally.hallucination ||
                (!isCombatUnit(ally.kind) && !isStaticDefense(ally.kind)) ||
                outsidePixelRadius(ally.position, base.center, PixelRadius{800})) continue;
            defenders += unitStats(ally.kind).combatValue * std::clamp(ally.healthFraction(), 0.25, 1.0);
        }
        exposedEconomy = exposedEconomy || breached || attackers >= std::max(1.0, defenders * 0.65);
    }
    if (exposedEconomy) {
        // An attack on the natural is an economic emergency too. The main-only
        // threat classifier used to keep banking another Nexus and training
        // Probes at every base while the mobile army defending it collapsed.
        result.desiredBases = std::min(result.desiredBases, existingNexuses);
        result.expansionTarget = {-1, -1};
        result.sustainEconomy = false;
        result.desiredWorkers = std::min(result.desiredWorkers, std::max(12, effectiveUnitCount(state, UnitKind::probe, UnitCountBasis::observed)));
        result.name += " [reinforce threatened economy]";
    }
    rangedNaturalWindow = rangedNaturalWindow && !exposedEconomy;
    pvpCoveredRangedNaturalActive_ = rangedNaturalWindow;
    pvpCoveredRangedNaturalLastFrame_ = state.frame;
    if (rangedNaturalWindow) {
        // A covered pressure wave is not an all-in. Grow behind the field
        // screen rather than paying for a Cannon shell and two completed
        // Reavers on a saturated one-base economy.
        result.desiredBases = 2;
        result.maximumBases = std::max(2, result.maximumBases);
        result.expansionTarget = nearestExpansionSite(state);
        result.rallyPoint = result.expansionTarget;
        result.posture = Posture::hold;
        result.sustainEconomy = true;
        result.breakContainment = false;
        result.name += " [covered ranged natural]";
    }
    // Start saving while income still exists. Matchup army checkpoints and
    // stale perimeter pressure must not strand a surviving force on an empty
    // main after losing its natural. A real breach still cancels this bank.
    const auto ownedMinerals = std::accumulate(state.bases.begin(), state.bases.end(), 0,
        [&state](int total, const BaseSnapshot& base) {
            return total + (base.ownerId == state.self.id ? base.mineralsRemaining : 0);
        });
    const auto miningSite = nearestExpansionSite(state);
    const auto pendingNexus = existingNexuses > effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::completed);
    const auto preserveMining = state.frame >= framesForMinutes(10) && existingNexuses > 0 &&
        !supplyCapCloseout && !decisiveLeadCloseout &&
        existingNexuses < 8 && !pendingNexus && ownedMinerals < 4500 &&
        effectiveUnitCount(state, UnitKind::probe, UnitCountBasis::completed) >= 12 && ownMobileCount >= 8 &&
        miningSite.valid() && !exposedEconomy && !hardBreachAtMain(state) &&
        threat.workerRush <= 0.30 && threat.proxy + threat.staticContain <= 0.34 &&
        !std::ranges::any_of(state.self.units, [](const UnitSnapshot& unit) {
            return isWorker(unit.kind) && unit.underAttack;
        });
    if (preserveMining) {
        result.desiredBases = existingNexuses + 1;
        result.maximumBases = std::max(result.maximumBases, result.desiredBases);
        result.expansionTarget = miningSite;
        result.sustainEconomy = true;
        result.name += " [protect remaining mining income]";
        goal(result, GoalKind::expand, UnitKind::nexus, result.desiredBases, 120,
             "fund replacement mining before depletion", true);
    }
    if (!result.expansionTarget.valid()) {
        for (const auto& nexus : state.self.units) {
            if (nexus.kind == UnitKind::nexus && !nexus.completed &&
                nexus.position.valid()) {
                result.expansionTarget = nexus.position;
                break;
            }
        }
    }
    if (!result.expansionTarget.valid() &&
        result.desiredBases > existingNexuses) {
        result.expansionTarget = nearestExpansionSite(state);
    }

    // Infrastructure must follow the final intent: a safety reaction or an
    // opening style can change the economy after the matchup plan is made.
    if (result.posture == Posture::defend && !result.sustainEconomy &&
        !defensiveExpansionWindow(state, threat)) {
        const auto existingBases = std::max(1, effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed));
        result.desiredBases = std::min(result.desiredBases, existingBases);
    }
    // Grow into a paid-for Nexus while it warps in, not into an expansion
    // that is merely desired. Otherwise repeated hold/pressure transitions
    // train two bases' workers on one mineral line and consume its Nexus bank.
    const auto committedBases = effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed);
    result.desiredWorkers = std::min(
        result.desiredWorkers, std::max(14, committedBases * 22));
    for (auto& objective : result.goals) {
        if (objective.goal == GoalKind::expand) {
            objective.desiredCount = std::min(objective.desiredCount, result.desiredBases);
        } else if (objective.target == UnitKind::probe) {
            objective.desiredCount = std::min(objective.desiredCount, result.desiredWorkers);
        }
    }
    addInfrastructure(result, state, threat);

    result.prioritizeReinforcements = exposedEconomy || hardBreachAtMain(state) ||
        threat.combatEnemiesNearMain > 0 || activeApproach(state, threat) ||
        threat.immediateGround > 0.45 || threat.workerRush > 0.30 ||
        threat.proxy + threat.staticContain > 0.34;
    if (result.sustainEconomy) result.prioritizeReinforcements = false;
    // A two-Gateway Core opening showing only a token army can hide DT tech.
    // One observed Dragoon does not rule it out: in the Venator loss the
    // quiet opening waited for six completed Dragoons, then first saw a DT
    // with Robotics still unfinished. The remaining detector chain took
    // longer than the defending army survived. Buy one insurance Observer
    // after the initial ranged screen, using only our scouting evidence.
    // Failed scouting must not be treated as evidence that tech is absent.
    // In the archived RL Venator loss we had two Dragoons at6326 but had
    // scouted only a Pylon/Zealot; the first DT arrived8099, Observer9878.
    // Buy the same single detector in a quiet, poorly scouted mirror after
    // establishing a ranged screen and economy. Visible pressure still wins.
    const auto unscoutedMirrorTech = threat.uncertainty > 0.65 &&
        effectiveUnitCount(state, UnitKind::probe, UnitCountBasis::completed) >= 18 &&
        std::ranges::count(state.enemy.units, UnitKind::cyberneticsCore, &UnitSnapshot::kind) == 0;
    const auto scoutedMirrorTech =
        std::ranges::count(state.enemy.units, UnitKind::gateway, &UnitSnapshot::kind) >= 2 &&
        std::ranges::count(state.enemy.units, UnitKind::cyberneticsCore, &UnitSnapshot::kind) > 0;
    const auto mirrorTechGap = state.enemy.race == Race::protoss &&
        state.frame >= framesForMinutes(4) + framesForSeconds(12) && minute(state) < 8 &&
        effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::completed) > 0 &&
        effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed) >= 2 &&
        (scoutedMirrorTech || unscoutedMirrorTech) &&
        recentEnemyCount(state, UnitKind::dragoon) <= 1 &&
        recentEnemyCount(state, UnitKind::zealot) <= 1 &&
        !hardBreachAtMain(state) && threat.combatEnemiesNearMain == 0 &&
        !activeApproach(state, threat) && threat.immediateGround <= 0.45 &&
        threat.workerRush <= 0.30 && threat.proxy + threat.staticContain <= 0.34 &&
        threat.mostLikely != EnemyPlan::fastRush;
    // A one-Gateway Core with no observed army can reach DTs sooner than the
    // two-Gateway tech-gap window. Start one insurance detector after paying
    // the first ranged defender, rather than treating detection as optional scouting.
    const auto singleGatewayTechGap = state.enemy.race == Race::protoss &&
        state.frame >= framesForMinutes(3) + framesForSeconds(30) && minute(state) < 8 &&
        std::ranges::count(state.enemy.units, UnitKind::gateway, &UnitSnapshot::kind) == 1 &&
        std::ranges::count(state.enemy.units, UnitKind::cyberneticsCore, &UnitSnapshot::kind) > 0 &&
        effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::completed) > 0 &&
        effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::observed) >= 1 && effectiveUnitCount(state, UnitKind::probe, UnitCountBasis::completed) >= 18 &&
        recentEnemyCount(state, UnitKind::dragoon) == 0 &&
        recentEnemyCount(state, UnitKind::zealot) == 0 &&
        !hardBreachAtMain(state) && threat.combatEnemiesNearMain == 0 &&
        !activeApproach(state, threat) && threat.immediateGround <= 0.45 &&
        threat.workerRush <= 0.30 && threat.proxy + threat.staticContain <= 0.34 &&
        threat.mostLikely != EnemyPlan::fastRush;
    // A scouted two-Gateway army can still conceal Dark Templar when its tech
    // has not been revisited. Once Robotics and a home Cannon are present,
    // finish one mobile detector before spending the next gas on splash.
    // Keep this experiment behind a build option until matched games show
    // that its earlier Observatory repays the delayed Reaver.
    const auto twoGatewayFogCloakRisk = pvpFogDetection_ &&
        state.enemy.race == Race::protoss &&
        state.frame >= framesForMinutes(5) && minute(state) < 11 &&
        effectiveUnitCount(state, UnitKind::roboticsFacility, UnitCountBasis::completed) > 0 &&
        effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::completed) > 0 &&
        effectiveUnitCount(state, UnitKind::observer, UnitCountBasis::observed) == 0 &&
        std::ranges::count(state.self.queuedUnits, UnitKind::observer) == 0 &&
        std::ranges::count(state.enemy.units, UnitKind::gateway,
                           &UnitSnapshot::kind) >= 2 &&
        threat.uncertainty >= 0.75 &&
        effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed) +
            effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed) +
            effectiveUnitCount(state, UnitKind::reaver, UnitCountBasis::completed) >= 6 &&
        !hardBreachAtMain(state);
    result.requireMobileDetection = threat.cloak > 0.28 ||
        recentEnemyCount(state, UnitKind::spiderMine) > 0 ||
        recentEnemyCount(state, UnitKind::lurker) > 0 ||
        recentEnemyCount(state, UnitKind::darkTemplar) > 0 ||
        (state.enemy.race == Race::terran && minute(state) >= 5 &&
         ((threat.uncertainty > 0.65 && threat.combatEnemiesNearMain == 0 &&
           !activeApproach(state, threat) && threat.immediateGround <= 0.45) ||
          recentEnemyCount(state, UnitKind::factory) >= 2 ||
          recentEnemyCount(state, UnitKind::starport) > 0));
    const auto insuranceDetectorOnly =
        (mirrorTechGap || singleGatewayTechGap || twoGatewayFogCloakRisk) && !result.requireMobileDetection;
    result.requireMobileDetection = result.requireMobileDetection ||
        mirrorTechGap || singleGatewayTechGap || twoGatewayFogCloakRisk;
    if (result.requireMobileDetection) {
        result.desiredGasWorkers = std::max(3, result.desiredGasWorkers);
        goal(result, GoalKind::build, UnitKind::assimilator, 1, 123,
             "fund required mobile detection", true);
        goal(result, GoalKind::train, UnitKind::observer,
             insuranceDetectorOnly ? 1 : (effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed) >= 2 ? 3 : 2), 124,
             insuranceDetectorOnly ? "first Observer against an unscouted mirror tech path"
                                   : "replace and maintain mission detectors", true);
    }

    if (state.enemy.race == Race::protoss && effectiveUnitCount(state, UnitKind::reaver, UnitCountBasis::completed) < 2)
        std::erase_if(result.goals, [](const ProductionGoal& demand) {
            return demand.goal == GoalKind::train && demand.target == UnitKind::shuttle;
        });
    const auto rangedMirror = state.enemy.race == Race::protoss && minute(state) < 12 &&
        recentEnemyCount(state, UnitKind::dragoon) >= 3 &&
        recentEnemyCount(state, UnitKind::dragoon) >= 2 * recentEnemyCount(state, UnitKind::zealot);
    if (rangedMirror) {
        const auto meleeLimit = std::max(2, effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed) / 3);
        result.composition = {{UnitKind::dragoon, 0.85}, {UnitKind::zealot, 0.05}, {UnitKind::reaver, 0.10}};
        for (auto& demand : result.goals) {
            if (demand.goal == GoalKind::train && demand.target == UnitKind::zealot)
                demand.desiredCount = std::min(demand.desiredCount, meleeLimit);
        }
        if (!result.sustainEconomy && effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed) < 6 &&
            !hardBreachAtMain(state)) {
            result.posture = Posture::hold;
            result.minimumAttackSize = std::max(6, result.minimumAttackSize);
        }
    }
    // Reconcile all independent safety rules against the final observation.
    // Explicit fulfilled goals also clear MacroPlanner's older commitments;
    // simply erasing a demand lets yesterday's Cannon reservation survive.
    const auto suppressNew = [&result, &state](const UnitKind kind, const char* reason) {
        const auto existing = effectiveUnitCount(state, kind, UnitCountBasis::observed) + static_cast<int>(std::ranges::count(state.self.queuedUnits, kind));
        for (auto& demand : result.goals) {
            if (demand.target != kind || demand.goal == GoalKind::upgrade) continue;
            demand.desiredCount = existing;
            demand.blocking = false;
            demand.reason = reason;
        }
        goal(result, isBuilding(kind) ? GoalKind::build : GoalKind::train, kind, existing, 125, reason);
    };
    const auto visiblePerimeterRanged = home.valid() && std::ranges::any_of(
        state.enemy.units, [home](const UnitSnapshot& enemy) {
            return enemy.visible && enemy.completed && !enemy.flying && !isWorker(enemy.kind) &&
                enemy.position.valid() && enemy.groundWeapon.damage > 0 &&
                enemy.groundWeapon.maxRange >= 96 &&
                withinPixelRadius(home, enemy.position, PixelRadius{1200});
        });
    const auto containedByRanged = state.enemy.race == Race::protoss &&
        effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::completed) > 0 && !hardBreachAtMain(state) &&
        (visiblePerimeterRanged || threat.staticContain > 0.34) &&
        (recentEnemyCount(state, UnitKind::zealot) < 2 ||
         recentEnemyCount(state, UnitKind::dragoon) >= 2 * recentEnemyCount(state, UnitKind::zealot)) &&
        threat.workerRush <= 0.30 && threat.air <= 0.30;
    if (containedByRanged) {
        // A ranged escort does not make an undetected DT contain a ranged-only
        // fight. Keep the existing emergency static detector goals until the
        // cloaked attacker is actually covered. `detected` is the legal local
        // observation; an Observer elsewhere (or still building) is no cover.
        const auto undetectedCloakedContact = home.valid() && std::ranges::any_of(
            state.enemy.units, [home](const UnitSnapshot& enemy) {
                return enemy.visible && enemy.completed && !enemy.detected &&
                    (enemy.cloaked || enemy.burrowed || enemy.kind == UnitKind::darkTemplar) &&
                    isCombatUnit(enemy.kind) && enemy.position.valid() &&
                    withinPixelRadius(home, enemy.position, PixelRadius{1200});
            });
        if (!undetectedCloakedContact) {
            suppressNew(UnitKind::photonCannon, "break ranged containment with mobile units");
            suppressNew(UnitKind::forge, "fund the mobile breakout before more static defense");
        }
        if (!result.sustainEconomy) result.prioritizeReinforcements = true;
        result.desiredGasWorkers = std::max(3, result.desiredGasWorkers);
        goal(result, GoalKind::train, UnitKind::dragoon, std::max(6, effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::observed) + 1),
             114, "assemble a ranged breakout force", true);
        if (effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::observed) > 0)
            technologyGoal(result, TechnologyKind::singularityCharge, 1, 115,
                           "range before spending into containment", true);
        result.name += " [mobile breakout]";
    }
    if (state.enemy.race == Race::protoss && !result.requireMobileDetection &&
        effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed) < 6 && minute(state) < 10) {
        suppressNew(UnitKind::observer, "fund six Dragoons before optional scouting detection");
        suppressNew(UnitKind::observatory, "defer optional detection until the ranged screen is ready");
    }
    if (state.enemy.race == Race::protoss && effectiveUnitCount(state, UnitKind::roboticsFacility, UnitCountBasis::observed) > 0 &&
        (result.requireMobileDetection || effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed) >= 6 || minute(state) >= 10) &&
        effectiveUnitCount(state, UnitKind::observer, UnitCountBasis::observed) == 0 &&
        std::ranges::count(state.self.queuedUnits, UnitKind::observer) == 0) {
        // The first detector is a completed-screen checkpoint. Lower-priority
        // Observatory goals otherwise lose each gas deposit to splash and
        // Gateway production until a hidden DT is already killing workers.
        // A train demand also reserves its missing prerequisite at this tier.
        goal(result, GoalKind::train, UnitKind::observer, 1, 124,
             "complete the first detector before further splash and Gateway cycles", true);
    }
    if (state.enemy.race == Race::protoss && minute(state) < 6 &&
        !rangedMirrorExpansion && recentEnemyCount(state, UnitKind::gateway) >= 2 &&
        effectiveUnitCount(state, UnitKind::forge, UnitCountBasis::observed) > 0 && effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::observed) == 0 &&
        effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::observed) >= 1) {
        goal(result, GoalKind::build, UnitKind::photonCannon, 1, 122,
             "complete the first melee-rush anchor before further Gateway cycles", true);
    }
    const auto rangeCommitted = technologyLevel(state.self, TechnologyKind::singularityCharge) > 0 ||
        technologyInProgress(state.self, TechnologyKind::singularityCharge);
    if (rangedMirrorExpansion && !hardBreachAtMain(state) && rangeCommitted &&
        effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed) >= 2 && effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::observed) >= 4 &&
        effectiveUnitCount(state, UnitKind::reaver, UnitCountBasis::observed) == 0) {
        // The four committed Dragoons already own their production resources.
        // Start the splash chain while they finish instead of buying several
        // further Gateway cycles before even reserving Robotics gas.
        goal(result, GoalKind::build, UnitKind::roboticsFacility, 1, 122,
             "overlap splash technology with the committed ranged screen", true);
        if (effectiveUnitCount(state, UnitKind::roboticsFacility, UnitCountBasis::observed) > 0)
            goal(result, GoalKind::build, UnitKind::roboticsSupportBay, 1, 122,
                 "finish splash technology before further Gateway cycles", true);
    }
    const auto reaversCommitted = effectiveUnitCount(state, UnitKind::reaver, UnitCountBasis::observed) +
        static_cast<int>(std::ranges::count(state.self.queuedUnits, UnitKind::reaver));
    const auto fundedSplashTarget = effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed) >= 6 ? splashScreenTarget : 1;
    if (state.enemy.race == Race::protoss && reaversCommitted < fundedSplashTarget &&
        effectiveUnitCount(state, UnitKind::roboticsFacility, UnitCountBasis::observed) > 0 && effectiveUnitCount(state, UnitKind::roboticsSupportBay, UnitCountBasis::observed) > 0) {
        // Once the splash chain is paid for, four Gateway reinforcement goals
        // must not spend every new 50 gas before the planned Reaver screen.
        // One urgent detector still comes first; its backups can follow splash.
        for (auto& demand : result.goals) {
            if (demand.goal == GoalKind::train && demand.target == UnitKind::observer)
                demand.desiredCount = std::min(1, demand.desiredCount);
        }
        goal(result, GoalKind::train, UnitKind::reaver, fundedSplashTarget, 122,
             "complete the paid-for splash screen before further Gateway cycles", true);
    }
    if (rangedNaturalWindow) {
        suppressNew(UnitKind::forge, "fund the covered natural before optional static defense");
        suppressNew(UnitKind::photonCannon, "cover the natural with the existing ranged army");
        suppressNew(UnitKind::shieldBattery, "fund the natural before optional home sustain");
        suppressNew(UnitKind::gateway, "expand the mining economy before adding more Gateway capacity");
        for (auto& demand : result.goals) {
            if (demand.target == UnitKind::roboticsFacility ||
                demand.target == UnitKind::roboticsSupportBay || demand.target == UnitKind::reaver)
                demand.priority = std::min(demand.priority, 119);
        }
    }
    if ((result.posture == Posture::hold || result.posture == Posture::defend) &&
        !result.expansionTarget.valid() && !hardBreachAtMain(state)) {
        const auto base = std::ranges::find_if(state.bases, [&state, home](const BaseSnapshot& candidate) {
            return candidate.ownerId == state.self.id && candidate.defense.valid() &&
                   withinPixelRadius(candidate.center, home, PixelRadius{320});
        });
        if (base != state.bases.end()) result.rallyPoint = base->defense.anchor;
    }
    if (effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::completed) >= 2 && result.expansionTarget.valid() &&
        result.rallyPoint == result.expansionTarget && result.attackTarget.valid()) {
        const BaseSnapshot* front = nullptr;
        for (const auto& base : state.bases) {
            if (base.ownerId != state.self.id) continue;
            if (front == nullptr || distanceSquared(base.center, result.attackTarget) <
                                    distanceSquared(front->center, result.attackTarget)) front = &base;
        }
        if (front != nullptr)
            result.rallyPoint = front->defense.valid() ? front->defense.anchor : front->center;
    }
    const auto spellEconomy =
        effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::completed) >= 2 &&
        effectiveUnitCount(state, UnitKind::probe, UnitCountBasis::observed) >= 28;
    const auto establishedSpellArmy =
        effectiveUnitCount(state, UnitKind::reaver, UnitCountBasis::completed) >= 2 &&
        effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed) +
                effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed) >= 10;
    const auto templarTransition = state.enemy.race == Race::protoss &&
        (effectiveUnitCount(state, UnitKind::templarArchives, UnitCountBasis::observed) > 0 ||
         (spellEconomy && establishedSpellArmy));
    if (templarTransition) {
        // The economic transition replaces the opening composition. Reattach
        // its spell capability here so a two-base army does not fight forever
        // with only Gateway units and Reavers against a mature Protoss army.
        goal(result, GoalKind::build, UnitKind::citadelOfAdun, 1, 123,
             "unlock Storm for the established combined army", true);
        goal(result, GoalKind::build, UnitKind::templarArchives, 1, 123,
             "complete the combined army's spell technology", true);
        technologyGoal(result, TechnologyKind::psionicStorm, 1, 123,
                       "fund Storm before another economic expansion", true);
        goal(result, GoalKind::train, UnitKind::highTemplar, 2, 121,
             "field the first pair of army spellcasters", true);
        setCompositionWeight(result, UnitKind::highTemplar, 0.12);
        normalizeComposition(result);
    }
    addHarassmentProduction(result, state, pvzArchivesFirst_);
    if (effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed) == 0 &&
        effectiveUnitCount(state, UnitKind::probe, UnitCountBasis::completed) > 0) {
        // A dead last Nexus removes both the legal mineral drop-off and the
        // home anchor used by ordinary expansion selection. Only save for a
        // replacement when the current bank can pay for it and a neutral,
        // reachable, resourced site is not under observed ground threat.
        const auto rebuildSite = lastNexusRebuildSite(state);
        result.recoveringLastNexus = true;
        result.desiredBases = rebuildSite.valid() ? 1 : 0;
        result.maximumBases = result.desiredBases;
        result.desiredWorkers = effectiveUnitCount(state, UnitKind::probe, UnitCountBasis::observed);
        result.desiredGasWorkers = 0;
        result.expansionTarget = rebuildSite;
        result.goals.clear();
        result.composition.clear();
        result.sustainEconomy = rebuildSite.valid();
        result.prioritizeReinforcements = false;
        if (rebuildSite.valid()) {
            result.name += " [last Nexus recovery]";
            result.posture = Posture::hold;
            result.rallyPoint = rebuildSite;
            goal(result, GoalKind::expand, UnitKind::nexus, 1, 127,
                 "rebuild at a safe reachable mineral base", true);
        } else {
            result.name += " [waiting for a viable Nexus rebuild site]";
            result.posture = Posture::recover;
        }
    }
    if (threat.earliestApproachArrivalFrame >= state.frame) {
        for (auto& demand : result.goals) {
            if (detectorCheckpoint(result, demand)) {
                demand.timing = assessCriticalGoalTiming(
                    state, demand, threat.earliestApproachArrivalFrame,
                    CriticalReservationReason::detection);
            } else if (state.enemy.race == Race::protoss &&
                       demand.technology == TechnologyKind::singularityCharge) {
                demand.timing = assessCriticalGoalTiming(
                    state, demand, threat.earliestApproachArrivalFrame,
                    CriticalReservationReason::range);
            }
        }
    }
    std::ranges::stable_sort(result.goals, std::greater{}, &ProductionGoal::priority);
    return result;
}

void StrategyEngine::reset() noexcept {
    pvpCoveredRangedNaturalActive_ = false;
    pvpCoveredRangedNaturalLastFrame_ = -1;
    pvpMidfieldRally_ = {-1, -1};
    pvpMidfieldRallyLastFrame_ = -1;
}

void StrategyEngine::addPostPressureTransition(
    StrategicPlan& plan, const GameState& state, const ThreatAssessment& threat) {
    // A contained Gateway army needs a qualitative improvement before it can
    // win equal-income trades. Protect one splash-tech sequence behind an
    // existing screen instead of demanding eight units before funding it.
    if (state.enemy.race == Race::protoss && minute(state) >= 4 &&
        observedRoleCount(state, UnitRole::worker) >= 14 &&
        effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::completed) > 0 &&
        ((effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::completed) > 0 &&
          effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed) + effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed) >= 4) ||
         effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed) >= 4) &&
        !hardBreachAtMain(state)) {
        goal(plan, GoalKind::build, UnitKind::roboticsFacility, 1, 113,
             "splash transition behind the established defensive screen", true);
        if (effectiveUnitCount(state, UnitKind::roboticsFacility, UnitCountBasis::observed) > 0)
            goal(plan, GoalKind::build, UnitKind::roboticsSupportBay, 1, 113,
                 "complete the first containment-breaking splash chain", true);
        if (effectiveUnitCount(state, UnitKind::roboticsSupportBay, UnitCountBasis::observed) > 0)
            goal(plan, GoalKind::train, UnitKind::reaver, 1, 114,
                 "first splash reinforcement for the defensive army", true);
    }
    if (state.enemy.race != Race::protoss || minute(state) < 6 ||
        effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::completed) == 0 ||
        observedRoleCount(state, UnitRole::worker) < 12 ||
        effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::completed) == 0 ||
        threat.workerRush > 0.30 || threat.proxy + threat.staticContain > 0.34)
        return;

    const auto mobile = effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed) +
        effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed) + effectiveUnitCount(state, UnitKind::reaver, UnitCountBasis::completed);
    if (mobile < 6) return;
    const auto observer = effectiveUnitCount(state, UnitKind::observer, UnitCountBasis::completed) > 0;
    if ((threat.cloak > 0.28 || recentEnemyCount(state, UnitKind::darkTemplar) > 0) &&
        !observer) return;

    // Check every occupied economy. Historical rush classifications and a
    // distant enemy army do not describe the safety of our mineral lines.
    for (const auto& base : state.bases) {
        if (base.ownerId != state.self.id) continue;
        auto enemyPower = 0.0;
        auto friendlyPower = 0.0;
        std::vector<UnitSnapshot> localEnemy;
        std::vector<UnitSnapshot> localFriendly;
        for (const auto& enemy : state.enemy.units) {
            if ((!enemy.visible && (state.frame - enemy.lastSeen > framesForSeconds(8) ||
                                   state.frame < enemy.lastSeen)) ||
                !enemy.completed || enemy.disabled ||
                !enemy.position.valid() || isWorker(enemy.kind) ||
                enemy.groundWeapon.damage <= 0) continue;
            if (enemy.visible && withinPixelRadius(enemy.position, base.center, PixelRadius{320})) return;
            if (withinPixelRadius(enemy.position, base.center, PixelRadius{960})) {
                localEnemy.push_back(enemy);
                enemyPower += unitStats(enemy.kind).combatValue *
                    std::clamp(enemy.healthFraction(), 0.15, 1.0);
            }
        }
        for (const auto& friendly : state.self.units) {
            if (!friendly.completed || friendly.disabled || friendly.loaded ||
                friendly.hallucination || !friendly.position.valid() ||
                isBuilding(friendly.kind) || !isCombatUnit(friendly.kind) ||
                outsidePixelRadius(friendly.position, base.center, PixelRadius{960})) continue;
            if (enemyPower > 0.0 && std::ranges::none_of(state.enemy.units,
                [&friendly, &base](const UnitSnapshot& enemy) {
                    return enemy.visible && enemy.completed && !enemy.disabled &&
                        !isWorker(enemy.kind) && enemy.groundWeapon.damage > 0 &&
                        withinPixelRadius(enemy.position, base.center, PixelRadius{960}) &&
                        friendly.canAttack(enemy);
                })) continue;
            friendlyPower += unitStats(friendly.kind).combatValue *
                std::clamp(friendly.healthFraction(), 0.15, 1.0);
            localFriendly.push_back(friendly);
        }
        if (enemyPower > 0.0 && friendlyPower < enemyPower * 1.25) return;
        if (!localEnemy.empty()) {
            const auto localFight = CombatEvaluator{}.evaluate(
                localFriendly, localEnemy, 1.10, 0.15, true);
            if (localFight.decision != FightDecision::engage) return;
        }
    }
    if (hardBreachAtMain(state)) return;

    plan.sustainEconomy = true;
    plan.breakContainment = effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed) >= 2;
    plan.name = "PvP map control and expansion";
    plan.posture = plan.breakContainment ? Posture::pressure : Posture::hold;
    plan.minimumAttackSize = 6;
    plan.attackThreshold = 1.20;
    const auto bases = effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed);
    const auto completedBases = effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::completed);
    const auto workers = observedRoleCount(state, UnitRole::worker);
    const auto grow = bases == completedBases && workers >= bases * 16;
    plan.maximumBases = std::max(plan.maximumBases, std::min(5, bases + 1));
    plan.desiredBases = std::min(5, bases + (grow ? 1 : 0));
    plan.desiredWorkers = std::min(80, bases * 22);
    plan.desiredGasWorkers = std::min(9, bases * 3);
    // Attack the reachable economic investment with the least observed
    // protection. Always selecting the first remembered Nexus feeds the army
    // into the main while the opponent's outer bases mine uncontested.
    auto bestTargetScore = std::numeric_limits<double>::infinity();
    for (const auto& depot : state.enemy.units) {
        if (depot.role != UnitRole::resourceDepot || !depot.position.valid()) continue;
        auto score = distance(ourMain(state), depot.position);
        for (const auto& defender : state.enemy.units) {
            if (!defender.completed || defender.disabled || !defender.position.valid() ||
                defender.groundWeapon.damage <= 0 ||
                (!isBuilding(defender.kind) && !defender.visible &&
                 state.frame - defender.lastSeen > framesForSeconds(30)) ||
                outsidePixelRadius(defender.position, depot.position, PixelRadius{800})) continue;
            score += unitStats(defender.kind).combatValue * 192.0;
        }
        if (score < bestTargetScore) {
            bestTargetScore = score;
            plan.attackTarget = depot.position;
        }
    }
    plan.rallyPoint = moveToward(ourMain(state), plan.attackTarget, 320.0);
    // Cover a paid-for expansion while it warps in. The first field army
    // previously crossed the map during this window, leaving the new economy
    // exposed and separating it from the first Reaver reinforcements.
    for (const auto& nexus : state.self.units) {
        if (nexus.kind == UnitKind::nexus && !nexus.completed)
            plan.expansionTarget = nexus.position;
    }
    if (grow) {
        auto bestSiteScore = std::numeric_limits<double>::infinity();
        for (const auto& site : state.bases) {
            if (site.ownerId != -1 || site.island || !site.depotFootprintAvailable ||
                !site.center.valid() ||
                site.mineralsRemaining < 1000) continue;
            auto score = distance(ourMain(state), site.center);
            for (const auto& enemy : state.enemy.units) {
                if (!enemy.position.valid() || enemy.groundWeapon.damage <= 0 ||
                    (!enemy.visible && state.frame - enemy.lastSeen > framesForSeconds(8))) continue;
                score += std::max(0.0, 960.0 - distance(site.center, enemy.position)) * 3.0;
            }
            if (score < bestSiteScore) {
                bestSiteScore = score;
                plan.expansionTarget = site.center;
            }
        }
    }
    if (plan.expansionTarget.valid()) plan.rallyPoint = plan.expansionTarget;

    // Remove conflicting opening quotas. They otherwise keep reserving for
    // excess static defenses and parallel tech ahead of the next Nexus.
    const auto gateways = std::clamp((workers + 7) / 8, 2, 10);
    for (auto& demand : plan.goals) {
        if (demand.goal == GoalKind::expand) demand.desiredCount = plan.desiredBases;
        if (demand.target == UnitKind::gateway) demand.desiredCount = gateways;
        if (demand.target == UnitKind::photonCannon)
            demand.desiredCount = std::max(effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::observed), bases);
        if (demand.target == UnitKind::observer)
            demand.desiredCount = observer ? std::max(2, bases) : 1;
        if (demand.target != UnitKind::pylon && demand.target != UnitKind::probe)
            demand.priority = std::min(demand.priority, 98);
    }
    plan.composition = {{UnitKind::dragoon, 0.70}, {UnitKind::zealot, 0.20},
                        {UnitKind::reaver, 0.10}};
    goal(plan, GoalKind::build, UnitKind::gateway, gateways, 82,
         "production supported by the recovered mining economy");
    technologyGoal(plan, TechnologyKind::singularityCharge, 1, 105,
                   "range to contest the perimeter", true);
    goal(plan, GoalKind::train, UnitKind::observer, observer ? (bases >= 2 ? 3 : 2) : 1, 104,
         "detection for the assembled field army", true);
    goal(plan, GoalKind::train, UnitKind::reaver, bases >= 3 ? 4 : 2, 96,
         "splash to break a concentrated contain", true);
    technologyGoal(plan, TechnologyKind::protossGroundWeapons,
                   minute(state) >= 16 ? 3 : minute(state) >= 12 ? 2 : 1,
                   83, "keep the expanding army upgraded");
    if (bases >= 2)
        technologyGoal(plan, TechnologyKind::legEnhancements, 1, 84,
                       "mobile mineral reinforcement for the ranged army");
}

void StrategyEngine::addHarassmentProduction(StrategicPlan& plan, const GameState& state,
                                              const bool pvzArchivesFirst) {
    const auto bases = effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::completed);
    const auto workers = observedRoleCount(state, UnitRole::worker);
    const auto screen = effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed) + effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed);
    if (minute(state) < 7 || bases < 2 || workers < 28 || screen < 8 ||
        plan.prioritizeReinforcements || plan.posture == Posture::defend ||
        plan.posture == Posture::recover || hardBreachAtMain(state)) return;
    if (pvzArchivesFirst && state.enemy.race == Race::zerg && minute(state) < 12 &&
        (effectiveUnitCount(state, UnitKind::templarArchives, UnitCountBasis::completed) == 0 ||
         effectiveUnitCount(state, UnitKind::highTemplar, UnitCountBasis::completed) == 0)) return;
    const auto enemyEconomy = std::ranges::any_of(state.bases, [&](const BaseSnapshot& base) {
        return state.enemy.id >= 0 && base.ownerId == state.enemy.id && base.mineralsRemaining >= 500;
    }) || std::ranges::any_of(state.enemy.units, [](const UnitSnapshot& enemy) {
        return isWorker(enemy.kind) || enemy.role == UnitRole::resourceDepot;
    });
    if (!enemyEconomy) return;

    // Buy a separate drop package. The first army Reavers remain on the
    // ground; harassment receives an additional Reaver and its own Shuttle.
    const auto armyReavers = state.enemy.race == Race::protoss ? 2 : 1;
    const auto firstHarassmentGoal = plan.goals.size();
    plan.harassmentDrops = 1;
    goal(plan, GoalKind::build, UnitKind::roboticsFacility, 1, 102,
         "dedicated mineral-line drop technology", true);
    goal(plan, GoalKind::build, UnitKind::roboticsSupportBay, 1, 102,
         "unlock dedicated harassment Reaver", true);
    goal(plan, GoalKind::train, UnitKind::reaver, armyReavers + plan.harassmentDrops, 103,
         "extra Reaver for drops without borrowing army splash", true);
    if (effectiveUnitCount(state, UnitKind::reaver, UnitCountBasis::observed) >= armyReavers)
        goal(plan, GoalKind::train, UnitKind::shuttle, plan.harassmentDrops, 104,
             "dedicated transport to bypass the defended entrance", true);
    if (effectiveUnitCount(state, UnitKind::shuttle, UnitCountBasis::completed) > 0)
        technologyGoal(plan, TechnologyKind::graviticDrive, 1, 87,
                       "faster drop entry and extraction");
    if (state.enemy.race == Race::zerg && recentEnemyCount(state, UnitKind::overlord) > 0 &&
        effectiveUnitCount(state, UnitKind::stargate, UnitCountBasis::observed) > 0)
        goal(plan, GoalKind::train, UnitKind::corsair, 4, 94,
             "dedicated Overlord hunters alongside worker drops");
    if (effectiveUnitCount(state, UnitKind::templarArchives, UnitCountBasis::completed) > 0) {
        UnitSnapshot covert;
        covert.kind = UnitKind::darkTemplar;
        covert.position = ourMain(state);
        covert.cloaked = true;
        covert.groundWeapon = {40, 30, 0, 32, DamageType::normal, false, true};
        if (harassmentOpportunity(state, covert).target.valid())
            goal(plan, GoalKind::train, UnitKind::darkTemplar, 2, 94,
                 "dedicated cloaked raiders for an exposed economy");
    }
    for (auto i = firstHarassmentGoal; i < plan.goals.size(); ++i)
        plan.goals[i].harassmentOnly = true;
}

void StrategyEngine::addMapControlEconomy(
    StrategicPlan& plan, const GameState& state, const ThreatAssessment& threat) {
    const auto bases = effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed);
    const auto workers = observedRoleCount(state, UnitRole::worker);
    if (minute(state) < 12 || bases < 2 || workers < 32 ||
        plan.posture == Posture::recover || threat.combatEnemiesNearMain > 0 ||
        activeApproach(state, threat) || threat.immediateGround > 0.45 ||
        threat.workerRush > 0.30 || threat.proxy + threat.staticContain > 0.34 ||
        hardBreachAtMain(state)) return;

    auto armyPower = 0.0;
    auto mobileCount = 0;
    for (const auto& unit : state.self.units) {
        if (!unit.completed || unit.disabled || unit.loaded || unit.hallucination ||
            isBuilding(unit.kind) || !isCombatUnit(unit.kind)) continue;
        armyPower += unitStats(unit.kind).combatValue * unit.healthFraction();
        ++mobileCount;
    }
    auto enemyMobilePower = 0.0;
    auto fortifications = 0;
    for (const auto& enemy : state.enemy.units) {
        if (!enemy.completed || enemy.disabled || !enemy.position.valid() || enemy.hallucination) continue;
        const auto armed = enemy.groundWeapon.damage > 0 || enemy.airWeapon.damage > 0;
        if (!armed || isWorker(enemy.kind)) continue;
        // A raid at any owned base cancels growth, including remote economies
        // that the main-base threat classifier does not cover.
        if ((enemy.visible || state.frame - enemy.lastSeen <= framesForSeconds(8)) &&
            std::ranges::any_of(state.bases, [&](const BaseSnapshot& base) {
                return base.ownerId == state.self.id &&
                    withinPixelRadius(base.center, enemy.position, PixelRadius{800});
            })) return;
        if (!isBuilding(enemy.kind)) {
            if (enemy.visible || state.frame - enemy.lastSeen <= framesForSeconds(90))
                enemyMobilePower += unitStats(enemy.kind).combatValue * enemy.healthFraction();
        }
        if ((isStaticDefense(enemy.kind) ||
             (enemy.kind == UnitKind::siegeTank && enemy.groundWeapon.maxRange >= 320)) &&
            std::ranges::any_of(state.bases, [&](const BaseSnapshot& base) {
                return state.enemy.id >= 0 && base.ownerId == state.enemy.id &&
                    withinPixelRadius(base.center, enemy.position, PixelRadius{800});
            })) ++fortifications;
    }
    // Compare field armies rather than requiring a profitable frontal fight
    // into static defenses. Fog increases the margin required to spend.
    if (fortifications < 3 || mobileCount < 12 ||
        armyPower < std::max(12.0, enemyMobilePower) * (1.35 + threat.uncertainty * 0.35)) return;

    const auto home = ourMain(state);
    Position site{-1, -1};
    auto bestScore = std::numeric_limits<double>::infinity();
    for (const auto& base : state.bases) {
        if (base.ownerId != -1 || base.island || !base.depotFootprintAvailable ||
            !base.center.valid() ||
            base.mineralsRemaining < 1500) continue;
        auto danger = false;
        auto enemyDistance = 4096.0;
        for (const auto& enemy : state.enemy.units) {
            if (!enemy.position.valid() ||
                (!isBuilding(enemy.kind) && !enemy.visible && state.frame - enemy.lastSeen > framesForSeconds(30))) continue;
            enemyDistance = std::min(enemyDistance, distance(base.center, enemy.position));
            if ((enemy.groundWeapon.damage > 0 || enemy.role == UnitRole::resourceDepot) &&
                withinPixelRadius(base.center, enemy.position, PixelRadius{960})) danger = true;
        }
        if (danger) continue;
        auto friendlyDistance = distance(home, base.center);
        for (const auto& owned : state.bases)
            if (owned.ownerId == state.self.id && owned.center.valid())
                friendlyDistance = std::min(friendlyDistance, distance(owned.center, base.center));
        const auto score = friendlyDistance - enemyDistance * 0.20;
        if (score < bestScore) { bestScore = score; site = base.center; }
    }
    const auto pending = bases > effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::completed);
    if (!site.valid() && !pending) return;
    plan.sustainEconomy = true;
    plan.posture = Posture::pressure;
    plan.name += " [outgrow fortified opponent]";
    plan.maximumBases = 8;
    // Commit one Nexus at a time, then immediately reassess. A lead and a
    // bank can fund growth before every old mineral line is saturated.
    const auto grow = !pending && bases < 8 &&
        (workers >= bases * 16 || state.self.minerals >= 600);
    plan.desiredBases = bases + (grow ? 1 : 0);
    plan.desiredWorkers = std::min(80, bases * 22);
    plan.desiredGasWorkers = std::min(12, bases * 3);
    if (grow) plan.expansionTarget = site;
    for (const auto& nexus : state.self.units)
        if (nexus.kind == UnitKind::nexus && !nexus.completed) plan.expansionTarget = nexus.position;
    if (plan.expansionTarget.valid()) plan.rallyPoint = plan.expansionTarget;
}

void StrategyEngine::addInfrastructure(
    StrategicPlan& plan,
    const GameState& state,
    const ThreatAssessment& threat) {
    const auto bases = std::max(1, effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed));
    const auto completedBases = std::max(1, effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::completed));
    const auto workers = observedRoleCount(state, UnitRole::worker);
    const auto pylons = effectiveUnitCount(state, UnitKind::pylon, UnitCountBasis::observed);
    const auto pylonBuildFrames = unitStats(UnitKind::pylon).buildTime;
    const auto supplyDeadlineFrames = pylonBuildFrames + std::clamp(
        state.pylonBuilderTravelFrames, 0, framesForMinutes(2));
    const auto timelyPendingPylons = static_cast<int>(std::ranges::count_if(
        state.self.units, [supplyDeadlineFrames](const UnitSnapshot& unit) {
            if (unit.kind != UnitKind::pylon || unit.completed) return false;
            const auto remaining = unitStats(UnitKind::pylon).buildTime *
                (100 - std::clamp(unit.buildProgress, 0, 100)) / 100;
            return remaining <= supplyDeadlineFrames;
        }));
    const auto activeProduction = effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::completed) +
                                  effectiveUnitCount(state, UnitKind::roboticsFacility, UnitCountBasis::completed) +
                                  effectiveUnitCount(state, UnitKind::stargate, UnitCountBasis::completed);
    const auto desiredBuffer = state.self.supplyTotal <= 18
                                   ? 2
                                   : std::clamp(4 + activeProduction * 2, 6, 18);
    const auto projectedSupply = std::min(400, state.self.supplyTotal + timelyPendingPylons * 16);
    // BWAPI's used supply already includes units being trained. The buffer
    // forecasts the next production cycle; adding the queue again bought
    // redundant Pylons while the opening needed combat units.
    const auto projectedUsed = state.self.supplyUsed;
    const auto supplyNeeded = projectedSupply < 400 &&
                              projectedSupply - projectedUsed <= desiredBuffer;
    const auto expansionDue = plan.desiredBases > bases;
    const auto expansionReady = bases == completedBases &&
        (workers >= bases * 12 ||
         (plan.sustainEconomy && workers >= 32 && state.self.minerals >= 600));
    const auto defensiveGrowth = plan.sustainEconomy ||
        (plan.posture == Posture::defend && defensiveExpansionWindow(state, threat));
    const auto expansionBanking = expansionDue && expansionReady &&
                                  (plan.posture != Posture::defend || defensiveGrowth) &&
                                  plan.posture != Posture::recover &&
                                  (plan.sustainEconomy ||
                                   (threat.combatEnemiesNearMain == 0 &&
                                    !activeApproach(state, threat) &&
                                    threat.immediateGround <= 0.45)) &&
                                  !hardBreachAtMain(state);
    // When the natural is already a safe strategic goal, hold one small
    // supply margin instead of buying a Pylon ahead of the 400-mineral Nexus.
    // The old priority ordering spent the bank on a fifth Pylon at 76/82
    // supply, then restarted the save on every macro pass, so the expansion
    // never reached the command threshold before the next pressure wave.
    const auto desiredPylons = std::max(
        bases, pylons + (supplyNeeded && !expansionBanking ? 1 : 0));
    const auto openingPylonDeadline = pylons == 0 &&
                                      (state.self.supplyUsed >= 12 ||
                                       state.frame >= framesForSeconds(45));
    goal(plan, GoalKind::build, UnitKind::pylon, desiredPylons, 100,
         "maintain a supply buffer",
         openingPylonDeadline || state.self.supplyTotal - state.self.supplyUsed <= 4);
    goal(plan, GoalKind::train, UnitKind::probe, std::max(workers, plan.desiredWorkers), 93,
         "maintain continuous worker production");
    // An expansion cannot be bought opportunistically while every idle
    // producer keeps spending the same income. Once the strategic phase calls
    // for another base and the main is clear, reserve its full cost ahead of
    // routine workers, tech, and composition fills. Direct pressure cancels
    // the reservation immediately so a Nexus never starves emergency units.
    const auto expansionSafe = expansionDue && expansionReady &&
                               (plan.posture != Posture::defend || defensiveGrowth) &&
                               plan.posture != Posture::recover &&
                               (plan.sustainEconomy ||
                                (threat.combatEnemiesNearMain == 0 &&
                                 !activeApproach(state, threat) &&
                                 threat.immediateGround <= 0.45));
    if (plan.posture != Posture::defend || defensiveGrowth) {
        // In a stable counter-window the Nexus is the strategic play, not a
        // low-priority suggestion.  Robotics/Observer and Gateway filler
        // goals can otherwise reserve every mineral ahead of it, leaving a
        // permanently waiting expansion even with a 3k bank.  Match the
        // Stardust-style phase transition by reserving the natural before
        // optional tech/composition spends; emergency posture still cancels
        // the reservation through expansionSafe.
        const auto expansionPriority =
            expansionSafe && (plan.posture == Posture::pressure || defensiveGrowth) ? 120 :
            (expansionSafe ? 95 : 65);
        goal(plan, GoalKind::expand, UnitKind::nexus, plan.desiredBases,
             expansionPriority, "match economic phase", expansionSafe);
    }
    if (plan.desiredGasWorkers > 0) {
        goal(plan, GoalKind::build, UnitKind::assimilator, bases, 80, "fund technology");
    }

    // Size baseline production from the live economy. A useful tournament
    // rule is roughly one continuously-produced combat unit per six workers;
    // keep that throughput behind the intended expansion so it cannot crowd
    // out a planned Nexus.
    const auto gateways = effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::observed);
    const auto gatewayAwaitingPower = std::ranges::any_of(
        state.self.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::gateway && unit.completed &&
                   unitStats(unit.kind).requiresPsi && !unit.powered;
        });
    const auto productionReadiness = assessProductionReadiness(state.self);
    const auto gatewayCapacityBlocked = gatewayAwaitingPower ||
        (gateways > 0 && productionReadiness.producerSnapshotAvailable &&
         (productionReadiness.usableGateways == 0 ||
          productionReadiness.unobservedGateways > 0 ||
          productionReadiness.occupiedGateways < productionReadiness.usableGateways));
    const auto protectPvPTech = state.enemy.race == Race::protoss &&
        ((effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::observed) > 0 &&
          effectiveUnitCount(state, UnitKind::roboticsFacility, UnitCountBasis::observed) == 0 &&
          effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed) >= 4) ||
         (effectiveUnitCount(state, UnitKind::roboticsFacility, UnitCountBasis::observed) > 0 &&
          effectiveUnitCount(state, UnitKind::roboticsSupportBay, UnitCountBasis::observed) == 0));
    // A one-base mirror with a healthy bank can sustain four Gateways. The
    // old three-Gateway ceiling left minerals idle while the opponent's
    // production kept scaling, even after our army had stabilized the main.
    const auto productionCeiling = std::clamp(bases * 4, 2, 12);
    auto throughputTarget = std::clamp(
        (workers + 5) / 6, 1, productionCeiling);
    // Scouting multiple enemy production structures is direct evidence that
    // our income-only heuristic may be too slow. Match most of that observed
    // capacity without blindly copying it across asymmetric race mechanics.
    const auto observedParity = std::clamp(
        static_cast<int>(std::ceil(threat.enemyProductionCapacity * 0.8)),
        1, productionCeiling);
    throughputTarget = std::max(throughputTarget, observedParity);

    // The worker heuristic is only a fallback. Once the BWAPI bridge has a
    // full rolling sample window, constrain continuous Gateway production by
    // the actual composition's mineral and gas burn rates. Existing capacity
    // is never torn down when current income dips; the estimate only prevents
    // adding capacity that the observed economy cannot keep busy.
    const auto measuredIncome = state.estimatedMineralIncomePerMinute >= 0 &&
                                state.estimatedGasIncomePerMinute >= 0;
    auto sustainableGatewayCeiling = productionCeiling;
    if (measuredIncome) {
        const auto perGateway = gatewayProductionCost(plan);
        if (perGateway.mineralsPerMinute > 0.0) {
            sustainableGatewayCeiling = std::min(
                sustainableGatewayCeiling,
                static_cast<int>(state.estimatedMineralIncomePerMinute /
                                 perGateway.mineralsPerMinute));
        }
        if (perGateway.gasPerMinute > 0.0) {
            sustainableGatewayCeiling = std::min(
                sustainableGatewayCeiling,
                static_cast<int>(state.estimatedGasIncomePerMinute /
                                 perGateway.gasPerMinute));
        }
        sustainableGatewayCeiling = std::max(gateways, sustainableGatewayCeiling);
        throughputTarget = std::min(throughputTarget, sustainableGatewayCeiling);
    }

    // An idle powered Gateway is the immediate production shortfall to fix;
    // constructing another one cannot improve army readiness. Queued units
    // already contribute to readiness because the bridge exposes waiting
    // items separately from the in-progress unit in the army snapshot.
    const auto readinessDeficit = std::max(
        0, plan.minimumAttackSize - productionReadiness.armyReadySoon());
    for (auto& demand : plan.goals) {
        if (demand.goal != GoalKind::build || demand.target != UnitKind::gateway ||
            demand.blocking) {
            continue;
        }
        if (gatewayCapacityBlocked) {
            demand.desiredCount = std::min(demand.desiredCount, gateways);
        } else if (measuredIncome) {
            demand.desiredCount = std::min(demand.desiredCount, sustainableGatewayCeiling);
        }
    }

    if (!gatewayCapacityBlocked && !protectPvPTech && !plan.sustainEconomy &&
        state.frame >= framesForMinutes(4) && bases >= plan.desiredBases &&
        gateways < throughputTarget) {
        goal(plan, GoalKind::build, UnitKind::gateway, throughputTarget,
             observedParity > (workers + 5) / 6 ? 78 : 73,
             observedParity > (workers + 5) / 6
                 ? "match scouted enemy production capacity"
                 : "match army throughput to the mining economy");
    }
    // A large residual bank still means infrastructure is the bottleneck,
    // even if recent worker losses make the throughput estimate conservative.
    if (!gatewayCapacityBlocked && !protectPvPTech && state.frame >= framesForMinutes(4) &&
        state.self.minerals >= 650 &&
        bases >= plan.desiredBases && gateways < sustainableGatewayCeiling &&
        (readinessDeficit > 0 || observedParity > gateways ||
         gateways < throughputTarget)) {
        goal(plan, GoalKind::build, UnitKind::gateway, gateways + 1, 62,
             "convert sustained mineral surplus into army production");
    }
}

void StrategyEngine::addAdaptiveCounters(
    StrategicPlan& plan,
    const GameState& state) {
    // Convert legal observations into concrete production changes. Broad
    // air/cloak alarms keep bases alive; these matchup-aware counters prevent
    // the standing composition from continuing into a unit mix it cannot beat.
    if (state.enemy.race == Race::terran) {
        const auto bio = recentEnemyCount(state, UnitKind::marine) +
                         recentEnemyCount(state, UnitKind::medic) +
                         recentEnemyCount(state, UnitKind::firebat) +
                         recentEnemyCount(state, UnitKind::ghost);
        const auto mines = recentEnemyCount(state, UnitKind::spiderMine);
        const auto factories = recentEnemyCount(state, UnitKind::factory);
        const auto mech = recentEnemyCount(state, UnitKind::vulture) +
                          recentEnemyCount(state, UnitKind::siegeTank) +
                          recentEnemyCount(state, UnitKind::goliath) + mines;
        const auto capitalAir = recentEnemyCount(state, UnitKind::battlecruiser) +
                                recentEnemyCount(state, UnitKind::wraith) +
                                recentEnemyCount(state, UnitKind::valkyrie);

        const auto siegeTanks = recentEnemyCount(state, UnitKind::siegeTank);
        if (mines > 0 || siegeTanks >= 2) {
            goal(plan, GoalKind::train, UnitKind::observer, 3, 92,
                 "track mines and siege lines", true);
        }
        if (bio >= 7 && minute(state) >= 7) {
            plan.name += " [anti-bio storm]";
            // Storm is the actual scaling answer to a packed Marine force. A
            // minute-scaled Dragoon goal otherwise spends every 125-mineral
            // increment before the bank can reach Storm's 200-mineral cost.
            // Make the tech path reserve first, then fill idle Gateways with
            // Templar and routine ranged production.
            const auto frontline =
                effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed) +
                effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed) +
                effectiveUnitCount(state, UnitKind::darkTemplar, UnitCountBasis::completed);
            const auto spellWindow = plan.posture != Posture::defend || frontline >= 8;
            if (spellWindow) {
                goal(plan, GoalKind::build, UnitKind::citadelOfAdun, 1, 100,
                     "unlock the decisive anti-bio spell", true);
                goal(plan, GoalKind::build, UnitKind::templarArchives, 1, 100,
                     "unlock the decisive anti-bio spell", true);
                technologyGoal(plan, TechnologyKind::psionicStorm, 1, 100,
                               "counter observed bio mass", true);
                goal(plan, GoalKind::train, UnitKind::highTemplar,
                     std::clamp(bio / 3, 3, 7), 98,
                     "punish clustered Terran bio");
            }
            setCompositionWeight(plan, UnitKind::highTemplar, 0.24);
            setCompositionWeight(plan, UnitKind::zealot, 0.30);
        }
        const auto projectedMech = std::max(mech, factories * 3);
        if (mech >= 4 || siegeTanks >= 2 || factories >= 2) {
            plan.name += " [anti-mech mobility]";
            technologyGoal(plan, TechnologyKind::legEnhancements, 1, 96,
                           factories >= 2 && mech < 4
                               ? "close on scouted Factory production"
                               : "close on observed siege composition");
            goal(plan, GoalKind::train, UnitKind::zealot,
                 std::clamp(projectedMech, 8, 18), 95,
                 "absorb mines and surround tanks");
            setCompositionWeight(plan, UnitKind::zealot, 0.34);
            setCompositionWeight(plan, UnitKind::arbiter, 0.14);
        }
        if (capitalAir >= 3) {
            plan.name += " [anti-air fleet]";
            goal(plan, GoalKind::train, UnitKind::dragoon,
                 std::clamp(8 + capitalAir * 2, 10, 20), 95,
                 "counter observed Terran air", true);
            setCompositionWeight(plan, UnitKind::dragoon, 0.68);
        }
    } else if (state.enemy.race == Race::zerg) {
        const auto hydraLurker = recentEnemyCount(state, UnitKind::hydralisk) +
                                 recentEnemyCount(state, UnitKind::lurker);
        const auto zergAir = recentEnemyCount(state, UnitKind::mutalisk) +
                             recentEnemyCount(state, UnitKind::guardian) +
                             recentEnemyCount(state, UnitKind::devourer);
        const auto lateGround = recentEnemyCount(state, UnitKind::ultralisk) +
                                recentEnemyCount(state, UnitKind::defiler);
        const auto groundTechScreen = effectiveUnitCount(state, UnitKind::cyberneticsCore, UnitCountBasis::completed) > 0 &&
            effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed) + effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed) >= 4 &&
            !hardBreachAtMain(state);

        if (hydraLurker >= 7 && groundTechScreen) {
            plan.name += " [anti-hydra splash]";
            // This blocking structure checkpoint recursively stages Robotics
            // then Support Bay while leaving one Probe cycle available when
            // the full chain is not yet affordable. Keep the Reaver itself a
            // non-blocking train goal so the same minerals are not reserved
            // again while its tech prerequisites are being built.
            goal(plan, GoalKind::build, UnitKind::roboticsSupportBay, 1, 92,
                 "stage powered Robotics and Support Bay for ground splash", true);
            goal(plan, GoalKind::train, UnitKind::reaver,
                 std::clamp(hydraLurker / 5, 2, 4), 90,
                 "splash clustered hydralisks and lurkers");
            setCompositionWeight(plan, UnitKind::reaver, 0.16);
        }
        if (zergAir >= 4) {
            plan.name += " [anti-air control]";
            setCompositionWeight(plan, UnitKind::corsair, 0.28);
            setCompositionWeight(plan, UnitKind::dragoon, 0.22);
        }
        if (lateGround >= 3 && groundTechScreen) {
            const auto defilers = recentEnemyCount(state, UnitKind::defiler);
            const auto ultralisks = recentEnemyCount(state, UnitKind::ultralisk);
            if (defilers >= 2) {
                plan.name += " [observed-Defiler Storm package]";
                technologyGoal(plan, TechnologyKind::psionicStorm, 1, 94,
                               "unlock Storm against observed Defiler support", true);
                if (technologyLevel(state.self, TechnologyKind::psionicStorm) > 0) {
                    goal(plan, GoalKind::train, UnitKind::highTemplar,
                         std::clamp(2 + defilers / 2, 2, 4), 90,
                         "field bounded Storm casters against Defilers");
                    setCompositionWeight(plan, UnitKind::highTemplar, 0.24);
                }
            } else if (ultralisks >= 3) {
                plan.name += " [observed-Ultralisk Reaver package]";
                goal(plan, GoalKind::build, UnitKind::roboticsSupportBay, 1, 92,
                     "stage Robotics and Support Bay for Ultralisk splash", true);
                goal(plan, GoalKind::train, UnitKind::reaver,
                     std::clamp(ultralisks / 2, 2, 4), 90,
                     "splash the observed Ultralisk force");
                setCompositionWeight(plan, UnitKind::reaver, 0.16);
            }
        }
    } else if (state.enemy.race == Race::protoss) {
        const auto reavers = recentEnemyCount(state, UnitKind::reaver);
        const auto enemyFleet = recentEnemyCount(state, UnitKind::carrier) +
                                recentEnemyCount(state, UnitKind::scout) +
                                recentEnemyCount(state, UnitKind::corsair);
        if (reavers >= 2) {
            goal(plan, GoalKind::train, UnitKind::observer, 3, 89,
                 "maintain vision over enemy reavers");
            goal(plan, GoalKind::train, UnitKind::reaver, 3, 82,
                 "contest enemy reaver control");
        }
        if (enemyFleet >= 3) {
            plan.name += " [anti-carrier fleet]";
            goal(plan, GoalKind::train, UnitKind::dragoon,
                 std::clamp(8 + enemyFleet * 2, 10, 20), 94,
                 "pressure observed Protoss air", true);
            goal(plan, GoalKind::train, UnitKind::scout,
                 std::clamp(enemyFleet, 3, 6), 86,
                 "focus high-value Protoss capital ships");
            setCompositionWeight(plan, UnitKind::dragoon, 0.62);
            setCompositionWeight(plan, UnitKind::scout, 0.16);
        }
    }
    normalizeComposition(plan);
}

void StrategyEngine::addSafetyReactions(
    StrategicPlan& plan,
    const ThreatAssessment& threat) {
    if (threat.workerRush > 0.30) {
        plan.name += " [worker-rush hold]";
        plan.posture = Posture::defend;
        plan.desiredBases = 1;
        plan.attackThreshold = std::max(plan.attackThreshold, 1.55);
        goal(plan, GoalKind::build, UnitKind::gateway, 1, 100,
             "complete the first anti-worker combat unit", true);
        goal(plan, GoalKind::train, UnitKind::zealot, 3, 99,
             "end the worker rush without prolonged economic damage", true);
    }

    if (threat.proxy + threat.staticContain > 0.34 ||
        (threat.enemiesNearMain >= 2 && threat.proxy > 0.18)) {
        plan.name += threat.staticContain > threat.proxy
                         ? " [break static contain]"
                         : " [break proxy]";
        plan.posture = Posture::defend;
        plan.desiredBases = 1;
        plan.attackThreshold = std::max(plan.attackThreshold, 1.65);
        plan.goals.erase(
            std::remove_if(plan.goals.begin(), plan.goals.end(),
                           [](const ProductionGoal& candidate) {
                               return candidate.goal == GoalKind::expand;
                           }),
            plan.goals.end());
        goal(plan, GoalKind::build, UnitKind::gateway, 2, 100,
             "replace greed with proxy-breaking production", true);
        goal(plan, GoalKind::train, UnitKind::zealot, 5, 99,
             "clear unfinished or unsupported proxy structures", true);
        goal(plan, GoalKind::build, UnitKind::shieldBattery, 1, 95,
             "sustain the main-base defense");
    }

    if (threat.cloak > 0.28) {
        plan.desiredGasWorkers = std::max(3, plan.desiredGasWorkers);
        goal(plan, GoalKind::build, UnitKind::roboticsFacility, 1, 97,
             "detected cloaked threat", true);
        goal(plan, GoalKind::build, UnitKind::observatory, 1, 96,
             "unlock mobile detection", true);
        goal(plan, GoalKind::train, UnitKind::observer, 3, 99,
             "maintain detection coverage", true);
        goal(plan, GoalKind::build, UnitKind::photonCannon, 3, 91,
             "base detection coverage");
    }
    if (threat.air > 0.42) {
        plan.desiredGasWorkers = std::max(3, plan.desiredGasWorkers);
        goal(plan, GoalKind::train, UnitKind::dragoon, 10, 94, "mobile anti-air", true);
        goal(plan, GoalKind::build, UnitKind::photonCannon, 5, 90,
             "mineral-line anti-air");
    }
}

void StrategyEngine::addEconomicRecovery(
    StrategicPlan& plan,
    const GameState& state,
    const bool lateEconomyRecovery) {
    const auto workers = observedRoleCount(state, UnitRole::worker);
    const auto nexuses = effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::observed);
    const auto completedNexuses = effectiveUnitCount(state, UnitKind::nexus, UnitCountBasis::completed);
    const auto pendingNexuses = nexuses - completedNexuses;
    const auto activeBases = static_cast<int>(std::ranges::count_if(
        state.bases, [&state](const BaseSnapshot& base) {
            return base.ownerId == state.self.id && base.mineralsRemaining > 1000;
        }));
    const auto ownedMinerals = std::accumulate(state.bases.begin(), state.bases.end(), 0,
        [&state](const int total, const BaseSnapshot& base) {
            return total + (base.ownerId == state.self.id
                                ? std::max(0, base.mineralsRemaining) : 0);
        });
    plan.estimatedMiningRunwayFrames =
        estimateMiningRunwayFrames(state, workers, ownedMinerals);

    if (nexuses == 0 && workers > 0) {
        plan.name = "Emergency Nexus recovery";
        plan.posture = Posture::recover;
        plan.desiredBases = 1;
        plan.desiredWorkers = std::max(12, workers);
        goal(plan, GoalKind::expand, UnitKind::nexus, 1, 100,
             "replace the lost economy anchor", true);
    }

    if (state.frame >= framesForMinutes(4) && completedNexuses > 0 &&
        workers < std::min(12, completedNexuses * 8)) {
        const auto defending = plan.posture == Posture::defend;
        plan.name += " [worker recovery]";
        if (!defending) plan.posture = Posture::recover;
        plan.desiredWorkers = std::max(plan.desiredWorkers, completedNexuses * 14);
        goal(plan, GoalKind::train, UnitKind::probe,
             std::max(8, completedNexuses * 10), defending ? 70 : 96,
             "recover after severe worker losses", workers < 6 && !defending);
    }

    // Once a defensive anchor exists, losing every new mineral to an army
    // target is a death spiral: no economy remains to replace that army. A
    // critical worker floor therefore outranks continuing reinforcement, but
    // only after static/army safety exists (or the worker line is almost gone).
    const auto mobileAnchor = effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed) >= 3 ||
                              effectiveUnitCount(state, UnitKind::dragoon, UnitCountBasis::completed) >= 2;
    const auto completedCannons = effectiveUnitCount(state, UnitKind::photonCannon, UnitCountBasis::completed);
    const auto staticRecoveryAnchor = completedCannons >= 3;
    const auto defensiveAnchor = completedCannons > 0 ||
                                 effectiveUnitCount(state, UnitKind::shieldBattery, UnitCountBasis::completed) > 0 ||
                                 mobileAnchor;
    const auto recoveryCanSpend = plan.posture != Posture::defend ||
                                  mobileAnchor || staticRecoveryAnchor || workers <= 3;
    if (state.frame >= framesForMinutes(4) && completedNexuses > 0 && workers < 8 &&
        recoveryCanSpend && (defensiveAnchor || workers <= 3)) {
        plan.name += " [critical worker floor]";
        plan.desiredWorkers = std::max(plan.desiredWorkers, 8);
        goal(plan, GoalKind::train, UnitKind::probe, 8, 105,
             "rebuild a minimum income behind the defensive screen", true);
    }

    if (lateEconomyRecovery && state.frame >= framesForMinutes(12) &&
        plan.posture != Posture::defend && !hardBreachAtMain(state)) {
        const auto mobileArmy = std::ranges::count_if(state.self.units,
            [](const UnitSnapshot& unit) {
                return unit.completed && !unit.hallucination &&
                    !isWorker(unit.kind) && !isBuilding(unit.kind) &&
                    isCombatUnit(unit.kind);
            });
        if (mobileArmy >= 8 && activeBases > 0) {
            // The long PvZ control game fell to twelve Probes on two mineral
            // bases while ~35 combat units survived. The old threshold only
            // reacted below twelve and could not outbid routine production.
            const auto incomeFloor = std::clamp(activeBases * 12, 12, 28);
            if (workers < incomeFloor) {
                plan.name += " [protected income recovery]";
                plan.desiredWorkers = std::max(plan.desiredWorkers, incomeFloor);
                goal(plan, GoalKind::train, UnitKind::probe, incomeFloor, 115,
                     "restore income behind the surviving field army", true);
            }
            const auto neutralMinerals = readyReplacementExpansionSite(state);
            if (completedNexuses >= 2 && completedNexuses < 8 &&
                pendingNexuses == 0 && neutralMinerals.valid() &&
                ownedMinerals < activeBases * 4500) {
                plan.name += " [pre-depletion expansion]";
                plan.desiredBases = std::max(plan.desiredBases, completedNexuses + 1);
                plan.expansionTarget = neutralMinerals;
                goal(plan, GoalKind::expand, UnitKind::nexus,
                     completedNexuses + 1, 114,
                     "replace mining capacity before the owned patches empty", true);
            }
        }
    }

    constexpr Frame preparationRunway = framesForMinutes(9);
    const auto replacementSite = readyReplacementExpansionSite(state);
    const auto workerBreach = std::ranges::any_of(state.self.units,
        [](const UnitSnapshot& unit) {
            return isWorker(unit.kind) && unit.underAttack;
        });
    const auto terminalCloseout = plan.posture == Posture::attack &&
        state.self.supplyTotal >= 400 && state.self.supplyUsed >= 390;
    const auto replacementNeeded = workers > 0 && completedNexuses > 0 &&
        activeBases > 0 && ownedMinerals > 0 &&
        completedNexuses < 8 && pendingNexuses == 0 &&
        plan.estimatedMiningRunwayFrames >= 0 &&
        plan.estimatedMiningRunwayFrames <= preparationRunway &&
        !terminalCloseout && !hardBreachAtMain(state) && !workerBreach;
    if (replacementNeeded) {
        if (replacementSite.valid()) {
            plan.name += " [pre-depletion replacement]";
            plan.desiredBases = std::max(plan.desiredBases, completedNexuses + 1);
            plan.expansionTarget = replacementSite;
            plan.sustainEconomy = plan.posture != Posture::defend;
            goal(plan, GoalKind::expand, UnitKind::nexus,
                 completedNexuses + 1, 114,
                 "replace mining capacity before the income runway expires", true);
        } else if (activeBases > 0) {
            // No ready, reachable resource site exists. Stop paying for
            // optional infrastructure and rebuild the worker pool against the
            // remaining owned patches while the map is re-scouted.
            plan.name += " [survival: no ready replacement base]";
            if (plan.posture != Posture::defend) plan.posture = Posture::recover;
            plan.desiredGasWorkers = 0;
            plan.desiredWorkers = std::max(plan.desiredWorkers,
                                           std::min(16, activeBases * 12));
            plan.desiredBases = completedNexuses;
            plan.expansionTarget = {-1, -1};
            plan.deferExpansion = true;
            plan.sustainEconomy = true;
            plan.prioritizeReinforcements = false;
            goal(plan, GoalKind::train, UnitKind::probe,
                 std::min(16, activeBases * 12), 118,
                 "preserve the last mining income while searching for a legal base", true);
        }
    }

    const auto depletedEconomy = completedNexuses > 0 && activeBases < completedNexuses;
    const auto saturatedEconomy = activeBases > 0 && workers >= activeBases * 20;
    if (pendingNexuses == 0 && completedNexuses < 8 &&
        (depletedEconomy || saturatedEconomy)) {
        plan.desiredBases = std::max(plan.desiredBases, completedNexuses + 1);
        goal(plan, GoalKind::expand, UnitKind::nexus, plan.desiredBases, 86,
             depletedEconomy ? "replace a mined-out base" : "expand a saturated economy",
             depletedEconomy);
    }

    // BWAPI's Pylon field is narrower than two nearby building centers can
    // suggest. Keep recovery tasks conservative so each placement can be
    // confirmed by the stock engine instead of assuming a distant structure
    // will receive psi.
    constexpr int powerRecoveryRadius = 128;
    struct RecoverySite {
        const UnitSnapshot* anchor{};
    };
    std::vector<const UnitSnapshot*> unpoweredBuildings;
    for (const auto& unit : state.self.units) {
        if (unit.completed && isBuilding(unit.kind) &&
            unitStats(unit.kind).requiresPsi && !unit.powered) {
            unpoweredBuildings.push_back(&unit);
        }
    }
    std::ranges::sort(unpoweredBuildings, [](const UnitSnapshot* left,
                                             const UnitSnapshot* right) {
        return left->id < right->id;
    });

    std::vector<RecoverySite> recoverySites;
    auto needsUnlocatedRecovery = false;
    const auto coveredByPendingPylon = [&state](const Position position) {
        return std::ranges::any_of(state.self.units,
            [position](const UnitSnapshot& pylon) {
                return pylon.kind == UnitKind::pylon && !pylon.completed &&
                       pylon.position.valid() &&
                       withinPixelRadius(position, pylon.position, PixelRadius{powerRecoveryRadius});
            });
    };
    const auto coveredByExistingSite = [&plan](const Position position) {
        return std::ranges::any_of(plan.goals,
            [position](const ProductionGoal& candidate) {
                return candidate.goal == GoalKind::build &&
                       candidate.target == UnitKind::pylon &&
                       candidate.constructionSite.valid() &&
                       withinPixelRadius(position, candidate.constructionSite.anchor, PixelRadius{powerRecoveryRadius});
            });
    };
    for (const auto* building : unpoweredBuildings) {
        if (!building->position.valid()) {
            needsUnlocatedRecovery = true;
            continue;
        }
        if (coveredByPendingPylon(building->position) ||
            coveredByExistingSite(building->position)) continue;
        const auto alreadyGrouped = std::ranges::any_of(
            recoverySites, [building](const RecoverySite& site) {
                return withinPixelRadius(building->position, site.anchor->position, PixelRadius{powerRecoveryRadius});
            });
        if (alreadyGrouped) continue;
        recoverySites.push_back({building});
    }

    for (const auto& site : recoverySites) {
        if (site.anchor->id < 0) {
            needsUnlocatedRecovery = true;
            continue;
        }
        const auto taskId = 0x100000000ULL +
            static_cast<std::uint32_t>(site.anchor->id);
        const ConstructionTaskSite constructionSite{
            taskId, -1, site.anchor->position};
        goal(plan, GoalKind::build, UnitKind::pylon, 1, 98,
             "restore power to disabled production", true);
        plan.goals.back().constructionSite = constructionSite;
    }
    if (needsUnlocatedRecovery) {
        const auto anyPendingPylon = std::ranges::any_of(
            state.self.units, [](const UnitSnapshot& unit) {
                return unit.kind == UnitKind::pylon && !unit.completed;
            });
        if (!anyPendingPylon) {
            const auto pylons = effectiveUnitCount(state, UnitKind::pylon, UnitCountBasis::observed);
            goal(plan, GoalKind::build, UnitKind::pylon, pylons + 1, 98,
                 "restore power to disabled production", true);
        }
    }

    // A large mineral bank means production, not another passive combat-unit
    // target, is the bottleneck. Scale infrastructure with the live economy.
    if (state.self.minerals >= 900 && completedNexuses > 0) {
        const auto targetGateways = std::clamp(completedNexuses * 3, 3, 12);
        goal(plan, GoalKind::build, UnitKind::gateway, targetGateways, 73,
             "convert excess mineral bank into production");
        if (state.self.gas >= 500 && minute(state) >= 12 &&
            state.enemy.race != Race::zerg) {
            goal(plan, GoalKind::build, UnitKind::stargate,
                 std::clamp(completedNexuses, 1, 4), 61,
                 "add a late-game production branch");
        }
    }
}

void StrategyEngine::applyOpeningStyle(
    StrategicPlan& plan,
    const GameState& state,
    const OpeningStyle style) {
    const auto emergency = plan.posture == Posture::defend ||
                           plan.posture == Posture::recover;
    switch (style) {
        case OpeningStyle::standard: return;
        case OpeningStyle::aggressive:
            plan.name += " [pressure]";
            if (!emergency) {
                plan.posture = minute(state) < 5 ? Posture::hold : Posture::pressure;
            }
            if (minute(state) < 8) {
                plan.desiredBases = std::max(1, plan.desiredBases - 1);
            }
            plan.attackThreshold = std::max(1.05, plan.attackThreshold - 0.10);
            plan.minimumAttackSize = std::max(6, plan.minimumAttackSize - 2);
            if (supplyAtLeast(state, DisplayedSupply{10})) {
                goal(plan, GoalKind::build, UnitKind::gateway,
                     minute(state) < 8 ? 2 : 5, 91,
                     "opponent-specific pressure production");
            }
            if (supplyAtLeast(state, DisplayedSupply{14})) {
                goal(plan, GoalKind::train, UnitKind::dragoon,
                     std::max(5, minute(state) * 2), 87,
                     "opponent-specific pressure army");
            }
            return;
        case OpeningStyle::economic:
            plan.name += " [economic]";
            // Opponent-history exploration is a preference, not authority to
            // ignore a rush that is already visible inside our main.
            if (emergency) return;
            plan.posture = minute(state) < 9 ? Posture::hold : plan.posture;
            plan.desiredBases = std::min(4, plan.desiredBases + (minute(state) >= 5 ? 1 : 0));
            plan.desiredWorkers = std::min(76, plan.desiredWorkers + 6);
            plan.attackThreshold += 0.12;
            plan.minimumAttackSize += 2;
            goal(plan, GoalKind::expand, UnitKind::nexus, plan.desiredBases, 72,
                 "opponent-specific economic edge");
            return;
        case OpeningStyle::deceptive:
            plan.name += " [tech switch]";
            if (emergency) return;
            plan.attackThreshold += 0.05;
            if (minute(state) < 5 && !supplyAtLeast(state, DisplayedSupply{24})) return;
            if (state.enemy.race == Race::zerg) {
                goal(plan, GoalKind::build, UnitKind::roboticsFacility, 1, 79,
                     "reaver tech switch");
                goal(plan, GoalKind::build, UnitKind::roboticsSupportBay, 1, 78,
                     "reaver tech switch");
                goal(plan, GoalKind::train, UnitKind::reaver, 2, 77,
                     "punish static anti-air");
                goal(plan, GoalKind::train, UnitKind::shuttle, 1, 76,
                     "deliver tech switch");
            } else {
                goal(plan, GoalKind::build, UnitKind::citadelOfAdun, 1, 82,
                     "dark templar tech switch");
                goal(plan, GoalKind::build, UnitKind::templarArchives, 1, 81,
                     "dark templar tech switch");
                goal(plan, GoalKind::train, UnitKind::darkTemplar, 3, 80,
                     "punish weak detection");
            }
            return;
        case OpeningStyle::count: return;
    }
}

StrategicPlan StrategicDirector::stabilize(
    StrategicPlan candidate,
    const GameState& state,
    const ThreatAssessment& threat) {
    constexpr auto clearWindow = framesForSeconds(8);
    // A defensive candidate is not evidence by itself: the matchup planner
    // can remain defensive for several frames after a threat has cleared.
    // Only reset the emergency timer for a current breach, a meaningful army
    // approach, explicit rush/proxy evidence, or a very early high-pressure
    // opening. This prevents a stale nearby army from self-sustaining Defend.
    const auto directBreach = hardBreachAtMain(state);
    const auto home = ourMain(state);
    const auto visibleHomeContact = candidate.posture == Posture::defend &&
        hasVisibleGroundCombatContact(
            state, home.valid() ? std::optional<Position>{home} : std::nullopt,
            PixelRadius{800});
    const auto inferredBreach = threat.combatEnemiesNearMain > 0 &&
                                (state.enemy.units.empty() || directBreach);
    const auto meaningfulApproach = !candidate.breakContainment && activeApproach(state, threat) &&
                                    (directBreach ||
                                     threat.approachingArmyValue >= 10.0);
    const auto emergencyEvidence = candidate.posture == Posture::recover ||
                                   visibleHomeContact || inferredBreach || meaningfulApproach ||
                                   threat.workerRush > 0.30 ||
                                   threat.proxy + threat.staticContain > 0.34 ||
                                   (state.frame < framesForMinutes(8) &&
                                    threat.immediateGround > 0.60 &&
                                    (state.enemy.units.empty() || directBreach));

    if (!initialized_) {
        initialized_ = true;
        posture_ = candidate.posture;
        if (emergencyEvidence) lastEmergencyFrame_ = state.frame;
    } else if (emergencyEvidence) {
        posture_ = candidate.posture == Posture::recover
                       ? Posture::recover
                       : Posture::defend;
        lastEmergencyFrame_ = state.frame;
        candidate.posture = posture_;
    } else if ((posture_ == Posture::defend || posture_ == Posture::recover) &&
        lastEmergencyFrame_ >= 0 &&
        state.frame - lastEmergencyFrame_ < clearWindow) {
        candidate.posture = posture_;
        candidate.attackThreshold = std::max(candidate.attackThreshold, 1.40);
        candidate.name += " [regrouping after defense]";
    } else {
        posture_ = candidate.posture;
    }

    const auto completedBases = static_cast<int>(std::ranges::count_if(
        state.self.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::nexus && unit.completed;
        }));
    const auto workerUnderAttack = std::ranges::any_of(
        state.self.units, [](const UnitSnapshot& unit) {
            return isWorker(unit.kind) && unit.underAttack;
        });
    const auto explicitEconomicEmergency =
        candidate.name.find("[reinforce threatened economy]") != std::string::npos ||
        candidate.name.find("[survival:") != std::string::npos ||
        candidate.name.find("[supply-cap closeout]") != std::string::npos ||
        candidate.name.find("[decisive-lead closeout]") != std::string::npos;
    const auto hardExpansionStop = emergencyEvidence || directBreach || workerUnderAttack ||
        candidate.posture == Posture::defend || candidate.posture == Posture::recover ||
        candidate.posture == Posture::attack || candidate.deferExpansion ||
        candidate.recoveringLastNexus || explicitEconomicEmergency;
    const auto clearExpansionCommitment = [this] {
        expansionCommitment_ = {-1, -1};
        lastExpansionRequestFrame_ = -1;
        committedExpansionBases_ = 0;
    };
    if (hardExpansionStop) {
        clearExpansionCommitment();
        return candidate;
    }

    constexpr auto expansionCommitmentWindow = framesForSeconds(8);
    const auto requestedExpansion = candidate.expansionTarget.valid() &&
        candidate.desiredBases > completedBases && !candidate.deferExpansion;
    const auto recentCommitment = expansionCommitment_.valid() &&
        lastExpansionRequestFrame_ >= 0 && state.frame >= lastExpansionRequestFrame_ &&
        state.frame - lastExpansionRequestFrame_ <= expansionCommitmentWindow &&
        committedExpansionBases_ > completedBases;
    const auto commitmentSiteStillOpen = recentCommitment &&
        std::ranges::any_of(state.bases, [this](const BaseSnapshot& base) {
            return base.ownerId == -1 && !base.island && base.depotFootprintAvailable &&
                base.center.valid() && base.mineralLine.valid() && base.mineralPatches >= 4 &&
                base.mineralsRemaining >= 4000 && base.groundDistanceFromMain >= 0 &&
                distanceSquared(base.center, expansionCommitment_) <= 96 * 96;
        });
    const auto commitmentNexusWarping = recentCommitment &&
        std::ranges::any_of(state.self.units, [this](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::nexus && !unit.completed && unit.position.valid() &&
                distanceSquared(unit.position, expansionCommitment_) <= 96 * 96;
        });
    const auto priorSiteAvailable = commitmentSiteStillOpen || commitmentNexusWarping;
    const auto requestedDifferentSite = requestedExpansion && priorSiteAvailable &&
        distanceSquared(candidate.expansionTarget, expansionCommitment_) > 96 * 96;
    if (requestedExpansion && !requestedDifferentSite) {
        expansionCommitment_ = candidate.expansionTarget;
        lastExpansionRequestFrame_ = state.frame;
        committedExpansionBases_ = std::max(candidate.desiredBases, completedBases + 1);
        return candidate;
    }

    if (!priorSiteAvailable) {
        clearExpansionCommitment();
        return candidate;
    }

    // Pressure and tech observations can flicker for a few frames after an
    // expansion has already drawn the army toward its site. Keep that one
    // economic objective through a short transition even if the planner names
    // another viable site; otherwise a small plan change can reverse the force
    // across the map on every strategy callback. Actual defense, worker danger,
    // and explicit economy/recovery vetoes above still cancel it immediately;
    // ExpansionCoordinator continues to validate route/site safety and can
    // choose an alternative.
    candidate.expansionTarget = expansionCommitment_;
    candidate.rallyPoint = expansionCommitment_;
    candidate.desiredBases = std::max(candidate.desiredBases, committedExpansionBases_);
    candidate.maximumBases = std::max(candidate.maximumBases, candidate.desiredBases);
    candidate.sustainEconomy = true;
    candidate.name += " [maintaining expansion commitment]";
    const auto expansionGoal = std::ranges::find_if(candidate.goals,
        [](const ProductionGoal& goal) {
            return goal.goal == GoalKind::expand && goal.target == UnitKind::nexus;
        });
    if (expansionGoal == candidate.goals.end()) {
        goal(candidate, GoalKind::expand, UnitKind::nexus, candidate.desiredBases,
             120, "maintain the committed expansion through transient pressure", true);
    } else {
        expansionGoal->desiredCount = std::max(expansionGoal->desiredCount,
                                               candidate.desiredBases);
        expansionGoal->priority = std::max(expansionGoal->priority, 120);
        expansionGoal->blocking = true;
        expansionGoal->reason = "maintain the committed expansion through transient pressure";
    }
    std::ranges::stable_sort(candidate.goals, std::greater{}, &ProductionGoal::priority);
    return candidate;
}

void StrategicDirector::reset() noexcept {
    posture_ = Posture::hold;
    lastEmergencyFrame_ = -1;
    initialized_ = false;
    expansionCommitment_ = {-1, -1};
    lastExpansionRequestFrame_ = -1;
    committedExpansionBases_ = 0;
}

std::string_view postureName(const Posture posture) noexcept {
    switch (posture) {
        case Posture::hold: return "Hold";
        case Posture::defend: return "Defend";
        case Posture::pressure: return "Pressure";
        case Posture::attack: return "Attack";
        case Posture::harass: return "Harass";
        case Posture::recover: return "Recover";
    }
    return "Invalid";
}

std::string_view openingStyleName(const OpeningStyle style) noexcept {
    switch (style) {
        case OpeningStyle::standard: return "standard";
        case OpeningStyle::aggressive: return "aggressive";
        case OpeningStyle::economic: return "economic";
        case OpeningStyle::deceptive: return "deceptive";
        case OpeningStyle::count: break;
    }
    return "invalid";
}

}  // namespace protodd
