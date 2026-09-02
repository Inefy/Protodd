#include "astra/Combat.hpp"

#include "astra/UnitCatalog.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace astra {
namespace {

struct SimUnit {
    const UnitSnapshot* unit{};
    double durability{};
    int readyFrame{};
};

struct SimulationOutcome {
    double friendlyRemaining{};
    double enemyRemaining{};
    double friendlyInitial{};
    double enemyInitial{};
    double coverage{1.0};
};

double sizeMultiplier(const DamageType damage, const UnitSize size) noexcept {
    if (damage == DamageType::concussive) {
        if (size == UnitSize::medium) return 0.5;
        if (size == UnitSize::large) return 0.25;
    }
    if (damage == DamageType::explosive) {
        if (size == UnitSize::small) return 0.5;
        if (size == UnitSize::medium) return 0.75;
    }
    return 1.0;
}

double volleyDamage(const UnitSnapshot& attacker, const UnitSnapshot& target) noexcept {
    const auto& weapon = target.flying ? attacker.airWeapon : attacker.groundWeapon;
    if (weapon.damage <= 0) return 0.0;
    const auto raw = static_cast<double>(weapon.damage * std::max(1, weapon.hits));
    const auto modified = raw * sizeMultiplier(weapon.damageType, target.size);
    const auto armor = weapon.damageType == DamageType::ignoreArmor ? 0 : target.armor;
    return std::max(0.5, modified - static_cast<double>(armor));
}

double simulationValue(const SimUnit& unit) noexcept {
    const auto maximum = std::max(1, unit.unit->maxHitPoints + unit.unit->maxShields);
    const auto health = std::clamp(unit.durability / static_cast<double>(maximum), 0.0, 1.0);
    return unitStats(unit.unit->kind).combatValue * health;
}

std::vector<SimUnit> simulationUnits(const std::span<const UnitSnapshot> source) {
    std::vector<const UnitSnapshot*> selected;
    selected.reserve(source.size());
    for (const auto& unit : source) {
        if (unit.completed && (isCombatUnit(unit.kind) || isStaticDefense(unit.kind))) {
            selected.push_back(&unit);
        }
    }
    std::ranges::stable_sort(selected, [](const UnitSnapshot* left, const UnitSnapshot* right) {
        const auto leftScore = unitStats(left->kind).combatValue * left->healthFraction();
        const auto rightScore = unitStats(right->kind).combatValue * right->healthFraction();
        if (std::abs(leftScore - rightScore) > 0.001) return leftScore > rightScore;
        return left->id < right->id;
    });
    constexpr auto maximumUnits = std::size_t{96};
    if (selected.size() > maximumUnits) selected.resize(maximumUnits);

    std::vector<SimUnit> result;
    result.reserve(selected.size());
    for (const auto* unit : selected) {
        result.push_back({unit, static_cast<double>(std::max(1, unit->durability())),
                          std::max(0, unit->weaponCooldown)});
    }
    return result;
}

const UnitSnapshot* nearestLivingTarget(
    const SimUnit& attacker,
    const std::span<const SimUnit> defenders,
    const std::span<const double> pending,
    std::size_t& targetIndex,
    const int frame) {
    const UnitSnapshot* best = nullptr;
    auto bestScore = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < defenders.size(); ++i) {
        const auto& defender = defenders[i];
        if (defender.durability - pending[i] <= 0.0 ||
            !attacker.unit->canAttack(*defender.unit)) {
            continue;
        }
        const auto& weapon = defender.unit->flying ? attacker.unit->airWeapon
                                                    : attacker.unit->groundWeapon;
        const auto separation = distance(attacker.unit->position, defender.unit->position);
        const auto gap = std::max(0.0, separation - static_cast<double>(weapon.maxRange));
        const auto closingSpeed = std::max(0.1, attacker.unit->topSpeed +
                                                   defender.unit->topSpeed * 0.20);
        const auto contactFrame = static_cast<int>(std::ceil(gap / closingSpeed));
        if (frame < contactFrame) continue;
        const auto damage = volleyDamage(*attacker.unit, *defender.unit);
        if (damage <= 0.0) continue;
        const auto remaining = std::max(0.5, defender.durability - pending[i]);
        const auto volleys = std::ceil(remaining / damage);
        const auto value = std::max(0.1, unitStats(defender.unit->kind).combatValue);
        const auto score = volleys * std::max(1, weapon.cooldown) / value +
                           separation / 2048.0;
        if (score < bestScore || (std::abs(score - bestScore) < 0.001 &&
                                  (best == nullptr || defender.unit->id < best->id))) {
            best = defender.unit;
            bestScore = score;
            targetIndex = i;
        }
    }
    return best;
}

