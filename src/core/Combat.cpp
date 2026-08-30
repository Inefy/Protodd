#include "astra/Combat.hpp"

#include "astra/UnitCatalog.hpp"

#include <algorithm>
#include <array>
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
    const std::span<const UnitSnapshot> candidates,
    const std::span<const TargetAllocation> allocations) const {
    const UnitSnapshot* best = nullptr;
    auto bestScore = -std::numeric_limits<double>::infinity();
    for (const auto& target : candidates) {
        if (!target.visible || !target.detected || !attacker.canAttack(target)) {
            continue;
        }
        const auto allocation = std::ranges::find(
            allocations, target.id, &TargetAllocation::target);
        const auto committed = allocation != allocations.end()
                                   ? allocation->committedDamage
                                   : 0;
        const auto remainingHealth = target.durability() - committed;
        if (remainingHealth <= 0) continue;
        const auto range = distance(attacker.position, target.position);
        const auto weapon = target.flying ? attacker.airWeapon : attacker.groundWeapon;
        const auto splashTargets = attacker.kind == UnitKind::reaver
                                       ? std::ranges::count_if(
                                             candidates,
                                             [&target](const UnitSnapshot& candidate) {
                                                 return !candidate.flying && candidate.visible &&
                                                        distanceSquared(candidate.position,
                                                                        target.position) <=
                                                            96 * 96;
                                             })
                                       : 0;
        const auto priority = unitStats(target.kind).combatValue * 3.0 +
                              (target.role == UnitRole::spellcaster ? 4.0 : 0.0) +
                              (target.role == UnitRole::detector && attacker.cloaked ? 5.0 : 0.0) +
                              (isWorker(target.kind) ? 0.7 : 0.0) +
                              (attacker.kind == UnitKind::corsair &&
                                       target.kind == UnitKind::overlord
                                   ? 3.0
                                   : 0.0) +
                              static_cast<double>(splashTargets) * 0.9;
        const auto effectiveHealth = std::max(1, remainingHealth);
        const auto killEfficiency = static_cast<double>(weapon.damage) / effectiveHealth;
        const auto inRange = range <= weapon.maxRange + 16 ? 2.0 : 0.0;
        const auto score = priority + killEfficiency * 4.0 + inRange - range / 320.0;
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
    const InfluenceMap& influence,
    const Position formationCenter) const {
    std::vector<Command> commands;
    commands.reserve(friendly.size());
    CombatEvaluator evaluator;
    std::vector<TargetAllocation> allocations;
    allocations.reserve(enemy.size());

    std::vector<const UnitSnapshot*> ordered;
    ordered.reserve(friendly.size());
    for (const auto& unit : friendly) ordered.push_back(&unit);
    std::ranges::stable_sort(ordered, [](const UnitSnapshot* left, const UnitSnapshot* right) {
        if (left->weaponCooldown != right->weaponCooldown)
            return left->weaponCooldown < right->weaponCooldown;
        return left->id < right->id;
    });

    for (const auto* unitPointer : ordered) {
        const auto& unit = *unitPointer;
        if (!isCombatUnit(unit.kind) || !unit.completed) {
            continue;
        }
        const auto target = evaluator.selectTarget(unit, enemy, allocations);
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

        const auto supportCaster = unit.kind == UnitKind::highTemplar ||
                                   unit.kind == UnitKind::darkArchon ||
                                   unit.kind == UnitKind::arbiter;
        if (supportCaster) {
            const auto anchor = formationCenter.valid()
                                    ? moveToward(formationCenter, retreatPoint, 96.0)
                                    : retreatPoint;
            if (anchor.valid() && distanceSquared(unit.position, anchor) > 96 * 96) {
                commands.push_back({
                    unit.id, CommandType::move, -1,
                    influence.safestStep(unit.position, anchor, unit.flying),
                    UnitKind::unknown, 83, 0, "spellcaster-screen",
                });
            } else {
                commands.push_back({unit.id, CommandType::hold, -1, {-1, -1},
                                    UnitKind::unknown, 70, 0, "spellcaster-hold"});
            }
            continue;
        }

        if (target != nullptr) {
            const auto& weapon = target->flying ? unit.airWeapon : unit.groundWeapon;
            const auto range = distance(unit.position, target->position);
            const auto canFire = unit.weaponCooldown <= 1 && range <= weapon.maxRange + 12;
            if (unit.cloaked && local.detection > 0.1F &&
                target->role != UnitRole::detector && !canFire) {
                commands.push_back({
                    unit.id, CommandType::move, -1,
                    influence.safestStep(unit.position, retreatPoint, unit.flying, true),
                    UnitKind::unknown, 94, 0, "cloak-preservation",
                });
                continue;
            }

            static constexpr std::array surroundOffsets{
                Position{40, 0}, Position{28, 28}, Position{0, 40}, Position{-28, 28},
                Position{-40, 0}, Position{-28, -28}, Position{0, -40}, Position{28, -28},
            };
            if (unit.kind == UnitKind::zealot && !target->flying &&
                estimate.decision == FightDecision::engage && range > 52.0 && range < 192.0) {
                const auto offset = surroundOffsets[
                    static_cast<std::size_t>(unit.id) % surroundOffsets.size()];
                commands.push_back({unit.id, CommandType::move, -1,
                                    {target->position.x + offset.x,
                                     target->position.y + offset.y},
                                    UnitKind::unknown, 82, 0, "zealot-surround"});
                continue;
            }

            const auto ranged = weapon.maxRange >= 96;
            const auto targetWeapon = unit.flying ? target->airWeapon : target->groundWeapon;
            const auto rangeAdvantage = weapon.maxRange >= targetWeapon.maxRange + 48;
            const auto kite = estimate.decision == FightDecision::kite || rangeAdvantage;
            if (!canFire && kite && ranged && unit.weaponCooldown > 2 &&
                range <= weapon.maxRange + 64) {
                const Position away{
                    unit.position.x + unit.position.x - target->position.x,
                    unit.position.y + unit.position.y - target->position.y,
                };
                commands.push_back({
                    unit.id, CommandType::move, -1,
                    influence.safestStep(unit.position, away, unit.flying),
                    UnitKind::unknown, 84, 0, "combat-kite",
                });
            } else {
                commands.push_back({unit.id, CommandType::attackUnit, target->id, {-1, -1},
                                    UnitKind::unknown, 80, 0, "focus-fire"});
                if (canFire) {
                    const auto damage = std::max(1, weapon.damage - target->armor);
                    const auto allocation = std::ranges::find(
                        allocations, target->id, &TargetAllocation::target);
                    if (allocation == allocations.end()) {
                        allocations.push_back({target->id, damage});
                    } else {
                        allocation->committedDamage += damage;
                    }
                }
            }
        } else if (formationCenter.valid() && friendly.size() >= 4 &&
                   distanceSquared(unit.position, formationCenter) > 448 * 448) {
            commands.push_back({unit.id, CommandType::move, -1, formationCenter,
                                UnitKind::unknown, 62, 0, "regroup-formation"});
        } else if (objective.valid()) {
            commands.push_back({unit.id, CommandType::attackMove, -1, objective,
                                UnitKind::unknown, 50, 0, "squad-objective"});
        }
    }
    return commands;
}

}  // namespace astra
