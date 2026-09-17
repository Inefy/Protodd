#include "protodd/Strategy.hpp"
#include "protodd/Technology.hpp"
#include "protodd/Combat.hpp"
#include "protodd/Harassment.hpp"

#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace protodd {
namespace {

int count(const GameState& state, const UnitKind kind, const bool completedOnly = false) {
    return static_cast<int>(std::ranges::count_if(
        state.self.units,
        [kind, completedOnly](const UnitSnapshot& unit) {
            return unit.kind == kind && (!completedOnly || unit.completed);
        }));
}

int countRole(const GameState& state, const UnitRole role) {
    return static_cast<int>(std::ranges::count(state.self.units, role, &UnitSnapshot::role));
}

int minute(const GameState& state) {
    return state.frame / (24 * 60);
}

bool supplyAtLeast(const GameState& state, const int displayedSupply) {
    return state.self.supplyUsed >= displayedSupply * 2;
}

bool activeApproach(const GameState& state, const ThreatAssessment& threat) noexcept {
    // Early scouting should react to an army crossing the map. Once the game
    // is established, perimeter movement alone is not enough to keep the
    // economy and army permanently defensive; require a current breach or
    // explicit rush evidence instead.
    return state.frame < 10 * 60 * 24 && threat.approachingArmyValue >= 2.0;
}

bool openingPressureExpected(
    const GameState& state,
    const ThreatAssessment& threat) noexcept {
    const auto supported = threat.uncertainty <= 0.75 ||
                           threat.combatEnemiesNearMain > 0 ||
                           activeApproach(state, threat) ||
                           threat.immediateGround > 0.45;
    // A nearly uniform belief distribution still has a numerical winner.
    // Its stale label alone must not hold a ready army at home for 16 minutes.
    return minute(state) < 16 && supported &&
           (threat.mostLikely == EnemyPlan::fastRush ||
            threat.mostLikely == EnemyPlan::heavyPressure);
}

int recentEnemyCount(
    const GameState& state,
    const UnitKind kind,
    const Frame memory = 90 * 24) {
    return static_cast<int>(std::ranges::count_if(
        state.enemy.units, [kind, memory, &state](const UnitSnapshot& unit) {
            return unit.kind == kind && unit.completed &&
                   (unit.visible || state.frame - unit.lastSeen <= memory);
        }));
}

void setCompositionWeight(
    StrategicPlan& plan,
    const UnitKind kind,
    const double minimumWeight) {
    const auto existing = std::ranges::find(plan.composition, kind,
                                             &CompositionTarget::kind);
    if (existing == plan.composition.end()) {
        plan.composition.push_back({kind, minimumWeight});
    } else {
        existing->weight = std::max(existing->weight, minimumWeight);
    }
}

void normalizeComposition(StrategicPlan& plan) {
    const auto total = std::accumulate(
        plan.composition.begin(), plan.composition.end(), 0.0,
        [](const double sum, const CompositionTarget& target) {
            return sum + std::max(0.0, target.weight);
        });
    if (total <= 0.0) return;
    for (auto& target : plan.composition) target.weight /= total;
}

void goal(
    StrategicPlan& plan,
    const GoalKind kind,
    const UnitKind target,
    const int desired,
    const int priority,
    const std::string_view reason,
    const bool blocking = false) {
    plan.goals.push_back({kind, target, desired, priority, blocking, std::string(reason)});
}

void technologyGoal(
    StrategicPlan& plan,
    const TechnologyKind technology,
    const int desiredLevel,
    const int priority,
    const std::string_view reason,
    const bool blocking = false) {
    const auto kind = technology == TechnologyKind::psionicStorm ||
                              technology == TechnologyKind::stasisField ||
                              technology == TechnologyKind::recall
                          ? GoalKind::research
                          : GoalKind::upgrade;
    const auto existing = std::ranges::find(
        plan.goals, technology, &ProductionGoal::technology);
    if (existing != plan.goals.end()) {
        existing->desiredCount = std::max(existing->desiredCount, desiredLevel);
        if (priority > existing->priority) existing->reason = reason;
        existing->priority = std::max(existing->priority, priority);
        existing->blocking = existing->blocking || blocking;
        return;
    }
    plan.goals.push_back({kind, UnitKind::unknown, desiredLevel, priority, blocking,
                          std::string(reason), technology});
}

Position ourMain(const GameState& state) {
    const auto nexus = std::ranges::find(state.self.units, UnitKind::nexus, &UnitSnapshot::kind);
    return nexus != state.self.units.end() ? nexus->position : Position{-1, -1};
}

Position enemyMain(const GameState& state) {
    const auto depot = std::ranges::find_if(state.enemy.units, [](const UnitSnapshot& unit) {
        return unit.role == UnitRole::resourceDepot;
    });
    if (depot != state.enemy.units.end()) {
        return depot->position;
    }

    // Keep attacking known structures after the last depot falls. This avoids
    // the common cleanup failure where an army returns home while a tech
    // building survives elsewhere on the map.
    const auto building = std::ranges::find_if(state.enemy.units, [](const UnitSnapshot& unit) {
        return isBuilding(unit.kind) && unit.position.valid();
    });
    if (building != state.enemy.units.end()) return building->position;

    // Before the enemy start is confirmed, search the stalest plausible start.
    // Explicitly exclude our own start; the previous ownerId != -1 fallback
    // selected Protodd's main as soon as its Nexus was observed.
    const BaseSnapshot* candidate = nullptr;
    auto oldest = std::numeric_limits<Frame>::max();
    for (const auto& base : state.bases) {
        if (!base.startLocation || !base.center.valid() || base.ownerId == state.self.id) continue;
        if (base.ownerId == state.enemy.id) return base.center;
        if (base.ownerId == -1 && base.lastScouted < oldest) {
            oldest = base.lastScouted;
            candidate = &base;
        }
    }
    if (candidate != nullptr) return candidate->center;

    const auto visibleTarget = std::ranges::find_if(
        state.enemy.units, [](const UnitSnapshot& unit) {
            return unit.visible && unit.position.valid();
        });
    if (visibleTarget != state.enemy.units.end()) return visibleTarget->position;

    // Finally sweep stale non-owned expansions to reveal hidden buildings.
    oldest = std::numeric_limits<Frame>::max();
    for (const auto& base : state.bases) {
        if (!base.center.valid() || base.ownerId == state.self.id) continue;
        if (base.lastScouted < oldest) {
            oldest = base.lastScouted;
            candidate = &base;
        }
    }
    return candidate != nullptr ? candidate->center : Position{-1, -1};
}

Position nearestExpansionSite(const GameState& state) {
    const auto home = ourMain(state);
    if (!home.valid()) return {-1, -1};

    // BaseSnapshot centers are the canonical depot centers discovered by the
    // BWAPI bridge.  Pick the closest non-island, unowned resource base so a
    // strategic Nexus target is explicit before macro placement runs.  The
    // previous planners could request a second base without ever naming one;
    // the bridge then used the combat rally point and was free to select a
    // forward or otherwise inappropriate resource cluster.
    const auto gasBaseAvailable = std::ranges::any_of(
        state.bases, [](const BaseSnapshot& base) {
            return base.ownerId == -1 && !base.island && base.center.valid() &&
                   base.mineralsRemaining >= 1000 && base.geysers > 0;
        });
    const BaseSnapshot* best = nullptr;
    auto bestScore = std::numeric_limits<double>::infinity();
    for (const auto& base : state.bases) {
        if (base.ownerId != -1 || base.island || !base.center.valid() ||
            base.mineralsRemaining < 1000) continue;
        // The first expansion must be the normal gas natural when one exists.
        // Mineral-only pockets are useful later, but on maps such as
        // Destination one can look closer across a cliff while actually being
        // the fourth base along the ground route.
        if (gasBaseAvailable && base.geysers == 0) continue;
        const auto score = base.groundDistanceFromMain >= 0
                               ? static_cast<double>(base.groundDistanceFromMain)
                               : distance(home, base.center);
        if (score < bestScore ||
            (score == bestScore && (best == nullptr || base.id < best->id))) {
            bestScore = score;
            best = &base;
        }
    }
    return best != nullptr ? best->center : Position{-1, -1};
}

bool hardBreachAtMain(const GameState& state) noexcept {
    const auto home = ourMain(state);
    if (!home.valid()) return false;
    return std::ranges::any_of(
        state.enemy.units, [&home](const UnitSnapshot& enemy) {
            return enemy.visible && !enemy.flying && isCombatUnit(enemy.kind) &&
                   enemy.position.valid() &&
                   distanceSquared(enemy.position, home) <= 320 * 320;
        });
}

bool defensiveExpansionWindow(const GameState& state, const ThreatAssessment& threat) {
    // Holding an army at home and growing the economy are separate decisions.
    // Keep the rush/contain/detection vetoes, but don't require an attack order
    // before spending a saturated mineral line's income on another base.
    if (state.frame < 6 * 60 * 24 || hardBreachAtMain(state) ||
        threat.combatEnemiesNearMain > 0 || activeApproach(state, threat) ||
        threat.immediateGround > 0.45 || threat.workerRush > 0.30 ||
        threat.proxy + threat.staticContain > 0.34 ||
        (threat.cloak > 0.28 && count(state, UnitKind::observer, true) == 0)) return false;
    const auto mobileArmy = std::ranges::count_if(state.self.units, [](const UnitSnapshot& unit) {
        return unit.completed && !unit.hallucination && !isBuilding(unit.kind) &&
               isCombatUnit(unit.kind) && !isWorker(unit.kind);
    });
    return mobileArmy >= 6;
}

}  // namespace

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
    addEconomicRecovery(result, state);
    // Safety runs last so an opponent-specific economic style cannot override
    // direct evidence of an all-in at our main.
    addSafetyReactions(result, threat);

    const auto visibleGroundContact = std::ranges::any_of(
        state.enemy.units, [&home](const UnitSnapshot& enemy) {
            return enemy.visible && enemy.completed && !enemy.flying &&
                   isCombatUnit(enemy.kind) && enemy.position.valid() &&
                   (!home.valid() || distanceSquared(home, enemy.position) <=
                                       800 * 800);
        });
    const auto visibleProtossContact = state.enemy.race == Race::protoss &&
        std::ranges::any_of(state.enemy.units, [](const UnitSnapshot& enemy) {
            return enemy.visible && enemy.completed && !enemy.flying &&
                   isCombatUnit(enemy.kind) && enemy.position.valid();
        });
    const auto forwardCounter = visibleProtossContact &&
        state.frame >= 7 * 60 * 24 && state.frame < 11 * 60 * 24 &&
        count(state, UnitKind::zealot, true) >= 6 &&
        count(state, UnitKind::dragoon, true) >= 2 &&
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
            (!unit.visible && state.frame - unit.lastSeen > 60 * 24)) continue;
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
            const auto committedBases = count(state, UnitKind::nexus);
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
    const auto existingNexuses = count(state, UnitKind::nexus);
    const auto rangedMirrorExpansion = state.enemy.race == Race::protoss &&
        (std::ranges::any_of(state.enemy.units, [](const UnitSnapshot& enemy) {
             return enemy.kind == UnitKind::cyberneticsCore && enemy.position.valid();
         }) ||
         recentEnemyCount(state, UnitKind::dragoon) > 0 ||
         recentEnemyCount(state, UnitKind::reaver) > 0);
    const auto establishedRangedLead = count(state, UnitKind::dragoon, true) >= 8 &&
        count(state, UnitKind::observer, true) > 0 && recentEnemyMobilePower > 0.0 &&
        ownMobilePower >= recentEnemyMobilePower * 3.0;
    const auto splashScreenTarget = recentEnemyCount(state, UnitKind::dragoon) >= 5 ||
        recentEnemyCount(state, UnitKind::reaver) > 0 ? 2 : 1;
    if (existingNexuses == 1 && result.desiredBases > 1 && rangedMirrorExpansion &&
        count(state, UnitKind::reaver, true) < splashScreenTarget &&
        !establishedRangedLead && minute(state) < 12) {
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
        if (count(state, UnitKind::dragoon, true) >= 4) {
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
                distanceSquared(enemy.position, base.center) > 640 * 640) continue;
            attackers += unitStats(enemy.kind).combatValue * std::clamp(enemy.healthFraction(), 0.25, 1.0);
            breached = breached || distanceSquared(enemy.position, base.center) <= 320 * 320;
        }
        if (attackers <= 0.0) continue;
        for (const auto& ally : state.self.units) {
            if (!ally.completed || ally.disabled || ally.loaded || ally.hallucination ||
                (!isCombatUnit(ally.kind) && !isStaticDefense(ally.kind)) ||
                distanceSquared(ally.position, base.center) > 800 * 800) continue;
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
        result.desiredWorkers = std::min(result.desiredWorkers, std::max(12, count(state, UnitKind::probe)));
        result.name += " [reinforce threatened economy]";
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
        const auto existingBases = std::max(1, count(state, UnitKind::nexus));
        result.desiredBases = std::min(result.desiredBases, existingBases);
    }
    // Grow into a paid-for Nexus while it warps in, not into an expansion
    // that is merely desired. Otherwise repeated hold/pressure transitions
    // train two bases' workers on one mineral line and consume its Nexus bank.
    const auto committedBases = count(state, UnitKind::nexus);
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
    result.requireMobileDetection = threat.cloak > 0.28 ||
        recentEnemyCount(state, UnitKind::spiderMine) > 0 ||
        recentEnemyCount(state, UnitKind::lurker) > 0 ||
        recentEnemyCount(state, UnitKind::darkTemplar) > 0 ||
        (state.enemy.race == Race::terran && minute(state) >= 5 &&
         ((threat.uncertainty > 0.65 && threat.combatEnemiesNearMain == 0 &&
           !activeApproach(state, threat) && threat.immediateGround <= 0.45) ||
          recentEnemyCount(state, UnitKind::factory) >= 2 ||
          recentEnemyCount(state, UnitKind::starport) > 0));
    if (result.requireMobileDetection) {
        result.desiredGasWorkers = std::max(3, result.desiredGasWorkers);
        goal(result, GoalKind::build, UnitKind::assimilator, 1, 123,
             "fund required mobile detection", true);
        goal(result, GoalKind::train, UnitKind::observer,
             count(state, UnitKind::nexus) >= 2 ? 3 : 2, 124,
             "replace and maintain mission detectors", true);
    }

    if (state.enemy.race == Race::protoss && count(state, UnitKind::reaver, true) < 2)
        std::erase_if(result.goals, [](const ProductionGoal& demand) {
            return demand.goal == GoalKind::train && demand.target == UnitKind::shuttle;
        });
    const auto rangedMirror = state.enemy.race == Race::protoss && minute(state) < 12 &&
        recentEnemyCount(state, UnitKind::dragoon) >= 3 &&
        recentEnemyCount(state, UnitKind::dragoon) >= 2 * recentEnemyCount(state, UnitKind::zealot);
    if (rangedMirror) {
        const auto meleeLimit = std::max(2, count(state, UnitKind::dragoon, true) / 3);
        result.composition = {{UnitKind::dragoon, 0.85}, {UnitKind::zealot, 0.05}, {UnitKind::reaver, 0.10}};
        for (auto& demand : result.goals) {
            if (demand.goal == GoalKind::train && demand.target == UnitKind::zealot)
                demand.desiredCount = std::min(demand.desiredCount, meleeLimit);
        }
        if (!result.sustainEconomy && count(state, UnitKind::dragoon, true) < 6 &&
            !hardBreachAtMain(state)) {
            result.posture = Posture::hold;
            result.minimumAttackSize = std::max(6, result.minimumAttackSize);
        }
    }
    // Reconcile all independent safety rules against the final observation.
    // Explicit fulfilled goals also clear MacroPlanner's older commitments;
    // simply erasing a demand lets yesterday's Cannon reservation survive.
    const auto suppressNew = [&result, &state](const UnitKind kind, const char* reason) {
        const auto existing = count(state, kind) + static_cast<int>(std::ranges::count(state.self.queuedUnits, kind));
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
                distanceSquared(home, enemy.position) <= 1200 * 1200;
        });
    const auto containedByRanged = state.enemy.race == Race::protoss &&
        count(state, UnitKind::cyberneticsCore, true) > 0 && !hardBreachAtMain(state) &&
        (visiblePerimeterRanged || threat.staticContain > 0.34) &&
        (recentEnemyCount(state, UnitKind::zealot) < 2 ||
         recentEnemyCount(state, UnitKind::dragoon) >= 2 * recentEnemyCount(state, UnitKind::zealot)) &&
        threat.workerRush <= 0.30 && threat.air <= 0.30;
    if (containedByRanged) {
        suppressNew(UnitKind::photonCannon, "break ranged containment with mobile units");
        suppressNew(UnitKind::forge, "fund the mobile breakout before more static defense");
        if (!result.sustainEconomy) result.prioritizeReinforcements = true;
        result.desiredGasWorkers = std::max(3, result.desiredGasWorkers);
        goal(result, GoalKind::train, UnitKind::dragoon, std::max(6, count(state, UnitKind::dragoon) + 1),
             114, "assemble a ranged breakout force", true);
        if (count(state, UnitKind::dragoon) > 0)
            technologyGoal(result, TechnologyKind::singularityCharge, 1, 115,
                           "range before spending into containment", true);
        result.name += " [mobile breakout]";
    }
    if (state.enemy.race == Race::protoss && !result.requireMobileDetection &&
        count(state, UnitKind::dragoon, true) < 6 && minute(state) < 10) {
        suppressNew(UnitKind::observer, "fund six Dragoons before optional scouting detection");
        suppressNew(UnitKind::observatory, "defer optional detection until the ranged screen is ready");
    }
    if (state.enemy.race == Race::protoss && count(state, UnitKind::roboticsFacility) > 0 &&
        (result.requireMobileDetection || count(state, UnitKind::dragoon, true) >= 6 || minute(state) >= 10) &&
        count(state, UnitKind::observer) == 0 &&
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
        count(state, UnitKind::forge) > 0 && count(state, UnitKind::photonCannon) == 0 &&
        count(state, UnitKind::zealot) >= 1) {
        goal(result, GoalKind::build, UnitKind::photonCannon, 1, 122,
             "complete the first melee-rush anchor before further Gateway cycles", true);
    }
    const auto rangeCommitted = technologyLevel(state.self, TechnologyKind::singularityCharge) > 0 ||
        technologyInProgress(state.self, TechnologyKind::singularityCharge);
    if (rangedMirrorExpansion && !hardBreachAtMain(state) && rangeCommitted &&
        count(state, UnitKind::dragoon, true) >= 2 && count(state, UnitKind::dragoon) >= 4 &&
        count(state, UnitKind::reaver) == 0) {
        // The four committed Dragoons already own their production resources.
        // Start the splash chain while they finish instead of buying several
        // further Gateway cycles before even reserving Robotics gas.
        goal(result, GoalKind::build, UnitKind::roboticsFacility, 1, 122,
             "overlap splash technology with the committed ranged screen", true);
        if (count(state, UnitKind::roboticsFacility) > 0)
            goal(result, GoalKind::build, UnitKind::roboticsSupportBay, 1, 122,
                 "finish splash technology before further Gateway cycles", true);
    }
    const auto reaversCommitted = count(state, UnitKind::reaver) +
        static_cast<int>(std::ranges::count(state.self.queuedUnits, UnitKind::reaver));
    const auto fundedSplashTarget = count(state, UnitKind::dragoon, true) >= 6 ? splashScreenTarget : 1;
    if (state.enemy.race == Race::protoss && reaversCommitted < fundedSplashTarget &&
        count(state, UnitKind::roboticsFacility) > 0 && count(state, UnitKind::roboticsSupportBay) > 0) {
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
    if ((result.posture == Posture::hold || result.posture == Posture::defend) &&
        !result.expansionTarget.valid() && !hardBreachAtMain(state)) {
        const auto base = std::ranges::find_if(state.bases, [&state, home](const BaseSnapshot& candidate) {
            return candidate.ownerId == state.self.id && candidate.defense.valid() &&
                   distanceSquared(candidate.center, home) <= 320 * 320;
        });
        if (base != state.bases.end()) result.rallyPoint = base->defense.anchor;
    }
    if (count(state, UnitKind::nexus, true) >= 2 && result.expansionTarget.valid() &&
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
    const auto spellEconomy = (count(state, UnitKind::nexus, true) >= 2 && count(state, UnitKind::probe) >= 28) ||
        (minute(state) >= 9 && count(state, UnitKind::probe) >= 22 && !hardBreachAtMain(state));
    const auto templarTransition = state.enemy.race == Race::protoss && spellEconomy &&
        ((count(state, UnitKind::reaver, true) >= 2 &&
          count(state, UnitKind::dragoon, true) + count(state, UnitKind::zealot, true) >= 10) ||
         count(state, UnitKind::templarArchives) > 0);
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
    addHarassmentProduction(result, state);
    std::ranges::stable_sort(result.goals, std::greater{}, &ProductionGoal::priority);
    return result;
}

StrategicPlan StrategyEngine::planPvT(
    const GameState& state,
    const ThreatAssessment& threat) const {
    StrategicPlan result;
    result.name = "PvT 28 Nexus";
    result.desiredWorkers = std::min(72, 22 + minute(state) * 4);
    const auto expansionReady = count(state, UnitKind::dragoon, true) >= 3 &&
                                supplyAtLeast(state, 28);
    result.desiredBases = expansionReady || count(state, UnitKind::nexus) >= 2 ?
                              (minute(state) < 11 ? 2 : 3) : 1;
    result.maximumBases = count(state, UnitKind::nexus) < 2 ? result.desiredBases : 8;
    result.desiredGasWorkers = !supplyAtLeast(state, 11) ? 0 :
                               (minute(state) < 7 ? 3 :
                                (minute(state) < 12 ? 6 : 9));
    result.posture = minute(state) < 6 || openingPressureExpected(state, threat)
                         ? Posture::hold
                         : Posture::pressure;
    result.attackThreshold = 1.32;
    result.minimumAttackSize = 14;
    result.composition = {{UnitKind::dragoon, 0.55}, {UnitKind::zealot, 0.22},
                          {UnitKind::highTemplar, 0.13}, {UnitKind::arbiter, 0.10}};

    const auto home = ourMain(state);
    const auto enemyHome = enemyMain(state);
    const auto forwardBio = minute(state) < 7 && home.valid() &&
        std::ranges::any_of(state.enemy.units, [home, enemyHome](const UnitSnapshot& enemy) {
            if (!enemy.visible || !enemy.completed || !enemy.position.valid() ||
                (enemy.kind != UnitKind::marine && enemy.kind != UnitKind::firebat)) return false;
            return distanceSquared(enemy.position, home) <= 1600 * 1600 ||
                (enemyHome.valid() && distanceSquared(enemy.position, home) <
                                      distanceSquared(enemy.position, enemyHome));
        });
    // Fortify only when current observations justify the economic cost.
    // Unknown Terran openings use the Gateway/Core baseline.
    const auto rushEvidence = forwardBio || threat.workerRush > 0.30 ||
        threat.proxy + threat.staticContain > 0.34 ||
        threat.combatEnemiesNearMain > 0 || activeApproach(state, threat) ||
        threat.immediateGround > 0.45;
    if (rushEvidence && minute(state) < 4 && count(state, UnitKind::zealot, true) == 0 &&
        count(state, UnitKind::photonCannon, true) == 0) {
        result.desiredWorkers = std::min(result.desiredWorkers, 7);
    }
    if (rushEvidence && supplyAtLeast(state, 7)) {
        goal(result, GoalKind::build, UnitKind::forge, 1, forwardBio ? 113 : 100,
             "fortified anti-bio opening anchor", true);
        goal(result, GoalKind::build, UnitKind::photonCannon, 2, forwardBio ? 112 : 99,
             "overlap the intercept before first contact", true);
        goal(result, GoalKind::build, UnitKind::gateway, 1, 98,
             "field mobile defense behind the completed static intercept", true);
    }

    if (supplyAtLeast(state, 10) || (rushEvidence && supplyAtLeast(state, 8))) {
        goal(result, GoalKind::build, UnitKind::gateway, minute(state) < 6 ? 1 : 3, 88,
             "Gateway opening before gas and Core", count(state, UnitKind::gateway) == 0);
    }
    if (count(state, UnitKind::gateway) > 0 || supplyAtLeast(state, 10)) {
        goal(result, GoalKind::train, UnitKind::zealot, 1, 98,
             "opening bodyguard before vulnerable dragoon tech",
             count(state, UnitKind::zealot) == 0);
    }
    if (rushEvidence && (count(state, UnitKind::gateway) > 0 || supplyAtLeast(state, 10))) {
        goal(result, GoalKind::build, UnitKind::shieldBattery, 1, 89,
             "opening sustain against bio pressure");
    }
    if (supplyAtLeast(state, 13)) {
        goal(result, GoalKind::build, UnitKind::cyberneticsCore, 1, 97,
             "thirteen-supply cybernetics core", true);
    }
    if (supplyAtLeast(state, 11)) {
        goal(result, GoalKind::build, UnitKind::assimilator, 1, 96,
             "opening gas for Dragoons and range", true);
    }
    if (supplyAtLeast(state, 14)) {
        goal(result, GoalKind::train, UnitKind::dragoon, std::max(3, minute(state) * 2), 91,
             "range control against Terran");
        if (count(state, UnitKind::dragoon) >= 1)
            technologyGoal(result, TechnologyKind::singularityCharge, 1, 98,
                           "range follows the first Dragoon before expansion", true);
    }
    if (count(state, UnitKind::nexus) >= 2 || rushEvidence || threat.cloak > 0.28) {
        goal(result, GoalKind::build, UnitKind::roboticsFacility, 1, 78,
             "observers against mines and tech scouting");
        goal(result, GoalKind::build, UnitKind::observatory, 1, 77, "observer access");
        goal(result, GoalKind::train, UnitKind::observer, minute(state) < 12 ? 2 : 4, 84,
             "mine detection and army tracking");
    }

    if (minute(state) >= 10) {
        goal(result, GoalKind::build, UnitKind::citadelOfAdun, 1, 68, "zealot speed path");
        goal(result, GoalKind::build, UnitKind::templarArchives, 1, 64,
             "storm and late-game composition");
        goal(result, GoalKind::train, UnitKind::highTemplar, 4, 60,
             "storm clustered bio and support tanks");
        technologyGoal(result, TechnologyKind::legEnhancements, 1, 69,
                       "speed closes on siege lines");
        technologyGoal(result, TechnologyKind::psionicStorm, 1, 67,
                       "enable templar before mass production", true);
    }
    if (minute(state) >= 15 && threat.air < 0.35) {
        goal(result, GoalKind::build, UnitKind::arbiterTribunal, 1, 52,
             "stasis and recall transition");
        goal(result, GoalKind::train, UnitKind::arbiter, 2, 50, "late-game control");
        technologyGoal(result, TechnologyKind::stasisField, 1, 56,
                       "neutralize clustered siege armies");
        technologyGoal(result, TechnologyKind::recall, 1, 48,
                       "create a late-game positional threat");
    }
    if (minute(state) >= 7) {
        const auto weaponLevel = minute(state) >= 18 ? 3 : (minute(state) >= 12 ? 2 : 1);
        technologyGoal(result, TechnologyKind::protossGroundWeapons, weaponLevel, 63,
                       "scale the core ground army");
    }
    if (minute(state) >= 13) {
        technologyGoal(result, TechnologyKind::khaydarinAmulet, 1, 54,
                       "increase storm availability");
        technologyGoal(result, TechnologyKind::protossGroundArmor,
                       minute(state) >= 19 ? 2 : 1, 51,
                       "improve zealot durability");
    }
    if (forwardBio || threat.combatEnemiesNearMain > 0 || activeApproach(state, threat) ||
        (minute(state) < 8 && threat.aggression > 0.62)) {
        result.name = "PvT anti-pressure hold";
        result.posture = Posture::defend;
        result.desiredBases = minute(state) < 8 ? 1 : std::min(result.desiredBases, 2);
        result.desiredWorkers = std::min(result.desiredWorkers, 14);
        result.attackThreshold = 1.55;
        if (count(state, UnitKind::gateway) > 0 &&
            count(state, UnitKind::zealot) == 0) {
            goal(result, GoalKind::train, UnitKind::zealot, 1, 105,
                 "field one mobile defender before adding more structures", true);
        }
        if (count(state, UnitKind::photonCannon, true) >= 2) {
            goal(result, GoalKind::build, UnitKind::pylon, 2, 102,
                 "give the forward defensive shell redundant power", true);
        }
        // Four Cannons are a screen, not the army. Continuing to replace an
        // aspirational six-Cannon target under fire can consume every mineral
        // and leave completed Gateways idle. Stop at four, then add the third
        // Gateway only after a mobile front line exists.
        const auto cannonTarget = minute(state) >= 5 ? 4 : 3;
        const auto gatewayTarget = minute(state) >= 5 &&
                                           count(state, UnitKind::zealot, true) >= 4
                                       ? 3
                                       : 2;
        goal(result, GoalKind::build, UnitKind::photonCannon, cannonTarget, 101,
             "scale the mineral-line anchor with sustained bio", true);
        goal(result, GoalKind::build, UnitKind::gateway, gatewayTarget, 100,
             "add emergency anti-pressure throughput", true);
        goal(result, GoalKind::build, UnitKind::shieldBattery, 1, 99,
             "complete a sustainable defensive screen", true);
        const auto screenEstablished =
            count(state, UnitKind::photonCannon, true) >= 3;
        if (screenEstablished) {
            // Once three Cannons are complete this checkpoint is permanent.
            // Tying the priority to a live Zealot count made one combat loss
            // demote the Core before its saved minerals could be spent.
            goal(result, GoalKind::build, UnitKind::cyberneticsCore, 1, 99,
                 "unlock the ranged counter after emergency production", true);
            goal(result, GoalKind::build, UnitKind::assimilator, 1, 99,
                 "fund the ranged counter after emergency production", true);
            const auto cloakWindow =
                count(state, UnitKind::photonCannon, true) >= 4;
            const auto cloakCommitted = count(state, UnitKind::citadelOfAdun) > 0 ||
                                        count(state, UnitKind::templarArchives) > 0 ||
                                        count(state, UnitKind::darkTemplar) > 0;
            if (count(state, UnitKind::cyberneticsCore, true) > 0 &&
                (cloakWindow || cloakCommitted)) {
                result.name += " [anti-bio dark templar]";
                goal(result, GoalKind::build, UnitKind::citadelOfAdun, 1, 99,
                     "cloak transition against sustained opening bio", true);
                if (count(state, UnitKind::citadelOfAdun, true) > 0) {
                    goal(result, GoalKind::build, UnitKind::templarArchives, 1, 99,
                         "complete the cloak transition", true);
                }
                if (count(state, UnitKind::templarArchives, true) > 0) {
                    goal(result, GoalKind::train, UnitKind::darkTemplar, 3, 102,
                         "clear bio and counterattack before detection", true);
                    setCompositionWeight(result, UnitKind::darkTemplar, 0.24);
                }
            }
        }
        goal(result, GoalKind::train, UnitKind::zealot,
             minute(state) >= 5 ? 10 : 6, 98,
             "field bodies before vulnerable dragoon tech", true);
        const auto dragoonInfrastructureReady =
            count(state, UnitKind::cyberneticsCore, true) > 0;
        const auto rangedReservationSafe =
            dragoonInfrastructureReady && state.self.gas >= 50 &&
            count(state, UnitKind::photonCannon, true) >= 2;
        if (dragoonInfrastructureReady && count(state, UnitKind::dragoon) == 0) {
            goal(result, GoalKind::train, UnitKind::dragoon, 1, 104,
                 "field the first ranged defender before support infrastructure",
                 rangedReservationSafe);
        }
        goal(result, GoalKind::train, UnitKind::dragoon, 8,
             dragoonInfrastructureReady ? 99 : 96,
             "transition to ranged defense after its prerequisite completes",
             rangedReservationSafe);
    }
    return result;
}

StrategicPlan StrategyEngine::planPvZ(
    const GameState& state,
    const ThreatAssessment& threat) const {
    StrategicPlan result;
    result.name = "PvZ fortified gateway into corsair-templar";
    result.desiredWorkers = std::min(70, 20 + minute(state) * 4);
    result.desiredBases = minute(state) < 3 ? 1 : (minute(state) < 11 ? 2 : 3);
    result.desiredGasWorkers = !supplyAtLeast(state, 14) ? 0 :
                               (minute(state) < 8 ? 3 :
                                (minute(state) < 12 ? 6 : 9));
    result.posture = minute(state) < 8 ? Posture::hold : Posture::harass;
    result.attackThreshold = 1.2;
    result.minimumAttackSize = 14;
    result.composition = {{UnitKind::zealot, 0.35}, {UnitKind::dragoon, 0.12},
                          {UnitKind::highTemplar, 0.25}, {UnitKind::corsair, 0.18},
                          {UnitKind::archon, 0.10}};

    // A pool-first Zerg can make contact before a conventional Gateway army
    // has enough surface area. Pause briefly at eight workers and establish a
    // static anchor; resume Probe growth as soon as either that anchor or two
    // Zealots are complete. This remains safe even when the first scout dies.
    if (minute(state) < 4 && count(state, UnitKind::zealot, true) < 2 &&
        count(state, UnitKind::photonCannon, true) == 0) {
        result.desiredWorkers = std::min(result.desiredWorkers, 8);
    }

    if (supplyAtLeast(state, 10) && count(state, UnitKind::gateway) >= 2 &&
        count(state, UnitKind::zealot) >= 2) {
        goal(result, GoalKind::build, UnitKind::forge, 1, 82,
             "forge after two-gate opening safety");
    }
    if (minute(state) >= 4 && threat.immediateGround <= 0.45 &&
        count(state, UnitKind::forge) > 0) {
        technologyGoal(result, TechnologyKind::protossGroundWeapons, 1, 87,
                       "zealot attack timing");
    }
    const auto defensiveBases = std::max(1, count(state, UnitKind::nexus));
    const auto openingGroundSafe = count(state, UnitKind::gateway) >= 2 &&
                                   count(state, UnitKind::zealot) >= 3;
    const auto canCommitToForge = count(state, UnitKind::forge) > 0 ||
                                  openingGroundSafe || minute(state) >= 4;
    if (canCommitToForge &&
        (defensiveBases >= 2 || threat.immediateGround > 0.25 || threat.air > 0.35)) {
        const auto safetyCannons = defensiveBases + (threat.air > 0.45 ? 2 : 0);
        goal(result, GoalKind::build, UnitKind::photonCannon,
             std::min(6, safetyCannons), 89, "ling and mutalisk coverage");
    }
    if (supplyAtLeast(state, 7)) {
        goal(result, GoalKind::build, UnitKind::forge, 1, 100,
             "fortified PvZ opening anchor", true);
        goal(result, GoalKind::build, UnitKind::photonCannon, 1, 99,
             "baseline anti-ling safety before economic commitment", true);
        const auto openingGateways = supplyAtLeast(state, 8) ? 2 : 1;
        goal(result, GoalKind::build, UnitKind::gateway,
             minute(state) < 8 ? openingGateways : 4,
             openingGateways == 1 ? 98 : 97,
             "seven-supply gateway into two-gate Zerg safety",
             count(state, UnitKind::gateway) < openingGateways);
        goal(result, GoalKind::train, UnitKind::zealot,
             std::max(4, minute(state)), 99,
             "opening defenders before Forge economy",
             count(state, UnitKind::zealot) < 3);
    }
    if (supplyAtLeast(state, 15)) {
        goal(result, GoalKind::build, UnitKind::cyberneticsCore, 1, 82,
             "air and dragoon access");
    }
    if (supplyAtLeast(state, 22)) {
        goal(result, GoalKind::build, UnitKind::stargate, 1, 76,
             "overlord denial and scouting");
        goal(result, GoalKind::train, UnitKind::corsair, threat.air > 0.45 ? 7 : 4, 75,
             "air superiority");
    }
    if (supplyAtLeast(state, 28)) {
        goal(result, GoalKind::build, UnitKind::citadelOfAdun, 1, 73,
             "speed and templar path");
        goal(result, GoalKind::build, UnitKind::templarArchives, 1, 71,
             "storm versus Zerg mass");
        goal(result, GoalKind::train, UnitKind::highTemplar,
             std::max(2, minute(state) / 3), 72, "storm support");
    }

    if (minute(state) >= 8) {
        technologyGoal(result, TechnologyKind::legEnhancements, 1, 78,
                       "speed for surrounds and reinforcement");
        technologyGoal(result, TechnologyKind::psionicStorm, 1, 77,
                       "storm is the core anti-swarm tool", true);
    }
    if (minute(state) >= 11) {
        technologyGoal(result, TechnologyKind::khaydarinAmulet, 1, 65,
                       "sustain repeated storms");
        technologyGoal(result, TechnologyKind::protossAirWeapons, 1, 58,
                       "keep corsairs ahead of mutalisks");
    }

    if (threat.combatEnemiesNearMain > 0 || activeApproach(state, threat) ||
        (minute(state) < 8 && threat.immediateGround > 0.45)) {
        result.name = "PvZ emergency gateway hold";
        result.posture = Posture::defend;
        result.desiredBases = 1;
        const auto stabilized = count(state, UnitKind::photonCannon, true) >= 2 &&
                                count(state, UnitKind::zealot, true) >= 4;
        if (!stabilized && minute(state) < 7) result.desiredGasWorkers = 0;
        else result.desiredGasWorkers = std::max(3, result.desiredGasWorkers);
        result.desiredWorkers = std::min(result.desiredWorkers, 12);
        goal(result, GoalKind::build, UnitKind::gateway, 2, 99, "anti-rush production", true);
        goal(result, GoalKind::train, UnitKind::zealot, 8, 98, "hold early ground rush", true);
        if (minute(state) < 6) {
            goal(result, GoalKind::build, UnitKind::shieldBattery, 1, 96,
                 "sustain the anti-ling screen", true);
        }
        goal(result, GoalKind::build, UnitKind::photonCannon, 2, 100,
             "double mineral-line cover against the ground all-in");
        if (count(state, UnitKind::photonCannon, true) >= 2 &&
            count(state, UnitKind::probe) < 10) {
            goal(result, GoalKind::train, UnitKind::probe, 10, 100,
                 "recover mining behind completed static safety", true);
        }
        if (count(state, UnitKind::photonCannon, true) >= 2) {
            goal(result, GoalKind::build, UnitKind::pylon, 2, 100,
                 "secure reinforcement supply inside the Cannon shell", true);
        }
    } else if (minute(state) >= 3 &&
               (count(state, UnitKind::zealot) >= 2 ||
                count(state, UnitKind::photonCannon) >= 1)) {
        goal(result, GoalKind::expand, UnitKind::nexus, 2, 91,
             "forge-fast-expand timing");
    }
    return result;
}

StrategicPlan StrategyEngine::planPvP(
    const GameState& state,
    const ThreatAssessment& threat) const {
    // Supply and actual structures own the opening. Re-evaluate safety every
    // pass; no stored phase or irreversible script cursor is required.
    const auto meleeEvidence = recentEnemyCount(state, UnitKind::zealot) >= 2 ||
        (recentEnemyCount(state, UnitKind::gateway) >= 2 &&
         recentEnemyCount(state, UnitKind::cyberneticsCore) == 0);
    const auto pressureEvidence = hardBreachAtMain(state) || meleeEvidence ||
        threat.combatEnemiesNearMain > 0 || activeApproach(state, threat) ||
        threat.immediateGround > 0.45 || threat.workerRush > 0.30 ||
        threat.proxy + threat.staticContain > 0.34 || threat.cloak > 0.20;
    if (minute(state) < 8 && !pressureEvidence) {
        StrategicPlan opening;
        opening.name = "PvP ranged economy into Robo";
        opening.desiredWorkers = 32;
        opening.desiredGasWorkers = supplyAtLeast(state, 12) ? 3 : 0;
        opening.minimumAttackSize = 8;
        opening.attackThreshold = 1.25;
        const auto roboCommitted = count(state, UnitKind::roboticsFacility) > 0;
        const auto dragoonsReady = count(state, UnitKind::dragoon, true);
        const auto detectorReady = count(state, UnitKind::observer, true) > 0;
        opening.desiredBases = dragoonsReady >= 6 ? 2 : 1;
        opening.maximumBases = opening.desiredBases;
        opening.posture = dragoonsReady >= 6 ?
                              Posture::pressure : Posture::hold;
        const auto rangedScreenCommitted = count(state, UnitKind::dragoon) >= 2;
        // The explicit bodyguard goal owns the first Zealot. Letting the
        // composition filler keep making melee during Core construction
        // occupies the Gateway when its first Dragoons become available.
        opening.composition = rangedScreenCommitted
                                  ? std::vector<CompositionTarget>{{UnitKind::dragoon, 0.85},
                                                                   {UnitKind::zealot, 0.15}}
                                  : std::vector<CompositionTarget>{{UnitKind::dragoon, 1.0}};
        if (supplyAtLeast(state, 10))
            goal(opening, GoalKind::build, UnitKind::gateway, 1, 100, "opening Gateway", true);
        if (supplyAtLeast(state, 12))
            goal(opening, GoalKind::build, UnitKind::assimilator, 1, 99, "opening gas", true);
        if (supplyAtLeast(state, 12) && count(state, UnitKind::assimilator) > 0 &&
            count(state, UnitKind::gateway) > 0)
            goal(opening, GoalKind::build, UnitKind::cyberneticsCore, 1, 101,
                 "ranged mirror Core before the optional melee queue", true);
        if (supplyAtLeast(state, 14)) {
            goal(opening, GoalKind::train, UnitKind::zealot, 1, 98, "opening bodyguard", true);
            goal(opening, GoalKind::build, UnitKind::cyberneticsCore, 1, 97, "timely ranged access", true);
        }
        if (count(state, UnitKind::cyberneticsCore) > 0) {
            goal(opening, GoalKind::train, UnitKind::dragoon, 4, 108, "fund four Dragoons before optional tech", true);
            if (count(state, UnitKind::dragoon) >= 1)
                technologyGoal(opening, TechnologyKind::singularityCharge, 1, 98, "opening range", true);
        }
        if (dragoonsReady >= 4 || roboCommitted) {
            goal(opening, GoalKind::build, UnitKind::roboticsFacility, 1, 95, "splash after the ranged screen", true);
        }
        if (supplyAtLeast(state, 22)) {
            const auto throughput = supplyAtLeast(state, 32) ? 3 : 2;
            goal(opening, GoalKind::build, UnitKind::gateway, throughput, 104, "army throughput before optional detection", true);
        }
        if (roboCommitted) {
            if (dragoonsReady >= 6) {
                goal(opening, GoalKind::build, UnitKind::observatory, 1, 86, "scouting after six ranged defenders");
                goal(opening, GoalKind::train, UnitKind::observer, 1, 87, "first optional army scout");
            }
            goal(opening, GoalKind::build, UnitKind::roboticsSupportBay, 1, 95,
                 "prepare splash behind the ranged screen", true);
            if (count(state, UnitKind::roboticsSupportBay) > 0)
                goal(opening, GoalKind::train, UnitKind::reaver, 1, 105,
                     "first Reaver with the ranged screen", true);
        }
        if (detectorReady && count(state, UnitKind::nexus, true) >= 2) {
            goal(opening, GoalKind::train, UnitKind::observer, 2, 85, "spare detector and tech scout");
        }
        return opening;
    }
    StrategicPlan result;
    result.name = "PvP two-gate robotics control";
    result.desiredWorkers = std::min(66, 18 + minute(state) * 4);
    result.desiredBases = minute(state) < 6 ? 1 : (minute(state) < 12 ? 2 : 3);
    result.desiredGasWorkers = !supplyAtLeast(state, 11) ? 0 :
                               (minute(state) < 8 ? 3 :
                                (minute(state) < 12 ? 6 : 9));
    result.posture = minute(state) < 6 || openingPressureExpected(state, threat)
                         ? Posture::hold
                         : Posture::pressure;
    // A ranged mirror must meet the reinforcement stream in the open. The
    // old twenty-unit gate left a six-to-ten Dragoon force parked beside the
    // Nexus while BananaBrain's next wave surrounded it. Start with a modest
    // ratio gate, then lower the launch size once our own ranged screen is
    // established; the emergency branch below can still restore a cautious
    // defense when the mineral line is directly breached.
    result.attackThreshold = 1.32;
    result.minimumAttackSize = 14;
    result.composition = {{UnitKind::dragoon, 0.58}, {UnitKind::zealot, 0.14},
                          {UnitKind::reaver, 0.18}, {UnitKind::highTemplar, 0.10}};

    const auto rangedOpening = std::ranges::any_of(
        state.enemy.units, [&state](const UnitSnapshot& unit) {
            const auto tech = unit.kind == UnitKind::cyberneticsCore ||
                              unit.kind == UnitKind::roboticsFacility;
            const auto rangedArmy = unit.kind == UnitKind::dragoon ||
                                    unit.kind == UnitKind::reaver;
            return tech || (rangedArmy &&
                           (unit.visible || state.frame - unit.lastSeen <= 90 * 24));
        });
    const auto enemyGatewayCount = static_cast<int>(std::ranges::count_if(
        state.enemy.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::gateway;
        }));
    const auto enemyRangedCount = recentEnemyCount(state, UnitKind::dragoon) +
                                  recentEnemyCount(state, UnitKind::reaver);
    // A two-Gateway mirror that still has no ranged units after the first
    // five-and-a-half minutes is the same production/tech gap Stardust uses
    // to recognize a likely DT transition.  Keep this as a soft suspicion:
    // it raises detection priority and protects the mineral line, but does
    // not replace the direct rush evidence or force an all-in response.
    const auto suspectedCovertTech = state.enemy.race == Race::protoss &&
                                     state.frame >= 4 * 60 * 24 + 12 * 24 &&
                                     state.frame < 11 * 60 * 24 &&
                                     enemyGatewayCount >= 2 &&
                                     enemyRangedCount == 0;
    const auto enemyCoreScouted = std::ranges::any_of(
        state.enemy.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::cyberneticsCore;
        });
    const auto enemyCoreFinished = std::ranges::any_of(
        state.enemy.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::cyberneticsCore && unit.completed;
        });
    // In a melee-only mirror the first five minutes are decided by Gateway
    // throughput, not by an early gas bank. Three gas workers can remove
    // roughly one Zealot's minerals before the first flood arrives; keep gas
    // off until the enemy shows ranged tech (or the opening window closes).
    const auto visibleRangedOpening = std::ranges::any_of(
        state.enemy.units, [](const UnitSnapshot& unit) {
            return unit.visible && unit.completed &&
                   (unit.kind == UnitKind::cyberneticsCore ||
                    unit.kind == UnitKind::roboticsFacility ||
                    unit.kind == UnitKind::dragoon ||
                   unit.kind == UnitKind::reaver);
        });
    // A Gateway-only Protoss opening is the actionable fork for the first
    // static checkpoint.  Once one enemy Gateway is known, stop the second
    // wave at three Zealots long enough to bank the 150 minerals for Forge;
    // otherwise the two-Gateway queue consumes the entire bank and the Forge
    // remains theoretical until the rush is already in the main.
    const auto openingForgeSignal =
        state.enemy.race == Race::protoss &&
        state.frame >= 2 * 60 * 24 + 36 * 24 &&
        state.frame < 6 * 60 * 24 && enemyGatewayCount >= 1 &&
        enemyRangedCount == 0 && !visibleRangedOpening;
    // `rangedOpening` is intentionally persistent: once a Core or Robotics
    // Facility has been scouted it should continue to unlock our own tech.
    // It must not, however, erase the melee response when the remembered
    // building is hidden and the enemy is still producing only Zealots. Keep
    // a separate, current-pressure signal for opening defense decisions.
    const auto meleeOnlyOpening = enemyGatewayCount >= 2 &&
                                  enemyRangedCount == 0 &&
                                  !visibleRangedOpening &&
                                  (threat.mostLikely == EnemyPlan::fastRush ||
                                   threat.combatEnemiesNearMain > 0 ||
                                   threat.approachingArmyValue >= 1.5 ||
                                   !rangedOpening);
    // The threat label is intentionally allowed to cool when scouting data
    // disappears, but an army already touching the mineral line must not
    // lose its response on the next planning pass.  Stardust keeps an active
    // play/contain state until the engagement is resolved; mirror that with a
    // current-contact signal that supplements (rather than replaces) the
    // persistent two-Gateway opening memory.
    const auto visibleMeleeContactNow = std::ranges::any_of(
        state.enemy.units, [](const UnitSnapshot& enemy) {
            return enemy.visible && enemy.completed && !enemy.flying &&
                   (enemy.kind == UnitKind::zealot || enemy.kind == UnitKind::darkTemplar) &&
                   enemy.position.valid();
        });
    const auto currentMeleeRush = meleeOnlyOpening || visibleMeleeContactNow ||
                                  (threat.mostLikely == EnemyPlan::fastRush &&
                                   threat.approachingArmyValue >= 1.5);
    if (!visibleRangedOpening && state.frame >= 2 * 60 * 24 &&
        state.frame < 4 * 60 * 24) {
        result.desiredGasWorkers = 0;
    }
    const auto cannonsReady = count(state, UnitKind::photonCannon, true);
    const auto zealotsReady = count(state, UnitKind::zealot, true);
    const auto dragoonsReady = count(state, UnitKind::dragoon, true);
    if (state.frame >= 3 * 60 * 24 && state.frame < 5 * 60 * 24 &&
        zealotsReady >= 2) {
        // Do not let the generic melee gas pause win after the two-unit
        // screen is already fielded. This also covers a hidden enemy Core,
        // which disables the early Forge anchor but still needs our own Core
        // and Robotics Facility to start on time.
        result.desiredGasWorkers = std::max(result.desiredGasWorkers, 3);
    }
    // A missing Core means something different before and after Dragoons have
    // existed. Treat the latter as destroyed infrastructure so an emergency
    // response does not strand a surviving army with an unusable bank.
    const auto coreLost = count(state, UnitKind::cyberneticsCore) == 0 &&
                          count(state, UnitKind::dragoon) > 0;
    const auto twoGateOpening = minute(state) < 8 && currentMeleeRush;
    const auto earlyMeleeAnchor = minute(state) >= 3 && minute(state) < 5 &&
                                  currentMeleeRush && zealotsReady >= 2 &&
                                  (threat.combatEnemiesNearMain > 0 ||
                                   threat.approachingArmyValue >= 2.0 ||
                                   threat.mostLikely == EnemyPlan::fastRush);
    const auto home = ourMain(state);
    const auto visibleGroundContact = std::ranges::any_of(
        state.enemy.units, [&home](const UnitSnapshot& enemy) {
            return enemy.visible && enemy.completed && !enemy.flying &&
                   isCombatUnit(enemy.kind) && enemy.position.valid() &&
                   (!home.valid() || distanceSquared(home, enemy.position) <= 800 * 800);
        });
    const auto earlyMeleeScreen = zealotsReady >= 2 || rangedOpening;
    const auto mobileOpening = zealotsReady + dragoonsReady +
                               count(state, UnitKind::reaver, true);
    const auto earlyTechWindow = state.frame >= 4 * 60 * 24 &&
                                 zealotsReady >= 4 &&
                                 threat.combatEnemiesNearMain == 0 &&
                                 !visibleGroundContact &&
                                 !activeApproach(state, threat);
    if (dragoonsReady >= 4) {
        result.attackThreshold = 1.22;
        result.minimumAttackSize = 8;
        const auto enemyArmyUnseen = std::ranges::none_of(
            state.enemy.units, [](const UnitSnapshot& enemy) {
                return enemy.visible && enemy.completed && !enemy.flying &&
                       isCombatUnit(enemy.kind);
            });
        if (state.frame >= 7 * 60 * 24 && enemyArmyUnseen &&
            threat.combatEnemiesNearMain == 0) {
            // Do not wait for the eighth body when the map is empty. A
            // seven-unit ranged screen that reaches the enemy production at
            // eight minutes can stop the next flood from ever assembling.
            result.attackThreshold = 1.15;
            result.minimumAttackSize = 6;
        }
    }
    if (minute(state) < 14 && mobileOpening < 24) {
        result.maximumBases = 2;
        result.desiredBases = std::min(result.desiredBases, 2);
    }
    if (minute(state) < 10 && mobileOpening < 10) {
        result.desiredBases = 1;
        result.maximumBases = std::max(1, count(state, UnitKind::nexus));
    }
    // Do not buy a second Nexus during an unresolved opening fight. Once the
    // home screen is stable, however, expansion and splash tech must overlap:
    // waiting for a completed Reaver before taking the natural is exactly the
    // one-base turtle that lets BananaBrain bank an unanswerable Dragoon wave.
    const auto firstReaverTechReady =
        count(state, UnitKind::roboticsFacility, true) > 0 &&
        count(state, UnitKind::roboticsSupportBay, true) > 0 &&
        count(state, UnitKind::reaver, true) > 0;
    // A two-Cannon/two-Zealot screen is already a real defensive economy when
    // the enemy army is outside the home perimeter.  Do not let the old
    // "wait for Reaver" gate turn a stable one-base hold into a permanent
    // turtle while the opponent takes its natural and third bases.
    const auto workers = countRole(state, UnitRole::worker);
    const auto noVisibleGroundContact = std::ranges::none_of(
        state.enemy.units, [](const UnitSnapshot& enemy) {
            return enemy.visible && enemy.completed && !enemy.flying &&
                   isCombatUnit(enemy.kind) && enemy.position.valid();
        });
    // Stardust's economic transition is a state change, not a clocked Nexus:
    // once the home screen is complete and the map is quiet, start banking
    // before the opponent's midgame wave can make the one-base hold
    // permanent.  The five-minute branch is deliberately evidence-gated so
    // it cannot run during a visible rush, breach, or covert-tech alarm.
    const auto earlyStableCounter = state.frame >= 5 * 60 * 24 &&
                                    state.frame < 8 * 60 * 24 &&
                                    cannonsReady >= 2 && zealotsReady >= 5 &&
                                    workers >= 16 && noVisibleGroundContact &&
                                    threat.combatEnemiesNearMain == 0 &&
                                    !activeApproach(state, threat) &&
                                    threat.immediateGround <= 0.35 &&
                                    !hardBreachAtMain(state) &&
                                    threat.cloak <= 0.20 &&
                                    threat.mostLikely != EnemyPlan::fastRush;
    const auto stableTechCounter = state.frame >= 6 * 60 * 24 &&
                                   state.frame < 13 * 60 * 24 &&
                                   workers >= 18 && zealotsReady >= 6 &&
                                   cannonsReady >= 1 &&
                                   count(state, UnitKind::cyberneticsCore, true) > 0 &&
                                   noVisibleGroundContact &&
                                   threat.combatEnemiesNearMain == 0 &&
                                   !activeApproach(state, threat) &&
                                   threat.immediateGround <= 0.55 &&
                                   !hardBreachAtMain(state) &&
                                   threat.cloak <= 0.20 &&
                                   threat.mostLikely != EnemyPlan::fastRush;
    const auto economicCounterWindow =
        earlyStableCounter ||
        stableTechCounter ||
        (minute(state) >= 6 && cannonsReady >= 2 && zealotsReady >= 2 &&
         workers >= 12 && threat.combatEnemiesNearMain == 0 &&
         !activeApproach(state, threat) && threat.immediateGround <= 0.45 &&
         !hardBreachAtMain(state) && threat.cloak <= 0.20);
    if (economicCounterWindow) {
        result.desiredBases = std::max(result.desiredBases, 2);
        result.maximumBases = std::max(result.maximumBases, 2);
        result.posture = Posture::pressure;
        result.attackThreshold = std::min(result.attackThreshold, 1.35);
        result.minimumAttackSize = std::min(result.minimumAttackSize, 8);
        result.name += " [economic counter-window]";
        if (stableTechCounter) result.name += " [stable two-base transition]";
    }
    if (minute(state) < 10 && !firstReaverTechReady && !economicCounterWindow) {
        result.desiredBases = 1;
        result.maximumBases = std::max(1, count(state, UnitKind::nexus));
    }
    if (minute(state) < 8 && !earlyMeleeScreen) {
        // Composition fills run after fixed goals. Keep them melee-only until
        // the opening screen has two Zealots, otherwise an idle second
        // Gateway can spend the rush window on a Dragoon.
        result.composition = {{UnitKind::zealot, 1.0}};
    }
    const auto canTransition = count(state, UnitKind::cyberneticsCore) > 0 ||
                               (zealotsReady >= 2 && rangedOpening) ||
                               (cannonsReady >= 2 && zealotsReady >= 1 && rangedOpening) ||
                               (cannonsReady >= 3 && zealotsReady >= 4) ||
                               // Two completed Cannons plus two Zealots are
                               // already a meaningful mirror anchor. Do not
                               // wait for a third Cannon while the Core and
                               // gas transition remain permanently erased.
                               (cannonsReady >= 2 && zealotsReady >= 2);

    // Buying a Forge and two Cannons on seven Probes conceded the economy
    // before the first engagement. Establish mobile production while growing
    // workers; direct rush evidence below reserves extra melee and sustain.
    if (supplyAtLeast(state, 9)) {
        goal(result, GoalKind::build, UnitKind::gateway, 1, 100,
             "nine-supply mobile production", true);
    }
    if (supplyAtLeast(state, 10)) {
        goal(result, GoalKind::build, UnitKind::assimilator, 1, 101,
             "opening gas before the Core completes", true);
    }
    if (count(state, UnitKind::gateway) > 0 &&
        count(state, UnitKind::gateway) < 2) {
        // Do not spend the opening Core's minerals while a single Gateway has
        // produced only one body. Two early Zealots give the Probe line time
        // to survive the first mirror contact and let the second Gateway
        // complete without conceding the game to a four-Zealot rush.
        goal(result, GoalKind::train, UnitKind::zealot, 2, 99,
             "field two mobile defenders before ranged tech", true);
    }

    if (supplyAtLeast(state, 9)) {
        const auto roboticsInPlay = count(state, UnitKind::roboticsFacility) > 0;
        const auto reaverOnline = count(state, UnitKind::reaver, true) > 0;
        const auto gatewayTarget = supplyAtLeast(state, 10) ?
                                       (!roboticsInPlay || !reaverOnline ? 2 :
                                        (minute(state) < 12 ? 3 : 4)) : 1;
        // BananaBrain's opening is a live production race, not just a tech
        // race.  When the first Gateway exists but the second one has not
        // started, let that throughput checkpoint outrank optional Core/
        // Dragoon prerequisites until the early production pair is secured.
        // The old priority (97) let the generic Dragoon goal materialize a
        // Core first, leaving one Gateway and one Zealot when the rush arrived.
        const auto earlyGatewayPriority = state.frame < 5 * 60 * 24 &&
                                          count(state, UnitKind::gateway) < 2 &&
                                          !visibleRangedOpening ? 115 : 97;
        goal(result, GoalKind::build, UnitKind::gateway, gatewayTarget,
             earlyGatewayPriority,
             "nine-supply gateway into two-gate control",
             count(state, UnitKind::gateway) < gatewayTarget && gatewayTarget <= 2);
    }
    if (count(state, UnitKind::gateway) >= 2) {
        // Keep both early Gateways on melee bodies long enough to contest a
        // mirror flood. Transitioning at three Zealots left BananaBrain's
        // first wave unopposed while the Core consumed the bank.
        const auto earlyZealotTarget =
            count(state, UnitKind::roboticsFacility) > 0
                ? ((currentMeleeRush && state.frame < 10 * 60 * 24 &&
                    (threat.mostLikely == EnemyPlan::fastRush ||
                     threat.uncertainty > 0.85)) ? 8 :
                   (minute(state) < 6 ? 6 : 3))
                : (currentMeleeRush && state.frame >= 4 * 60 * 24
                       ? (count(state, UnitKind::forge) > 0 ? 6 : 5)
                       : (openingForgeSignal && count(state, UnitKind::forge) == 0 ? 3 : 4));
        goal(result, GoalKind::train, UnitKind::zealot,
             earlyZealotTarget, 96,
             "fill secured opening production with defenders",
             count(state, UnitKind::zealot) < 2);
        const auto batteryRangedEvidence = visibleRangedOpening ||
            enemyRangedCount > 0 ||
            (rangedOpening && threat.mostLikely != EnemyPlan::fastRush);
        const auto batteryEvidence = batteryRangedEvidence ||
            threat.combatEnemiesNearMain > 0 ||
            threat.approachingArmyValue >= 2.0;
        // Against a melee-only two-Gateway line, the first Battery is a
        // lower-value mineral sink than the Forge/Cannon checkpoint.  If the
        // Battery starts while the Forge is still missing, it can consume the
        // entire 100-mineral margin that would otherwise start the static
        // anchor before contact.  Ranged openings keep the Battery path.
        const auto meleeAnchorPending = currentMeleeRush &&
                                        count(state, UnitKind::forge) == 0 &&
                                        cannonsReady == 0 &&
                                        // Four Zealots are the minimum mobile
                                        // escort, not proof that a static
                                        // anchor is no longer needed.  Keep
                                        // the Battery out of the bank until
                                        // the six-body checkpoint or Forge
                                        // completion, whichever comes first.
                                        count(state, UnitKind::zealot) < 6;
        if (count(state, UnitKind::zealot) >= 2 && batteryEvidence &&
            !meleeAnchorPending &&
            // A Battery is sustain, not the first emergency anchor. Keep its
            // 100 minerals behind a Cannon investment or the Core so it
            // cannot delay both static defense and ranged transition.
            (cannonsReady >= 1 || count(state, UnitKind::photonCannon) > 0 ||
             count(state, UnitKind::cyberneticsCore) > 0)) {
            goal(result, GoalKind::build, UnitKind::shieldBattery, 1, 93,
                 "sustain the two-gate defensive screen");
        }
    }
    if (earlyMeleeAnchor) {
        // Two completed Zealots by the three-minute mark are the only
        // reliable signal available before a fast mirror flood is visible.
        // Start one Forge while the bank is still small; this is a single
        // anchor, not the old blind two-Cannon opening, and it buys time for
        // the Core and ranged transition to complete.
        // This is a low-priority insurance layer, not an emergency reserve.
        // Keep Assimilator/Core/Zealot funding ahead of it; once those costs
        // are protected, a single Forge can finish before the first ranged
        // pressure wave and let the six-minute Cannon fallback execute.
        goal(result, GoalKind::build, UnitKind::forge, 1, 95,
             "early mirror Forge insurance", false);
        if (count(state, UnitKind::forge) > 0 && currentMeleeRush) {
            // Overlap the second Cannon once a rush is actually approaching;
            // waiting for the first structure to finish lets the attacker
            // focus it down before the next anchor can even start.
            const auto earlyCannonTarget =
                threat.mostLikely == EnemyPlan::fastRush || visibleGroundContact ||
                        threat.approachingArmyValue >= 2.0
                    ? 2
                    : 1;
            goal(result, GoalKind::build, UnitKind::photonCannon,
                 earlyCannonTarget, 94,
                 "early Cannon screen behind the Forge insurance", false);
        }
    }
    // A remembered first Gateway with no Core, Dragoon, or Reaver by the
    // 2:36 mark is useful information even when the enemy army is
    // still hidden. BananaBrain's two-Gateway/DT lines often keep the second
    // production structure out of the worker scout's vision; waiting for the
    // FastRush label then leaves no mineral window for a Forge. Stage one
    // blocking Forge insurance once two Zealots are fielded. The reservation
    // is allowed to wait behind the current bank, but it prevents a routine
    // Probe/Dragoon spend from consuming the exact 150 minerals needed to
    // start the anchor on the next macro pass.
    const auto staticFirstOpening = state.frame >= 3 * 60 * 24 &&
                                    state.frame < 6 * 60 * 24 &&
                                    count(state, UnitKind::gateway) >= 2 &&
                                    enemyGatewayCount >= 1 &&
                                    enemyRangedCount == 0 &&
                                    !visibleRangedOpening &&
                                    count(state, UnitKind::forge) == 0 &&
                                    cannonsReady == 0;
    const auto hiddenMeleeTechSignal = staticFirstOpening || openingForgeSignal ||
                                       (state.frame >= 6 * 60 * 24 &&
                                        state.frame < 8 * 60 * 24 &&
                                        enemyGatewayCount >= 1 &&
                                        enemyRangedCount == 0 &&
                                        !visibleRangedOpening &&
                                        zealotsReady >= 1);
    if (hiddenMeleeTechSignal) {
        goal(result, GoalKind::build, UnitKind::forge, 1, 118,
             "insurance Forge for an unscouted melee transition", true);
        result.name += " [melee-tech insurance]";
    }
    const auto stabilizingMeleeAnchor = state.frame >= 5 * 60 * 24 &&
                                        state.frame < 8 * 60 * 24 &&
                                        zealotsReady >= 6 &&
                                        !hardBreachAtMain(state) &&
                                        (!visibleGroundContact ||
                                         threat.mostLikely == EnemyPlan::fastRush) &&
                                        (threat.mostLikely == EnemyPlan::fastRush ||
                                         threat.uncertainty > 0.85 ||
                                         count(state, UnitKind::cyberneticsCore) > 0);
    if (stabilizingMeleeAnchor) {
        // A hidden two-gate flood can cross the map before it becomes visible
        // at the mineral line. Once six Zealots and the first Core bank are
        // secured, reserve one Forge/Cannon without buying the old blind
        // two-Cannon opening. This gives the home screen time to finish while
        // Robotics and the first Reaver remain the primary tech plan.
        goal(result, GoalKind::build, UnitKind::forge, 1, 94,
             "stabilize the six-Zealot mirror screen", false);
        if (count(state, UnitKind::forge) > 0) {
            goal(result, GoalKind::build, UnitKind::photonCannon, 1, 93,
                 "single Cannon behind the six-Zealot screen", false);
        }
    }
    if (state.frame >= 6 * 60 * 24 && state.frame < 9 * 60 * 24 &&
        count(state, UnitKind::forge) > 0 && cannonsReady < 2) {
        // One Cannon buys the opening time; a second one before nine minutes
        // keeps a Dragoon-heavy push from deleting the mineral line while the
        // Core and Robotics chain finishes.  This is still capped at two.
        goal(result, GoalKind::build, UnitKind::photonCannon, 2, 94,
             "second Cannon before the ranged pressure window", false);
    }
    if (supplyAtLeast(state, 12)) {
        const auto quietTechWindow = state.frame >= 5 * 60 * 24 &&
                                     threat.combatEnemiesNearMain == 0 &&
                                     !visibleGroundContact &&
                                     (!activeApproach(state, threat) ||
                                      zealotsReady >= 8 || cannonsReady >= 1);
        const auto screenedTechWindow = state.frame >= 5 * 60 * 24 &&
                                        threat.combatEnemiesNearMain == 0 &&
                                        !visibleGroundContact &&
                                        zealotsReady >= 6;
    // A scouted two-Gateway mirror is already enough evidence to start the
    // ranged transition behind two Zealots.  Waiting for four completed
    // Zealots made the Core a reaction to the first flood instead of a
    // production checkpoint, which in turn pushed Robotics/Observer past the
    // hidden-tech timing.  Keep the ordinary quiet-window predicates, but let
    // the observed double-production line unlock the Core earlier.
    const auto earlyMirrorTech = twoGateOpening &&
                                 state.frame >= 3 * 60 * 24 + 24 * 24 &&
                                 zealotsReady >= 2 &&
                                 threat.combatEnemiesNearMain == 0 &&
                                 !hardBreachAtMain(state);
    // In an unscouted mirror, do not let the optional range prerequisite
    // pull a 200-mineral Core through the queue before the first melee/static
    // checkpoint.  Four minutes is still an early Core timing, but it leaves
    // the hidden two-Gateway branch one complete Forge window.  Confirmed
    // ranged tech, a Cannon, or synthetic frame-zero unit tests bypass this
    // delay.
    const auto coreTimingWindow = !staticFirstOpening &&
                                  (rangedOpening || enemyCoreScouted ||
                                   cannonsReady >= 1 || state.frame == 0 ||
                                   state.frame >= 4 * 60 * 24);
    const auto productionSecured = coreTimingWindow &&
                                   count(state, UnitKind::gateway) >= 1 &&
                                   (state.frame < 2 * 60 * 24 ||
                                        (count(state, UnitKind::zealot) >= 4 &&
                                         (rangedOpening || cannonsReady >= 1 ||
                                          count(state, UnitKind::photonCannon) >= 1 ||
                                          quietTechWindow || screenedTechWindow ||
                                          earlyTechWindow)) ||
                                    earlyMirrorTech);
        goal(result, GoalKind::build, UnitKind::cyberneticsCore, 1, 98,
             "dragoon access after opening production", productionSecured);
    }
    if (supplyAtLeast(state, 14)) {
        // Do not let a filler Dragoon goal invent its own Cybernetics Core in
        // the opening.  A generic prerequisite chain cannot see the PvP
        // production checkpoint, so it used to spend 200 minerals on the
        // Core while the second Gateway was still waiting.  The explicit
        // Core goal below (or a known ranged opening) owns this transition.
        const auto rangedProductionUnlocked =
            count(state, UnitKind::cyberneticsCore) > 0 || rangedOpening ||
            cannonsReady >= 2;
        if (earlyMeleeScreen && rangedProductionUnlocked) {
            goal(result, GoalKind::train, UnitKind::dragoon,
                 std::max(4, minute(state) * 2), 91, "core PvP army");
        }
        technologyGoal(result, TechnologyKind::singularityCharge, 1, 86,
                       "range follows the first defensive dragoons");
    }
    if (count(state, UnitKind::dragoon) >= 3) {
        // Reserve the splash-tech investment before routine Dragoon fills can
        // consume the bank. A Reaver is the only cost-effective answer once
        // the mirror reaches a packed ranged fight.
        const auto earlyRobotics = dragoonsReady >= 2;
        goal(result, GoalKind::build, UnitKind::roboticsFacility, 1,
             earlyRobotics ? 104 : 92,
             "reaver pressure and detection", earlyRobotics);
        goal(result, GoalKind::build, UnitKind::observatory, 1, 83, "DT safety");
        goal(result, GoalKind::train, UnitKind::observer, 2, 88, "DT detection", true);
    }
    // Reserve the Support Bay as soon as the Robotics Facility is underway.
    // Waiting until the facility is complete lets routine Gateway production
    // consume the exact 150 minerals/100 gas needed for the first Reaver.
    const auto supportNeeded = state.frame >= 4 * 60 * 24 &&
                               count(state, UnitKind::roboticsFacility) > 0 &&
                               // In a normal ranged mirror, Support Bay can
                               // build in parallel with the Observatory. Only
                               // serialize it behind detection when the
                               // persistent covert-tech recognizer is active.
                               (!suspectedCovertTech ||
                                count(state, UnitKind::observatory, true) > 0) &&
                               count(state, UnitKind::roboticsSupportBay) == 0;
    const auto recentDarkTemplar = recentEnemyCount(state, UnitKind::darkTemplar);
    // Stardust keeps a confirmed melee all-in separate from a covert-tech
    // hypothesis.  Do the same here: during a FastRush, do not let a soft
    // "two Gateways/no ranged units" prior pull the bank into Robotics before
    // the first Cannon is online.  A seen/recent Dark Templar still bypasses
    // this hold immediately.
    const auto holdTechForMeleeRush = currentMeleeRush &&
                                      threat.mostLikely == EnemyPlan::fastRush &&
                                      minute(state) < 8 && cannonsReady == 0 &&
                                      zealotsReady < 6 && recentDarkTemplar == 0;
    // Stardust treats a completed Core as a production checkpoint, not as a
    // reason to keep filling Gateways indefinitely.  Once the first mobile
    // screen or one Cannon is online, start Robotics immediately; otherwise
    // a lost Zealot can make the old four-body predicate false for the entire
    // next pressure cycle and leave the Core idle while the opponent adds
    // ranged production or covert tech.
    const auto earlyDetectionCheckpoint = state.frame >= 4 * 60 * 24 + 12 * 24 &&
                                          count(state, UnitKind::cyberneticsCore, true) > 0 &&
                                          count(state, UnitKind::roboticsFacility) == 0 &&
                                          !hardBreachAtMain(state) &&
                                          !holdTechForMeleeRush &&
                                          (suspectedCovertTech ||
                                           (state.frame >= 6 * 60 * 24 &&
                                            threat.mostLikely != EnemyPlan::fastRush &&
                                            threat.combatEnemiesNearMain == 0 &&
                                            count(state, UnitKind::zealot) >= 3));
    const auto roboticsCheckpoint = state.frame >= 6 * 60 * 24 &&
                                    count(state, UnitKind::cyberneticsCore, true) > 0 &&
                                    (zealotsReady >= 3 || cannonsReady >= 1 ||
                                     enemyCoreScouted || enemyCoreFinished);
    const auto roboticsNeeded = state.frame >= 4 * 60 * 24 &&
                                count(state, UnitKind::cyberneticsCore, true) > 0 &&
                                count(state, UnitKind::roboticsFacility) == 0 &&
                                !holdTechForMeleeRush &&
                                (earlyDetectionCheckpoint || zealotsReady >= 4 || roboticsCheckpoint ||
                                 (cannonsReady >= 2 &&
                                  (!enemyCoreScouted || enemyCoreFinished)));
    if (roboticsNeeded) {
        result.desiredGasWorkers = std::max(3, result.desiredGasWorkers);
        goal(result, GoalKind::build, UnitKind::roboticsFacility, 1,
             earlyDetectionCheckpoint ? 116 : 104,
             "reserve the first Reaver tech window", true);
    }
    if (supportNeeded) {
        // In a ranged mirror the first Reaver is a timing unit, not a late
        // composition luxury. Reserve its Support Bay before extra Gateways
        // and routine Dragoon fills consume the mineral/gas bank.
        result.desiredGasWorkers = std::max(6, result.desiredGasWorkers);
        goal(result, GoalKind::build, UnitKind::roboticsSupportBay, 1, 106,
             "early Reaver splash against the mirror army", true);
    }
    const auto cloakedThreat =
        threat.cloak > 0.20 || threat.mostLikely == EnemyPlan::cloakedTech ||
        recentEnemyCount(state, UnitKind::darkTemplar) > 0 ||
        suspectedCovertTech;
    if (cloakedThreat && count(state, UnitKind::cyberneticsCore, true) > 0 &&
        (!holdTechForMeleeRush || recentDarkTemplar > 0)) {
        // A Protoss Dark Templar line can end an otherwise stable two-Cannon
        // defense before the normal three-Dragoon trigger asks for Robotics.
        // Reserve the detection chain as soon as the assessment turns covert,
        // then let the first Observer take precedence over routine Gateway
        // fills and the Reaver follow-up.
        result.desiredGasWorkers = std::max(6, result.desiredGasWorkers);
        goal(result, GoalKind::build, UnitKind::roboticsFacility, 1, 108,
             "detection against cloaked Protoss tech", true);
        goal(result, GoalKind::build, UnitKind::observatory, 1, 107,
             "unlock an Observer against Dark Templar", true);
        goal(result, GoalKind::train, UnitKind::observer, 1, 106,
             "field the first emergency Observer", true);
        if (suspectedCovertTech) result.name += " [suspected covert tech]";
        if (count(state, UnitKind::observer, true) == 0) {
            // Do not send an undetected army across the map into a DT line.
            // Hold the home perimeter, add a static detector, and resume
            // pressure once the first Observer is available.
            result.posture = Posture::defend;
            result.attackThreshold = std::max(result.attackThreshold, 1.55);
            result.minimumAttackSize = std::max(result.minimumAttackSize, 14);
            result.desiredBases = 1;
            goal(result, GoalKind::build, UnitKind::photonCannon, 2, 104,
                 "static detection while the first Observer is pending", true);
        }
    }
    if (supplyAtLeast(state, 28)) {
        goal(result, GoalKind::build, UnitKind::roboticsSupportBay, 1, 69,
             "reaver access");
        goal(result, GoalKind::train, UnitKind::reaver, 2, 70, "area control");
        goal(result, GoalKind::train, UnitKind::shuttle, 1, 67, "reaver mobility");
    }
    if (count(state, UnitKind::roboticsSupportBay) > 0 &&
        mobileOpening >= 8) {
        // One early Reaver changes the first Dragoon contact; do not wait for
        // the later composition filler to notice that the Robotics Facility
        // is idle.
        goal(result, GoalKind::train, UnitKind::reaver, 2, 105,
             "two-Reaver splash before the first full attack wave", true);
    }

    // Robotics is already the normal PvP splash-tech checkpoint.  Attach an
    // Observatory to that same checkpoint instead of waiting for three
    // Dragoons or a fully observed cloak alarm: a hidden Dark Templar can
    // arrive during the Robotics/Support build window, before the reactive
    // branch has any chance to finish detection.  Detection is deliberately
    // ahead of the Support Bay here; a delayed Reaver is recoverable, while a
    // first Observer that starts after the DT wave has entered the mineral
    // line is not.
    if (count(state, UnitKind::roboticsFacility) > 0) {
        // Reserve detection as soon as Robotics is underway, not only after
        // its long build timer completes.  A hidden DT line can arrive during
        // that timer; waiting for a completed facility delayed Observatory
        // and Observer until after the first cloak wave in direct matches.
        const auto observatoryPriority =
            count(state, UnitKind::observatory) == 0 ? 111 : 108;
        goal(result, GoalKind::build, UnitKind::observatory, 1,
             observatoryPriority,
             "early PvP detection before splash support", true);
        if (count(state, UnitKind::observatory, true) > 0) {
            goal(result, GoalKind::train, UnitKind::observer, 1, 107,
                 "field the first Observer before the hidden-tech timing", true);
        }
    }

    if (minute(state) >= 8) {
        technologyGoal(result, TechnologyKind::protossGroundWeapons,
                       minute(state) >= 15 ? 2 : 1, 66,
                       "scale dragoon volleys");
    }
    if (minute(state) >= 10) {
        technologyGoal(result, TechnologyKind::reaverCapacity, 1, 64,
                       "increase reaver combat endurance");
        technologyGoal(result, TechnologyKind::scarabDamage, 1, 61,
                       "improve reaver breakpoints");
    }

    const auto evidencedMeleeRush = minute(state) < 8 && currentMeleeRush &&
                                    (twoGateOpening ||
                                     hardBreachAtMain(state) ||
                                     // If the mobile screen has already been
                                     // thinned below four completed Zealots,
                                     // add one static anchor before the next
                                     // wave arrives.
                                     (threat.combatEnemiesNearMain > 0 &&
                                      zealotsReady < 4));
    if (evidencedMeleeRush) {
        // A Forge is expensive, so only open this branch after direct mirror
        // evidence. It gives the two-gate screen one static anchor without
        // returning to the old blind seven-Probe Cannon opening.
        goal(result, GoalKind::build, UnitKind::forge, 1, 96,
             "conditional static anchor against evidenced Zealot pressure");
        if (count(state, UnitKind::forge) > 0) {
            goal(result, GoalKind::build, UnitKind::photonCannon, 1, 95,
                 "conditional Cannon behind the mobile mirror screen");
        }
    }

    // Once a fast melee opening is actually visible, the mobile screen and a
    // single early Cannon are both more valuable than another delayed ranged
    // tech cycle. This branch is evidence-gated, so an ordinary mirror does
    // not pay the blind Forge tax, but a four-Zealot flood cannot walk through
    // an entirely unanchored mineral line.
    const auto rushStaticNeeded = minute(state) < 8 && currentMeleeRush &&
                                   // A visible melee pack inside the home
                                   // perimeter is already actionable evidence;
                                   // waiting for BWAPI's narrower hard-breach
                                   // ring left the emergency Forge at a low
                                   // priority until the Probe line was gone.
                                   (hardBreachAtMain(state) ||
                                    visibleGroundContact ||
                                     threat.combatEnemiesNearMain > 0 ||
                                     threat.approachingArmyValue >= 2.0);
    // A Cannon under construction is already a committed static investment:
    // its minerals are gone and a Probe is tied up. Treat it as an anchor
    // checkpoint for the ranged transition instead of waiting for the long
    // Cannon timer to finish.
    const auto cannonInvestment = count(state, UnitKind::photonCannon) > 0;
    // A long Cannon build is not yet a safe mineral-line screen. Do not spend
    // the next 200 minerals on Core while the first anchor is incomplete
    // unless the mobile screen has reached six Zealots.
    const auto staticCheckpoint = cannonsReady >= 1 ||
                                  (cannonInvestment && zealotsReady >= 6);
    const auto preserveEarlyCore =
        state.frame >= 4 * 60 * 24 &&
        // A FastRush that is still outside the hard-breach radius is exactly
        // the narrow window where starting the Core saves a full
        // Robotics/Observer cycle. Once one Cannon is complete, keep that
        // checkpoint even if the front line briefly touches the Nexus: the
        // old Zealot-only predicate erased the Core after one combat loss and
        // left the bot permanently melee-only.
        (zealotsReady >= 4 && !hardBreachAtMain(state) && staticCheckpoint);
    const auto preserveCoreBehindCannon =
        // Once a Cannon has actually been started, do not let the emergency
        // melee branch erase the Core until the static screen is finished.
        // The old six-minute gate was too late: on a committed rush the bot
        // could reach five Zealots plus an unfinished Cannon, repeatedly
        // cancel the Core, and die before Dragoons/Observers came online.
        state.frame >= 4 * 60 * 24 && staticCheckpoint && zealotsReady >= 4;

    // A visible army just outside the 448px emergency ring is the midfield,
    // not the mineral line.  Stardust keeps a stable vanguard there so the
    // opponent cannot walk from the edge of vision to the Nexus for free.
    // Treat this as a pressure contact when our compact screen can contest it;
    // only a hard breach or a clearly superior approach should collapse the
    // whole plan back to Defend.
    const auto midfieldVisibleCount = static_cast<int>(std::ranges::count_if(
        state.enemy.units, [](const UnitSnapshot& enemy) {
            return enemy.visible && enemy.completed && !enemy.flying &&
                   isCombatUnit(enemy.kind) && enemy.position.valid();
        }));
    const auto midfieldContestable = midfieldVisibleCount <=
                                     mobileOpening + cannonsReady;
    const auto midfieldContact = visibleGroundContact &&
                                 !hardBreachAtMain(state) &&
                                 threat.combatEnemiesNearMain == 0 &&
                                 mobileOpening >= 5 &&
                                 (zealotsReady >= 5 || cannonsReady >= 1) &&
                                 !cloakedThreat &&
                                 (threat.immediateGround <= 0.55 ||
                                  (midfieldContestable &&
                                   threat.immediateGround <= 0.80));
    if (midfieldContact) {
        result.posture = Posture::pressure;
        result.attackThreshold = std::min(result.attackThreshold, 1.35);
        result.minimumAttackSize = std::min(result.minimumAttackSize, 6);
        const auto nearest = std::ranges::min_element(
            state.enemy.units, {}, [&home](const UnitSnapshot& enemy) {
                return enemy.visible && enemy.completed && !enemy.flying &&
                               isCombatUnit(enemy.kind) && enemy.position.valid()
                           ? distanceSquared(home, enemy.position)
                           : std::numeric_limits<int>::max();
            });
        if (nearest != state.enemy.units.end() && nearest->position.valid()) {
            result.rallyPoint = moveToward(home, nearest->position, 320.0);
        }
        result.name += " [midfield pressure]";
    }

    if (threat.combatEnemiesNearMain > 0 ||
        (activeApproach(state, threat) && !midfieldContact) ||
        (visibleGroundContact && !midfieldContact) ||
        (minute(state) < 8 && threat.aggression > 0.6 && !midfieldContact)) {
        result.name = "PvP two-gate emergency defense";
        result.posture = Posture::defend;
        result.desiredBases = 1;
        result.desiredWorkers = std::min(result.desiredWorkers, canTransition ? 22 : 12);
        const auto meleeOnlyEmergency = rushStaticNeeded &&
                                        dragoonsReady < 2 &&
                                        // One completed Cannon is already a
                                        // meaningful static checkpoint.  Do
                                        // not keep erasing Robotics after the
                                        // wall exists: that left a completed
                                        // Core idle until the DT wave was on
                                        // top of the mineral line.
                                        cannonsReady == 0 &&
                                        zealotsReady < 6 &&
                                        !preserveEarlyCore &&
                                        !preserveCoreBehindCannon;
        if ((count(state, UnitKind::cyberneticsCore) == 0 && !canTransition &&
             !preserveEarlyCore && !preserveCoreBehindCannon) ||
            meleeOnlyEmergency) {
            result.desiredGasWorkers = 0;
            result.goals.erase(
                std::remove_if(result.goals.begin(), result.goals.end(),
                               [](const ProductionGoal& candidate) {
                                   return candidate.target == UnitKind::cyberneticsCore ||
                                          candidate.target == UnitKind::assimilator ||
                                          candidate.target == UnitKind::roboticsFacility ||
                                          candidate.target == UnitKind::observatory ||
                                          candidate.target == UnitKind::roboticsSupportBay ||
                                          candidate.technology ==
                                              TechnologyKind::singularityCharge;
                               }),
                result.goals.end());
            result.composition = {{UnitKind::zealot, 1.0}};
            if (coreLost) {
                // This is a rebuild, not a greedy tech opening. Restore the
                // production prerequisite and gas immediately; otherwise the
                // already-earned Dragoon army can only watch its bank grow.
                result.desiredGasWorkers = std::max(3, result.desiredGasWorkers);
                goal(result, GoalKind::build, UnitKind::cyberneticsCore, 1, 110,
                     "rebuild the destroyed ranged-tech core", true);
                goal(result, GoalKind::build, UnitKind::assimilator, 1, 109,
                     "restore gas after the destroyed ranged-tech core", true);
            }
        }
        if ((preserveEarlyCore || preserveCoreBehindCannon) &&
            count(state, UnitKind::cyberneticsCore) == 0) {
            // Four Zealots and no hard breach are enough to start the ranged
            // transition. Do not let the emergency branch erase this goal
            // while a mirror rush is merely approaching the wall.
            result.desiredGasWorkers = std::max(3, result.desiredGasWorkers);
            goal(result, GoalKind::build, UnitKind::cyberneticsCore, 1, 113,
                 "start ranged tech behind the four-Zealot screen", true);
            goal(result, GoalKind::build, UnitKind::assimilator, 1, 112,
                 "feed the protected early Core", true);
        }
        const auto widenRushScreen =
            hardBreachAtMain(state) ||
            (cannonsReady >= 1 &&
             (visibleGroundContact || threat.combatEnemiesNearMain > 0 ||
              threat.approachingArmyValue >= 2.0));
        const auto secondCannonSafe = workers >= 14 || zealotsReady >= 6 ||
                                      hardBreachAtMain(state);
        const auto rushCannonTarget =
            // One Cannon is the pre-contact insurance anchor.  Once that
            // anchor is complete and the approaching melee pack is visible,
            // overlap a second before the first structure is focused down;
            // waiting for a literal hard breach loses the wall and the Probe
            // line in the same engagement.
            !widenRushScreen ? 1 :
            // One Cannon plus the mobile screen is the right emergency
            // trade while the Core is missing. Starting two long Cannon
            // builds at once delayed the Core until eight minutes and let a
            // Dragoon/DT transition scale uncontested.
            count(state, UnitKind::cyberneticsCore, true) == 0 ? 1 :
            count(state, UnitKind::roboticsFacility) == 0 ? (secondCannonSafe ? 2 : 1) :
            state.self.minerals >= 800 ? 4 : 3;
        if (rushStaticNeeded && cannonsReady < rushCannonTarget) {
            goal(result, GoalKind::build, UnitKind::forge, 1, 112,
                 "anchor the mineral line against observed Zealot pressure", true);
            goal(result, GoalKind::build, UnitKind::photonCannon, rushCannonTarget, 111,
                 "overlap the emergency mineral-line anchor", true);
        }
    const auto lateRangedStatic = state.frame >= 6 * 60 * 24 &&
                                   (threat.combatEnemiesNearMain > 0 ||
                                        visibleGroundContact ||
                                        threat.mostLikely == EnemyPlan::heavyPressure) &&
                                      dragoonsReady >= 2 && cannonsReady < 2 &&
                                      // When the threat is cloaked, the
                                      // Observatory/Observer chain is the
                                      // urgent resource sink.  Letting this
                                      // generic ranged-pressure branch outrank
                                      // it repeatedly delayed detection until
                                      // the mineral line was already lost.
                                      !cloakedThreat;
        if (lateRangedStatic) {
            goal(result, GoalKind::build, UnitKind::forge, 1, 110,
                 "restore a home anchor against the ranged push", true);
            goal(result, GoalKind::build, UnitKind::photonCannon, 2, 109,
                 "two-Cannon home screen against the ranged push", true);
        }
        const auto directBreach = hardBreachAtMain(state);
        const auto forwardRangedScreen = dragoonsReady >= 4 && !directBreach &&
                                         !visibleGroundContact;
        const auto visibleMeleeContact = zealotsReady >= 2 &&
            std::ranges::any_of(state.enemy.units, [](const UnitSnapshot& enemy) {
                return enemy.visible && enemy.completed && !enemy.flying &&
                       isCombatUnit(enemy.kind) && enemy.position.valid();
            });
        const auto forwardMeleeScreen = visibleMeleeContact && !directBreach &&
                                        !visibleGroundContact &&
                                        mobileOpening >= 8 &&
                                        (!currentMeleeRush || zealotsReady >= 8);
        result.attackThreshold = forwardRangedScreen ? 1.22 :
                                 (forwardMeleeScreen ? 1.32 : 1.50);
        result.minimumAttackSize = (forwardRangedScreen || forwardMeleeScreen)
                                       ? 8 : 14;
        if (forwardRangedScreen || forwardMeleeScreen) {
            result.posture = Posture::pressure;
            const auto homePosition = ourMain(state);
            if (homePosition.valid()) {
                const auto nearest = std::ranges::min_element(
                    state.enemy.units, {}, [&homePosition](const UnitSnapshot& enemy) {
                        return enemy.visible && enemy.completed && !enemy.flying &&
                                       isCombatUnit(enemy.kind) && enemy.position.valid()
                                   ? distanceSquared(homePosition, enemy.position)
                                   : std::numeric_limits<int>::max();
                    });
                if (nearest != state.enemy.units.end() && nearest->position.valid()) {
                    result.rallyPoint = moveToward(homePosition, nearest->position, 320.0);
                }
            }
            result.name += forwardRangedScreen ? " [forward ranged intercept]"
                                               : " [forward melee intercept]";
        }
        if (count(state, UnitKind::gateway) > 0 &&
            count(state, UnitKind::zealot) == 0) {
            goal(result, GoalKind::train, UnitKind::zealot, 1, 105,
                 "field one mobile defender before adding more structures", true);
        }
        if (cannonsReady >= 2 || zealotsReady >= 2) {
            // Income is a live invariant during the hold. Once the line drops
            // below fourteen workers, queue the next Probe ahead of optional
            // Core/Cannon waits; otherwise those blocking goals can reserve
            // the entire bank and turn a recoverable raid into a zero-worker
            // death spiral.
            const auto probePriority = workers < 14 &&
                                       state.enemy.race == Race::protoss &&
                                       visibleGroundContact
                                           ? 123
                                           : 104;
            goal(result, GoalKind::train, UnitKind::probe, cannonsReady >= 2 ? 10 : 12,
                 probePriority,
                 "fund sustained mirror defense behind the Cannon screen", true);
            if (count(state, UnitKind::gateway, true) > 0) {
                goal(result, GoalKind::build, UnitKind::shieldBattery, 1, 103,
                     "finish defensive sustain before the ongoing Zealot target", true);
            }
        }
        if (count(state, UnitKind::forge) > 0) {
        const auto emergencyCannonTarget =
                workers < 8 ? cannonsReady :
                cannonsReady >= 2 ? 2 :
                !hardBreachAtMain(state) ? 1 :
                count(state, UnitKind::cyberneticsCore, true) > 0 ? 3 : 2;
        goal(result, GoalKind::build, UnitKind::photonCannon,
             emergencyCannonTarget, 101,
             "reinforce an existing static defense investment", true);
        }
        goal(result, GoalKind::build, UnitKind::gateway, 2, 100,
             "guarantee two-gate defensive throughput", true);
        const auto emergencyMeleeTarget = cannonsReady >= 2 &&
                                                  count(state, UnitKind::cyberneticsCore, true) > 0 &&
                                                  (!enemyCoreScouted || enemyCoreFinished)
                                              ? 4
                                              : 8;
        goal(result, GoalKind::train, UnitKind::zealot, emergencyMeleeTarget, 99,
             "continuously reinforce against opening pressure", true);
        const auto directMeleePressure = currentMeleeRush &&
            (threat.combatEnemiesNearMain > 0 || visibleGroundContact ||
             hardBreachAtMain(state));
        if (directMeleePressure) {
            // A completed Core must not turn the only free Gateway into a
            // Dragoon while Zealots are fighting at the mineral line.  Keep
            // the emergency composition melee-only until the rush is cleared
            // or the two-Cannon screen can safely absorb the transition.
            result.composition = {{UnitKind::zealot, 1.0}};
        }
        goal(result, GoalKind::build, UnitKind::shieldBattery, 1, 97,
             "defensive sustain", true);
    }
    if (canTransition) {
        // A completed screen is a window to build a sustainable ranged army.
        // Repeated melee reservations previously kept twelve Probes replacing
        // Zealots while an opponent with a scouted Core scaled Dragoons.
        result.desiredGasWorkers = std::max(3, result.desiredGasWorkers);
        goal(result, GoalKind::train, UnitKind::probe, 16, 104,
             "grow the income protected by the completed defensive screen", true);
        goal(result, GoalKind::build, UnitKind::cyberneticsCore, 1, 102,
             "convert the defensive window into ranged production", true);
        goal(result, GoalKind::build, UnitKind::assimilator, 1, 102,
             "fund the ranged transition before repeated melee replacement", true);
        const auto gasReady = state.self.gas >= 50;
        const auto meleePressure = currentMeleeRush &&
            (threat.combatEnemiesNearMain > 0 || visibleGroundContact ||
             hardBreachAtMain(state));
        if (earlyMeleeScreen &&
            (!meleePressure || zealotsReady >= 6 || cannonsReady >= 2)) {
            goal(result, GoalKind::train, UnitKind::dragoon, 4, 101,
                 "establish a ranged defensive core", gasReady);
        }
        if (count(state, UnitKind::dragoon, true) >= 2) {
            technologyGoal(result, TechnologyKind::singularityCharge, 1, 102,
                           "contest ranged pressure behind the mobile screen", true);
        }
        if (rangedOpening) {
            const auto meleeScreen = std::clamp(recentEnemyCount(state, UnitKind::zealot) / 2,
                                               2, 4);
            for (auto& objective : result.goals) {
                if (objective.target == UnitKind::zealot) {
                    objective.desiredCount = std::min(objective.desiredCount, meleeScreen);
                } else if (objective.target == UnitKind::photonCannon &&
                           objective.desiredCount > 2) {
                    objective.priority = 95;
                    objective.blocking = false;
                }
            }
            result.composition = {{UnitKind::dragoon, 0.80}, {UnitKind::zealot, 0.20}};
        }
    }
    if (twoGateOpening) {
        // A scouted double Gateway without ranged tech warrants a compact
        // static screen. Buy the first mobile units before that investment,
        // and do not bank an expansion while the first flood is unaccounted for.
        result.name += " [scouted two-gate screen]";
        result.desiredBases = 1;
        result.maximumBases = std::max(1, count(state, UnitKind::nexus));
        result.desiredWorkers = std::min(result.desiredWorkers, 22);
        // Stop gas while the opponent is still melee-only. The Forge/Cannon
        // anchor must begin as soon as 150 minerals are available; leaving
        // three Probes on gas here delayed it until the mineral line was
        // already lost in the live rush trace.
        const auto gasTransitionWindow = state.frame >= 3 * 60 * 24 + 24 * 24 &&
                                         zealotsReady >= 2 &&
                                         threat.combatEnemiesNearMain == 0 &&
                                         !hardBreachAtMain(state);
        result.desiredGasWorkers = gasTransitionWindow ? 3 : 0;
        // The first response window is too short for a Forge plus a Cannon
        // while the mobile screen is still tiny. Keep four bodies mobile
        // before paying the Forge tax; that gives the Probe line a real
        // escort and lets the first Cannon finish behind the screen instead
        // of being surrounded during construction.
        const auto mobileRushOpening = state.frame < 4 * 60 * 24 &&
                                       zealotsReady < 4;
        // A covert-tech suspicion is normally enough to reserve Robotics and
        // detection before the first visible ranged unit.  It must not win
        // over a current two-Gateway melee opening, though: in the live
        // mirror trace the suspicion was raised one planning pass before
        // FastRush was recognized, so the Forge was skipped at 150 minerals
        // and the later Cannon could not finish before contact.  The
        // melee-only signal is deliberately current and takes precedence.
        const auto detectionFirst = earlyDetectionCheckpoint &&
                                     !currentMeleeRush &&
                                     !hardBreachAtMain(state);
        if (mobileRushOpening) {
            // Keep the early hidden-melee insurance Forge alive once the
            // rush label catches up.  The old cleanup erased every static
            // goal in this window, so a two-Gateway opponent could trigger
            // the exact frame where the Forge became affordable and leave
            // us with no Cannon path at all.  Only the insurance Forge is
            // preserved; Cannons and Batteries still wait for the mobile
            // screen as intended.
            const auto preserveEmergencyForge = hiddenMeleeTechSignal ||
                                                 rushStaticNeeded;
            std::erase_if(result.goals, [preserveEmergencyForge](
                              const ProductionGoal& candidate) {
                if (candidate.target == UnitKind::forge &&
                    preserveEmergencyForge) {
                    return false;
                }
                return candidate.target == UnitKind::forge ||
                       candidate.target == UnitKind::photonCannon ||
                       candidate.target == UnitKind::shieldBattery;
            });
            goal(result, GoalKind::train, UnitKind::zealot, 6, 114,
                 "mobile-first response to scouted double production", true);
        } else {
            // Once the mobile screen has four bodies, gas is no longer a
            // luxury: the opponent's next wave is likely Dragoons. Re-enable
            // three gas workers before the eight-minute scouting window ends
            // so the completed Core can actually turn the saved bank into
            // ranged defenders.
            result.desiredGasWorkers = 3;
            goal(result, GoalKind::train, UnitKind::zealot, 2, 105,
                 "field a mobile screen against scouted double production", true);
            if (!detectionFirst) {
                goal(result, GoalKind::build, UnitKind::forge, 1, 112,
                     "prepare a static answer to scouted melee production", true);
            }
            // One Cannon is enough to stabilize a mobile-first opening. Only
            // buy the second mineral sink when the army is actually moving
            // toward the main (or a hard breach is already present); a timer
            // alone made the bot spend the exact minerals needed for the Core
            // and Robotics transition while BananaBrain's army grew.
            const auto staticCannonTarget =
                (hardBreachAtMain(state) || threat.immediateGround > 0.25 ||
                 threat.approachingArmyValue >= 4.0) &&
                        (state.frame >= 6 * 60 * 24 || zealotsReady >= 4)
                    ? 2
                    : 1;
            if (!detectionFirst || staticCannonTarget > 0) {
                goal(result, GoalKind::build, UnitKind::photonCannon,
                     detectionFirst ? std::min(1, staticCannonTarget) : staticCannonTarget,
                     111, "support the mobile army against a melee flood", true);
            }
        }

        // Once the first ranged screen is online, meet a suspected flood in
        // the open instead of waiting for it to enter the Probe line. The
        // small 160px default rally is deliberately conservative for normal
        // games, but is too close to the Nexus for a scouted two-Gateway all-in.
        // Only do this while the threat is still outside the main; the direct
        // breach branch above must retain control once contact is established.
        const auto hardBreach = hardBreachAtMain(state);
        if (!mobileRushOpening && state.frame >= 4 * 60 * 24 &&
            mobileOpening >= 2 && !hardBreach && !visibleGroundContact) {
            result.posture = Posture::pressure;
            if (home.valid()) {
                const auto nearest = std::ranges::min_element(
                    state.enemy.units, {}, [&home](const UnitSnapshot& enemy) {
                        return enemy.visible && enemy.completed && !enemy.flying &&
                                       isCombatUnit(enemy.kind) && enemy.position.valid()
                                   ? distanceSquared(home, enemy.position)
                                   : std::numeric_limits<int>::max();
                    });
                if (nearest != state.enemy.units.end() && nearest->position.valid()) {
                    result.rallyPoint = moveToward(home, nearest->position, 320.0);
                } else if (result.attackTarget.valid()) {
                    result.rallyPoint = moveToward(home, result.attackTarget, 640.0);
                }
            }
            result.name += " [forward intercept]";
        }

        // A compact pressure group is still useful before the full ranged
        // army exists.  Stardust locks an attack/contain target once the map
        // is quiet and its home screen is stable; mirror that behavior here
        // instead of leaving four-to-six Zealots parked at the Nexus until a
        // fourteen-unit threshold becomes reachable.  Require two completed
        // Cannons (or an Observer) while covert tech is suspected so this is
        // not a blind march into an undetected DT line.
        const auto perimeterEnemyCount = static_cast<int>(std::ranges::count_if(
            state.enemy.units, [&home](const UnitSnapshot& enemy) {
                return enemy.visible && enemy.completed && !enemy.flying &&
                       isCombatUnit(enemy.kind) && enemy.position.valid() &&
                       (!home.valid() || distanceSquared(home, enemy.position) <=
                                             900 * 900);
            }));
        const auto safeVisibleContact = visibleGroundContact &&
                                        cannonsReady >= 2 &&
                                        perimeterEnemyCount <= std::max(2, mobileOpening / 2);
        const auto compactMeleeTiming = state.frame >= 5 * 60 * 24 &&
                                        state.frame < 11 * 60 * 24 &&
                                        mobileOpening >= 4 &&
                                        (cannonsReady >= 2 ||
                                         count(state, UnitKind::observer, true) > 0) &&
                                        !hardBreach &&
                                        (!visibleGroundContact || safeVisibleContact) &&
                                        threat.combatEnemiesNearMain == 0 &&
                                        threat.immediateGround <= 0.35 &&
                                        (threat.uncertainty >= 0.90 ||
                                         threat.mostLikely == EnemyPlan::fastExpand);
        if (compactMeleeTiming) {
            result.posture = Posture::pressure;
            result.attackThreshold = std::min(result.attackThreshold, 1.22);
            result.minimumAttackSize = std::min(result.minimumAttackSize, 4);
            const auto target = enemyMain(state);
            if (target.valid()) {
                result.attackTarget = target;
                result.rallyPoint = target;
            }
            result.name += " [compact melee timing]";
        }

        // A remembered enemy Core is useful for our tech schedule, but it is
        // not evidence that the current fight is ranged.  While the opponent
        // still has no recent Dragoon/Reaver and our static anchor is missing,
        // keep every Gateway on Zealots.  Otherwise the generic transition
        // goal can spend the exact 125 gas/ minerals on a Dragoon while the
        // first melee wave is already crossing the map.
        if (currentMeleeRush && zealotsReady < 6 && cannonsReady == 0) {
            result.composition = {{UnitKind::zealot, 1.0}};
            result.goals.erase(
                std::remove_if(result.goals.begin(), result.goals.end(),
                               [](const ProductionGoal& candidate) {
                                   return candidate.goal == GoalKind::train &&
                                          candidate.target == UnitKind::dragoon &&
                                          candidate.priority < 114;
                               }),
                result.goals.end());
        }
    }

    const auto visibleEnemyArmy = std::ranges::any_of(
        state.enemy.units, [](const UnitSnapshot& enemy) {
            return enemy.visible && enemy.completed && !enemy.flying &&
                   isCombatUnit(enemy.kind);
        });
    const auto earlyMeleeStrike = currentMeleeRush &&
                                  state.frame >= 4 * 60 * 24 &&
                                  state.frame < 8 * 60 * 24 &&
                                  mobileOpening >= 4 &&
                                  threat.mostLikely == EnemyPlan::fastExpand &&
                                  !visibleEnemyArmy &&
                                  threat.combatEnemiesNearMain == 0 &&
                                  !activeApproach(state, threat) &&
                                  !hardBreachAtMain(state);
    if (earlyMeleeStrike) {
        // A compact six-Zealot mirror force should pressure production before
        // both sides have enough Dragoons to turn the game into a bank race.
        // This is only enabled after the mobile-first opening is assembled;
        // current contact or a crossing army keeps the emergency branch in
        // control instead.
        const auto target = enemyMain(state);
        result.posture = Posture::attack;
        result.attackThreshold = 1.10;
        result.minimumAttackSize = 5;
        if (target.valid()) {
            result.attackTarget = target;
            result.rallyPoint = target;
        }
        result.name += " [early melee strike]";
    }

    // Once the first ranged screen has a decisive local edge, convert that
    // edge into map pressure instead of repeatedly meeting small waves on
    // the home perimeter.  The previous plan stayed in Pressure with a
    // perimeter rally, so the squad could win several defensive skirmishes
    // yet never force BananaBrain to defend its production.  Require a real
    // visible comparison and a clear main so this cannot turn an unseen
    // all-in into a blind march across the map.
    const auto visibleEnemyPower = [&state]() {
        double power = 0.0;
        for (const auto& unit : state.enemy.units) {
            if (!unit.visible || !unit.completed || unit.flying ||
                !isCombatUnit(unit.kind)) {
                continue;
            }
            power += unitStats(unit.kind).combatValue *
                     std::clamp(unit.healthFraction(), 0.15, 1.0);
        }
        return power;
    }();
    const auto ownMobilePower = [&state]() {
        double power = 0.0;
        for (const auto& unit : state.self.units) {
            if (!unit.completed || unit.flying || !isCombatUnit(unit.kind)) {
                continue;
            }
            power += unitStats(unit.kind).combatValue *
                     std::clamp(unit.healthFraction(), 0.15, 1.0);
        }
        return power;
    }();
    const auto directBreach = hardBreachAtMain(state);
    const auto visibleArmyNearHome = std::ranges::any_of(
        state.enemy.units, [&home](const UnitSnapshot& enemy) {
            return enemy.visible && enemy.completed && !enemy.flying &&
                   isCombatUnit(enemy.kind) && enemy.position.valid() &&
            (!home.valid() || distanceSquared(home, enemy.position) <= 960 * 960);
        });
    const auto earlyTimingAttack = state.frame >= 8 * 60 * 24 + 12 * 24 &&
                                   state.frame < 11 * 60 * 24 &&
                                   mobileOpening >= 8 && !visibleEnemyArmy &&
                                   !visibleArmyNearHome && !directBreach &&
                                   threat.mostLikely != EnemyPlan::fastRush &&
                                   (threat.mostLikely == EnemyPlan::unknown ||
                                    threat.uncertainty > 0.90);
    if (earlyTimingAttack) {
        // When the map is empty, a compact Zealot/Dragoon/Reaver group should
        // hit production before BananaBrain can bank a second wave. The home
        // guard formed by SquadPlanner keeps this from becoming an all-in.
        result.posture = Posture::attack;
        result.attackThreshold = std::min(result.attackThreshold, 1.20);
        result.minimumAttackSize = std::min(result.minimumAttackSize, 10);
        const auto target = enemyMain(state);
        if (target.valid()) {
            result.attackTarget = target;
            result.rallyPoint = target;
        }
        result.name += " [empty-map timing attack]";
    }
    const auto reaverTimingStrike = state.frame >= 9 * 60 * 24 &&
                                    state.frame < 13 * 60 * 24 &&
                                    count(state, UnitKind::reaver, true) >= 1 &&
                                    mobileOpening >= 12 &&
                                    threat.mostLikely != EnemyPlan::heavyPressure &&
                                    threat.combatEnemiesNearMain == 0 &&
                                    !visibleArmyNearHome && !visibleEnemyArmy &&
                                    !directBreach && enemyMain(state).valid();
    if (reaverTimingStrike) {
        // Once the first Reaver is ready, a compact timing attack is safer
        // than parking the army until BananaBrain's next wave is complete.
        // Keep this window bounded and require a clear home so an all-in never
        // overrides the emergency defense branch.
        result.posture = Posture::attack;
        result.attackThreshold = std::min(result.attackThreshold, 1.12);
        result.minimumAttackSize = std::min(result.minimumAttackSize, 10);
        result.attackTarget = enemyMain(state);
        result.rallyPoint = result.attackTarget;
        result.name += " [Reaver timing strike]";
    }
    const auto counterPushWindow = state.frame >= 8 * 60 * 24 &&
                                   mobileOpening >= 16 && !directBreach &&
                                   !visibleArmyNearHome &&
                                   visibleEnemyPower >= 3.0 &&
                                   ownMobilePower >= visibleEnemyPower * 1.35;
    if (counterPushWindow) {
        result.posture = Posture::attack;
        result.attackThreshold = std::min(result.attackThreshold, 1.18);
        result.minimumAttackSize = std::min(result.minimumAttackSize, 8);
        const auto target = enemyMain(state);
        if (target.valid()) {
            result.attackTarget = target;
            result.rallyPoint = target;
        }
        result.name += " [counter-push advantage]";
    }
    // A defended main is not a reason to stay on one base forever. Once the
    // first Robotics/Observer chain and a three-Cannon shell are complete, a
    // healthy 18-worker economy can start the natural even while a small
    // enemy group remains on the perimeter. Require a local power edge and no
    // hard breach so this is a controlled Stardust-style phase transition,
    // not a blind Nexus during an all-in.
    const auto defensiveEconomyWindow = state.frame >= 10 * 60 * 24 &&
                                        workers >= 18 && mobileOpening >= 10 &&
                                        count(state, UnitKind::photonCannon, true) >= 3 &&
                                        count(state, UnitKind::cyberneticsCore, true) > 0 &&
                                        count(state, UnitKind::roboticsFacility, true) > 0 &&
                                        count(state, UnitKind::observatory, true) > 0 &&
                                        !directBreach &&
                                        threat.combatEnemiesNearMain <= 2 &&
                                        (!activeApproach(state, threat) ||
                                         ownMobilePower >= visibleEnemyPower * 1.30) &&
                                        threat.immediateGround <= 0.55 &&
                                        (visibleEnemyPower <= 0.1 ||
                                         ownMobilePower >= visibleEnemyPower * 1.20);
    if (defensiveEconomyWindow) {
        result.desiredBases = std::max(result.desiredBases, 2);
        result.maximumBases = std::max(result.maximumBases, 2);
        result.posture = Posture::pressure;
        result.attackThreshold = std::min(result.attackThreshold, 1.30);
        result.minimumAttackSize = std::min(result.minimumAttackSize, 10);
        result.name += " [defensive economic transition]";
    }
    if ((roboticsNeeded || supportNeeded) && !hardBreachAtMain(state) &&
        !economicCounterWindow) {
        // Protect the Robotics -> Support Bay sequence from composition filler.
        // A brief pause is cheaper than entering the first Reaver fight with
        // an empty gas/mineral bank; direct pressure keeps the normal
        // defensive queue in control.
        const auto preserveDragoonScreen = supportNeeded && dragoonsReady < 6;
        const auto preserveMeleeScreen = currentMeleeRush &&
                                         state.frame < 10 * 60 * 24 &&
                                         zealotsReady < 8 &&
                                         (threat.uncertainty > 0.85 ||
                                          threat.mostLikely == EnemyPlan::fastRush);
        result.goals.erase(
            std::remove_if(result.goals.begin(), result.goals.end(),
                           [preserveDragoonScreen, preserveMeleeScreen](
                               const ProductionGoal& candidate) {
                               if (candidate.target == UnitKind::gateway &&
                                   candidate.goal == GoalKind::build &&
                                   candidate.priority < 106) {
                                   return true;
                               }
                               if (candidate.goal != GoalKind::train) return false;
                               switch (candidate.target) {
                                   case UnitKind::zealot: return !preserveMeleeScreen;
                                   case UnitKind::dragoon: return !preserveDragoonScreen;
                                   case UnitKind::reaver:
                                   case UnitKind::highTemplar:
                                   case UnitKind::darkTemplar:
                                   case UnitKind::archon:
                                   case UnitKind::darkArchon:
                                   case UnitKind::scout:
                                   case UnitKind::corsair:
                                   case UnitKind::carrier:
                                   case UnitKind::arbiter:
                                       return true;
                                   default:
                                       return false;
                               }
                           }),
            result.goals.end());
        result.composition.clear();
        result.desiredBases = std::min(result.desiredBases,
                                       std::max(1, count(state, UnitKind::nexus)));
        result.maximumBases = std::max(1, count(state, UnitKind::nexus));
        result.name += roboticsNeeded ? " [reserve Robotics tech]"
                                      : " [reserve Reaver tech]";
    }
    if (openingPressureExpected(state, threat)) {
        // A fixed one-base flood is beaten by compact two-base production,
        // not by taking a third Nexus while the first decisive army is still
        // assembling. The cap disappears after the opening pressure window.
        result.desiredBases = std::min(result.desiredBases, 2);
        result.desiredWorkers = std::min(result.desiredWorkers, 36);
    }
    return result;
}

