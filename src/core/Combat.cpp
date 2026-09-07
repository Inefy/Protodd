#include "protodd/Combat.hpp"

#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace protodd {
namespace {

struct SimUnit {
    const UnitSnapshot* unit{};
    double durability{};
    int readyFrame{};
    int ammunition{};
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

bool combatReady(const UnitSnapshot& unit) noexcept {
    return unit.completed && !unit.loaded && !unit.disabled && !unit.hallucination &&
           (!unitStats(unit.kind).requiresPsi || unit.powered);
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
        if (combatReady(unit) &&
            (isCombatUnit(unit.kind) || isStaticDefense(unit.kind))) {
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
                          std::max(0, unit->weaponCooldown), std::max(0, unit->ammo)});
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
            defender.unit->invincible ||
            (attacker.unit->ours && !defender.unit->detected) ||
            !attacker.unit->canAttack(*defender.unit)) {
            continue;
        }
        const auto& weapon = defender.unit->flying ? attacker.unit->airWeapon
                                                    : attacker.unit->groundWeapon;
        const auto separation = weaponDistance(*attacker.unit, *defender.unit);
        if (separation < weapon.minRange) continue;
        const auto gap = std::max(0.0, separation - static_cast<double>(weapon.maxRange));
        const auto& opposingWeapon = attacker.unit->flying ? defender.unit->airWeapon
                                                           : defender.unit->groundWeapon;
        if (isBuilding(attacker.unit->kind) && gap > 0.0 &&
            (!defender.unit->canAttack(*attacker.unit) ||
             opposingWeapon.maxRange >= weapon.maxRange ||
             isBuilding(defender.unit->kind))) continue;
        const auto closingSpeed = std::max(0.1, attacker.unit->topSpeed +
                                                   defender.unit->topSpeed * 0.20);
        const auto contactFrame = static_cast<int>(std::ceil(gap / closingSpeed));
        if (frame < contactFrame) continue;
        const auto damage = attackDamage(*attacker.unit, *defender.unit,
                                          defender.durability - pending[i]);
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
        if (attacker.unit->kind == UnitKind::reaver && attacker.ammunition <= 0) continue;
        auto targetIndex = std::size_t{0};
        const auto* target = nearestLivingTarget(attacker, defenders, pending,
                                                 targetIndex, frame);
        if (target == nullptr) continue;
        const auto& weapon = target->flying ? attacker.unit->airWeapon
                                            : attacker.unit->groundWeapon;
        pending[targetIndex] += attackDamage(*attacker.unit, *target,
            defenders[targetIndex].durability - pending[targetIndex]);
        attacker.readyFrame = frame + std::max(1, weapon.cooldown);
        // Scarabs are consumed; Interceptors return and must not be consumed.
        // Future Scarab production is not guaranteed by the observed bank.
        if (attacker.unit->kind == UnitKind::reaver) --attacker.ammunition;
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

double weaponDistance(const UnitSnapshot& a, const UnitSnapshot& b) noexcept {
    const auto dx = std::max({0, a.position.x - a.dimensionLeft -
                                   (b.position.x + b.dimensionRight),
                                b.position.x - b.dimensionLeft -
                                   (a.position.x + a.dimensionRight)});
    const auto dy = std::max({0, a.position.y - a.dimensionUp -
                                   (b.position.y + b.dimensionDown),
                                b.position.y - b.dimensionUp -
                                   (a.position.y + a.dimensionDown)});
    return std::hypot(static_cast<double>(dx), static_cast<double>(dy));
}

double attackDamage(const UnitSnapshot& attacker, const UnitSnapshot& target,
                    const double remainingDurability) noexcept {
    const auto& weapon = target.flying ? attacker.airWeapon : attacker.groundWeapon;
    if (weapon.damage <= 0 || target.invincible) return 0.0;
    auto shields = remainingDurability < 0.0 ? static_cast<double>(target.shields)
        : std::clamp(remainingDurability - target.hitPoints, 0.0,
                     static_cast<double>(target.shields));
    const auto ignoresArmor = weapon.damageType == DamageType::ignoreArmor;
    auto damage = 0.0;
    for (auto hit = 0; hit < std::max(1, weapon.hits); ++hit) {
        auto raw = static_cast<double>(weapon.damage);
        if (shields > 0.0) {
            raw = std::max(0.5, raw - (ignoresArmor ? 0 : target.shieldArmor));
            const auto absorbed = std::min(shields, raw);
            shields -= absorbed;
            damage += absorbed;
            raw -= absorbed;
            if (raw <= 0.0) continue;
        }
        // Shields always take full-size damage. HP armor applies before the
        // explosive/concussive size modifier, independently for every hit.
        damage += std::max(0.5, (raw - (ignoresArmor ? 0 : target.armor)) *
                                   sizeMultiplier(weapon.damageType, target.size));
    }
    return damage;
}

void accountIncomingDamage(GameState& state,
                           const std::span<const IncomingProjectile> projectiles) {
    for (auto& target : state.enemy.units) target.incomingDamage = 0.0;
    auto ordered = std::vector<IncomingProjectile>(projectiles.begin(), projectiles.end());
    std::ranges::sort(ordered, {}, &IncomingProjectile::id);
    auto previousId = -1;
    for (const auto& projectile : ordered) {
        if (projectile.id < 0 || projectile.id == previousId) continue;
        previousId = projectile.id;
        const auto source = std::ranges::find(state.self.units, projectile.source, &UnitSnapshot::id);
        const auto target = std::ranges::find(state.enemy.units, projectile.target, &UnitSnapshot::id);
        if (source == state.self.units.end() || target == state.enemy.units.end() ||
            !target->visible || !target->detected || target->invincible ||
            target->durability() <= 0) continue;
        auto attacker = *source;
        // A projectile is a hit, not an entire multi-hit volley.
        attacker.groundWeapon.hits = 1;
        attacker.airWeapon.hits = 1;
        const auto remaining = std::max(0.0, target->durability() - target->incomingDamage);
        if (remaining > 0.0) {
            target->incomingDamage += std::min(remaining, attackDamage(attacker, *target, remaining));
        }
    }
}

CombatEstimate CombatEvaluator::evaluate(
    const std::span<const UnitSnapshot> friendly,
    const std::span<const UnitSnapshot> enemy,
    const double requiredRatio,
    const double uncertainty,
    const bool runSimulation) const {
    CombatEstimate result;
    for (const auto& unit : friendly) {
        result.friendlyPower += unitPower(unit, enemy);
    }
    for (const auto& unit : enemy) {
        result.enemyPower += unitPower(unit, friendly);
    }

    const auto rawFriendlyPower = result.friendlyPower;
    const auto rawEnemyPower = result.enemyPower;
    auto simulation = SimulationOutcome{};
    if (runSimulation) {
        simulation = simulateEngagement(friendly, enemy);
    } else {
        simulation.friendlyInitial = rawFriendlyPower;
        simulation.enemyInitial = rawEnemyPower;
        simulation.friendlyRemaining = rawFriendlyPower;
        simulation.enemyRemaining = rawEnemyPower;
    }
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
    if (rawEnemyPower <= 0.0 && std::ranges::any_of(friendly, combatReady)) {
        // An army with no weapons that can affect this squad is no reason to
        // abandon its mission (for example, Corsairs flying past Zealots).
        result.ratio = std::max(result.ratio, requiredRatio);
    }
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

FightDecision EngagementTracker::stabilize(
    const std::uint64_t squadSignature,
    const FightDecision proposed,
    const double ratio,
    const double requiredRatio,
    const Frame frame) {
    constexpr auto staleFrames = 10 * 24;
    if (memory_.size() > 128U) {
        std::erase_if(memory_, [frame](const auto& entry) {
            return frame - entry.second.lastSeen > staleFrames;
        });
    }
    auto found = memory_.find(squadSignature);
    if (found == memory_.end() || frame - found->second.lastSeen > staleFrames) {
        memory_.insert_or_assign(
            squadSignature, Memory{proposed, proposed, 0, frame});
        return proposed;
    }

    auto& memory = found->second;
    memory.lastSeen = frame;
    if (proposed == memory.decision) {
        memory.candidate = proposed;
        memory.consecutive = 0;
        return memory.decision;
    }

    // Catastrophic estimates bypass hysteresis. Likewise, a decisive local
    // advantage may start an attack immediately rather than wasting a timing
    // window waiting for several redundant simulation passes.
    if ((proposed == FightDecision::retreat && ratio < requiredRatio * 0.55) ||
        (proposed == FightDecision::engage && ratio >= requiredRatio * 1.35)) {
        memory.decision = proposed;
        memory.candidate = proposed;
        memory.consecutive = 0;
        return memory.decision;
    }

    if (memory.candidate != proposed) {
        memory.candidate = proposed;
        memory.consecutive = 1;
    } else {
        ++memory.consecutive;
    }
    if (memory.consecutive >= 3) {
        memory.decision = proposed;
        memory.consecutive = 0;
    }
    return memory.decision;
}

void EngagementTracker::reset() {
    memory_.clear();
}

const UnitSnapshot* CombatEvaluator::selectTarget(
    const UnitSnapshot& attacker,
    const std::span<const UnitSnapshot> candidates,
    const std::span<const TargetAllocation> allocations) const {
    const UnitSnapshot* best = nullptr;
    auto bestScore = -std::numeric_limits<double>::infinity();
    const auto meleeAttacker = attacker.groundWeapon.targetsGround &&
                               attacker.groundWeapon.maxRange < 96;
    const auto hasCloseMeleeTarget = meleeAttacker && std::ranges::any_of(
        candidates, [&attacker](const UnitSnapshot& target) {
            return target.visible && target.detected && !target.flying &&
                   !target.hallucination && attacker.canAttack(target) &&
                   distanceSquared(attacker.position, target.position) <= 224 * 224;
        });
    const auto hasShot = !meleeAttacker && std::ranges::any_of(candidates,
        [&attacker, allocations](const UnitSnapshot& target) {
            const auto reserved = std::ranges::find(allocations, target.id, &TargetAllocation::target);
            const auto committed = reserved == allocations.end() ? 0 : reserved->committedDamage;
            const auto& weapon = target.flying ? attacker.airWeapon : attacker.groundWeapon;
            const auto separation = weaponDistance(attacker, target);
            return target.visible && target.detected && !target.invincible && !target.hallucination &&
                   attacker.canAttack(target) && target.durability() > target.incomingDamage + committed &&
                   separation >= weapon.minRange && separation <= weapon.maxRange;
        });
    for (const auto& target : candidates) {
        if (!target.visible || !target.detected || target.hallucination || target.invincible ||
            !attacker.canAttack(target)) {
            continue;
        }
        const auto allocation = std::ranges::find(
            allocations, target.id, &TargetAllocation::target);
        const auto committed = allocation != allocations.end()
                                   ? allocation->committedDamage
                                   : 0;
        const auto remainingHealth = static_cast<double>(target.durability() - committed) -
                                     target.incomingDamage;
        if (remainingHealth <= 0) continue;
        const auto range = weaponDistance(attacker, target);
        if (hasCloseMeleeTarget && range > 224.0) continue;
        const auto weapon = target.flying ? attacker.airWeapon : attacker.groundWeapon;
        if (range < weapon.minRange || (hasShot && range > weapon.maxRange + 32)) continue;
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
                              (target.canAttack(attacker) ? 2.0 : 0.0) +
                              (isWorker(target.kind) ? 0.7 : 0.0) +
                              (attacker.kind == UnitKind::corsair &&
                                       target.kind == UnitKind::overlord
                                   ? 3.0
                                   : 0.0) +
                              static_cast<double>(splashTargets) * 0.9;
        const auto effectiveHealth = std::max(1.0, remainingHealth);
        const auto killEfficiency = attackDamage(attacker, target, remainingHealth) / effectiveHealth;
        const auto inRange = range <= weapon.maxRange + 16 ? 2.0 : 0.0;
        const auto targetStability = target.id == attacker.orderTargetId &&
                                             range <= weapon.maxRange + 256
                                         ? 1.5
                                         : 0.0;
        const auto distanceDivisor = meleeAttacker ? 112.0 : 320.0;
        const auto score = priority + killEfficiency * 4.0 + inRange + targetStability -
                           range / distanceDivisor;
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
    if (!combatReady(unit) ||
        (!isCombatUnit(unit.kind) && !isStaticDefense(unit.kind))) {
        return 0.0;
    }
    if ((unit.kind == UnitKind::reaver || unit.kind == UnitKind::carrier) &&
        unit.ammo <= 0) {
        return unitStats(unit.kind).combatValue * 0.12;
    }
    const auto usefulTarget = [&unit](const UnitSnapshot& target) {
        if (target.invincible || target.loaded || !unit.canAttack(target)) return false;
        if (!isBuilding(unit.kind)) return true;
        const auto& weapon = target.flying ? unit.airWeapon : unit.groundWeapon;
        const auto& response = unit.flying ? target.airWeapon : target.groundWeapon;
        const auto separation = weaponDistance(unit, target);
        return (separation >= weapon.minRange && separation <= weapon.maxRange) ||
               (!isBuilding(target.kind) && target.topSpeed > 0.0 && target.canAttack(unit) &&
                response.maxRange < weapon.maxRange);
    };
    const auto airUseful = std::ranges::any_of(opposition, [&usefulTarget](const UnitSnapshot& target) {
        return target.flying && usefulTarget(target);
    });
    const auto groundUseful = std::ranges::any_of(opposition, [&usefulTarget](const UnitSnapshot& target) {
        return !target.flying && usefulTarget(target);
    });
    if (!airUseful && !groundUseful && !opposition.empty()) {
        return unit.role == UnitRole::spellcaster ? unitStats(unit.kind).combatValue * 0.6 : 0.0;
    }

    const auto& weapon = airUseful ? unit.airWeapon : unit.groundWeapon;
    const auto dps = static_cast<double>(weapon.damage * std::max(1, weapon.hits)) /
                     std::max(1, weapon.cooldown);
    const auto rangeFactor = 1.0 + std::clamp(weapon.maxRange / 256.0, 0.0, 1.0) * 0.35;
    const auto mobility = 1.0 + std::clamp(unit.topSpeed / 8.0, 0.0, 1.0) * 0.2;
    const auto vitality = std::clamp(unit.healthFraction(), 0.08, 1.0);
    return (unitStats(unit.kind).combatValue + dps * 0.45) * rangeFactor * mobility * vitality;
}

std::vector<Command> TacticalController::recharge(
    const std::span<const UnitSnapshot> friendly, const bool defending) const {
    std::vector<Command> commands;
    for (const auto& unit : friendly) {
        if ((!isCombatUnit(unit.kind) && !(defending && isWorker(unit.kind))) ||
            !combatReady(unit) || unit.maxShields <= 0 ||
            unit.shields * 5 >= unit.maxShields * 2 || unit.attackFrame ||
            (unit.underAttack && unit.healthFraction() >= 0.5)) continue;
        const UnitSnapshot* battery = nullptr;
        auto bestDistance = 256 * 256 + 1;
        for (const auto& candidate : friendly) {
            if (candidate.kind != UnitKind::shieldBattery || !combatReady(candidate) ||
                !candidate.powered || candidate.energy < 10 ||
                !candidate.position.valid()) continue;
            const auto candidateDistance = distanceSquared(unit.position, candidate.position);
            if (candidateDistance < bestDistance) {
                bestDistance = candidateDistance;
                battery = &candidate;
            }
        }
        if (battery != nullptr) {
            // A critically wounded unit's retreat has priority 100. Healing
            // must beat it or the most damaged units can never use a Battery.
            commands.push_back({unit.id, CommandType::recharge, battery->id, {-1, -1},
                                UnitKind::shieldBattery,
                                unit.healthFraction() < 0.28 ? 101 : 92, 0,
                                "shield-battery-recharge"});
        }
    }
    return commands;
}

std::vector<Command> TacticalController::control(
    const std::span<const UnitSnapshot> friendly,
    const std::span<const UnitSnapshot> enemy,
    const CombatEstimate& estimate,
    const Position objective,
    const Position retreatPoint,
    const InfluenceMap& influence,
    const Position formationCenter,
    const int latencyFrames,
    const bool psionicStormAvailable,
    const DefenseArea defense) const {
    std::vector<Command> commands;
    commands.reserve(friendly.size());
    CombatEvaluator evaluator;
    std::vector<TargetAllocation> allocations;
    allocations.reserve(enemy.size());
    std::vector<Position> plannedStorms;

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
        if (!isCombatUnit(unit.kind) || !combatReady(unit)) {
            continue;
        }
        const auto local = influence.at(unit.position);
        const auto localThreat = unit.flying ? local.airThreat : local.groundThreat;
        const auto protectedStep = [&defense](const Position proposed) {
            return defense.front.valid() && !defense.contains(proposed) ? defense.center : proposed;
        };
        const auto fragile = unit.healthFraction() < 0.28;
        // Our BWAPI detected flag describes our own vision, not the enemy's.
        // Use observed detection influence and actual incoming attacks to
        // decide whether a Dark Templar can probe a contain. This is a fog-of-
        // war risk estimate: unseen detectors or a future scan can invalidate it.
        const auto covertAdvance = unit.kind == UnitKind::darkTemplar && unit.cloaked &&
                                   !unit.underAttack && !fragile && local.detection <= 0.1F;
        const auto locallyOverwhelmed = localThreat > 5.0F &&
                                        estimate.decision != FightDecision::engage &&
                                        estimate.ratio < 1.0;

        if (unit.underStorm) {
            const auto escape = retreatPoint.valid() &&
                                        distanceSquared(unit.position, retreatPoint) > 96 * 96
                                    ? retreatPoint
                                    : Position{unit.position.x + 128, unit.position.y};
            commands.push_back({unit.id, CommandType::move, -1,
                                influence.safestStep(unit.position, escape, unit.flying),
                                UnitKind::unknown, 110, 0, "storm-escape"});
            continue;
        }

        // BWAPI explicitly warns that issuing an order during an attack frame
        // can interrupt the attack sequence. Let the shot complete instead of
        // producing stutter, cancelled Dragoon volleys, and indecisive melee.
        if (unit.attackFrame) continue;

        // Apply the mission gate before cloak-preserving advances and target
        // pursuit. Air-only harassment remains independent; endangered ground
        // units can still escape while the escort catches up.
        if (estimate.advanceBlocked && !unit.flying) {
            const auto escape = fragile || unit.underAttack || localThreat > 0.05F;
            commands.push_back({unit.id, escape ? CommandType::move : CommandType::hold, -1,
                escape ? influence.safestStep(unit.position, retreatPoint, false) : unit.position,
                UnitKind::unknown, 100, 0, "wait-for-mobile-detection"});
            continue;
        }

        // An attack-unit order follows a kiting opponent indefinitely. Return
        // stragglers to the protected area, and never acquire a distant target
        // merely because it is visible to another member of the squad.
        if (defense.active() && !covertAdvance && !defense.contains(unit.position)) {
            commands.push_back({unit.id, CommandType::move, -1,
                                defense.center,
                                UnitKind::unknown, 90, 0, "defense-return"});
            continue;
        }
        std::vector<UnitSnapshot> defenseTargets;
        if (defense.active() && !covertAdvance) {
            for (const auto& candidate : enemy) {
                const auto& weapon = candidate.flying ? unit.airWeapon : unit.groundWeapon;
                if (defense.contains(candidate.position) ||
                    weaponDistance(unit, candidate) <= weapon.maxRange) {
                    defenseTargets.push_back(candidate);
                }
            }
        }
        const auto targets = defense.active() && !covertAdvance
                                 ? std::span<const UnitSnapshot>{defenseTargets} : enemy;
        const auto target = evaluator.selectTarget(unit, targets, allocations);

        if (psionicStormAvailable && unit.kind == UnitKind::highTemplar &&
            unit.energy >= 75) {
            constexpr auto stormRadius = 80;
            constexpr auto castRange = 9 * 32;
            Position bestPosition{-1, -1};
            auto bestScore = 1.8;
            for (const auto& candidate : enemy) {
                if (!candidate.visible || !candidate.detected || candidate.invincible ||
                    candidate.underStorm || isBuilding(candidate.kind) ||
                    !candidate.position.valid() ||
                    distance(unit.position, candidate.position) > castRange ||
                    std::ranges::any_of(plannedStorms, [&candidate](const Position position) {
                        return distanceSquared(position, candidate.position) <= 112 * 112;
                    })) {
                    continue;
                }
                auto enemyValue = 0.0;
                auto friendlyValue = 0.0;
                for (const auto& nearby : enemy) {
                    if (!nearby.visible || nearby.invincible || nearby.underStorm ||
                        isBuilding(nearby.kind) ||
                        distanceSquared(nearby.position, candidate.position) >
                            stormRadius * stormRadius) {
                        continue;
                    }
                    enemyValue += unitStats(nearby.kind).combatValue *
                                  std::clamp(nearby.healthFraction(), 0.2, 1.0);
                }
                for (const auto& nearby : friendly) {
                    if (nearby.invincible || isBuilding(nearby.kind) ||
                        distanceSquared(nearby.position, candidate.position) >
                            stormRadius * stormRadius) {
                        continue;
                    }
                    friendlyValue += unitStats(nearby.kind).combatValue *
                                     std::clamp(nearby.healthFraction(), 0.2, 1.0);
                }
                const auto score = enemyValue - friendlyValue * 1.75;
                if (score > bestScore) {
                    bestScore = score;
                    bestPosition = candidate.position;
                }
            }
            if (bestPosition.valid()) {
                plannedStorms.push_back(bestPosition);
                commands.push_back({
                    unit.id, CommandType::useTech, -1, bestPosition,
                    UnitKind::unknown, 98, 0, "psionic-storm",
                    TechnologyKind::psionicStorm,
                });
                continue;
            }
        }

        if (fragile || (!covertAdvance &&
                       (estimate.decision == FightDecision::retreat || locallyOverwhelmed))) {
            // Falling back must not silence a ready ranged volley. Only fire
            // at targets already in range; an attack order toward a distant
            // target would reverse the retreat. Wounded units keep escaping.
            if (!fragile && unit.weaponCooldown == 0 &&
                ((unit.kind != UnitKind::reaver && unit.kind != UnitKind::carrier) ||
                 unit.ammo > 0)) {
                std::vector<UnitSnapshot> firingTargets;
                for (const auto& candidate : targets) {
                    const auto& weapon = candidate.flying ? unit.airWeapon : unit.groundWeapon;
                    const auto range = weaponDistance(unit, candidate);
                    if (weapon.maxRange >= 96 && range >= weapon.minRange &&
                        range <= weapon.maxRange) firingTargets.push_back(candidate);
                }
                if (const auto* shot = evaluator.selectTarget(unit, firingTargets, allocations)) {
                    commands.push_back({unit.id, CommandType::attackUnit, shot->id, {-1, -1},
                                        UnitKind::unknown, 86, 0, "retreat-volley"});
                    const auto allocation = std::ranges::find(
                        allocations, shot->id, &TargetAllocation::target);
                    const auto committed = allocation == allocations.end() ? 0.0 : allocation->committedDamage;
                    const auto remaining = std::max(0.0, shot->durability() - shot->incomingDamage - committed);
                    const auto damage = std::min(remaining, attackDamage(unit, *shot, remaining));
                    if (allocation == allocations.end()) allocations.push_back({shot->id, damage});
                    else allocation->committedDamage += damage;
                    continue;
                }
            }
            commands.push_back({
                unit.id, CommandType::move, -1,
                localThreat > 0.05F
                    ? protectedStep(influence.safestStep(unit.position, retreatPoint, unit.flying))
                    : protectedStep(retreatPoint),
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
            const auto range = weaponDistance(unit, *target);
            const auto readySoon = unit.weaponCooldown <= std::max(1, latencyFrames + 2);
            const auto canFire = readySoon && range <= weapon.maxRange + 12;
            if (unit.cloaked && local.detection > 0.1F &&
                target->role != UnitRole::detector && !canFire) {
                commands.push_back({
                    unit.id, CommandType::move, -1,
                    influence.safestStep(unit.position, retreatPoint, unit.flying, true),
                    UnitKind::unknown, 94, 0, "cloak-preservation",
                });
                continue;
            }

            const auto ranged = weapon.maxRange >= 96;
            const auto targetWeapon = unit.flying ? target->airWeapon : target->groundWeapon;
            const auto rangeAdvantage = weapon.maxRange >= targetWeapon.maxRange + 48;
            const auto kite = estimate.decision == FightDecision::kite || rangeAdvantage;
            const auto targetCanPressure = targetWeapon.damage > 0 &&
                                           range <= targetWeapon.maxRange + 64;
            if (!canFire && kite && ranged && targetCanPressure &&
                unit.weaponCooldown > latencyFrames + 2 &&
                range <= weapon.maxRange + 64) {
                const Position away{
                    unit.position.x + unit.position.x - target->position.x,
                    unit.position.y + unit.position.y - target->position.y,
                };
                commands.push_back({
                    unit.id, CommandType::move, -1,
                    protectedStep(influence.safestStep(unit.position, away, unit.flying)),
                    UnitKind::unknown, 84, 0, "combat-kite",
                });
            } else {
                commands.push_back({unit.id, CommandType::attackUnit, target->id, {-1, -1},
                                    UnitKind::unknown, 80, 0, "focus-fire"});
                // Reserve exact damage for imminent volleys and a conservative
                // half-volley for nearby melee commitments. Without the latter,
                // every Zealot paths to the same unit before any hit lands.
                const auto approachingMelee = !ranged &&
                    range <= weapon.maxRange + 96;
                if (canFire || approachingMelee) {
                    const auto allocation = std::ranges::find(
                        allocations, target->id, &TargetAllocation::target);
                    const auto committed = allocation == allocations.end() ? 0.0 : allocation->committedDamage;
                    const auto remaining = std::max(0.0, target->durability() -
                        target->incomingDamage - committed);
                    const auto fullDamage = attackDamage(unit, *target, remaining);
                    const auto damage = std::min(remaining, fullDamage * (canFire ? 1.0 : 0.5));
                    if (allocation == allocations.end()) {
                        allocations.push_back({target->id, damage});
                    } else {
                        allocation->committedDamage += damage;
                    }
                }
            }
        } else if (defense.active()) {
            // Attack-move would let the engine acquire the same forbidden
            // pursuit between control ticks. Move into the screen, then hold.
            const auto anchor = objective.valid() && defense.contains(objective)
                                    ? objective : defense.center;
            if (distanceSquared(unit.position, anchor) > 96 * 96) {
                commands.push_back({unit.id, CommandType::move, -1, anchor,
                                    UnitKind::unknown, 82, 0, "defense-screen"});
            } else {
                commands.push_back({unit.id, CommandType::hold, -1, {-1, -1},
                                    UnitKind::unknown, 82, 0, "defense-hold"});
            }
        } else if (!enemy.empty() && formationCenter.valid() && friendly.size() >= 4 &&
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

}  // namespace protodd
