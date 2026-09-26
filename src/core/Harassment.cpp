#include "protodd/Harassment.hpp"

#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <cmath>

namespace protodd {
namespace {

bool recent(const GameState& state, const UnitSnapshot& enemy) {
    return enemy.position.valid() && (enemy.visible || state.frame - enemy.lastSeen <= 20 * 24);
}

bool safeRaidRoute(const GameState& state, const UnitSnapshot& raider,
                   const Position target, const bool flyingRoute) {
    for (const auto& enemy : state.enemy.units) {
        if (!enemy.completed || enemy.disabled || enemy.loaded || enemy.hallucination ||
            (unitStats(enemy.kind).requiresPsi && !enemy.powered) ||
            isWorker(enemy.kind) || !enemy.position.valid() ||
            (!isBuilding(enemy.kind) && !recent(state, enemy))) continue;
        const auto detector = raider.cloaked && (enemy.role == UnitRole::detector || enemy.kind == UnitKind::photonCannon ||
            enemy.kind == UnitKind::missileTurret || enemy.kind == UnitKind::sporeColony || enemy.kind == UnitKind::overlord);
        const auto& weapon = raider.flying ? enemy.airWeapon : enemy.groundWeapon;
        const auto targetClearance = detector ? std::max(352, enemy.sightRange + 32) :
            std::max(160, weapon.maxRange + 96);
        if ((weapon.damage > 0 || detector) &&
            distanceSquared(enemy.position, target) <= targetClearance * targetClearance) return false;
        const auto& routeWeapon = (flyingRoute || raider.flying) ? enemy.airWeapon : enemy.groundWeapon;
        if (routeWeapon.damage <= 0 && !detector) continue;
        // Distance to the entire segment catches a contain between the army
        // and a seemingly undefended mineral line.
        const auto dx = static_cast<double>(target.x - raider.position.x);
        const auto dy = static_cast<double>(target.y - raider.position.y);
        const auto t = std::clamp(((enemy.position.x - raider.position.x) * dx +
            (enemy.position.y - raider.position.y) * dy) / std::max(1.0, dx * dx + dy * dy), 0.0, 1.0);
        const Position nearest{raider.position.x + static_cast<int>(std::lround(dx * t)),
                               raider.position.y + static_cast<int>(std::lround(dy * t))};
        const auto clearance = detector ? std::max(352, enemy.sightRange + 32) :
            std::max(224, routeWeapon.maxRange + 96);
        if (distanceSquared(nearest, enemy.position) <= clearance * clearance) return false;
    }
    return true;
}

Position raidWaypoint(const GameState& state, const UnitSnapshot& raider,
                      const Position target, const bool flyingRoute,
                      const NavigationGrid* navigation) {
    const auto legSafe = [&](const UnitSnapshot& from, const Position to) {
        return safeRaidRoute(state, from, to, flyingRoute) &&
            (flyingRoute || raider.flying || navigation == nullptr || navigation->empty() ||
             navigation->lineWalkable(from.position, to));
    };
    if (legSafe(raider, target)) return target;
    // Only attempt bypasses when terrain is available. Both legs must be
    // traversable and outside observed weapon/detector coverage.
    if (navigation == nullptr || navigation->empty()) return {-1, -1};
    const auto length = std::max(1.0, distance(raider.position, target));
    const auto dx = static_cast<double>(target.x - raider.position.x) / length;
    const auto dy = static_cast<double>(target.y - raider.position.y) / length;
    for (const auto offset : {384, -384, 768, -768, 1152, -1152}) {
        const Position via{(raider.position.x + target.x) / 2 - static_cast<int>(dy * offset),
                           (raider.position.y + target.y) / 2 + static_cast<int>(dx * offset)};
        if (!via.valid() || via.x >= state.mapWidthPixels || via.y >= state.mapHeightPixels) continue;
        auto secondLeg = raider;
        secondLeg.position = via;
        if (legSafe(raider, via) && legSafe(secondLeg, target)) return via;
    }
    return {-1, -1};
}

} // namespace

bool harassmentRouteSafe(const GameState& state, const UnitSnapshot& raider,
                         const Position target, const bool flyingRoute) {
    return raider.position.valid() && target.valid() && safeRaidRoute(state, raider, target, flyingRoute);
}

HarassmentOpportunity harassmentOpportunity(const GameState& state, const UnitSnapshot& raider,
                                            const bool flyingRoute, const NavigationGrid* navigation) {
    HarassmentOpportunity best;
    if (!raider.position.valid()) return best;
    for (const auto& enemy : state.enemy.units) {
        if (!recent(state, enemy) || enemy.invincible || !enemy.detected ||
            (!isWorker(enemy.kind) && enemy.kind != UnitKind::overlord &&
             enemy.kind != UnitKind::shuttle && enemy.kind != UnitKind::dropship) ||
            !raider.canAttack(enemy)) continue;
        const auto waypoint = raidWaypoint(state, raider, enemy.position, flyingRoute, navigation);
        if (!waypoint.valid()) continue;
        auto targets = 0;
        for (const auto& nearby : state.enemy.units)
            if (recent(state, nearby) && raider.canAttack(nearby) &&
                (isWorker(nearby.kind) || nearby.kind == UnitKind::overlord) &&
                distanceSquared(nearby.position, enemy.position) <= 224 * 224) ++targets;
        const auto score = 4.0 * std::min(targets, 8) + (enemy.visible ? 3.0 : 0.0) -
            distance(raider.position, enemy.position) / 600.0;
        if (score > best.score || (score == best.score && enemy.position.x < best.target.x))
            best = {enemy.position, score, targets, waypoint, false};
    }
    // A remembered, occupied economy is a scouting objective even after its
    // workers disappear into fog. It is not evidence that workers still exist.
    if (raider.groundWeapon.damage > 0 && !raider.flying) {
        for (const auto& base : state.bases) {
            if (state.enemy.id < 0 || base.ownerId != state.enemy.id || (base.island && !flyingRoute) ||
                base.mineralsRemaining < 500 || !base.mineralLine.valid() ||
                (base.lastConfirmedEmpty >= 0 && base.lastConfirmedEmpty >= base.lastScouted)) continue;
            const auto waypoint = raidWaypoint(state, raider, base.mineralLine, flyingRoute, navigation);
            if (!waypoint.valid()) continue;
            const auto score = 6.0 - distance(raider.position, base.mineralLine) / 1200.0;
            if (score > best.score) best = {base.mineralLine, score, 0, waypoint, true};
        }
    }
    return best;
}

RaidMission HarassmentPlanner::update(const GameState& state,
    const std::span<const UnitSnapshot> available, const StrategicPlan& plan,
    const Position home, const bool baseThreat, const NavigationGrid* navigation) {
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
                !safeRaidRoute(state, *unit, mission_.waypoint, false) ||
                !safeRaidRoute(state, secondLeg, mission_.target, false);
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
        const auto opportunity = harassmentOpportunity(state, lead, false, navigation);
        if (!opportunity.target.valid() || (!opportunity.probing && opportunity.economicTargets < 2) ||
            (state.frame < revisitAfter_ && distanceSquared(opportunity.target, lastTarget_) <= 320 * 320)) continue;
        const auto buddy = std::ranges::find_if(candidates, [&lead](const UnitSnapshot& unit) {
            return unit.id != lead.id && distanceSquared(unit.position, lead.position) <= 288 * 288;
        });
        if (buddy == candidates.end() || !safeRaidRoute(state, *buddy, opportunity.waypoint, false)) continue;
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
                safeRaidRoute(state, extra, opportunity.waypoint, false)) mission_.members.push_back(extra.id);
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

} // namespace protodd