void StrategyEngine::addPostPressureTransition(
    StrategicPlan& plan, const GameState& state, const ThreatAssessment& threat) {
    // A contained Gateway army needs a qualitative improvement before it can
    // win equal-income trades. Protect one splash-tech sequence behind an
    // existing screen instead of demanding eight units before funding it.
    if (state.enemy.race == Race::protoss && minute(state) >= 4 &&
        countRole(state, UnitRole::worker) >= 14 &&
        count(state, UnitKind::cyberneticsCore, true) > 0 &&
        ((count(state, UnitKind::photonCannon, true) > 0 &&
          count(state, UnitKind::zealot, true) + count(state, UnitKind::dragoon, true) >= 4) ||
         count(state, UnitKind::dragoon, true) >= 4) &&
        !hardBreachAtMain(state)) {
        goal(plan, GoalKind::build, UnitKind::roboticsFacility, 1, 113,
             "splash transition behind the established defensive screen", true);
        if (count(state, UnitKind::roboticsFacility) > 0)
            goal(plan, GoalKind::build, UnitKind::roboticsSupportBay, 1, 113,
                 "complete the first containment-breaking splash chain", true);
        if (count(state, UnitKind::roboticsSupportBay) > 0)
            goal(plan, GoalKind::train, UnitKind::reaver, 1, 114,
                 "first splash reinforcement for the defensive army", true);
    }
    if (state.enemy.race != Race::protoss || minute(state) < 6 ||
        count(state, UnitKind::nexus, true) == 0 ||
        countRole(state, UnitRole::worker) < 12 ||
        count(state, UnitKind::cyberneticsCore, true) == 0 ||
        threat.workerRush > 0.30 || threat.proxy + threat.staticContain > 0.34)
        return;

    const auto mobile = count(state, UnitKind::zealot, true) +
        count(state, UnitKind::dragoon, true) + count(state, UnitKind::reaver, true);
    if (mobile < 6) return;
    const auto observer = count(state, UnitKind::observer, true) > 0;
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
            if ((!enemy.visible && (state.frame - enemy.lastSeen > 8 * 24 ||
                                   state.frame < enemy.lastSeen)) ||
                !enemy.completed || enemy.disabled ||
                !enemy.position.valid() || isWorker(enemy.kind) ||
                enemy.groundWeapon.damage <= 0) continue;
            if (enemy.visible && distanceSquared(enemy.position, base.center) <= 320 * 320) return;
            if (distanceSquared(enemy.position, base.center) <= 960 * 960) {
                localEnemy.push_back(enemy);
                enemyPower += unitStats(enemy.kind).combatValue *
                    std::clamp(enemy.healthFraction(), 0.15, 1.0);
            }
        }
        for (const auto& friendly : state.self.units) {
            if (!friendly.completed || friendly.disabled || friendly.loaded ||
                friendly.hallucination || !friendly.position.valid() ||
                isBuilding(friendly.kind) || !isCombatUnit(friendly.kind) ||
                distanceSquared(friendly.position, base.center) > 960 * 960) continue;
            if (enemyPower > 0.0 && std::ranges::none_of(state.enemy.units,
                [&friendly, &base](const UnitSnapshot& enemy) {
                    return enemy.visible && enemy.completed && !enemy.disabled &&
                        !isWorker(enemy.kind) && enemy.groundWeapon.damage > 0 &&
                        distanceSquared(enemy.position, base.center) <= 960 * 960 &&
                        friendly.canAttack(enemy);
                })) continue;
            friendlyPower += unitStats(friendly.kind).combatValue *
                std::clamp(friendly.healthFraction(), 0.15, 1.0);
            localFriendly.push_back(friendly);
        }
        if (enemyPower > 0.0 && friendlyPower < enemyPower * 1.25) return;
        if (!localEnemy.empty() &&
            CombatEvaluator{}.evaluate(localFriendly, localEnemy, 1.10, 0.15, true).ratio < 1.10)
            return;
    }
    if (hardBreachAtMain(state)) return;

    plan.sustainEconomy = true;
    plan.breakContainment = count(state, UnitKind::dragoon, true) >= 2;
    plan.name = "PvP map control and expansion";
    plan.posture = plan.breakContainment ? Posture::pressure : Posture::hold;
    plan.minimumAttackSize = 6;
    plan.attackThreshold = 1.20;
    const auto bases = count(state, UnitKind::nexus);
    const auto completedBases = count(state, UnitKind::nexus, true);
    const auto workers = countRole(state, UnitRole::worker);
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
                 state.frame - defender.lastSeen > 30 * 24) ||
                distanceSquared(defender.position, depot.position) > 800 * 800) continue;
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
            if (site.ownerId != -1 || site.island || !site.center.valid() ||
                site.mineralsRemaining < 1000) continue;
            auto score = distance(ourMain(state), site.center);
            for (const auto& enemy : state.enemy.units) {
                if (!enemy.position.valid() || enemy.groundWeapon.damage <= 0 ||
                    (!enemy.visible && state.frame - enemy.lastSeen > 8 * 24)) continue;
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
            demand.desiredCount = std::max(count(state, UnitKind::photonCannon), bases);
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

void StrategyEngine::addHarassmentProduction(StrategicPlan& plan, const GameState& state) {
    const auto bases = count(state, UnitKind::nexus, true);
    const auto workers = countRole(state, UnitRole::worker);
    const auto screen = count(state, UnitKind::dragoon, true) + count(state, UnitKind::zealot, true);
    if (minute(state) < 7 || bases < 2 || workers < 28 || screen < 8 ||
        plan.prioritizeReinforcements || plan.posture == Posture::defend ||
        plan.posture == Posture::recover || hardBreachAtMain(state)) return;
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
    if (count(state, UnitKind::reaver) >= armyReavers)
        goal(plan, GoalKind::train, UnitKind::shuttle, plan.harassmentDrops, 104,
             "dedicated transport to bypass the defended entrance", true);
    if (count(state, UnitKind::shuttle, true) > 0)
        technologyGoal(plan, TechnologyKind::graviticDrive, 1, 87,
                       "faster drop entry and extraction");
    if (state.enemy.race == Race::zerg && recentEnemyCount(state, UnitKind::overlord) > 0 &&
        count(state, UnitKind::stargate) > 0)
        goal(plan, GoalKind::train, UnitKind::corsair, 4, 94,
             "dedicated Overlord hunters alongside worker drops");
    if (count(state, UnitKind::templarArchives, true) > 0) {
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
    const auto bases = count(state, UnitKind::nexus);
    const auto workers = countRole(state, UnitRole::worker);
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
        if ((enemy.visible || state.frame - enemy.lastSeen <= 8 * 24) &&
            std::ranges::any_of(state.bases, [&](const BaseSnapshot& base) {
                return base.ownerId == state.self.id &&
                    distanceSquared(base.center, enemy.position) <= 800 * 800;
            })) return;
        if (!isBuilding(enemy.kind)) {
            if (enemy.visible || state.frame - enemy.lastSeen <= 90 * 24)
                enemyMobilePower += unitStats(enemy.kind).combatValue * enemy.healthFraction();
        }
        if ((isStaticDefense(enemy.kind) ||
             (enemy.kind == UnitKind::siegeTank && enemy.groundWeapon.maxRange >= 320)) &&
            std::ranges::any_of(state.bases, [&](const BaseSnapshot& base) {
                return state.enemy.id >= 0 && base.ownerId == state.enemy.id &&
                    distanceSquared(base.center, enemy.position) <= 800 * 800;
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
        if (base.ownerId != -1 || base.island || !base.center.valid() ||
            base.mineralsRemaining < 1500) continue;
        auto danger = false;
        auto enemyDistance = 4096.0;
        for (const auto& enemy : state.enemy.units) {
            if (!enemy.position.valid() ||
                (!isBuilding(enemy.kind) && !enemy.visible && state.frame - enemy.lastSeen > 30 * 24)) continue;
            enemyDistance = std::min(enemyDistance, distance(base.center, enemy.position));
            if ((enemy.groundWeapon.damage > 0 || enemy.role == UnitRole::resourceDepot) &&
                distanceSquared(base.center, enemy.position) <= 960 * 960) danger = true;
        }
        if (danger) continue;
        auto friendlyDistance = distance(home, base.center);
        for (const auto& owned : state.bases)
            if (owned.ownerId == state.self.id && owned.center.valid())
                friendlyDistance = std::min(friendlyDistance, distance(owned.center, base.center));
        const auto score = friendlyDistance - enemyDistance * 0.20;
        if (score < bestScore) { bestScore = score; site = base.center; }
    }
    const auto pending = bases > count(state, UnitKind::nexus, true);
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
    const auto bases = std::max(1, count(state, UnitKind::nexus));
    const auto completedBases = std::max(1, count(state, UnitKind::nexus, true));
    const auto workers = countRole(state, UnitRole::worker);
    const auto pylons = count(state, UnitKind::pylon);
    const auto pendingPylons = static_cast<int>(std::ranges::count_if(
        state.self.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::pylon && !unit.completed;
        }));
    const auto activeProduction = count(state, UnitKind::gateway, true) +
                                  count(state, UnitKind::roboticsFacility, true) +
                                  count(state, UnitKind::stargate, true);
    const auto desiredBuffer = state.self.supplyTotal <= 18
                                   ? 2
                                   : std::clamp(4 + activeProduction * 2, 6, 18);
    const auto projectedSupply = state.self.supplyTotal + pendingPylons * 16;
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
                                       state.frame >= 45 * 24);
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
    const auto gateways = count(state, UnitKind::gateway);
    const auto protectPvPTech = state.enemy.race == Race::protoss &&
        ((count(state, UnitKind::cyberneticsCore) > 0 &&
          count(state, UnitKind::roboticsFacility) == 0 &&
          count(state, UnitKind::zealot, true) >= 4) ||
         (count(state, UnitKind::roboticsFacility) > 0 &&
          count(state, UnitKind::roboticsSupportBay) == 0));
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
    if (!protectPvPTech && !plan.sustainEconomy && state.frame >= 4 * 60 * 24 && bases >= plan.desiredBases &&
        gateways < throughputTarget) {
        goal(plan, GoalKind::build, UnitKind::gateway, throughputTarget,
             observedParity > (workers + 5) / 6 ? 78 : 73,
             observedParity > (workers + 5) / 6
                 ? "match scouted enemy production capacity"
                 : "match army throughput to the mining economy");
    }
    // A large residual bank still means infrastructure is the bottleneck,
    // even if recent worker losses make the throughput estimate conservative.
    if (!protectPvPTech && state.frame >= 4 * 60 * 24 && state.self.minerals >= 650 &&
        bases >= plan.desiredBases && gateways < productionCeiling) {
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
                count(state, UnitKind::zealot, true) +
                count(state, UnitKind::dragoon, true) +
                count(state, UnitKind::darkTemplar, true);
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

        if (hydraLurker >= 7) {
            plan.name += " [anti-hydra splash]";
            goal(plan, GoalKind::build, UnitKind::roboticsSupportBay, 1, 84,
                 "unlock reavers against observed ground mass");
            goal(plan, GoalKind::train, UnitKind::reaver,
                 std::clamp(hydraLurker / 5, 2, 4), 85,
                 "splash clustered hydralisks and lurkers");
            goal(plan, GoalKind::train, UnitKind::observer, 3, 91,
                 "maintain lurker detection", true);
            setCompositionWeight(plan, UnitKind::reaver, 0.16);
            setCompositionWeight(plan, UnitKind::highTemplar, 0.28);
        }
        if (zergAir >= 4) {
            plan.name += " [anti-air control]";
            goal(plan, GoalKind::train, UnitKind::corsair,
                 std::clamp(4 + zergAir / 2, 5, 10), 94,
                 "win air control against observed Zerg flyers", true);
            goal(plan, GoalKind::train, UnitKind::dragoon,
                 std::clamp(zergAir, 6, 12), 89,
                 "protect ground army from Zerg flyers");
            setCompositionWeight(plan, UnitKind::corsair, 0.28);
            setCompositionWeight(plan, UnitKind::dragoon, 0.22);
        }
        if (lateGround >= 3) {
            goal(plan, GoalKind::train, UnitKind::highTemplar, 6, 88,
                 "zone ultralisks and defiler support");
            goal(plan, GoalKind::train, UnitKind::reaver, 3, 82,
                 "add durable late-game ground splash");
            setCompositionWeight(plan, UnitKind::highTemplar, 0.30);
            setCompositionWeight(plan, UnitKind::archon, 0.18);
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
    const GameState& state) {
    const auto workers = countRole(state, UnitRole::worker);
    const auto nexuses = count(state, UnitKind::nexus);
    const auto completedNexuses = count(state, UnitKind::nexus, true);
    const auto pendingNexuses = nexuses - completedNexuses;
    const auto activeBases = static_cast<int>(std::ranges::count_if(
        state.bases, [&state](const BaseSnapshot& base) {
            return base.ownerId == state.self.id && base.mineralsRemaining > 1000;
        }));

    if (nexuses == 0 && workers > 0) {
        plan.name = "Emergency Nexus recovery";
        plan.posture = Posture::recover;
        plan.desiredBases = 1;
        plan.desiredWorkers = std::max(12, workers);
        goal(plan, GoalKind::expand, UnitKind::nexus, 1, 100,
             "replace the lost economy anchor", true);
    }

    if (state.frame >= 4 * 60 * 24 && completedNexuses > 0 &&
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
    const auto mobileAnchor = count(state, UnitKind::zealot, true) >= 3 ||
                              count(state, UnitKind::dragoon, true) >= 2;
    const auto completedCannons = count(state, UnitKind::photonCannon, true);
    const auto staticRecoveryAnchor = completedCannons >= 3;
    const auto defensiveAnchor = completedCannons > 0 ||
                                 count(state, UnitKind::shieldBattery, true) > 0 ||
                                 mobileAnchor;
    const auto recoveryCanSpend = plan.posture != Posture::defend ||
                                  mobileAnchor || staticRecoveryAnchor || workers <= 3;
    if (state.frame >= 4 * 60 * 24 && completedNexuses > 0 && workers < 8 &&
        recoveryCanSpend && (defensiveAnchor || workers <= 3)) {
        plan.name += " [critical worker floor]";
        plan.desiredWorkers = std::max(plan.desiredWorkers, 8);
        goal(plan, GoalKind::train, UnitKind::probe, 8, 105,
             "rebuild a minimum income behind the defensive screen", true);
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

    const auto unpowered = std::ranges::any_of(
        state.self.units, [](const UnitSnapshot& unit) {
            return unit.completed && isBuilding(unit.kind) &&
                   unitStats(unit.kind).requiresPsi && !unit.powered;
        });
    if (unpowered) {
        const auto pylons = count(state, UnitKind::pylon);
        goal(plan, GoalKind::build, UnitKind::pylon, pylons + 1, 98,
             "restore power to disabled production", true);
    }

    // A large mineral bank means production, not another passive combat-unit
    // target, is the bottleneck. Scale infrastructure with the live economy.
    if (state.self.minerals >= 900 && completedNexuses > 0) {
        const auto targetGateways = std::clamp(completedNexuses * 3, 3, 12);
        goal(plan, GoalKind::build, UnitKind::gateway, targetGateways, 73,
             "convert excess mineral bank into production");
        if (state.self.gas >= 500 && minute(state) >= 12) {
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
            if (supplyAtLeast(state, 10)) {
                goal(plan, GoalKind::build, UnitKind::gateway,
                     minute(state) < 8 ? 2 : 5, 91,
                     "opponent-specific pressure production");
            }
            if (supplyAtLeast(state, 14)) {
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
            if (minute(state) < 5 && !supplyAtLeast(state, 24)) return;
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
    constexpr auto clearWindow = 8 * 24;
    // A defensive candidate is not evidence by itself: the matchup planner
    // can remain defensive for several frames after a threat has cleared.
    // Only reset the emergency timer for a current breach, a meaningful army
    // approach, explicit rush/proxy evidence, or a very early high-pressure
    // opening. This prevents a stale nearby army from self-sustaining Defend.
    const auto directBreach = hardBreachAtMain(state);
    const auto inferredBreach = threat.combatEnemiesNearMain > 0 &&
                                (state.enemy.units.empty() || directBreach);
    const auto meaningfulApproach = !candidate.breakContainment && activeApproach(state, threat) &&
                                    (directBreach ||
                                     threat.approachingArmyValue >= 10.0);
    const auto emergencyEvidence = candidate.posture == Posture::recover ||
                                   inferredBreach || meaningfulApproach ||
                                   threat.workerRush > 0.30 ||
                                   threat.proxy + threat.staticContain > 0.34 ||
                                   (state.frame < 8 * 60 * 24 &&
                                    threat.immediateGround > 0.60 &&
                                    (state.enemy.units.empty() || directBreach));

    if (!initialized_) {
        initialized_ = true;
        posture_ = candidate.posture;
        if (emergencyEvidence) lastEmergencyFrame_ = state.frame;
        return candidate;
    }

    if (emergencyEvidence) {
        posture_ = candidate.posture == Posture::recover
                       ? Posture::recover
                       : Posture::defend;
        lastEmergencyFrame_ = state.frame;
        candidate.posture = posture_;
        return candidate;
    }

    if ((posture_ == Posture::defend || posture_ == Posture::recover) &&
        lastEmergencyFrame_ >= 0 &&
        state.frame - lastEmergencyFrame_ < clearWindow) {
        candidate.posture = posture_;
        candidate.attackThreshold = std::max(candidate.attackThreshold, 1.40);
        candidate.name += " [regrouping after defense]";
        return candidate;
    }

    posture_ = candidate.posture;
    return candidate;
}

void StrategicDirector::reset() noexcept {
    posture_ = Posture::hold;
    lastEmergencyFrame_ = -1;
    initialized_ = false;
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
