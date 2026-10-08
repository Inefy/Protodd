#include "protodd/Combat.hpp"
#include "protodd/TemplarManagement.hpp"
#include "protodd/TacticalTargetModel.hpp"

#include "protodd/UnitCatalog.hpp"
#include "protodd/Navigation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace protodd {
namespace {

constexpr auto simulationHorizonFrames = 24 * 14;
constexpr auto routeSearchExpansions = 512;
constexpr auto movingScarabImpactRetention = 0.65;

bool suicideAttacker(const UnitKind kind) noexcept {
    return kind == UnitKind::scourge || kind == UnitKind::spiderMine ||
           kind == UnitKind::infestedTerran;
}

MovementFootprint movementFootprint(const UnitSnapshot& unit) noexcept {
    return {unit.dimensionLeft, unit.dimensionRight,
            unit.dimensionUp, unit.dimensionDown};
}

double modeledAttackDamage(const UnitSnapshot& attacker,
                           const UnitSnapshot& target,
                           const double remainingDurability) noexcept {
    const auto nominal = attackDamage(attacker, target, remainingDurability);
    // Scarabs can be launched and still miss a moving target. Use expected
    // damage instead of treating each observed ammunition decrement as a hit.
    return attacker.kind == UnitKind::reaver && target.topSpeed > 0.05
               ? nominal * movingScarabImpactRetention
               : nominal;
}

struct SimUnit {
    const UnitSnapshot* unit{};
    double durability{};
    int readyFrame{};
    int ammunition{};
    bool expended{};
};

struct SimulationOutcome {
    double friendlyRemaining{};
    double enemyRemaining{};
    double friendlyInitial{};
    double enemyInitial{};
    double coverage{1.0};
    Frame frames{};
    int friendlyDeaths{};
    int enemyDeaths{};
    bool outcomeReached{};
    bool routingDeferred{};
    SimulationOmissions omittedFriendly{};
    SimulationOmissions omittedEnemy{};
};

struct SimulationSelection {
    std::vector<SimUnit> units;
    std::size_t eligibleCount{};
    double representedValue{};
    double representedGroundWeaponValue{};
    double representedAirWeaponValue{};
    SimulationOmissions omitted{};
};

struct ApproachRoute {
    bool reachable{true};
    double distance{};
    Position bottleneck{-1, -1};
    int bottleneckWidth{};
};

struct ApproachWorkBudget {
    int pathSearches{};
    int clearanceSamples{};
    bool deferred{};
};

class ApproachRoutes {
public:
    ApproachRoutes(const NavigationGrid* navigation, ApproachWorkBudget& budget)
        : navigation_(navigation), budget_(budget) {}

    [[nodiscard]] int cellSize() const noexcept {
        return navigation_ == nullptr ? 32 : std::max(1, navigation_->cellSize());
    }

    const ApproachRoute& get(const std::size_t attackerIndex,
                             const std::size_t defenderIndex,
                             const UnitSnapshot& attacker,
                             const UnitSnapshot& defender) {
        const auto key = (static_cast<std::uint64_t>(attackerIndex) << 32U) |
                         static_cast<std::uint64_t>(defenderIndex);
        if (const auto found = routes_.find(key); found != routes_.end()) return found->second;
        return routes_.emplace(key, calculate(attacker, defender)).first->second;
    }

private:
    const NavigationGrid* navigation_{};
    ApproachWorkBudget& budget_;
    std::unordered_map<std::uint64_t, ApproachRoute> routes_;

    [[nodiscard]] int clearWidth(const Position point, const double normalX,
                                 const double normalY) const {
        const auto cell = std::max(1, navigation_->cellSize());
        const auto maximumSideCells = (128 + cell - 1) / cell;
        auto left = 0;
        auto right = 0;
        for (auto step = 1; step <= maximumSideCells; ++step) {
            const Position sample{
                point.x + static_cast<int>(std::lround(normalX * step * cell)),
                point.y + static_cast<int>(std::lround(normalY * step * cell))};
            if (!navigation_->walkable(sample) || !navigation_->lineWalkable(point, sample)) break;
            ++left;
        }
        for (auto step = 1; step <= maximumSideCells; ++step) {
            const Position sample{
                point.x - static_cast<int>(std::lround(normalX * step * cell)),
                point.y - static_cast<int>(std::lround(normalY * step * cell))};
            if (!navigation_->walkable(sample) || !navigation_->lineWalkable(point, sample)) break;
            ++right;
        }
        return (left + right + 1) * cell;
    }

    void measureClearance(const std::span<const Position> path, ApproachRoute& result) const {
        if (path.size() < 2U) return;
        auto narrowest = std::numeric_limits<int>::max();
        for (std::size_t index = 0; index < path.size(); ++index) {
            if (budget_.clearanceSamples >= maximumEngagementClearanceSamples) {
                budget_.deferred = true;
                return;
            }
            ++budget_.clearanceSamples;
            const auto before = path[index == 0 ? index : index - 1];
            const auto after = path[index + 1 < path.size() ? index + 1 : index];
            const auto dx = static_cast<double>(after.x - before.x);
            const auto dy = static_cast<double>(after.y - before.y);
            const auto length = std::hypot(dx, dy);
            if (length < 1.0) continue;
            const auto width = clearWidth(path[index], -dy / length, dx / length);
            if (width >= narrowest) continue;
            narrowest = width;
            result.bottleneck = path[index];
        }
        if (narrowest != std::numeric_limits<int>::max())
            result.bottleneckWidth = narrowest;
    }

