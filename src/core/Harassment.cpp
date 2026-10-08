#include "protodd/Harassment.hpp"

#include "protodd/UnitCatalog.hpp"
#include "protodd/RouteSafety.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace protodd {
namespace {

bool recent(const GameState& state, const UnitSnapshot& enemy) {
    return enemy.position.valid() && (enemy.visible || state.frame - enemy.lastSeen <= 20 * 24);
}

MovementFootprint movementFootprint(const UnitSnapshot& unit) noexcept {
    return {unit.dimensionLeft, unit.dimensionRight,
            unit.dimensionUp, unit.dimensionDown};
}

bool raidTargetSafe(const GameState& state, const UnitSnapshot& raider,
                    const Position target) {
    for (const auto& enemy : state.enemy.units) {
        if (!enemy.completed || enemy.disabled || enemy.loaded || enemy.hallucination ||
            (unitStats(enemy.kind).requiresPsi && !enemy.powered) ||
            isWorker(enemy.kind) || !enemy.position.valid() ||
            (!isBuilding(enemy.kind) && !recent(state, enemy))) continue;
        const auto detector = raider.cloaked && (enemy.role == UnitRole::detector ||
            enemy.kind == UnitKind::observer || enemy.kind == UnitKind::scienceVessel ||
            enemy.kind == UnitKind::photonCannon || enemy.kind == UnitKind::missileTurret ||
            enemy.kind == UnitKind::sporeColony || enemy.kind == UnitKind::overlord);
        if (raider.kind == UnitKind::darkTemplar && raider.cloaked && !raider.underAttack && !detector)
            continue;
        const auto& weapon = raider.flying ? enemy.airWeapon : enemy.groundWeapon;
        const auto clearance = detector ? std::max(352, enemy.sightRange + 32) :
            std::max(160, weapon.maxRange + 96);
        if ((weapon.damage > 0 || detector) &&
            distanceSquared(enemy.position, target) <= clearance * clearance) return false;
    }
    return true;
}

std::vector<Position> raidPath(const UnitSnapshot& raider, const Position target,
                              const bool flyingRoute, const NavigationGrid* navigation,
                              HarassmentRouteBudget& budget) {
    if (!raider.position.valid() || !target.valid()) return {};
    if (flyingRoute || raider.flying || navigation == nullptr || navigation->empty())
        return {raider.position, target};
    const auto footprint = movementFootprint(raider);
    if (navigation->lineWalkable(raider.position, target, footprint))
        return {raider.position, target};
    if (budget.pathSearches >= HarassmentRouteBudget::maximumPathSearches) {
        ++budget.deferredChecks;
        return {};
    }
    ++budget.pathSearches;
    auto route = navigation->findPath(raider.position, target,
        HarassmentRouteBudget::maximumPathExpansions, footprint);
    if (!route.reached()) return {};
    return std::move(route.points);
}

bool safeRaidPath(const GameState& state, const UnitSnapshot& raider,
                  const Position target, const bool flyingRoute,
                  const std::span<const Position> route) {
    if (route.empty() || !raidTargetSafe(state, raider, target)) return false;
    for (const auto& enemy : state.enemy.units) {
        if (!enemy.completed || enemy.disabled || enemy.loaded || enemy.hallucination ||
            (unitStats(enemy.kind).requiresPsi && !enemy.powered) ||
            isWorker(enemy.kind) || !enemy.position.valid() ||
            (!isBuilding(enemy.kind) && !recent(state, enemy))) continue;
        const auto detector = raider.cloaked && (enemy.role == UnitRole::detector ||
            enemy.kind == UnitKind::observer || enemy.kind == UnitKind::scienceVessel ||
            enemy.kind == UnitKind::photonCannon || enemy.kind == UnitKind::missileTurret ||
            enemy.kind == UnitKind::sporeColony || enemy.kind == UnitKind::overlord);
        // Own-unit detected is not the enemy's detection access. A covert DT
        // can pass ordinary defenders, but never known detection or real hits.
        if (raider.kind == UnitKind::darkTemplar && raider.cloaked && !raider.underAttack && !detector)
            continue;
        const auto& weapon = raider.flying ? enemy.airWeapon : enemy.groundWeapon;
        const auto targetClearance = detector ? std::max(352, enemy.sightRange + 32) :
            std::max(160, weapon.maxRange + 96);
        if ((weapon.damage > 0 || detector) &&
            distanceSquared(enemy.position, target) <= targetClearance * targetClearance) return false;
        const auto& routeWeapon = (flyingRoute || raider.flying) ? enemy.airWeapon : enemy.groundWeapon;
        if (routeWeapon.damage <= 0 && !detector) continue;
        const auto clearance = detector ? std::max(352, enemy.sightRange + 32) :
            std::max(224, routeWeapon.maxRange + 96);
        for (std::size_t leg = 1; leg < route.size(); ++leg) {
            const auto from = route[leg - 1];
            const auto to = route[leg];
            const auto dx = static_cast<double>(to.x - from.x);
            const auto dy = static_cast<double>(to.y - from.y);
            const auto lengthSquared = dx * dx + dy * dy;
            const auto t = std::clamp(((enemy.position.x - from.x) * dx +
                (enemy.position.y - from.y) * dy) / std::max(1.0, lengthSquared), 0.0, 1.0);
            const auto nearestX = from.x + dx * t;
            const auto nearestY = from.y + dy * t;
            const auto ex = enemy.position.x - nearestX;
            const auto ey = enemy.position.y - nearestY;
            if (ex * ex + ey * ey <= static_cast<double>(clearance) * clearance) return false;
        }
    }
    return true;
}

bool safeRaidRoute(const GameState& state, const UnitSnapshot& raider,
                   const Position target, const bool flyingRoute,
                   const NavigationGrid* navigation, HarassmentRouteBudget& budget) {
    // A defended destination cannot become safe through a longer detour.
    // Reject it before any terrain search, then inspect the proven path once.
    if (!raidTargetSafe(state, raider, target)) return false;
    const auto route = raidPath(raider, target, flyingRoute, navigation, budget);
    return safeRaidPath(state, raider, target, flyingRoute, route);
}

double raidRouteExposure(const GameState& state, const UnitSnapshot& raider,
                        const std::vector<Position>& route, const bool flyingRoute) {
    auto totalRisk = 0.0;
    auto peakRisk = 0.0;
    auto threats = 0;
    for (const auto& enemy : state.enemy.units) {
        if (!enemy.completed || enemy.disabled || enemy.loaded || enemy.hallucination ||
            (unitStats(enemy.kind).requiresPsi && !enemy.powered) || isWorker(enemy.kind) ||
            !enemy.position.valid() || (!isBuilding(enemy.kind) && !recent(state, enemy))) continue;
        const auto detector = raider.cloaked && (enemy.role == UnitRole::detector ||
            enemy.kind == UnitKind::observer || enemy.kind == UnitKind::scienceVessel ||
            enemy.kind == UnitKind::photonCannon || enemy.kind == UnitKind::missileTurret ||
            enemy.kind == UnitKind::sporeColony || enemy.kind == UnitKind::overlord);
        if (raider.kind == UnitKind::darkTemplar && raider.cloaked &&
            !raider.underAttack && !detector) continue;
        const auto& weapon = (flyingRoute || raider.flying)
            ? enemy.airWeapon : enemy.groundWeapon;
        if (weapon.damage <= 0 && !detector) continue;
        const auto clearance = detector ? std::max(352, enemy.sightRange + 32) :
            std::max(224, weapon.maxRange + 96);
        auto nearest = std::numeric_limits<double>::infinity();
        for (std::size_t leg = 1; leg < route.size(); ++leg) {
            const auto from = route[leg - 1];
            const auto to = route[leg];
            const auto dx = static_cast<double>(to.x - from.x);
            const auto dy = static_cast<double>(to.y - from.y);
            const auto lengthSquared = dx * dx + dy * dy;
            const auto t = std::clamp(((enemy.position.x - from.x) * dx +
                (enemy.position.y - from.y) * dy) / std::max(1.0, lengthSquared), 0.0, 1.0);
            const auto ex = enemy.position.x - (from.x + dx * t);
            const auto ey = enemy.position.y - (from.y + dy * t);
            nearest = std::min(nearest, std::hypot(ex, ey));
        }
        if (!std::isfinite(nearest)) continue;
        // Safe paths stay outside one clearance. Give extra room a smooth
        // preference so alternatives do not all look identical at the limit.
        const auto risk = std::clamp(
            (2.0 * clearance - nearest) / static_cast<double>(clearance), 0.0, 1.0);
        totalRisk += risk;
        peakRisk = std::max(peakRisk, risk);
        ++threats;
    }
    return threats == 0 ? 0.0 : totalRisk / threats + peakRisk * 0.65;
}

Position raidWaypoint(const GameState& state, const UnitSnapshot& raider,
                      const Position target, const bool flyingRoute,
                      const NavigationGrid* navigation, HarassmentRouteBudget& budget) {
    if (!raidTargetSafe(state, raider, target)) return {-1, -1};
    const auto legPath = [&](const UnitSnapshot& from, const Position to) {
        return raidPath(from, to, flyingRoute, navigation, budget);
    };
    auto best = Position{-1, -1};
    auto bestCost = std::numeric_limits<double>::infinity();
    const auto baseRoute = legPath(raider, target);
    if (safeRaidPath(state, raider, target, flyingRoute, baseRoute)) {
        auto baseDistance = 0.0;
        for (std::size_t i = 1; i < baseRoute.size(); ++i)
            baseDistance += distance(baseRoute[i - 1], baseRoute[i]);
        best = target;
        bestCost = baseDistance + raidRouteExposure(state, raider, baseRoute, flyingRoute) * 320.0;
    }
    // Only attempt bypasses when terrain is available. Both legs must be
    // traversable and outside observed weapon/detector coverage.
    if (navigation == nullptr || navigation->empty()) return best;
    const auto length = std::max(1.0, distance(raider.position, target));
    const auto dx = static_cast<double>(target.x - raider.position.x) / length;
    const auto dy = static_cast<double>(target.y - raider.position.y) / length;
    for (const auto offset : {384, -384, 768, -768, 1152, -1152}) {
        const Position via{(raider.position.x + target.x) / 2 - static_cast<int>(dy * offset),
                           (raider.position.y + target.y) / 2 + static_cast<int>(dx * offset)};
        if (!via.valid() || via.x >= state.mapWidthPixels || via.y >= state.mapHeightPixels) continue;
        auto secondLeg = raider;
        secondLeg.position = via;
        if (!raidTargetSafe(state, raider, via)) continue;
        const auto firstPath = legPath(raider, via);
        if (!safeRaidPath(state, raider, via, flyingRoute, firstPath)) continue;
        const auto secondPath = legPath(secondLeg, target);
        if (!safeRaidPath(state, secondLeg, target, flyingRoute, secondPath)) continue;
        auto route = firstPath;
        for (const auto point : secondPath)
            if (route.empty() || route.back() != point) route.push_back(point);
        auto routeDistance = 0.0;
        for (std::size_t i = 1; i < route.size(); ++i)
            routeDistance += distance(route[i - 1], route[i]);
        const auto cost = routeDistance +
            raidRouteExposure(state, raider, route, flyingRoute) * 320.0;
        if (cost + 1e-9 < bestCost) {
            best = via;
            bestCost = cost;
        }
    }
    // The complete base path already covers winding terrain and is checked
    // for exposure segment by segment. Re-searching it per point adds no proof.
    return best;
}

} // namespace

