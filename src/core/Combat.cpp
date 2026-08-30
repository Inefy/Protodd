#include "astra/Combat.hpp"

#include "astra/UnitCatalog.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace astra {

CombatEstimate CombatEvaluator::evaluate(
    const std::span<const UnitSnapshot> friendly,
    const std::span<const UnitSnapshot> enemy,
    const double requiredRatio,
    const double uncertainty) const {
    CombatEstimate result;
    for (const auto& unit : friendly) {
        result.friendlyPower += unitPower(unit, enemy);
    }
    for (const auto& unit : enemy) {
        result.enemyPower += unitPower(unit, friendly);
    }

    const auto uncertaintyPenalty = 1.0 + std::clamp(uncertainty, 0.0, 1.0) * 0.28;
    result.enemyPower *= uncertaintyPenalty;
    result.ratio = result.friendlyPower / std::max(0.1, result.enemyPower);
    result.confidence = std::clamp(1.0 - uncertainty * 0.65, 0.2, 1.0);
    if (result.ratio >= requiredRatio) {
        result.decision = FightDecision::engage;
    } else if (result.ratio >= requiredRatio * 0.72) {
        result.decision = FightDecision::kite;
    } else {
        result.decision = FightDecision::retreat;
    }
    return result;
}

const UnitSnapshot* CombatEvaluator::selectTarget(
    const UnitSnapshot& attacker,
    const std::span<const UnitSnapshot> candidates) const {
    const UnitSnapshot* best = nullptr;
    auto bestScore = -std::numeric_limits<double>::infinity();
    for (const auto& target : candidates) {
        if (!target.detected || !attacker.canAttack(target)) {
            continue;
        }
        const auto range = distance(attacker.position, target.position);
        const auto weapon = target.flying ? attacker.airWeapon : attacker.groundWeapon;
        const auto priority = unitStats(target.kind).combatValue * 3.0 +
                              (target.role == UnitRole::spellcaster ? 4.0 : 0.0) +
                              (target.role == UnitRole::detector && attacker.cloaked ? 5.0 : 0.0) +
                              (isWorker(target.kind) ? 0.7 : 0.0);
        const auto effectiveHealth = std::max(1, target.durability());
        const auto killEfficiency = static_cast<double>(weapon.damage) / effectiveHealth;
        const auto inRange = range <= weapon.maxRange + 16 ? 2.0 : 0.0;
        const auto overkillPenalty = target.healthFraction() < 0.12 ? 0.8 : 0.0;
        const auto score = priority + killEfficiency * 4.0 + inRange - range / 320.0 -
                           overkillPenalty;
        if (score > bestScore || (std::abs(score - bestScore) < 0.001 &&
                                  (best == nullptr || target.id < best->id))) {
            best = &target;
            bestScore = score;
        }
    }
    return best;
}

double CombatEvaluator::unitPower(
    const UnitSnapshot& unit,
    const std::span<const UnitSnapshot> opposition) {
    if (!isCombatUnit(unit.kind) && !isStaticDefense(unit.kind)) {
        return 0.0;
    }
    const auto hasAirTargets = std::ranges::any_of(opposition, [](const UnitSnapshot& target) {
        return target.flying;
    });
    const auto hasGroundTargets = std::ranges::any_of(opposition, [](const UnitSnapshot& target) {
        return !target.flying;
    });
    const auto airUseful = hasAirTargets && unit.airWeapon.damage > 0;
    const auto groundUseful = hasGroundTargets && unit.groundWeapon.damage > 0;
    if (!airUseful && !groundUseful && !opposition.empty()) {
        return unit.role == UnitRole::spellcaster ? unitStats(unit.kind).combatValue * 0.6 : 0.1;
    }

    const auto& weapon = airUseful ? unit.airWeapon : unit.groundWeapon;
    const auto dps = static_cast<double>(weapon.damage) / std::max(1, weapon.cooldown);
    const auto rangeFactor = 1.0 + std::clamp(weapon.maxRange / 256.0, 0.0, 1.0) * 0.35;
    const auto mobility = 1.0 + std::clamp(unit.topSpeed / 8.0, 0.0, 1.0) * 0.2;
    const auto vitality = std::clamp(unit.healthFraction(), 0.08, 1.0);
    return (unitStats(unit.kind).combatValue + dps * 0.45) * rangeFactor * mobility * vitality;
}

std::vector<Command> TacticalController::control(
    const std::span<const UnitSnapshot> friendly,
    const std::span<const UnitSnapshot> enemy,
    const CombatEstimate& estimate,
    const Position objective,
    const Position retreatPoint,
    const InfluenceMap& influence) const {
    std::vector<Command> commands;
    commands.reserve(friendly.size());
    CombatEvaluator evaluator;

    for (const auto& unit : friendly) {
        if (!isCombatUnit(unit.kind) || !unit.completed) {
            continue;
        }
        const auto target = evaluator.selectTarget(unit, enemy);
        const auto local = influence.at(unit.position);
        const auto localThreat = unit.flying ? local.airThreat : local.groundThreat;
        const auto fragile = unit.healthFraction() < 0.28;

        if (estimate.decision == FightDecision::retreat || fragile || localThreat > 3.5F) {
            commands.push_back({
                unit.id, CommandType::move, -1,
                influence.safestStep(unit.position, retreatPoint, unit.flying),
                UnitKind::unknown, fragile ? 100 : 86, 0, "combat-retreat",
            });
            continue;
        }

        if (target != nullptr) {
            const auto& weapon = target->flying ? unit.airWeapon : unit.groundWeapon;
            const auto range = distance(unit.position, target->position);
            const auto canFire = unit.weaponCooldown <= 1 && range <= weapon.maxRange + 12;
            const auto kite = estimate.decision == FightDecision::kite ||
                              (unit.topSpeed > target->topSpeed && weapon.maxRange > 64);
            if (!canFire && kite && unit.weaponCooldown > 2) {
                commands.push_back({
                    unit.id, CommandType::move, -1,
                    influence.safestStep(unit.position, retreatPoint, unit.flying),
                    UnitKind::unknown, 84, 0, "combat-kite",
                });
            } else {
                commands.push_back({unit.id, CommandType::attackUnit, target->id, {-1, -1},
                                    UnitKind::unknown, 80, 0, "focus-fire"});
            }
        } else if (objective.valid()) {
            commands.push_back({unit.id, CommandType::attackMove, -1, objective,
                                UnitKind::unknown, 50, 0, "squad-objective"});
        }
    }
    return commands;
}

}  // namespace astra