void scheduleVolleys(
    std::vector<SimUnit>& attackers,
    const std::vector<SimUnit>& defenders,
    std::vector<double>& pending,
    const int frame) {
    for (auto& attacker : attackers) {
        if (attacker.durability <= 0.0 || frame < attacker.readyFrame) continue;
        auto targetIndex = std::size_t{0};
        const auto* target = nearestLivingTarget(attacker, defenders, pending,
                                                 targetIndex, frame);
        if (target == nullptr) continue;
        const auto& weapon = target->flying ? attacker.unit->airWeapon
                                            : attacker.unit->groundWeapon;
        pending[targetIndex] += volleyDamage(*attacker.unit, *target);
        attacker.readyFrame = frame + std::max(1, weapon.cooldown);
    }
}

SimulationOutcome simulateEngagement(
    const std::span<const UnitSnapshot> friendlySource,
    const std::span<const UnitSnapshot> enemySource) {
    auto friendly = simulationUnits(friendlySource);
    auto enemy = simulationUnits(enemySource);
    SimulationOutcome result;
    for (const auto& unit : friendly) result.friendlyInitial += simulationValue(unit);
    for (const auto& unit : enemy) result.enemyInitial += simulationValue(unit);

    constexpr auto stepFrames = 6;
    constexpr auto horizonFrames = 24 * 14;
    for (auto frame = 0; frame <= horizonFrames; frame += stepFrames) {
        std::vector<double> damageToFriendly(friendly.size(), 0.0);
        std::vector<double> damageToEnemy(enemy.size(), 0.0);
        scheduleVolleys(friendly, enemy, damageToEnemy, frame);
        scheduleVolleys(enemy, friendly, damageToFriendly, frame);
        for (std::size_t i = 0; i < friendly.size(); ++i) {
            friendly[i].durability -= damageToFriendly[i];
        }
        for (std::size_t i = 0; i < enemy.size(); ++i) {
            enemy[i].durability -= damageToEnemy[i];
        }
        const auto friendlyAlive = std::ranges::any_of(
            friendly, [](const SimUnit& unit) { return unit.durability > 0.0; });
        const auto enemyAlive = std::ranges::any_of(
            enemy, [](const SimUnit& unit) { return unit.durability > 0.0; });
        if (!friendlyAlive || !enemyAlive) break;
    }
    for (const auto& unit : friendly) {
        if (unit.durability > 0.0) result.friendlyRemaining += simulationValue(unit);
    }
    for (const auto& unit : enemy) {
        if (unit.durability > 0.0) result.enemyRemaining += simulationValue(unit);
    }
    const auto considered = friendly.size() + enemy.size();
    const auto available = friendlySource.size() + enemySource.size();
    result.coverage = available == 0U
                          ? 1.0
                          : static_cast<double>(considered) /
                                static_cast<double>(available);
    return result;
}

}  // namespace

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

    const auto rawFriendlyPower = result.friendlyPower;
    const auto rawEnemyPower = result.enemyPower;
    const auto simulation = simulateEngagement(friendly, enemy);
    result.simulatedFriendlyRemaining = simulation.friendlyRemaining;
    result.simulatedEnemyRemaining = simulation.enemyRemaining;

    const auto friendlySurvival = simulation.friendlyInitial > 0.0
                                      ? simulation.friendlyRemaining /
                                            simulation.friendlyInitial
                                      : (friendly.empty() ? 0.0 : 1.0);
    const auto enemySurvival = simulation.enemyInitial > 0.0
                                   ? simulation.enemyRemaining /
                                         simulation.enemyInitial
                                   : (enemy.empty() ? 0.0 : 1.0);
    // Blend the fast static estimate with the bounded local simulation. The
    // static component covers unsupported spell effects while the simulated
    // survival component captures range, approach time, focus fire, armor,
    // damage type, cooldowns, and simultaneous volleys.
    result.friendlyPower = rawFriendlyPower * (0.35 + 0.65 * friendlySurvival);
    result.enemyPower = rawEnemyPower * (0.35 + 0.65 * enemySurvival);

    const auto uncertaintyPenalty = 1.0 + std::clamp(uncertainty, 0.0, 1.0) * 0.28;
    result.enemyPower *= uncertaintyPenalty;
    result.ratio = result.friendlyPower / std::max(0.1, result.enemyPower);
    result.confidence = std::clamp((1.0 - uncertainty * 0.65) * simulation.coverage,
                                   0.2, 1.0);
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
