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
        if (!enemy.completed || enemy.disabled || isWorker(enemy.kind) || !enemy.position.valid() ||
            (!isBuilding(enemy.kind) && !recent(state, enemy))) continue;
        const auto detector = raider.cloaked && (enemy.role == UnitRole::detector || enemy.kind == UnitKind::photonCannon ||
            enemy.kind == UnitKind::missileTurret || enemy.kind == UnitKind::sporeColony || enemy.kind == UnitKind::overlord);
        const auto& weapon = raider.flying ? enemy.airWeapon : enemy.groundWeapon;
        if ((weapon.damage > 0 || detector) && distanceSquared(enemy.position, target) <= 480 * 480) return false;
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
        const auto clearance = detector ? 352 : std::max(224, routeWeapon.maxRange + 96);
        if (distanceSquared(nearest, enemy.position) <= clearance * clearance) return false;
    }
    return true;
}

} // namespace

HarassmentOpportunity harassmentOpportunity(const GameState& state, const UnitSnapshot& raider,
                                            const bool flyingRoute) {
    HarassmentOpportunity best;
    if (!raider.position.valid()) return best;
    for (const auto& enemy : state.enemy.units) {
        if (!recent(state, enemy) || enemy.invincible || !enemy.detected ||
            (!isWorker(enemy.kind) && enemy.kind != UnitKind::overlord &&
             enemy.kind != UnitKind::shuttle && enemy.kind != UnitKind::dropship) ||
            !raider.canAttack(enemy)) continue;
        if (!safeRaidRoute(state, raider, enemy.position, flyingRoute)) continue;
        auto targets = 0;
        for (const auto& nearby : state.enemy.units)
            if (recent(state, nearby) && raider.canAttack(nearby) &&
                (isWorker(nearby.kind) || nearby.kind == UnitKind::overlord) &&
                distanceSquared(nearby.position, enemy.position) <= 224 * 224) ++targets;
        const auto score = 4.0 * std::min(targets, 8) + (enemy.visible ? 3.0 : 0.0) -
            distance(raider.position, enemy.position) / 600.0;
        if (score > best.score || (score == best.score && enemy.position.x < best.target.x))
            best = {enemy.position, score, targets};
    }
    return best;
}

RaidMission HarassmentPlanner::update(const GameState& state,
    const std::span<const UnitSnapshot> available, const StrategicPlan& plan,
    const Position home, const bool baseThreat) {
    if (state.frame < lastFrame_) reset();
    lastFrame_ = state.frame;
    const auto mainArmy = std::ranges::count_if(available, [](const UnitSnapshot& unit) {
        return !unit.flying && unit.kind != UnitKind::darkTemplar && unit.completed && !unit.disabled && !unit.loaded;
    });
    const auto emergency = baseThreat || plan.prioritizeReinforcements || plan.requireMobileDetection ||
        plan.posture == Posture::defend || plan.posture == Posture::recover || !home.valid();
    if (!mission_.members.empty()) {
        std::erase_if(mission_.members, [&available](const UnitId id) {
            return std::ranges::find(available, id, &UnitSnapshot::id) == available.end();
        });
        bool endangered = mission_.members.size() < 2 || mainArmy < std::max(10, plan.minimumAttackSize + 2);
        bool returned = true;
        for (const auto id : mission_.members) {
            const auto unit = std::ranges::find(available, id, &UnitSnapshot::id);
            endangered = endangered || unit->healthFraction() < 0.60 ||
                !safeRaidRoute(state, *unit, mission_.target, false);
            returned = returned && distanceSquared(unit->position, home) <= 256 * 256;
        }
        const auto workersRemain = std::ranges::any_of(state.enemy.units, [&state, this](const UnitSnapshot& unit) {
            return recent(state, unit) && isWorker(unit.kind) &&
                distanceSquared(unit.position, mission_.target) <= 320 * 320;
        });
        if (!mission_.withdrawing && (emergency || endangered || !workersRemain || state.frame - started_ > 60 * 24)) {
            mission_.withdrawing = true;
            mission_.reason = emergency ? "Raid recalled: protect main army" : endangered ?
                "Raid escape: defenders or damage" : "Raid complete: target cleared or time budget used";
        }
        if (mission_.members.empty() || (mission_.withdrawing && returned)) {
            mission_ = {};
            nextAttempt_ = state.frame + 30 * 24;
        }
        return mission_;
    }
    if (state.frame < nextAttempt_ || emergency || mainArmy < std::max(10, plan.minimumAttackSize + 2)) return {};
    nextAttempt_ = state.frame + 120;
    std::vector<UnitSnapshot> candidates;
    for (const auto& unit : available)
        if ((unit.kind == UnitKind::zealot || unit.kind == UnitKind::dragoon) && unit.completed &&
            !unit.disabled && !unit.loaded && !unit.underAttack && unit.healthFraction() >= 0.85)
            candidates.push_back(unit);
    std::ranges::sort(candidates, {}, &UnitSnapshot::id);
    for (const auto& lead : candidates) {
        const auto opportunity = harassmentOpportunity(state, lead);
        if (!opportunity.target.valid() || opportunity.economicTargets < 2) continue;
        const auto buddy = std::ranges::find_if(candidates, [&lead](const UnitSnapshot& unit) {
            return unit.id != lead.id && distanceSquared(unit.position, lead.position) <= 288 * 288;
        });
        if (buddy == candidates.end() || !safeRaidRoute(state, *buddy, opportunity.target, false)) continue;
        mission_ = {{lead.id, buddy->id}, opportunity.target, false, "Raid exposed workers with two spare fighters"};
        started_ = state.frame;
        break;
    }
    return mission_;
}

void HarassmentPlanner::reset() {
    mission_ = {};
    started_ = nextAttempt_ = 0;
    lastFrame_ = -1;
}

} // namespace protodd