    [[nodiscard]] ApproachRoute calculate(const UnitSnapshot& attacker,
                                          const UnitSnapshot& defender) {
        ApproachRoute result;
        if (budget_.deferred) { result.reachable = false; return result; }
        const auto directDistance = weaponDistance(attacker, defender);
        result.distance = directDistance;
        if (attacker.flying || defender.flying || navigation_ == nullptr ||
            navigation_->empty() || !attacker.position.valid() || !defender.position.valid()) {
            return result;
        }

        std::vector<Position> path;
        const auto footprint = movementFootprint(attacker);
        if (navigation_->lineWalkable(attacker.position, defender.position, footprint)) {
            const auto centerDistance = distance(attacker.position, defender.position);
            result.distance = directDistance;
            const auto steps = std::max(1, static_cast<int>(std::ceil(
                centerDistance / std::max(1, navigation_->cellSize()))));
            path.reserve(static_cast<std::size_t>(steps + 1));
            for (auto step = 0; step <= steps; ++step) {
                const auto fraction = static_cast<double>(step) / steps;
                path.push_back({
                    static_cast<int>(std::lround(attacker.position.x +
                        (defender.position.x - attacker.position.x) * fraction)),
                    static_cast<int>(std::lround(attacker.position.y +
                        (defender.position.y - attacker.position.y) * fraction))});
            }
        } else {
            if (budget_.pathSearches >= maximumEngagementRouteSearches) {
                budget_.deferred = true;
                result.reachable = false;
                return result;
            }
            ++budget_.pathSearches;
            auto route = navigation_->findPath(attacker.position, defender.position,
                                               routeSearchExpansions, footprint);
            if (!route.reached()) {
                // A bounded prefix or exhausted allowance is not proof that
                // the unit cannot contribute. Abandon the detailed outcome.
                if (route.status != NavigationStatus::unreachable)
                    budget_.deferred = true;
                result.reachable = false;
                return result;
            }
            path = std::move(route.points);
            auto pathLength = 0.0;
            for (auto point = path.begin() + 1; point != path.end(); ++point)
                pathLength += distance(*(point - 1), *point);
            const auto centerDistance = distance(attacker.position, defender.position);
            // Navigation paths follow unit centers. Convert the detour to an
            // edge-to-edge distance so weapon range remains consistent.
            result.distance = std::max(directDistance,
                pathLength - std::max(0.0, centerDistance - directDistance));
        }
        measureClearance(path, result);
        return result;
    }
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

void simulationUnits(const std::span<const UnitSnapshot> source,
                    SimulationSelection& selection) {
    std::vector<const UnitSnapshot*> selected;
    selected.reserve(source.size());
    for (const auto& unit : source) {
        if (combatReady(unit) &&
            (isCombatUnit(unit.kind) || isStaticDefense(unit.kind) || isWorker(unit.kind))) {
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
    selection.eligibleCount = selected.size();
    const auto selectedCount = std::min(maximumUnits, selected.size());

    selection.units.reserve(selectedCount);
    const auto addRepresented = [&selection](const UnitSnapshot& unit) {
        const auto value = unitStats(unit.kind).combatValue * unit.healthFraction();
        selection.representedValue += value;
        if (unit.groundWeapon.damage > 0 && unit.groundWeapon.targetsGround)
            selection.representedGroundWeaponValue += value;
        if (unit.airWeapon.damage > 0 && unit.airWeapon.targetsAir)
            selection.representedAirWeaponValue += value;
    };
    for (auto index = std::size_t{0}; index < selectedCount; ++index) {
        const auto* unit = selected[index];
        selection.units.push_back({unit, static_cast<double>(std::max(1, unit->durability())),
                                   std::max(0, unit->weaponCooldown), std::max(0, unit->ammo)});
        addRepresented(*unit);
    }
    for (auto index = selectedCount; index < selected.size(); ++index) {
        const auto& unit = *selected[index];
        const auto value = unitStats(unit.kind).combatValue * unit.healthFraction();
        ++selection.omitted.unitCount;
        selection.omitted.combatValue += value;
        const auto role = static_cast<std::uint32_t>(unit.role);
        if (role < 32U) selection.omitted.roleMask |= std::uint32_t{1} << role;
        if (unit.groundWeapon.damage > 0 && unit.groundWeapon.targetsGround)
            selection.omitted.groundWeaponValue += value;
        if (unit.airWeapon.damage > 0 && unit.airWeapon.targetsAir)
            selection.omitted.airWeaponValue += value;
    }
}

std::uint32_t roleMask(const UnitRole role) noexcept {
    const auto index = static_cast<std::uint32_t>(role);
    return index < 32U ? std::uint32_t{1} << index : 0U;
}

double selectionConfidenceCoverage(const SimulationSelection& selection,
                                   const std::span<const UnitSnapshot> opposition) noexcept {
    if (selection.eligibleCount == 0U) return 1.0;
    const auto retainedCount = selection.units.size();
    const auto countCoverage = static_cast<double>(retainedCount) /
                               static_cast<double>(selection.eligibleCount);
    const auto totalValue = selection.representedValue + selection.omitted.combatValue;
    const auto valueCoverage = totalValue > 1e-9
        ? selection.representedValue / totalValue : countCoverage;
    auto coverage = std::min(countCoverage, valueCoverage);

    // Any capped force is visibly approximate, even when the 97th unit is a
    // small fraction of the aggregate value. Keep that fact in the public
    // confidence instead of allowing a 96/97 count to look nearly complete.
    if (selection.omitted.unitCount > 0U) coverage = std::min(coverage, 0.85);

    const auto hasAirTargets = std::ranges::any_of(opposition, [](const UnitSnapshot& unit) {
        return combatReady(unit) && unit.flying && !unit.invincible;
    });
    const auto hasGroundTargets = std::ranges::any_of(opposition, [](const UnitSnapshot& unit) {
        return combatReady(unit) && !unit.flying && !unit.invincible;
    });
    if (hasAirTargets && selection.omitted.airWeaponValue > 0.0) {
        if (selection.representedAirWeaponValue <= 0.0) {
            // The only modeled counter to an airborne enemy may be beyond the
            // cap, so this fight estimate cannot authorize a confident call.
            coverage = std::min(coverage, 0.45);
        } else {
            const auto domainTotal = selection.representedAirWeaponValue +
                                     selection.omitted.airWeaponValue;
            const auto omittedShare = selection.omitted.airWeaponValue / domainTotal;
            coverage *= 1.0 - 0.5 * omittedShare;
        }
    }
    if (hasGroundTargets && selection.omitted.groundWeaponValue > 0.0 &&
        selection.representedGroundWeaponValue <= 0.0)
        coverage = std::min(coverage, 0.65);
    if ((selection.omitted.roleMask & roleMask(UnitRole::spellcaster)) != 0U)
        coverage = std::min(coverage, 0.65);
    return std::clamp(coverage, 0.2, 1.0);
}

const UnitSnapshot* nearestLivingTarget(
    const SimUnit& attacker,
    const std::size_t attackerIndex,
    const std::span<const SimUnit> attackers,
    const std::span<const SimUnit> defenders,
    const std::span<const double> pending,
    std::size_t& targetIndex,
    const int frame,
    int& nextContactFrame,
    ApproachRoutes& routes) {
    const UnitSnapshot* best = nullptr;
    auto bestScore = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < defenders.size(); ++i) {
        const auto& defender = defenders[i];
        if (defender.durability - pending[i] <= 0.0 ||
            defender.unit->invincible ||
            (attacker.unit->ours && !defender.unit->detected &&
             defender.unit->requiresDetection()) ||
            !attacker.unit->canAttack(*defender.unit)) {
            continue;
        }
        const auto& weapon = defender.unit->flying ? attacker.unit->airWeapon
                                                    : attacker.unit->groundWeapon;
        const auto separation = weaponDistance(*attacker.unit, *defender.unit);
        if (separation < weapon.minRange) continue;
        // An observed target already in legal weapon range does not require
        // a ground approach route (ranged fire can cross impassable terrain).
        const ApproachRoute inRange{true, separation};
        const auto& route = separation <= weapon.maxRange ? inRange :
            routes.get(attackerIndex, i, *attacker.unit, *defender.unit);
        if (!route.reachable) continue;
        const auto& opposingWeapon = attacker.unit->flying ? defender.unit->airWeapon
                                                           : defender.unit->groundWeapon;
        const auto gap = std::max(0.0, route.distance - static_cast<double>(weapon.maxRange));
        if (isBuilding(attacker.unit->kind) && gap > 0.0 &&
            (!defender.unit->canAttack(*attacker.unit) ||
             opposingWeapon.maxRange >= weapon.maxRange ||
             isBuilding(defender.unit->kind))) continue;
        auto contactFrame = 0;
        if (gap > 0.0) {
            auto closingSpeed = isBuilding(attacker.unit->kind)
                ? 0.0 : std::max(0.0, attacker.unit->topSpeed);
            if (defender.unit->visible && defender.unit->detected &&
                defender.unit->lastPosition.valid() && defender.unit->position.valid() &&
                defender.unit->topSpeed > 0.0 &&
                distance(defender.unit->position, attacker.unit->position) -
                    distance(defender.unit->lastPosition, attacker.unit->position) >= 2.0) {
                // A target observed moving away keeps its full plausible speed.
                // Do not credit an arbitrary fraction as closing movement.
                closingSpeed -= defender.unit->topSpeed;
            }
            if (closingSpeed <= 0.05) continue;
            contactFrame = static_cast<int>(std::ceil(gap / closingSpeed));
        }
        if (route.bottleneck.valid() && route.bottleneckWidth > 0 &&
            route.bottleneckWidth <= 4 * routes.cellSize() &&
            !attacker.unit->flying && !defender.unit->flying) {
            const auto cellSize = routes.cellSize();
            const auto chokeX = route.bottleneck.x / cellSize;
            const auto chokeY = route.bottleneck.y / cellSize;
            std::vector<UnitId> queued;
            queued.reserve(attackers.size());
            for (std::size_t peer = 0; peer < attackers.size(); ++peer) {
                const auto& candidate = attackers[peer];
                if (candidate.durability <= 0.0 || candidate.unit->flying ||
                    isBuilding(candidate.unit->kind) || candidate.unit->topSpeed <= 0.05 ||
                    !candidate.unit->canAttack(*defender.unit) ||
                    (candidate.unit->kind == UnitKind::reaver && candidate.ammunition <= 0))
                    continue;
                const auto& peerRoute = routes.get(peer, i, *candidate.unit, *defender.unit);
                if (!peerRoute.reachable || !peerRoute.bottleneck.valid() ||
                    peerRoute.bottleneckWidth <= 0 ||
                    peerRoute.bottleneckWidth > 4 * cellSize ||
                    peerRoute.bottleneck.x / cellSize != chokeX ||
                    peerRoute.bottleneck.y / cellSize != chokeY) continue;
                queued.push_back(candidate.unit->id);
            }
            std::ranges::sort(queued);
            const auto rank = static_cast<int>(
                std::ranges::lower_bound(queued, attacker.unit->id) - queued.begin());
            const auto lanes = std::max(1, route.bottleneckWidth / 64);
            contactFrame += (rank / lanes) * 8;
        }
        if (frame < contactFrame) {
            nextContactFrame = std::min(nextContactFrame, contactFrame);
            continue;
        }
        const auto damage = modeledAttackDamage(*attacker.unit, *defender.unit,
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
    std::vector<double>& pendingDefenders,
    std::vector<double>& pendingAttackers,
    const int frame,
    ApproachRoutes& routes) {
    for (std::size_t attackerIndex = 0; attackerIndex < attackers.size(); ++attackerIndex) {
        auto& attacker = attackers[attackerIndex];
        if (attacker.durability <= 0.0 || frame < attacker.readyFrame) continue;
        if (attacker.unit->kind == UnitKind::reaver && attacker.ammunition <= 0) {
            attacker.readyFrame = std::numeric_limits<int>::max();
            continue;
        }
        auto targetIndex = std::size_t{0};
        auto nextContactFrame = std::numeric_limits<int>::max();
        const auto* target = nearestLivingTarget(attacker, attackerIndex, attackers,
            defenders, pendingDefenders, targetIndex, frame, nextContactFrame, routes);
        if (target == nullptr) {
            // Retry at the earliest possible contact. A unit with no legal
            // opponent cannot acquire one later as this simulation only
            // removes units; avoid rescanning it at every other unit's event.
            attacker.readyFrame = nextContactFrame;
            continue;
        }
        const auto& weapon = target->flying ? attacker.unit->airWeapon
                                            : attacker.unit->groundWeapon;
        pendingDefenders[targetIndex] += modeledAttackDamage(*attacker.unit, *target,
            defenders[targetIndex].durability - pendingDefenders[targetIndex]);
        if (weapon.splashOuter > 0) {
            UnitSnapshot impact;
            impact.position = target->position;
            for (std::size_t index = 0; index < defenders.size(); ++index) {
                const auto& collateral = defenders[index];
                if (index == targetIndex ||
                    collateral.durability <= pendingDefenders[index] ||
                    collateral.unit->flying != target->flying || collateral.unit->invincible ||
                    !attacker.unit->canAttack(*collateral.unit)) continue;
                const auto radius = weaponDistance(impact, *collateral.unit);
                if (radius > weapon.splashOuter) continue;
                const auto scale = radius <= weapon.splashInner ? 1.0 :
                    radius <= weapon.splashMiddle ? 0.5 : 0.25;
                const auto movementRetention = collateral.unit->topSpeed > 0.05 ? 0.5 : 1.0;
                pendingDefenders[index] += modeledAttackDamage(*attacker.unit, *collateral.unit,
                    collateral.durability - pendingDefenders[index]) * scale * movementRetention;
            }
            if (weapon.splashFriendlyFire) {
                for (std::size_t index = 0; index < attackers.size(); ++index) {
                    const auto& collateral = attackers[index];
                    if (collateral.durability <= pendingAttackers[index] ||
                        collateral.unit->flying != target->flying || collateral.unit->invincible ||
                        !attacker.unit->canAttack(*collateral.unit)) continue;
                    const auto radius = weaponDistance(impact, *collateral.unit);
                    if (radius > weapon.splashOuter) continue;
                    const auto scale = radius <= weapon.splashInner ? 1.0 :
                        radius <= weapon.splashMiddle ? 0.5 : 0.25;
                    const auto movementRetention = collateral.unit->topSpeed > 0.05 ? 0.5 : 1.0;
                    pendingAttackers[index] += modeledAttackDamage(*attacker.unit, *collateral.unit,
                        collateral.durability - pendingAttackers[index]) * scale * movementRetention;
                }
            }
        }
        attacker.readyFrame = frame + std::max(1, weapon.cooldown);
        // Apply consumption after both armies schedule this frame's damage.
        // A suicide impact must not erase an opposing simultaneous volley.
        attacker.expended = suicideAttacker(attacker.unit->kind);
        // Scarabs are consumed; Interceptors return and must not be consumed.
        // Future Scarab production is not guaranteed by the observed bank.
        if (attacker.unit->kind == UnitKind::reaver) --attacker.ammunition;
    }
}

SimulationOutcome simulateEngagement(
    const std::span<const UnitSnapshot> friendlySource,
    const std::span<const UnitSnapshot> enemySource,
    const NavigationGrid* navigation) {
    SimulationSelection friendlySelection;
    SimulationSelection enemySelection;
    simulationUnits(friendlySource, friendlySelection);
    simulationUnits(enemySource, enemySelection);
    auto& friendly = friendlySelection.units;
    auto& enemy = enemySelection.units;
    SimulationOutcome result;
    result.omittedFriendly = friendlySelection.omitted;
    result.omittedEnemy = enemySelection.omitted;
    for (const auto& unit : friendly) result.friendlyInitial += simulationValue(unit);
    for (const auto& unit : enemy) result.enemyInitial += simulationValue(unit);

    std::vector<double> damageToFriendly(friendly.size(), 0.0);
    std::vector<double> damageToEnemy(enemy.size(), 0.0);
    ApproachWorkBudget routeBudget;
    ApproachRoutes friendlyRoutes(navigation, routeBudget);
    ApproachRoutes enemyRoutes(navigation, routeBudget);
    // Advance directly to the next cooldown/contact event. Fixed six-frame
    // ticks turned eight-frame weapons into twelve-frame weapons and kept
    // rounding every subsequent volley. Damage on one frame remains atomic.
    for (auto frame = 0; frame <= simulationHorizonFrames;) {
        std::ranges::fill(damageToFriendly, 0.0);
        std::ranges::fill(damageToEnemy, 0.0);
        scheduleVolleys(friendly, enemy, damageToEnemy, damageToFriendly,
                        frame, friendlyRoutes);
        if (routeBudget.deferred) { result.routingDeferred = true; return result; }
        scheduleVolleys(enemy, friendly, damageToFriendly, damageToEnemy,
                        frame, enemyRoutes);
        if (routeBudget.deferred) { result.routingDeferred = true; return result; }
        auto nextFrame = std::numeric_limits<int>::max();
        for (std::size_t i = 0; i < friendly.size(); ++i) {
            auto& unit = friendly[i];
            unit.durability = unit.expended ? 0.0 : unit.durability - damageToFriendly[i];
            if (unit.durability > 0.0) nextFrame = std::min(nextFrame, unit.readyFrame);
        }
        for (std::size_t i = 0; i < enemy.size(); ++i) {
            auto& unit = enemy[i];
            unit.durability = unit.expended ? 0.0 : unit.durability - damageToEnemy[i];
            if (unit.durability > 0.0) nextFrame = std::min(nextFrame, unit.readyFrame);
        }
        const auto friendlyAlive = std::ranges::any_of(
            friendly, [](const SimUnit& unit) { return unit.durability > 0.0; });
        const auto enemyAlive = std::ranges::any_of(
            enemy, [](const SimUnit& unit) { return unit.durability > 0.0; });
        if (!friendlyAlive || !enemyAlive) {
            result.frames = frame;
            result.outcomeReached = true;
            break;
        }
        if (nextFrame == std::numeric_limits<int>::max() ||
            nextFrame > simulationHorizonFrames) {
            result.frames = simulationHorizonFrames;
            break;
        }
        frame = nextFrame;
    }
    for (const auto& unit : friendly) {
        if (unit.durability > 0.0) result.friendlyRemaining += simulationValue(unit);
        else ++result.friendlyDeaths;
    }
    for (const auto& unit : enemy) {
        if (unit.durability > 0.0) result.enemyRemaining += simulationValue(unit);
        else ++result.enemyDeaths;
    }
    result.coverage = std::min(
        selectionConfidenceCoverage(friendlySelection, enemySource),
        selectionConfidenceCoverage(enemySelection, friendlySource));
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

double psionicStormValue(const Position center,
                         const std::span<const UnitSnapshot> enemies,
                         const std::span<const UnitSnapshot> allies) noexcept {
    if (!center.valid()) return 0.0;
    const auto radiusSquared = psionicStormRadiusPixels * psionicStormRadiusPixels;
    auto enemyValue = 0.0;
    auto friendlyValue = 0.0;
    for (const auto& unit : enemies) {
        if (!unit.visible || !unit.detected || unit.invincible || unit.underStorm ||
            unit.loaded || unit.hallucination || !unit.position.valid() || isBuilding(unit.kind) ||
            distanceSquared(unit.position, center) > radiusSquared) continue;
        enemyValue += unitStats(unit.kind).combatValue *
                      std::clamp(unit.healthFraction(), 0.2, 1.0);
    }
    for (const auto& unit : allies) {
        if (unit.loaded || unit.invincible || unit.hallucination ||
            isBuilding(unit.kind) || !unit.position.valid() ||
            distanceSquared(unit.position, center) > radiusSquared) continue;
        friendlyValue += unitStats(unit.kind).combatValue *
                         std::clamp(unit.healthFraction(), 0.2, 1.0);
    }
    return enemyValue - friendlyValue * 1.75;
}

bool psionicStormSafe(const Position center,
                      const std::span<const UnitSnapshot> enemies,
                      const std::span<const UnitSnapshot> allies) noexcept {
    return psionicStormValue(center, enemies, allies) > minimumPsionicStormValue;
}

void accountIncomingDamage(GameState& state,
                           const std::span<const IncomingProjectile> projectiles) {
    for (auto& target : state.enemy.units) target.incomingDamage = 0.0;
    auto ordered = std::vector<IncomingProjectile>(projectiles.begin(), projectiles.end());
    std::erase_if(ordered, [](const IncomingProjectile& projectile) {
        return projectile.id < 0 || !std::isfinite(projectile.impactFrames) ||
               projectile.impactFrames <= 1.0 || projectile.impactFrames > 24.0 ||
               projectile.sourceFirstSeen < 0 || projectile.targetFirstSeen < 0;
    });
    std::ranges::sort(ordered, {}, &IncomingProjectile::id);
    ordered.erase(std::unique(ordered.begin(), ordered.end(),
        [](const IncomingProjectile& a, const IncomingProjectile& b) {
            return a.id == b.id;
        }), ordered.end());
    std::ranges::sort(ordered, [](const IncomingProjectile& a,
                                  const IncomingProjectile& b) {
        if (a.impactFrames != b.impactFrames) return a.impactFrames < b.impactFrames;
        return a.id < b.id;
    });
    for (const auto& projectile : ordered) {
        const auto source = std::ranges::find(state.self.units, projectile.source, &UnitSnapshot::id);
        const auto target = std::ranges::find(state.enemy.units, projectile.target, &UnitSnapshot::id);
        if (target == state.enemy.units.end() ||
            !target->visible || !target->detected || target->invincible ||
            target->durability() <= 0 ||
            target->firstSeen != projectile.targetFirstSeen) continue;
        const auto sourceKind =
            projectile.family == IncomingProjectileFamily::dragoonPhaseDisruptor
                ? UnitKind::dragoon
            : projectile.family == IncomingProjectileFamily::photonCannonOverlay
                ? UnitKind::photonCannon
                : UnitKind::unknown;
        if (sourceKind == UnitKind::unknown ||
            (source != state.self.units.end() &&
             (source->kind != sourceKind ||
              source->firstSeen != projectile.sourceFirstSeen)) ||
            (source == state.self.units.end() && !projectile.sourceLifetimeVerified)) continue;
        UnitSnapshot attacker;
        attacker.kind = sourceKind;
        attacker.groundWeapon = projectile.groundWeapon;
        attacker.airWeapon = projectile.airWeapon;
        // A projectile is a hit, not an entire multi-hit volley.
        attacker.groundWeapon.hits = 1;
        attacker.airWeapon.hits = 1;
        const auto remaining = std::max(0.0, target->durability() - target->incomingDamage);
        if (remaining > 0.0) {
            target->incomingDamage += std::min(remaining, attackDamage(attacker, *target, remaining));
        }
    }
}

FightValuation valueFightMission(const FightMission mission,
                                 const double plannedRequiredRatio) noexcept {
    const auto baseline = std::clamp(plannedRequiredRatio, 0.75, 2.5);
    switch (mission) {
        case FightMission::lastBaseDefense:
            return {std::max(0.75, baseline * 0.82),
                    "Last base defense: accept a near-even trade to keep the final economy alive"};
        case FightMission::preserveValuable:
            return {std::min(3.0, baseline * 1.25),
                    "Preserve valuable units: require a larger margin around high-investment assets"};
        case FightMission::harassment:
            return {std::max(baseline, 1.50),
                    "Harassment: require a clear local edge before risking an isolated raiding force"};
        case FightMission::delay:
            return {std::max(0.75, baseline * 0.92),
                    "Delay: accept a controlled near-even trade to buy time at a threatened base"};
        case FightMission::cleanup:
            return {std::max(0.75, baseline * 0.85),
                    "Cleanup: finish a small low-threat remainder at a modestly reduced margin"};
        case FightMission::advance:
        default:
            return {baseline,
                    "Advance: require the planned combat margin before committing the field force"};
    }
}

std::string_view fightMissionName(const FightMission mission) noexcept {
    switch (mission) {
        case FightMission::lastBaseDefense: return "last-base-defense";
        case FightMission::preserveValuable: return "preserve-valuable";
        case FightMission::harassment: return "harassment";
        case FightMission::delay: return "delay";
        case FightMission::cleanup: return "cleanup";
        case FightMission::advance:
        default: return "advance";
    }
}

CombatEstimate CombatEvaluator::evaluate(
    const std::span<const UnitSnapshot> friendly,
    std::span<const UnitSnapshot> enemy,
    const double requiredRatio,
    const double uncertainty,
    const bool runSimulation,
    const NavigationGrid* navigation) const {
    // BWAPI reports zero HP/shields when enemy detection access is unavailable.
    // Treat that sentinel as unknown health, not a nearly dead attacker. Keep
    // the conservative estimate local so observations and targetability remain
    // unchanged, and static power, simulation selection and damage all agree.
    const auto unknownHealth = [](const UnitSnapshot& unit) {
        return !unit.ours && !unit.detected && unit.hitPoints == 0 && unit.shields == 0;
    };
    std::vector<UnitSnapshot> estimatedEnemy;
    if (std::ranges::any_of(enemy, unknownHealth)) {
        estimatedEnemy.assign(enemy.begin(), enemy.end());
        for (auto& unit : estimatedEnemy) {
            if (!unknownHealth(unit)) continue;
            unit.hitPoints = unit.maxHitPoints;
            unit.shields = unit.maxShields;
        }
        enemy = estimatedEnemy;
    }
    CombatEstimate result;
    for (const auto& unit : friendly) {
        result.friendlyPower += unitPower(unit, enemy);
    }
    for (const auto& unit : enemy) {
        result.enemyPower += unitPower(unit, friendly);
    }

    const auto rawFriendlyPower = result.friendlyPower;
    const auto rawEnemyPower = result.enemyPower;
    const auto rawStaticRatio = rawFriendlyPower / std::max(0.1, rawEnemyPower);
    auto simulation = SimulationOutcome{};
    if (runSimulation) {
        simulation = simulateEngagement(friendly, enemy, navigation);
    }
    if (!runSimulation || simulation.routingDeferred) {
        const auto routingDeferred = simulation.routingDeferred;
        simulation = {};
        simulation.routingDeferred = routingDeferred;
        simulation.friendlyInitial = rawFriendlyPower;
        simulation.enemyInitial = rawEnemyPower;
        simulation.friendlyRemaining = rawFriendlyPower;
        simulation.enemyRemaining = rawEnemyPower;
        SimulationSelection friendlySelection;
        SimulationSelection enemySelection;
        simulationUnits(friendly, friendlySelection);
        simulationUnits(enemy, enemySelection);
        simulation.omittedFriendly = friendlySelection.omitted;
        simulation.omittedEnemy = enemySelection.omitted;
        simulation.coverage = 0.65 * std::min(
            selectionConfidenceCoverage(friendlySelection, enemy),
            selectionConfidenceCoverage(enemySelection, friendly));
    }
    result.simulatedFriendlyRemaining = simulation.friendlyRemaining;
    result.simulatedEnemyRemaining = simulation.enemyRemaining;
    result.simulatedFriendlyLoss = std::max(0.0, simulation.friendlyInitial - simulation.friendlyRemaining);
    result.simulatedEnemyLoss = std::max(0.0, simulation.enemyInitial - simulation.enemyRemaining);
    result.simulatedFrames = simulation.frames;
    result.simulatedFriendlyDeaths = simulation.friendlyDeaths;
    result.simulatedEnemyDeaths = simulation.enemyDeaths;
    result.simulatedOutcomeReached = simulation.outcomeReached;
    result.simulationRoutingDeferred = simulation.routingDeferred;
    result.omittedFriendly = simulation.omittedFriendly;
    result.omittedEnemy = simulation.omittedEnemy;

    const auto friendlySurvival = simulation.friendlyInitial > 0.0
                                      ? simulation.friendlyRemaining /
                                            simulation.friendlyInitial
                                      : (friendly.empty() ? 0.0 : 1.0);
    const auto enemySurvival = simulation.enemyInitial > 0.0
                                   ? simulation.enemyRemaining /
                                         simulation.enemyInitial
                                   : (enemy.empty() ? 0.0 : 1.0);
    // Blend the fast legal-weapon estimate with the bounded local simulation.
    // Unsupported spell effects are omitted; the simulated survival component
    // captures range, approach time, focus fire, armor, damage type, cooldowns,
    // and simultaneous volleys.
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
    constexpr auto minimumConfidenceForCommitment = 0.80;
    const auto decisiveAdvantage = requiredRatio * 1.25;
    if (rawEnemyPower > 0.0 && result.confidence < minimumConfidenceForCommitment) {
        if (result.decision == FightDecision::engage && result.ratio < decisiveAdvantage) {
            // Probe/reassess when the estimate is close enough that omitted
            // units or a skipped simulation could reverse the commitment.
            result.decision = FightDecision::kite;
            result.confidenceStaged = true;
        } else if (result.decision == FightDecision::retreat &&
                   rawStaticRatio >= decisiveAdvantage) {
            // Low coverage cannot turn a strong legal weapon advantage into a
            // hard retreat. Keep the force probing at range while evidence
            // improves; the tracker can promote it after stable contact.
            result.decision = FightDecision::kite;
            result.confidenceStaged = true;
        }
    }
    return result;
}

std::uint64_t EngagementTracker::identify(const std::span<const UnitSnapshot> members, const Frame frame) {
    std::erase_if(groups_, [frame](const Group& group) {
        return frame < group.lastSeen || frame - group.lastSeen > 10 * 24;
    });
    for (const auto& group : groups_) {
        const auto exact = group.lastSeen == frame && group.members.size() == members.size() &&
            std::ranges::all_of(members, [&group](const UnitSnapshot& unit) {
                return std::ranges::find(group.members, unit.id) != group.members.end();
            });
        if (exact) return group.key;
    }
    Group* best = nullptr;
    std::size_t bestOverlap = 0;
    for (auto& group : groups_) {
        if (group.lastSeen == frame) continue; // A split child gets a cloned record under a new key below.
        const auto overlap = static_cast<std::size_t>(std::ranges::count_if(members, [&group](const UnitSnapshot& unit) {
            return std::ranges::find(group.members, unit.id) != group.members.end();
        }));
        if (overlap > bestOverlap && overlap * 2 >= members.size() && overlap * 2 >= group.members.size()) {
            best = &group;
            bestOverlap = overlap;
        }
    }
    if (!best) {
        Group* splitSource = nullptr;
        std::size_t splitOverlap = 0;
        for (auto& group : groups_) {
            if (group.splitSourceFrame != frame || group.splitSourceMembers.empty()) continue;
            const auto overlap = static_cast<std::size_t>(std::ranges::count_if(
                members, [&group](const UnitSnapshot& unit) {
                    return std::ranges::find(group.splitSourceMembers, unit.id) !=
                           group.splitSourceMembers.end();
                }));
            if (overlap > splitOverlap) {
                splitSource = &group;
                splitOverlap = overlap;
            }
        }

        Group child;
        child.key = nextKey_++;
        child.lastSeen = frame;
        for (const auto& unit : members) child.members.push_back(unit.id);
        std::optional<Memory> inherited;
        if (splitSource != nullptr && splitOverlap > 0) {
            child.splitSourceMembers = splitSource->splitSourceMembers;
            child.splitSourceFrame = frame;
            if (const auto memory = memory_.find(splitSource->key); memory != memory_.end()) {
                inherited = memory->second;
                inherited->lastSeen = frame;
            }
        }
        const auto key = child.key;
        groups_.push_back(std::move(child));
        if (inherited.has_value()) memory_.insert_or_assign(key, std::move(*inherited));
        return key;
    }
    if (best->lastSeen != frame) {
        best->splitSourceMembers = best->members;
        best->splitSourceFrame = frame;
    }
    best->members.clear();
    for (const auto& unit : members) best->members.push_back(unit.id);
    best->lastSeen = frame;
    return best->key;
}

FightDecision EngagementTracker::stabilize(
    const std::uint64_t squadSignature,
    const FightDecision proposed,
    const double ratio,
    const double requiredRatio,
    const Frame frame,
    const bool contact,
    const EngagementContext context) {
    constexpr auto staleFrames = 10 * 24;
    if (memory_.size() > 128U) {
        std::erase_if(memory_, [frame](const auto& entry) {
            return frame - entry.second.lastSeen > staleFrames;
        });
    }
    const auto updateContext = [frame, &context](Memory& memory) {
        const auto missionChanged = memory.hasMission && memory.mission != context.mission;
        memory.hasMission = true;
        memory.mission = context.mission;
        const auto detectorLost = memory.hasDetectionState && memory.detectionReady &&
            context.needsDetection && !context.detectionReady;
        const auto detectorArrived = memory.hasDetectionState && !memory.detectionReady &&
            context.needsDetection && context.detectionReady;
        memory.hasDetectionState = true;
        memory.detectionReady = !context.needsDetection || context.detectionReady;

        const auto recent = [frame](const EnemyProfile& enemy) {
            return frame >= enemy.lastSeen && frame - enemy.lastSeen <= 96;
        };
        auto previousCount = std::size_t{};
        auto previousValue = 0.0;
        for (const auto& enemy : memory.enemies) {
            if (!recent(enemy)) continue;
            ++previousCount;
            previousValue += enemy.combatValue;
        }
        std::erase_if(memory.enemies, [&recent](const EnemyProfile& enemy) {
            return !recent(enemy);
        });
        memory.enemies.reserve(memory.enemies.size() + context.enemies.size());
        auto addedCount = std::size_t{};
        auto addedValue = 0.0;
        auto newSiegePosition = false;
        for (const auto& enemy : context.enemies) {
            if (!enemy.visible || !enemy.detected || !enemy.completed || enemy.hallucination ||
                (!isCombatUnit(enemy.kind) && !isStaticDefense(enemy.kind))) continue;
            const auto old = std::ranges::find(memory.enemies, enemy.id, &EnemyProfile::id);
            const auto value = unitStats(enemy.kind).combatValue;
            const auto sieged = enemy.kind == UnitKind::siegeTank &&
                                enemy.groundWeapon.maxRange >= 320;
            if (old == memory.enemies.end()) {
                ++addedCount;
                addedValue += value;
                memory.enemies.push_back({enemy.id, enemy.kind, enemy.position, value,
                                          sieged, frame});
                if (sieged) newSiegePosition = true;
                continue;
            }
            if (value > old->combatValue) {
                addedValue += value - old->combatValue;
                if (value - old->combatValue >= std::max(0.75, old->combatValue * 0.5))
                    ++addedCount;
            }
            if (sieged && (!old->siegePosition ||
                (old->position.valid() && enemy.position.valid() &&
                 distanceSquared(old->position, enemy.position) > 96 * 96))) {
                newSiegePosition = true;
            }
            old->kind = enemy.kind;
            old->position = enemy.position;
            old->combatValue = value;
            old->siegePosition = sieged;
            old->lastSeen = frame;
        }
        const auto requiredNewCount = std::max<std::size_t>(2, (previousCount + 1) / 2);
        const auto majorReinforcement =
            (addedCount >= requiredNewCount &&
             addedValue >= std::max(0.75, previousValue * 0.5)) ||
            addedValue >= std::max(2.8, previousValue * 0.75);
        return std::array{detectorLost, detectorArrived, majorReinforcement,
                          newSiegePosition, missionChanged};
    };

    auto found = memory_.find(squadSignature);
    if (found == memory_.end() || frame < found->second.lastSeen || frame - found->second.lastSeen > staleFrames) {
        auto initial = proposed;
        if (context.needsDetection && !context.detectionReady &&
            initial == FightDecision::engage) initial = FightDecision::kite;
        Memory memory{initial, initial, frame, frame, frame, contact ? frame : -1};
        memory.hasMission = true;
        memory.mission = context.mission;
        static_cast<void>(updateContext(memory));
        memory_.insert_or_assign(squadSignature, std::move(memory));
        return initial;
    }

    auto& memory = found->second;
    memory.lastSeen = frame;
    if (contact) memory.lastContact = frame;
    const auto [detectorLost, detectorArrived, majorReinforcement,
                newSiegePosition, missionChanged] = updateContext(memory);
    const auto materialChange = detectorLost || detectorArrived || majorReinforcement ||
                                newSiegePosition || context.retreatRouteFailed ||
                                (missionChanged && contact);
    if (materialChange) {
        auto immediate = proposed;
        if (context.needsDetection && !context.detectionReady &&
            immediate == FightDecision::engage) immediate = FightDecision::kite;
        memory.decision = immediate;
        memory.candidate = immediate;
        memory.candidateSince = frame;
        memory.changedAt = frame;
        return immediate;
    }
    if (context.needsDetection && !context.detectionReady &&
        (memory.decision == FightDecision::engage ||
         proposed == FightDecision::engage)) {
        memory.decision = proposed == FightDecision::retreat
                              ? FightDecision::retreat
                              : FightDecision::kite;
        memory.candidate = memory.decision;
        memory.candidateSince = frame;
        memory.changedAt = frame;
        return memory.decision;
    }
    // Enemies falling out of the local combat radius is not evidence that a
    // retreat succeeded. Finish regrouping before travelling back into range.
    if (!contact && memory.decision != FightDecision::engage && frame - memory.lastContact < 72)
        return memory.decision;
    if (proposed == memory.decision) {
        memory.candidate = proposed;
        memory.candidateSince = frame;
        return memory.decision;
    }

    // New overwhelming danger always breaks commitment. An apparently easy
    // fight cannot bypass the regroup interval and send stragglers back in.
    if (contact && proposed == FightDecision::retreat && ratio < requiredRatio * 0.70) {
        memory.decision = proposed;
        memory.candidate = proposed;
        memory.candidateSince = memory.changedAt = frame;
        return memory.decision;
    }

    if (memory.candidate != proposed) {
        memory.candidate = proposed;
        memory.candidateSince = frame;
    }
    const auto commitment = memory.decision == FightDecision::engage ? 48 : 72;
    const auto evidenceFrames = proposed == FightDecision::engage ? 48 : 24;
    const auto recoveryMargin = !contact || proposed != FightDecision::engage || ratio >= requiredRatio * 1.08;
    if (!recoveryMargin) memory.candidateSince = frame;
    if (recoveryMargin && frame - memory.changedAt >= commitment && frame - memory.candidateSince >= evidenceFrames) {
        memory.decision = proposed;
        memory.changedAt = frame;
    }
    return memory.decision;
}

void EngagementTracker::reset() {
    memory_.clear();
    groups_.clear();
    nextKey_ = 1;
}

void EngagementTracker::forgetUnit(const UnitId id) {
    if (id < 0) return;
    for (auto group = groups_.begin(); group != groups_.end();) {
        std::erase(group->members, id);
        std::erase(group->splitSourceMembers, id);
        if (group->members.empty()) {
            memory_.erase(group->key);
            group = groups_.erase(group);
        } else {
            ++group;
        }
    }
}

const UnitSnapshot* CombatEvaluator::selectTarget(
    const UnitSnapshot& attacker,
    const std::span<const UnitSnapshot> candidates,
    const std::span<const TargetAllocation> allocations,
    const TacticalTargetModel* targetModel) const {
    const UnitSnapshot* best = nullptr;
    auto bestScore = -std::numeric_limits<double>::infinity();
    const UnitSnapshot* heuristicBest = nullptr;
    auto heuristicBestScore = -std::numeric_limits<double>::infinity();
    const auto meleeAttacker = attacker.groundWeapon.targetsGround &&
                               attacker.groundWeapon.maxRange < 96;
    const auto hasCloseMeleeTarget = meleeAttacker && std::ranges::any_of(
        candidates, [&attacker, allocations](const UnitSnapshot& target) {
            const auto reserved = std::ranges::find(allocations, target.id, &TargetAllocation::target);
            const auto committed = reserved == allocations.end() ? 0 : reserved->committedDamage;
            return target.visible && target.detected && !target.flying &&
                   !target.loaded && !target.invincible && !target.hallucination &&
                   attacker.canAttack(target) &&
                   target.durability() > target.incomingDamage + committed &&
                   weaponDistance(attacker, target) <= 224.0;
        });
    const auto hasShot = std::ranges::any_of(candidates,
        [&attacker, allocations](const UnitSnapshot& target) {
            const auto reserved = std::ranges::find(allocations, target.id, &TargetAllocation::target);
            const auto committed = reserved == allocations.end() ? 0 : reserved->committedDamage;
            const auto& weapon = target.flying ? attacker.airWeapon : attacker.groundWeapon;
            const auto separation = weaponDistance(attacker, target);
            return target.visible && target.detected && !target.loaded && !target.invincible && !target.hallucination &&
                   attacker.canAttack(target) && target.durability() > target.incomingDamage + committed &&
                   separation >= weapon.minRange && separation <= weapon.maxRange;
        });
    for (const auto& target : candidates) {
        if (!target.visible || !target.detected || target.loaded || target.hallucination || target.invincible ||
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
        // A nearby legal hit is worth more than chasing a wounded unit through
        // its entire army. Zealots likewise fight the blocking front rank.
        if (range < weapon.minRange || (hasShot && range > weapon.maxRange)) continue;
        const auto splashTargets = attacker.kind == UnitKind::reaver
                                       ? std::ranges::count_if(
                                             candidates,
                                             [&target](const UnitSnapshot& candidate) {
                                                 return !candidate.flying && candidate.visible &&
                                                        !candidate.loaded && !candidate.invincible &&
                                                        !candidate.hallucination &&
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
        const auto killEfficiency = std::min(1.0,
            modeledAttackDamage(attacker, target, remainingHealth) / effectiveHealth);
        const auto inRange = range <= weapon.maxRange + 16 ? 2.0 : 0.0;
        const auto targetStability = target.id == attacker.orderTargetId &&
                                             range <= weapon.maxRange + 256
                                         ? 1.5
                                         : 0.0;
        const auto distanceDivisor = meleeAttacker ? 112.0 : 320.0;
        const auto approachFrames = std::max(0.0, range - weapon.maxRange) /
                                    std::max(0.5, attacker.topSpeed);
        const auto heuristicScore = priority + killEfficiency * 4.0 + inRange + targetStability -
                                    range / distanceDivisor -
                                    approachFrames / std::max(12, weapon.cooldown) * 4.0;
        if (targetModel != nullptr && targetModel->valid() &&
            (heuristicScore > heuristicBestScore ||
             (std::abs(heuristicScore - heuristicBestScore) < 0.001 &&
              (heuristicBest == nullptr || target.id < heuristicBest->id)))) {
            heuristicBest = &target;
            heuristicBestScore = heuristicScore;
        }
        const auto learned = targetModel != nullptr && targetModel->valid();
        const auto score = learned
            ? static_cast<double>(targetModel->score(attacker, target, candidates)) : heuristicScore;
        if (score > bestScore || (std::abs(score - bestScore) < 0.001 &&
                                  (best == nullptr || target.id < best->id))) {
            best = &target;
            bestScore = score;
        }
    }
    if (best == nullptr && targetModel != nullptr && targetModel->valid())
        return selectTarget(attacker, candidates, allocations, nullptr);
    if (best != nullptr && heuristicBest != nullptr && targetModel != nullptr &&
        targetModel->valid())
        targetModel->observeComparison(attacker, *best, *heuristicBest);
    return best;
}

double CombatEvaluator::unitPower(
    const UnitSnapshot& unit,
    const std::span<const UnitSnapshot> opposition) {
    if (!combatReady(unit) ||
        (!isCombatUnit(unit.kind) && !isStaticDefense(unit.kind) && !isWorker(unit.kind))) {
        return 0.0;
    }
    if ((unit.kind == UnitKind::reaver || unit.kind == UnitKind::carrier) &&
        unit.ammo <= 0) {
        // Potential future ammunition still has to be able to hit this squad.
        // Otherwise an empty Reaver becomes an anti-air threat while a loaded
        // one correctly contributes zero power against an all-flying force.
        const auto compatible = opposition.empty() || std::ranges::any_of(
            opposition, [&unit](const UnitSnapshot& target) {
                const auto& weapon = target.flying ? unit.airWeapon : unit.groundWeapon;
                return !target.invincible && !target.loaded && weapon.damage > 0 &&
                    (target.flying ? weapon.targetsAir : weapon.targetsGround);
            });
        return compatible ? unitStats(unit.kind).combatValue * 0.12 : 0.0;
    }
    const auto usefulTarget = [&unit](const UnitSnapshot& target) {
        if (target.invincible || target.loaded ||
            (unit.ours && !target.detected && target.requiresDetection()) ||
            !unit.canAttack(target)) return false;
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
        // Spell value is applied by TacticalController only when an ability,
        // energy, target geometry and friendly-fire checks are available.
        // A spellcaster's unit cost alone is not evidence of usable damage.
        return 0.0;
    }

    const auto& weapon = airUseful ? unit.airWeapon : unit.groundWeapon;
    if (weapon.damage <= 0) return 0.0;
    // BWAPI's one-frame suicide cooldown does not mean another impact every
    // frame. Spread its single payload over this estimate's fight horizon.
    const auto period = suicideAttacker(unit.kind) ? simulationHorizonFrames :
                                                    std::max(1, weapon.cooldown);
    auto ammunitionRetention = 1.0;
    if (unit.kind == UnitKind::reaver) {
        ammunitionRetention = std::clamp(static_cast<double>(unit.ammo) / 2.0, 0.0, 1.0);
        if (std::ranges::any_of(opposition, [](const UnitSnapshot& target) {
                return !target.flying && !isBuilding(target.kind) && target.topSpeed > 0.05;
            })) {
            ammunitionRetention *= movingScarabImpactRetention;
        }
    }
    const auto dps = static_cast<double>(weapon.damage * std::max(1, weapon.hits)) /
                     period * ammunitionRetention;
    const auto rangeFactor = 1.0 + std::clamp(weapon.maxRange / 256.0, 0.0, 1.0) * 0.35;
    const auto mobility = 1.0 + std::clamp(unit.topSpeed / 8.0, 0.0, 1.0) * 0.2;
    const auto vitality = std::clamp(unit.healthFraction(), 0.08, 1.0);
    return (unitStats(unit.kind).combatValue + dps * 0.45) * rangeFactor * mobility * vitality;
}

std::vector<Command> TacticalController::recharge(
    const std::span<const UnitSnapshot> friendly, const bool defending) const {
    std::vector<Command> commands;
    for (const auto& unit : friendly) {
        const auto supportUnit = unit.kind == UnitKind::observer || unit.kind == UnitKind::shuttle ||
            unit.kind == UnitKind::reaver || unit.kind == UnitKind::highTemplar ||
            unit.kind == UnitKind::darkArchon || unit.kind == UnitKind::arbiter;
        const auto canRecharge = (isCombatUnit(unit.kind) &&
                                  (defending || supportUnit || unit.recharging)) ||
                                 (supportUnit && !isCombatUnit(unit.kind)) ||
                                 (defending && isWorker(unit.kind));
        if (!canRecharge ||
            !combatReady(unit) || unit.maxShields <= 0 || unit.shields >= unit.maxShields ||
            unit.attackFrame || unit.attackWindup || unit.underStorm || unit.underAttack) continue;
        const UnitSnapshot* battery = nullptr;
        auto bestDistance = 256 * 256 + 1;
        for (const auto& candidate : friendly) {
            if (candidate.kind != UnitKind::shieldBattery || !combatReady(candidate) ||
                !candidate.powered || candidate.energy < 10 ||
                !candidate.position.valid()) continue;
            if (unit.recharging && unit.orderTargetId == candidate.id) {
                battery = &candidate;
                break;
            }
        }
        if (battery == nullptr &&
            (!unit.recharging || unit.shields * 5 < unit.maxShields * 2)) {
            for (const auto& candidate : friendly) {
                if (candidate.kind != UnitKind::shieldBattery || !combatReady(candidate) ||
                    !candidate.powered || candidate.energy < 10 ||
                    !candidate.position.valid()) continue;
                const auto candidateDistance = distanceSquared(unit.position, candidate.position);
                if (candidateDistance >= bestDistance) continue;
                bestDistance = candidateDistance;
                battery = &candidate;
            }
        }
        if (battery != nullptr) {
            // Keep an existing, safe recharge mission above ordinary combat
            // orders until full shields. Explicit storm escape and extraction
            // remain higher priority, while fragile units can still retreat.
            const auto activeMission = unit.recharging;
            const auto priority = activeMission ? 104 :
                unit.healthFraction() < 0.28 ? 101 : 92;
            commands.push_back({unit.id, CommandType::recharge, battery->id, {-1, -1},
                                UnitKind::shieldBattery, priority, 0,
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
    const DefenseArea defense, const TacticalIntent intent,
    const std::span<const UnitSnapshot> support,
    const NavigationGrid* navigation,
    const std::span<const UnitSnapshot> obstacles,
    const TacticalTargetModel* targetModel,
    const bool detectorWaitVolley,
    const std::span<const Position> reservedStormZones,
    const Position stagingGoal) const {
    std::vector<Command> commands;
    commands.reserve(friendly.size());
    CombatEvaluator evaluator;
    std::vector<TargetAllocation> allocations;
    allocations.reserve(enemy.size());
    std::vector<Position> plannedStorms(reservedStormZones.begin(), reservedStormZones.end());
    // Live squads contain only their own members. The support and obstacle
    // snapshots also contain allies (including workers) that Storm can hurt.
    // Build one union only when this squad has a caster ready to use it.
    std::vector<const UnitSnapshot*> stormAllies;
    std::vector<UnitSnapshot> stormAllySnapshots;
    if (psionicStormAvailable && std::ranges::any_of(friendly, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::highTemplar &&
                   unit.energy >= psionicStormEnergyCost && combatReady(unit);
        })) {
        for (const auto allies : {friendly, support, obstacles}) {
            for (const auto& ally : allies) {
                if (ally.loaded || ally.invincible || ally.hallucination ||
                    isBuilding(ally.kind) || !ally.position.valid()) continue;
                stormAllies.push_back(&ally);
            }
        }
        std::ranges::stable_sort(stormAllies, {}, [](const UnitSnapshot* ally) { return ally->id; });
        const auto duplicates = std::ranges::unique(stormAllies, {},
            [](const UnitSnapshot* ally) { return ally->id; });
        stormAllies.erase(duplicates.begin(), duplicates.end());
        stormAllySnapshots.reserve(stormAllies.size());
        for (const auto* ally : stormAllies) stormAllySnapshots.push_back(*ally);
    }
    const auto nearbyArmy = support.empty() ? friendly : support;

    // Routing can replace the mission with a short centroid waypoint. That
    // waypoint crossing the leash must not alternate idle defenders between
    // a forward screen and home. Stage toward the actual mission, inside the
    // defensive ring/terrain boundary; target and emergency micro stay above
    // this fallback in the per-unit decision order.
    auto screenAnchor = objective.valid() && defense.contains(objective)
        ? objective : defense.center;
    if (defense.active() && stagingGoal.valid()) {
        screenAnchor = stagingGoal;
        if (!defense.contains(screenAnchor)) {
            auto limit = static_cast<double>(std::max(0, defense.pursuitRadius - 32));
            auto candidate = moveToward(defense.center, stagingGoal, limit);
            if (!defense.contains(candidate)) {
                // A forward terrain plane can be tighter than the radial
                // leash. Clip the same segment, without crossing that plane.
                auto lower = 0.0;
                auto upper = limit;
                for (int iteration = 0; iteration < 16; ++iteration) {
                    const auto middle = (lower + upper) * 0.5;
                    if (defense.contains(moveToward(defense.center, stagingGoal, middle)))
                        lower = middle;
                    else
                        upper = middle;
                }
                candidate = moveToward(defense.center, stagingGoal, std::max(0.0, lower - 32));
            }
            screenAnchor = defense.contains(candidate) ? candidate : defense.center;
        }
    }

    // A single 96px hold disk cannot accommodate a late-game ground army.
    // Stable, separated staging positions keep the front rank from blocking
    // every arriving unit and give each unit its own destination while clear.
    std::vector<Position> screenSlots;
    std::vector<UnitId> screenMembers;
    // Keep stable screen slots when the squad sees irrelevant enemies too.
    // Collapsing every unit onto the shared anchor whenever any enemy appears
    // makes an otherwise-safe army fan out and regroup at combat-tick speed.
    if (defense.active() && friendly.size() >= 6) {
        const auto anchor = screenAnchor;
        for (int row = -5; row <= 5; ++row) {
            for (int column = -5; column <= 5; ++column) {
                const Position candidate{anchor.x + column * 64, anchor.y + row * 64};
                const auto reservedNexus = defense.economyCenter.valid() &&
                    std::abs(candidate.x - defense.economyCenter.x) <= 112 &&
                    std::abs(candidate.y - defense.economyCenter.y) <= 96;
                const auto occupied = std::ranges::any_of(obstacles, [candidate](const UnitSnapshot& obstacle) {
                    return isBuilding(obstacle.kind) && obstacle.position.valid() &&
                        candidate.x >= obstacle.position.x - obstacle.dimensionLeft - 32 &&
                        candidate.x <= obstacle.position.x + obstacle.dimensionRight + 32 &&
                        candidate.y >= obstacle.position.y - obstacle.dimensionUp - 32 &&
                        candidate.y <= obstacle.position.y + obstacle.dimensionDown + 32;
                });
                if (!candidate.valid() || reservedNexus || occupied || !defense.contains(candidate) ||
                    (navigation != nullptr && !navigation->empty() &&
                     !navigation->lineWalkable(anchor, candidate))) continue;
                screenSlots.push_back(candidate);
            }
        }
        std::ranges::sort(screenSlots, [anchor](const Position a, const Position b) {
            const auto first = distanceSquared(a, anchor);
            const auto second = distanceSquared(b, anchor);
            if (first != second) return first < second;
            return a.y != b.y ? a.y < b.y : a.x < b.x;
        });
        for (const auto& member : friendly)
            if (!member.flying && combatReady(member)) screenMembers.push_back(member.id);
        std::ranges::sort(screenMembers);
    }

    std::unordered_map<UnitId, std::vector<Position>> meleeContactReservations;
    const auto chooseMeleeContact = [&](const UnitSnapshot& unit,
                                       const UnitSnapshot& target) {
        if (navigation == nullptr || navigation->empty() || unit.flying || target.flying ||
            !unit.position.valid() || !target.position.valid()) return Position{-1, -1};
        const auto footprint = movementFootprint(unit);
        auto& reserved = meleeContactReservations[target.id];
        if (reserved.empty()) {
            for (const auto& ally : nearbyArmy) {
                if (ally.id == unit.id || ally.flying || ally.loaded ||
                    ally.groundWeapon.maxRange >= 96 || !ally.canAttack(target) ||
                    !ally.position.valid()) continue;
                if (weaponDistance(ally, target) <= ally.groundWeapon.maxRange + 4)
                    reserved.push_back(ally.position);
            }
        }

        constexpr std::array<Position, 16> spokes{{
            {1, 0}, {2, 1}, {1, 1}, {1, 2}, {0, 1}, {-1, 2}, {-1, 1}, {-2, 1},
            {-1, 0}, {-2, -1}, {-1, -1}, {-1, -2}, {0, -1}, {1, -2}, {1, -1}, {2, -1},
        }};
        const auto unitExtent = std::max({unit.dimensionLeft, unit.dimensionRight,
                                          unit.dimensionUp, unit.dimensionDown});
        const auto targetExtent = std::max({target.dimensionLeft, target.dimensionRight,
                                            target.dimensionUp, target.dimensionDown});
        const auto radius = std::max(8.0, unit.groundWeapon.maxRange + unitExtent +
                                           targetExtent - 4.0);
        auto best = Position{-1, -1};
        auto bestScore = std::numeric_limits<double>::infinity();
        for (const auto spoke : spokes) {
            const auto length = std::hypot(static_cast<double>(spoke.x),
                                           static_cast<double>(spoke.y));
            const Position candidate{
                target.position.x + static_cast<int>(std::lround(spoke.x * radius / length)),
                target.position.y + static_cast<int>(std::lround(spoke.y * radius / length)),
            };
            if (candidate.x < 0 || candidate.y < 0 ||
                candidate.x >= navigation->width() * navigation->cellSize() ||
                candidate.y >= navigation->height() * navigation->cellSize() ||
                !navigation->walkable(candidate, footprint)) continue;
            auto firingPosition = unit;
            firingPosition.position = candidate;
            if (weaponDistance(firingPosition, target) > unit.groundWeapon.maxRange) continue;
            const auto overlapsBuilding = std::ranges::any_of(obstacles,
                [candidate](const UnitSnapshot& obstacle) {
                    return isBuilding(obstacle.kind) && obstacle.position.valid() &&
                        candidate.x >= obstacle.position.x - obstacle.dimensionLeft - 24 &&
                        candidate.x <= obstacle.position.x + obstacle.dimensionRight + 24 &&
                        candidate.y >= obstacle.position.y - obstacle.dimensionUp - 24 &&
                        candidate.y <= obstacle.position.y + obstacle.dimensionDown + 24;
                });
            if (overlapsBuilding) continue;

            if (std::ranges::any_of(reserved, [candidate](const Position other) {
                    return distanceSquared(candidate, other) < 40 * 40;
                })) continue;
            auto crowding = 0.0;
            for (const auto& ally : nearbyArmy) {
                if (ally.id == unit.id || ally.flying || ally.loaded ||
                    !ally.position.valid()) continue;
                auto destination = ally.position;
                const auto order = std::ranges::find(commands, ally.id, &Command::actor);
                if (order != commands.end() && order->source == "melee-contact")
                    destination = order->targetPosition;
                crowding += std::max(0.0, 48.0 - distance(candidate, destination));
            }
            // The direct distance is a lower bound on the validated route.
            if (distance(unit.position, candidate) + crowding * 1.5 > bestScore) continue;

            auto routeOrigin = unit.position;
            auto routeOriginValid = navigation->walkable(routeOrigin, footprint);
            if (!routeOriginValid) {
                const auto cellSize = navigation->cellSize();
                for (auto stepDistance = 8; stepDistance <= cellSize * 2; stepDistance += 8) {
                    const auto next = moveToward(unit.position, candidate, stepDistance);
                    if (next.x / cellSize != unit.position.x / cellSize ||
                        next.y / cellSize != unit.position.y / cellSize) {
                        routeOrigin = next;
                        routeOriginValid = navigation->walkable(routeOrigin, footprint);
                        break;
                    }
                }
            }
            if (!routeOriginValid) continue;
            auto routeLength = distance(unit.position, candidate);
            if (!navigation->lineWalkable(unit.position, candidate, footprint)) {
                const auto path = navigation->findPath(routeOrigin, candidate, 4096, footprint);
                if (!path.reached()) continue;
                routeLength = distance(unit.position, routeOrigin);
                for (auto point = path.points.begin() + 1; point != path.points.end(); ++point)
                    routeLength += distance(*(point - 1), *point);
                routeLength += distance(path.points.back(), candidate);
            }
            const auto score = routeLength + crowding * 1.5;
            if (score < bestScore || (score == bestScore &&
                (candidate.y < best.y || (candidate.y == best.y && candidate.x < best.x)))) {
                bestScore = score;
                best = candidate;
            }
        }
        if (best.valid()) reserved.push_back(best);
        return best;
    };

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
        auto unitRetreatPoint = retreatPoint;
        if (!unitRetreatPoint.valid()) {
            const UnitSnapshot* nearestNexus = nullptr;
            auto nearestDistance = std::numeric_limits<int>::max();
            for (const auto& ally : obstacles) {
                if (ally.kind != UnitKind::nexus || !ally.position.valid()) continue;
                const auto candidateDistance = distanceSquared(unit.position, ally.position);
                if (candidateDistance < nearestDistance) {
                    nearestDistance = candidateDistance;
                    nearestNexus = &ally;
                }
            }
            unitRetreatPoint = nearestNexus != nullptr ? nearestNexus->position : unit.position;
        }
        const auto local = influence.at(unit.position);
        const auto localThreat = unit.flying ? local.airThreat : local.groundThreat;
        const auto protectedStep = [&defense](const Position proposed) {
            return defense.front.valid() && !defense.contains(proposed) ? defense.center : proposed;
        };
        const auto reachableStep = [&unit, navigation](const Position candidate) {
            if (!candidate.valid()) return false;
            if (navigation == nullptr || navigation->empty()) return true;
            if (candidate.x >= navigation->width() * navigation->cellSize() ||
                candidate.y >= navigation->height() * navigation->cellSize()) return false;
            if (unit.flying) return true;
            auto origin = unit.position;
            // A legally observed unit can stand on the passable edge of a
            // rejected coarse cell. Admit that origin only, then check every
            // following cell for retreats, kiting and splash spacing alike.
            if (!navigation->walkable(origin)) {
                const auto cellSize = navigation->cellSize();
                for (auto stepDistance = 8; stepDistance <= cellSize * 2; stepDistance += 8) {
                    const auto next = moveToward(unit.position, candidate, stepDistance);
                    if (next.x / cellSize != unit.position.x / cellSize ||
                        next.y / cellSize != unit.position.y / cellSize) {
                        origin = next;
                        break;
                    }
                }
            }
            return navigation->lineWalkable(origin, candidate, movementFootprint(unit));
        };
        const auto reposition = [&](const Position toward, const bool retreating = false,
                                    const bool avoidDetection = false) {
            // Small combat moves need a walkable segment, not just a safe
            // destination across a cliff. Account for allies already moving
            // this tick so retreating units do not all choose the same tile.
            auto best = unit.position;
            auto bestScore = std::numeric_limits<double>::infinity();
            std::vector<Position> occupied;
            for (const auto& ally : nearbyArmy) {
                if (ally.id == unit.id || ally.flying != unit.flying || ally.loaded ||
                    !ally.position.valid() || distanceSquared(unit.position, ally.position) > 160 * 160) continue;
                const auto order = std::ranges::find(commands, ally.id, &Command::actor);
                occupied.push_back(order != commands.end() && order->type == CommandType::move
                    ? order->targetPosition : ally.position);
            }
            constexpr std::array<Position, 9> steps{{
                {0, 0}, {-64, 0}, {64, 0}, {0, -64}, {0, 64},
                {-45, -45}, {-45, 45}, {45, -45}, {45, 45}}};
            for (const auto step : steps) {
                const Position candidate{unit.position.x + step.x, unit.position.y + step.y};
                if (!reachableStep(candidate)) continue;
                if (!retreating && defense.active() && defense.contains(unit.position) && !defense.contains(candidate)) continue;
                const auto field = influence.at(candidate);
                const auto threat = unit.flying ? field.airThreat : field.groundThreat;
                const auto detectionPenalty = avoidDetection
                    ? static_cast<double>(field.detection) * 6.0 : 0.0;
                auto crowding = 0.0;
                for (const auto destination : occupied) {
                    crowding += std::max(0.0, 48.0 - distance(candidate, destination)) / 48.0;
                }
                const auto score = threat * 5.0 + detectionPenalty + crowding * 0.8 +
                    (distance(candidate, toward) - distance(unit.position, toward)) / 96.0;
                if (score < bestScore) { bestScore = score; best = candidate; }
            }
            return best;
        };
        const auto fragile = unit.healthFraction() < 0.28;
        // Our BWAPI detected flag describes our own vision, not the enemy's.
        // Use observed detection influence and actual incoming attacks to
        // decide whether a Dark Templar can probe a contain. This is a fog-of-
        // war risk estimate: unseen detectors or a future scan can invalidate it.
        const auto covertAdvance = unit.kind == UnitKind::darkTemplar && unit.cloaked &&
                                   !unit.underAttack && !unit.recentlyDamaged && !fragile &&
                                   local.detection <= 0.1F;
        const auto cloakCompromised = unit.kind == UnitKind::darkTemplar && unit.cloaked &&
            (unit.recentlyDamaged || local.detection > 0.1F);
        const auto locallyOverwhelmed = localThreat > 5.0F &&
                                        estimate.decision != FightDecision::engage &&
                                        estimate.ratio < 1.0;

        if (unit.underStorm || influence.stormDanger(unit.position) > 0.0F) {
            const auto escape = unitRetreatPoint.valid() &&
                                        distanceSquared(unit.position, unitRetreatPoint) > 96 * 96
                                    ? unitRetreatPoint
                                    : Position{unit.position.x + 128, unit.position.y};
            commands.push_back({unit.id, CommandType::move, -1,
                                reposition(escape, true),
                                UnitKind::unknown, 110, 0, "storm-escape"});
            continue;
        }

        // BWAPI explicitly warns that issuing an order during an attack frame
        // can interrupt the attack sequence. Let the shot complete instead of
        // producing stutter, cancelled Dragoon volleys, and indecisive melee.
        if (unit.attackFrame || (unit.attackWindup && !fragile && intent != TacticalIntent::withdraw)) continue;

        // Mission extraction is unconditional, including cloaked units and
        // ready volleys. Neither target pursuit nor detector waiting may
        // reverse a recalled detachment back into the mineral line.
        if (intent == TacticalIntent::withdraw) {
            commands.push_back({unit.id, CommandType::move, -1,
                localThreat > 0.05F || (unit.cloaked && local.detection > 0.1F)
                    ? influence.safestStep(unit.position, unitRetreatPoint, unit.flying, unit.cloaked)
                    : unitRetreatPoint,
                UnitKind::unknown, 115, 0, "raid-extract"});
            continue;
        }

        // Apply the mission gate before cloak-preserving advances and target
        // pursuit. A favorable squad stages in place while detection catches
        // up; only a losing fight retreats toward its safe anchor. Air-only
        // harassment remains independent.
        if (estimate.advanceBlocked && !unit.flying) {
            if (detectorWaitVolley && estimate.decision != FightDecision::retreat &&
                intent != TacticalIntent::withdraw &&
                unit.healthFraction() >= 0.28 &&
                unit.weaponCooldown == 0 &&
                ((unit.kind != UnitKind::reaver && unit.kind != UnitKind::carrier) ||
                 unit.ammo > 0)) {
                std::vector<UnitSnapshot> firingTargets;
                for (const auto& candidate : enemy) {
                    if (!candidate.visible || !candidate.detected || candidate.loaded ||
                        candidate.invincible || candidate.hallucination ||
                        !unit.canAttack(candidate)) continue;
                    const auto& weapon = candidate.flying ? unit.airWeapon : unit.groundWeapon;
                    const auto range = weaponDistance(unit, candidate);
                    if (range >= weapon.minRange && range <= weapon.maxRange)
                        firingTargets.push_back(candidate);
                }
                if (const auto* shot = evaluator.selectTarget(
                        unit, firingTargets, allocations, targetModel)) {
                    commands.push_back({unit.id, CommandType::attackUnit, shot->id,
                                        {-1, -1}, UnitKind::unknown, 100, 0,
                                        "detector-wait-volley"});
                    const auto allocation = std::ranges::find(
                        allocations, shot->id, &TargetAllocation::target);
                    const auto committed = allocation == allocations.end() ? 0.0 :
                        allocation->committedDamage;
                    const auto remaining = std::max(
                        0.0, shot->durability() - shot->incomingDamage - committed);
                    const auto damage = std::min(
                        remaining, modeledAttackDamage(unit, *shot, remaining));
                    if (allocation == allocations.end())
                        allocations.push_back({shot->id, damage});
                    else allocation->committedDamage += damage;
                    continue;
                }
            }
            if (estimate.decision == FightDecision::retreat) {
                commands.push_back({unit.id, CommandType::move, -1,
                    influence.safestStep(unit.position, unitRetreatPoint, false, unit.cloaked),
                    UnitKind::unknown, 105, 0, "detection-retreat"});
            } else {
                const auto stagingStep = localThreat > 0.05F
                    ? influence.safestStep(unit.position, unitRetreatPoint, false, unit.cloaked)
                    : unit.position;
                if (stagingStep == unit.position) {
                    commands.push_back({unit.id, CommandType::hold, -1, unit.position,
                        UnitKind::unknown, 100, 0, "wait-for-mobile-detection"});
                } else {
                    commands.push_back({unit.id, CommandType::move, -1, stagingStep,
                        UnitKind::unknown, 100, 0, "wait-for-mobile-detection"});
                }
            }
            continue;
        }

        const auto visibleSiegeThreat = [&unit, &defense](const UnitSnapshot& candidate) {
            return !unit.flying && candidate.visible && candidate.detected &&
                candidate.kind == UnitKind::siegeTank &&
                candidate.groundWeapon.damage > 0 &&
                candidate.groundWeapon.maxRange >= 320 &&
                (weaponDistance(unit, candidate) <= candidate.groundWeapon.maxRange + 32 ||
                 (defense.economyCenter.valid() &&
                  distance(candidate.position, defense.economyCenter) <=
                      candidate.groundWeapon.maxRange + 96));
        };
        const auto shellingDefender = std::ranges::any_of(enemy, visibleSiegeThreat);
        const UnitSnapshot* retreatThreat = nullptr;
        auto retreatPressure = std::numeric_limits<double>::infinity();
        for (const auto& threat : enemy) {
            if (!threat.visible || !combatReady(threat) || threat.invincible || !threat.canAttack(unit)) continue;
            const auto& response = unit.flying ? threat.airWeapon : threat.groundWeapon;
            const auto separation = weaponDistance(unit, threat);
            if (separation < response.minRange || separation > response.maxRange + 128) continue;
            if (separation - response.maxRange < retreatPressure) {
                retreatPressure = separation - response.maxRange;
                retreatThreat = &threat;
            }
        }
        // An attack-unit order follows a kiting opponent indefinitely. Return
        // stragglers to the protected area, unless visible siege artillery is
        // already shelling that unit or the economy. The old early return made
        // Dragoons walk away from a lone Tank on the ramp even after the fight
        // evaluator had accepted the engagement.
        const auto returningDefender = defense.active() && !covertAdvance &&
            !defense.contains(unit.position) && !shellingDefender &&
            estimate.decision != FightDecision::retreat;
        const auto readyDefensiveShot = !fragile && unit.weaponCooldown == 0 &&
            std::ranges::any_of(enemy, [&unit](const UnitSnapshot& candidate) {
                const auto& weapon = candidate.flying ? unit.airWeapon : unit.groundWeapon;
                const auto range = weaponDistance(unit, candidate);
                return candidate.visible && candidate.detected && !candidate.invincible &&
                    !candidate.loaded && !candidate.hallucination && unit.canAttack(candidate) &&
                    weapon.maxRange >= 96 && range >= weapon.minRange && range <= weapon.maxRange;
            });
        std::vector<UnitSnapshot> defenseTargets;
        if (defense.active() && !covertAdvance) {
            for (const auto& candidate : enemy) {
                const auto& weapon = candidate.flying ? unit.airWeapon : unit.groundWeapon;
                // Siege artillery can damage the screen while remaining
                // outside both the pursuit boundary and the defender's own
                // range. Treat only a visible Tank that can reach the squad
                // or protected economy as a legal counter-battery target.
                if (defense.contains(candidate.position) || visibleSiegeThreat(candidate) ||
                    weaponDistance(unit, candidate) <= weapon.maxRange) {
                    defenseTargets.push_back(candidate);
                }
            }
        }
        auto targets = defense.active() && !covertAdvance
                                 ? std::span<const UnitSnapshot>{defenseTargets} : enemy;
        std::vector<UnitSnapshot> raidTargets;
        if (intent == TacticalIntent::raid) {
            for (const auto& candidate : targets) {
                const auto& weapon = candidate.flying ? unit.airWeapon : unit.groundWeapon;
                const auto range = weaponDistance(unit, candidate);
                const auto economic = isWorker(candidate.kind) || candidate.kind == UnitKind::overlord ||
                    candidate.kind == UnitKind::shuttle || candidate.kind == UnitKind::dropship;
                // Ignore nearby bait buildings and bound worker pursuit to
                // this mineral line. Immediate threats remain legal targets.
                if ((economic && range <= 320 &&
                     distanceSquared(candidate.position, objective) <= 384 * 384) ||
                    (candidate.canAttack(unit) && range <= weapon.maxRange))
                    raidTargets.push_back(candidate);
            }
            targets = raidTargets;
        }
        const auto target = evaluator.selectTarget(unit, targets, allocations, targetModel);

        if (psionicStormAvailable && unit.kind == UnitKind::highTemplar &&
            unit.energy >= psionicStormEnergyCost) {
            constexpr auto castRange = 9 * 32;
            Position bestPosition{-1, -1};
            auto bestScore = minimumPsionicStormValue;
            for (const auto& candidate : enemy) {
                if (!candidate.visible || !candidate.detected || candidate.invincible ||
                    candidate.loaded || candidate.hallucination ||
                    candidate.underStorm || isBuilding(candidate.kind) ||
                    !candidate.position.valid() ||
                    distance(unit.position, candidate.position) > castRange ||
                    std::ranges::any_of(plannedStorms, [&candidate](const Position center) {
                        return distanceSquared(center, candidate.position) <=
                            psionicStormReservationDistance * psionicStormReservationDistance;
                    })) {
                    continue;
                }
                const auto score = psionicStormValue(candidate.position, enemy,
                                                     stormAllySnapshots);
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

        // An immediately useful spell is a completed attack opportunity too.
        // Returning to a pursuit boundary must not suppress a ready Storm.
        if (returningDefender && !readyDefensiveShot && !fragile && retreatThreat == nullptr) {
            commands.push_back({unit.id, CommandType::move, -1,
                                defense.center,
                                UnitKind::unknown, 90, 0, "defense-return"});
            continue;
        }

        const UnitSnapshot* closestThreat = nullptr;
        auto nearestPressure = std::numeric_limits<double>::infinity();
        auto pressurePower = 0.0;
        auto pressureCount = 0;
        for (const auto& threat : enemy) {
            if (!threat.visible || !combatReady(threat) || threat.invincible ||
                !threat.canAttack(unit)) continue;
            const auto& response = unit.flying ? threat.airWeapon : threat.groundWeapon;
            const auto separation = weaponDistance(unit, threat);
            if (separation < response.minRange || separation > response.maxRange + 32) continue;
            pressurePower += unitStats(threat.kind).combatValue * std::clamp(threat.healthFraction(), 0.5, 1.0);
            ++pressureCount;
            if (separation - response.maxRange < nearestPressure) {
                closestThreat = &threat;
                nearestPressure = separation - response.maxRange;
            }
        }
        auto coveringPower = 0.0;
        const UnitSnapshot* relief = nullptr;
        auto reliefDistance = 224 * 224 + 1;
        if (closestThreat != nullptr) {
            for (const auto& ally : nearbyArmy) {
                if (!combatReady(ally) || !ally.canAttack(*closestThreat)) continue;
                const auto& weapon = closestThreat->flying ? ally.airWeapon : ally.groundWeapon;
                const auto range = weaponDistance(ally, *closestThreat);
                if (range >= weapon.minRange && range <= weapon.maxRange + 48)
                    coveringPower += unitStats(ally.kind).combatValue * std::clamp(ally.healthFraction(), 0.5, 1.0);
                const auto separation = distanceSquared(unit.position, ally.position);
                if (ally.id == unit.id || ally.flying != unit.flying || isBuilding(ally.kind) ||
                    ally.healthFraction() < 0.65 || ally.healthFraction() < unit.healthFraction() + 0.15 ||
                    range > weapon.maxRange + 96 || separation < 32 * 32 || separation >= reliefDistance ||
                    distance(ally.position, closestThreat->position) + 16 <
                        distance(unit.position, closestThreat->position)) continue;
                relief = &ally;
                reliefDistance = separation;
            }
        }
        const auto rotateWounded = !covertAdvance && relief != nullptr && unit.maxShields > 0 &&
            unit.shields * 4 <= unit.maxShields && (unit.underAttack || unit.hitPoints * 5 < unit.maxHitPoints * 4);
        const auto regroupFront = !covertAdvance && nearbyArmy.size() >= 3 && pressureCount >= 3 &&
            formationCenter.valid() && closestThreat != nullptr &&
            (closestThreat->flying ? unit.airWeapon.maxRange : unit.groundWeapon.maxRange) >= 96 &&
            pressurePower > coveringPower * 1.6 &&
            distance(formationCenter, closestThreat->position) > distance(unit.position, closestThreat->position) + 48;

        if (fragile || cloakCompromised || returningDefender || rotateWounded || regroupFront || (!covertAdvance &&
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
                    const auto screenIntercept = estimate.holdScreen && weapon.maxRange < 96 &&
                        range <= 64 && (!defense.active() || defense.contains(candidate.position));
                    if (range >= weapon.minRange &&
                        ((weapon.maxRange >= 96 && range <= weapon.maxRange) || screenIntercept))
                        firingTargets.push_back(candidate);
                }
                if (const auto* shot = evaluator.selectTarget(unit, firingTargets, allocations, targetModel)) {
                    commands.push_back({unit.id, CommandType::attackUnit, shot->id, {-1, -1},
                                        UnitKind::unknown, 86, 0,
                                        estimate.holdScreen ? "screen-intercept" : "retreat-volley"});
                    const auto allocation = std::ranges::find(
                        allocations, shot->id, &TargetAllocation::target);
                    const auto committed = allocation == allocations.end() ? 0.0 : allocation->committedDamage;
                    const auto remaining = std::max(0.0, shot->durability() - shot->incomingDamage - committed);
                    const auto damage = std::min(remaining, modeledAttackDamage(unit, *shot, remaining));
                    if (allocation == allocations.end()) allocations.push_back({shot->id, damage});
                    else allocation->committedDamage += damage;
                    continue;
                }
            }
            auto fallback = returningDefender ? defense.center :
                rotateWounded ? moveToward(relief->position, unitRetreatPoint, 72) :
                regroupFront ? formationCenter : unitRetreatPoint;
            // A terrain rally can be in front of a unit that has already
            // fallen back. Retreat locally away from nearby fire instead of
            // walking back into it just to reach that strategic anchor.
            if (retreatThreat != nullptr && fallback.valid() &&
                distance(fallback, retreatThreat->position) + 32 < distance(unit.position, retreatThreat->position)) {
                fallback = {unit.position.x + unit.position.x - retreatThreat->position.x,
                            unit.position.y + unit.position.y - retreatThreat->position.y};
            }
            const auto escape = cloakCompromised
                ? reposition(fallback, true, true)
                : localThreat > 0.05F || rotateWounded || regroupFront || retreatThreat != nullptr
                    ? reposition(fallback, true)
                    : fallback;
            commands.push_back({
                unit.id, CommandType::move, -1,
                escape,
                UnitKind::unknown, fragile ? 100 : 86, 0,
                rotateWounded ? "rotate-wounded" : regroupFront ? "regroup-frontline" : "combat-retreat",
            });
            continue;
        }

        const auto supportCaster = unit.kind == UnitKind::highTemplar ||
                                   unit.kind == UnitKind::darkArchon ||
                                   unit.kind == UnitKind::arbiter;
        if (supportCaster) {
            // A detached caster's formation center may be itself. Anchoring
            // there during empty travel leaves it at home forever, even when
            // its squad has an explicit destination to join the main army.
            // Use the full route while clear; resume the rear screen on contact.
            const auto travelling = enemy.empty() && localThreat <= 0.05F && objective.valid();
            const auto anchor = travelling ? protectedStep(objective) :
                formationCenter.valid() ? moveToward(formationCenter, unitRetreatPoint, 96.0) : unitRetreatPoint;
            if (anchor.valid() && distanceSquared(unit.position, anchor) > 96 * 96) {
                commands.push_back({
                    unit.id, CommandType::move, -1,
                    travelling ? anchor : influence.safestStep(unit.position, anchor, unit.flying),
                    UnitKind::unknown, 83, 0, travelling ? "spellcaster-travel" : "spellcaster-screen",
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
            const auto canFire = readySoon && range >= weapon.minRange && range <= weapon.maxRange + 12;
            const auto ranged = weapon.maxRange >= 96;
            if (!canFire && influence.stormDanger(moveToward(unit.position, target->position, 64)) > 0.0F) {
                commands.push_back({unit.id, CommandType::move, -1,
                    reposition(target->position, true), UnitKind::unknown, 109, 0, "avoid-storm"});
                continue;
            }
            if (unit.cloaked && local.detection > 0.1F &&
                target->role != UnitRole::detector && !canFire) {
                commands.push_back({
                    unit.id, CommandType::move, -1,
                    influence.safestStep(unit.position, unitRetreatPoint, unit.flying, true),
                    UnitKind::unknown, 94, 0, "cloak-preservation",
                });
                continue;
            }

            const auto meleeAhead = !unit.flying && ranged && range > weapon.maxRange &&
                std::ranges::any_of(nearbyArmy, [thisUnit = &unit, target, navigation](
                    const UnitSnapshot& ally) {
                    if (ally.id == thisUnit->id || ally.flying || ally.loaded ||
                        ally.groundWeapon.maxRange >= 96 || !ally.canAttack(*target) ||
                        !ally.position.valid() ||
                        weaponDistance(ally, *target) <= ally.groundWeapon.maxRange + 96 ||
                        distanceSquared(ally.position, thisUnit->position) > 288 * 288)
                        return false;
                    return navigation == nullptr || navigation->empty() ||
                        navigation->findPath(ally.position, target->position, 2048,
                                             movementFootprint(ally)).reached();
                });
            if (!canFire && meleeAhead) {
                const auto anchorIsRearward = formationCenter.valid() &&
                    distance(formationCenter, target->position) >
                        distance(unit.position, target->position) + 16;
                if (anchorIsRearward &&
                    distanceSquared(unit.position, formationCenter) > 64 * 64) {
                    commands.push_back({unit.id, CommandType::move, -1, formationCenter,
                        UnitKind::unknown, 77, 0, "melee-support-regroup"});
                } else {
                    commands.push_back({unit.id, CommandType::hold, -1, {-1, -1},
                        UnitKind::unknown, 77, 0, "melee-support-hold"});
                }
                continue;
            }

            // Contact-slot micro belongs to the final approach. A distant
            // unit follows the normal attack order until it is close enough
            // to coordinate around the target; exhausting a local slot search
            // must not park the rear of a large army hundreds of pixels away.
            if (!unit.flying && !ranged && range > weapon.maxRange + 2 &&
                range <= weapon.maxRange + 224 &&
                navigation != nullptr && !navigation->empty()) {
                const auto contact = chooseMeleeContact(unit, *target);
                if (contact.valid()) {
                    commands.push_back({unit.id, CommandType::move, -1, contact,
                        UnitKind::unknown, 83, 0, "melee-contact"});
                } else {
                    commands.push_back({unit.id, CommandType::hold, -1, {-1, -1},
                        UnitKind::unknown, 61, 0, "melee-contact-wait"});
                }
                continue;
            }

            // The selected worker or building may be harmless while another
            // nearby unit is closing on us. Kite the actual pursuer on reload.
            const UnitSnapshot* pursuer = nullptr;
            auto closestPressure = std::numeric_limits<double>::infinity();
            for (const auto& candidate : enemy) {
                const auto& response = unit.flying ? candidate.airWeapon : candidate.groundWeapon;
                const auto separation = weaponDistance(unit, candidate);
                if (!candidate.visible || !combatReady(candidate) || !candidate.position.valid() ||
                    candidate.invincible || !candidate.canAttack(unit) ||
                    separation > response.maxRange + 64 || separation < response.minRange) continue;
                const auto pressureDistance = separation - response.maxRange;
                if (pressureDistance < closestPressure) { closestPressure = pressureDistance; pursuer = &candidate; }
            }
            const auto targetWeapon = pursuer == nullptr ? WeaponSnapshot{} :
                unit.flying ? pursuer->airWeapon : pursuer->groundWeapon;
            const auto rangeAdvantage = weapon.maxRange >= targetWeapon.maxRange + 48;
            const auto kite = estimate.decision == FightDecision::kite || rangeAdvantage;
            const auto targetCanPressure = pursuer != nullptr && targetWeapon.damage > 0 &&
                                           weaponDistance(unit, *pursuer) <= targetWeapon.maxRange + 64;
            // Spend reload time opening firing lanes against observed splash.
            // Keep ready volleys, retreats and attack frames on their existing
            // paths. Score destinations jointly so neighbors do not fan into
            // the same square, or step downhill across a defensive boundary.
            if (unit.kind == UnitKind::dragoon && !canFire &&
                unit.weaponCooldown > latencyFrames + 8 &&
                std::ranges::any_of(enemy, [&unit](const UnitSnapshot& threat) {
                    const auto splash = threat.kind == UnitKind::reaver ||
                        threat.kind == UnitKind::archon || threat.kind == UnitKind::lurker ||
                        (threat.kind == UnitKind::siegeTank && threat.groundWeapon.maxRange >= 320);
                    return splash && threat.visible && combatReady(threat) && !threat.invincible &&
                        threat.position.valid() && threat.canAttack(unit) &&
                        weaponDistance(unit, threat) >= threat.groundWeapon.minRange &&
                        weaponDistance(unit, threat) <= threat.groundWeapon.maxRange + 96;
                })) {
                const auto crowding = [&friendly, &commands, &unit](const Position position) {
                    auto score = 0.0;
                    for (const auto& neighbor : friendly) {
                        if (neighbor.id == unit.id || neighbor.flying || neighbor.loaded ||
                            !neighbor.position.valid()) continue;
                        auto destination = neighbor.position;
                        const auto order = std::ranges::find(commands, neighbor.id, &Command::actor);
                        if (order != commands.end() && order->source == "splash-spacing")
                            destination = order->targetPosition;
                        score += std::max(0.0, 80.0 - distance(position, destination));
                    }
                    return score;
                };
                const auto currentCrowding = crowding(unit.position);
                auto bestScore = currentCrowding - 12.0;
                Position best{-1, -1};
                constexpr std::array<Position, 8> offsets{{
                    {0, 64}, {0, -64}, {-64, 0}, {64, 0},
                    {-45, 45}, {45, -45}, {-45, -45}, {45, 45}}};
                for (const auto offset : offsets) {
                    const Position candidate{unit.position.x + offset.x, unit.position.y + offset.y};
                    if (!reachableStep(candidate) || (defense.active() && !defense.contains(candidate))) continue;
                    // Connectivity alone can accept a long detour around a
                    // cliff; reload spacing requires a clear local segment.
                    if (influence.at(candidate).groundThreat > local.groundThreat + 0.05F ||
                        distance(candidate, target->position) + 16 < distance(unit.position, target->position))
                        continue;
                    const auto score = crowding(candidate) +
                        std::max(0.0, distance(candidate, target->position) -
                                      distance(unit.position, target->position)) * 0.25;
                    if (score < bestScore) { bestScore = score; best = candidate; }
                }
                if (best.valid()) {
                    commands.push_back({unit.id, CommandType::move, -1, best,
                        UnitKind::unknown, 85, 0, "splash-spacing"});
                    continue;
                }
            }
            if (!canFire && kite && ranged && targetCanPressure &&
                unit.weaponCooldown > latencyFrames + 2 &&
                weaponDistance(unit, *pursuer) <= weapon.maxRange + 64) {
                const Position away{
                    unit.position.x + unit.position.x - pursuer->position.x,
                    unit.position.y + unit.position.y - pursuer->position.y,
                };
                commands.push_back({
                    unit.id, CommandType::move, -1,
                    reposition(away),
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
                    const auto fullDamage = modeledAttackDamage(unit, *target, remaining);
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
            auto anchor = screenAnchor;
            auto holdRadius = 96;
            if (!unit.flying && !screenMembers.empty()) {
                const auto member = std::ranges::find(screenMembers, unit.id);
                if (member != screenMembers.end()) {
                    const auto index = static_cast<std::size_t>(member - screenMembers.begin());
                    anchor = screenSlots.empty() ? unit.position : screenSlots[index % screenSlots.size()];
                    holdRadius = 24;
                }
            }
            if (distanceSquared(unit.position, anchor) > holdRadius * holdRadius) {
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
            // BWAPI rejects attack-move for payload units while they have no
            // Scarabs/Interceptors. Keep their route active during reload;
            // target selection resumes normally as soon as ammunition exists.
            const auto reloading = (unit.kind == UnitKind::reaver || unit.kind == UnitKind::carrier) &&
                unit.ammo <= 0;
            commands.push_back({unit.id, intent == TacticalIntent::raid || reloading
                                            ? CommandType::move : CommandType::attackMove,
                                -1, objective, UnitKind::unknown, 50, 0,
                                intent == TacticalIntent::raid ? "raid-travel" :
                                    reloading ? "payload-reload-travel" : "squad-objective"});
        }
    }
    return commands;
}

}  // namespace protodd