bool harassmentRouteSafe(const GameState& state, const UnitSnapshot& raider,
                         const Position target, const bool flyingRoute,
                         const NavigationGrid* navigation, HarassmentRouteBudget* routeBudget) {
    HarassmentRouteBudget localBudget;
    auto& budget = routeBudget != nullptr ? *routeBudget : localBudget;
    return raider.position.valid() && target.valid() &&
        safeRaidRoute(state, raider, target, flyingRoute, navigation, budget);
}

HarassmentOpportunity harassmentOpportunity(const GameState& state, const UnitSnapshot& raider,
                                            const bool flyingRoute, const NavigationGrid* navigation,
                                            HarassmentRouteBudget* routeBudget) {
    HarassmentRouteBudget localBudget;
    auto& budget = routeBudget != nullptr ? *routeBudget : localBudget;
    HarassmentOpportunity best;
    if (!raider.position.valid()) return best;
    std::vector<HarassmentOpportunity> candidates;
    for (const auto& enemy : state.enemy.units) {
        if (!recent(state, enemy) || enemy.invincible || !enemy.detected ||
            (!isWorker(enemy.kind) && enemy.kind != UnitKind::overlord &&
             enemy.kind != UnitKind::shuttle && enemy.kind != UnitKind::dropship) ||
            !raider.canAttack(enemy)) continue;
        auto targets = 0;
        for (const auto& nearby : state.enemy.units)
            if (recent(state, nearby) && raider.canAttack(nearby) &&
                (isWorker(nearby.kind) || nearby.kind == UnitKind::overlord) &&
                distanceSquared(nearby.position, enemy.position) <= 224 * 224) ++targets;
        if (raider.kind == UnitKind::darkTemplar && isWorker(enemy.kind) && targets < 2 &&
            std::ranges::none_of(state.bases, [&state, &enemy](const BaseSnapshot& base) {
                return state.enemy.id >= 0 && base.ownerId == state.enemy.id &&
                       distanceSquared(base.center, enemy.position) <= 640 * 640;
            })) continue;
        const auto score = 4.0 * std::min(targets, 8) + (enemy.visible ? 3.0 : 0.0) -
            distance(raider.position, enemy.position) / 600.0;
        candidates.push_back({enemy.position, score, targets, {-1, -1}, false});
    }
    // A remembered, occupied economy is a scouting objective even after its
    // workers disappear into fog. It is not evidence that workers still exist.
    if (raider.groundWeapon.damage > 0 && !raider.flying) {
        for (const auto& base : state.bases) {
            if (state.enemy.id < 0 || base.ownerId != state.enemy.id || (base.island && !flyingRoute) ||
                base.mineralsRemaining < 500 || !base.mineralLine.valid() ||
                (base.lastConfirmedEmpty >= 0 && base.lastConfirmedEmpty >= base.lastScouted)) continue;
            const auto score = 6.0 - distance(raider.position, base.mineralLine) / 1200.0;
            candidates.push_back({base.mineralLine, score, 0, {-1, -1}, true});
        }
    }
    // Score is independent of the route. Prove candidates in score order and
    // stop at the first safe one instead of routing every observed worker.
    std::ranges::stable_sort(candidates, [](const auto& left, const auto& right) {
        if (left.score != right.score) return left.score > right.score;
        if (left.probing != right.probing) return !left.probing;
        return !left.probing && left.target.x < right.target.x;
    });
    for (auto candidate : candidates) {
        if (candidate.score <= 0.0) break;
        candidate.waypoint = raidWaypoint(state, raider, candidate.target,
                                          flyingRoute, navigation, budget);
        if (candidate.waypoint.valid()) return candidate;
    }
    return best;
}

RaidMission HarassmentPlanner::update(const GameState& state,
    const std::span<const UnitSnapshot> available, const StrategicPlan& plan,
    const Position home, const bool baseThreat, const NavigationGrid* navigation,
    HarassmentRouteBudget* routeBudget) {
    HarassmentRouteBudget localBudget;
    auto& budget = routeBudget != nullptr ? *routeBudget : localBudget;
    if (state.frame < lastFrame_) reset();
    lastFrame_ = state.frame;
    const auto mainArmy = std::ranges::count_if(available, [](const UnitSnapshot& unit) {
        return !unit.flying && !isBuilding(unit.kind) && isCombatUnit(unit.kind) &&
            unit.kind != UnitKind::darkTemplar && unit.completed && !unit.disabled &&
            !unit.loaded && !unit.hallucination;
    });
    const auto emergency = baseThreat || plan.prioritizeReinforcements ||
        plan.posture == Posture::defend || plan.posture == Posture::recover || !home.valid();
    // A committed full-army attack owns its spare fighters. Do not create a
    // new raid that can immediately recall those front-line units all the way
    // home. Existing emergency extractions retain their latched withdrawal.
    if (plan.posture == Posture::attack && !emergency && !mission_.withdrawing) {
        mission_ = {};
        nextAttempt_ = state.frame + 120;
        return {};
    }
    if (!mission_.members.empty()) {
        std::erase_if(mission_.members, [&available](const UnitId id) {
            return std::ranges::find(available, id, &UnitSnapshot::id) == available.end();
        });
        bool endangered = mission_.members.size() < 2 ||
            mainArmy - static_cast<int>(mission_.members.size()) < std::max(10, plan.minimumAttackSize);
        bool returned = true;
        bool arrived = false;
        if (!mission_.members.empty()) {
            const auto lead = std::ranges::find(available, mission_.members.front(), &UnitSnapshot::id);
            // Keep the chosen bypass until reached; recomputing its midpoint
            // every frame would drag the squad around a moving waypoint.
            if (distanceSquared(lead->position, mission_.waypoint) <= 96 * 96)
                mission_.waypoint = mission_.target;
        }
        for (const auto id : mission_.members) {
            const auto unit = std::ranges::find(available, id, &UnitSnapshot::id);
            auto secondLeg = *unit;
            secondLeg.position = mission_.waypoint;
            endangered = endangered || unit->disabled || unit->loaded || unit->healthFraction() < 0.60 ||
                !safeRaidRoute(state, *unit, mission_.waypoint, false, navigation, budget) ||
                !safeRaidRoute(state, secondLeg, mission_.target, false, navigation, budget);
            returned = returned && distanceSquared(unit->position, home) <= 256 * 256;
            arrived = arrived || distanceSquared(unit->position, mission_.target) <= 192 * 192;
        }
        const auto workersRemain = std::ranges::any_of(state.enemy.units, [&state, this](const UnitSnapshot& unit) {
            return recent(state, unit) && isWorker(unit.kind) &&
                distanceSquared(unit.position, mission_.target) <= 320 * 320;
        });
        if (!mission_.withdrawing && (emergency || endangered || (arrived && !workersRemain) || state.frame - started_ > 90 * 24)) {
            mission_.withdrawing = true;
            mission_.reason = emergency ? "Raid recalled: protect main army" : endangered ?
                "Raid escape: defenders or damage" : "Raid complete: target cleared or time budget used";
        }
        if (mission_.members.empty() || (mission_.withdrawing && returned)) {
            lastTarget_ = mission_.target;
            revisitAfter_ = state.frame + 90 * 24;
            mission_ = {};
            nextAttempt_ = state.frame + 30 * 24;
        }
        return mission_;
    }
    if (state.frame < nextAttempt_ || emergency || plan.posture == Posture::attack ||
        mainArmy < std::max(10, plan.minimumAttackSize) + 2) return {};
    nextAttempt_ = state.frame + 120;
    std::vector<UnitSnapshot> candidates;
    for (const auto& unit : available)
        if ((unit.kind == UnitKind::zealot || unit.kind == UnitKind::dragoon) && unit.completed &&
            !unit.disabled && !unit.loaded && !unit.hallucination && !unit.underAttack && unit.healthFraction() >= 0.85)
            candidates.push_back(unit);
    std::ranges::sort(candidates, {}, &UnitSnapshot::id);
    for (const auto& lead : candidates) {
        const auto opportunity = harassmentOpportunity(state, lead, false, navigation, &budget);
        if (!opportunity.target.valid() || (!opportunity.probing && opportunity.economicTargets < 2) ||
            (state.frame < revisitAfter_ && distanceSquared(opportunity.target, lastTarget_) <= 320 * 320)) continue;
        const auto buddy = std::ranges::find_if(candidates, [&lead](const UnitSnapshot& unit) {
            return unit.id != lead.id && distanceSquared(unit.position, lead.position) <= 288 * 288;
        });
        if (buddy == candidates.end() ||
            !safeRaidRoute(state, *buddy, opportunity.waypoint, false, navigation, budget)) continue;
        mission_ = {{lead.id, buddy->id}, opportunity.target, false, "Raid exposed workers with two spare fighters"};
        mission_.waypoint = opportunity.waypoint;
        mission_.probing = opportunity.probing;
        if (opportunity.probing) mission_.reason = "Probe remembered enemy economy with spare fighters";
        const auto raidSize = std::min(4, static_cast<int>(mainArmy) / 6);
        for (const auto& extra : candidates) {
            if (static_cast<int>(mission_.members.size()) >= raidSize ||
                mainArmy - static_cast<int>(mission_.members.size()) <= std::max(10, plan.minimumAttackSize)) break;
            if (extra.id != lead.id && extra.id != buddy->id &&
                distanceSquared(extra.position, lead.position) <= 288 * 288 &&
                safeRaidRoute(state, extra, opportunity.waypoint, false, navigation, budget))
                mission_.members.push_back(extra.id);
        }
        started_ = state.frame;
        break;
    }
    return mission_;
}

void HarassmentPlanner::reset() {
    mission_ = {};
    started_ = nextAttempt_ = 0;
    lastFrame_ = -1;
    lastTarget_ = {-1, -1};
    revisitAfter_ = 0;
}

void HarassmentPlanner::forgetUnit(const UnitId id) {
    if (id < 0) return;
    std::erase(mission_.members, id);
    if (mission_.members.empty()) reset();
}

} // namespace protodd
