#include "astra/Squads.hpp"

#include "astra/UnitCatalog.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_set>

namespace astra {
namespace {

bool harassmentUnit(const UnitSnapshot& unit) {
    return unit.kind == UnitKind::darkTemplar || unit.kind == UnitKind::corsair;
}

bool detectionThreat(const UnitSnapshot& unit) {
    return unit.cloaked || unit.burrowed || !unit.detected ||
           unit.kind == UnitKind::darkTemplar || unit.kind == UnitKind::lurker ||
           unit.kind == UnitKind::wraith || unit.kind == UnitKind::ghost ||
           unit.kind == UnitKind::spiderMine;
}

const BaseSnapshot* nearestOwnedBase(const GameState& state, const Position position) {
    const BaseSnapshot* best = nullptr;
    auto bestDistance = std::numeric_limits<int>::max();
    for (const auto& base : state.bases) {
        if (base.ownerId != state.self.id || !base.center.valid()) continue;
        const auto candidate = distanceSquared(position, base.center);
        if (candidate < bestDistance) {
            bestDistance = candidate;
            best = &base;
        }
    }
    return best;
}

double allocationPower(const UnitSnapshot& unit) {
    return unitStats(unit.kind).combatValue *
           std::clamp(unit.healthFraction(), 0.15, 1.0);
}

Position defensiveScreen(const GameState& state, const BaseSnapshot& base) {
    if (!base.mineralLine.valid() || base.mineralLine == base.center) return base.center;
    const Position away{
        base.center.x + base.center.x - base.mineralLine.x,
        base.center.y + base.center.y - base.mineralLine.y,
    };
    auto result = moveToward(base.center, away, 144.0);
    if (state.mapWidthPixels > 0) {
        result.x = std::clamp(result.x, 0, state.mapWidthPixels - 1);
    }
    if (state.mapHeightPixels > 0) {
        result.y = std::clamp(result.y, 0, state.mapHeightPixels - 1);
    }
    return result;
}

std::uint64_t squadSignature(const Squad& squad) {
    auto hash = std::uint64_t{1469598103934665603ULL};
    const auto mix = [&hash](const std::uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    mix(static_cast<std::uint64_t>(squad.role));
    mix(static_cast<std::uint32_t>(squad.objective.x));
    mix(static_cast<std::uint32_t>(squad.objective.y));
    mix(static_cast<std::uint32_t>(squad.retreat.x));
    mix(static_cast<std::uint32_t>(squad.retreat.y));
    for (const auto& unit : squad.units) {
        mix(static_cast<std::uint32_t>(unit.id));
    }
    return hash;
}

void finishSquad(Squad& squad) {
    std::ranges::sort(squad.units, {}, &UnitSnapshot::id);
    squad.signature = squadSignature(squad);
}

}  // namespace

std::vector<Squad> SquadPlanner::form(
    const GameState& state,
    const std::span<const UnitSnapshot> friendly,
    const std::span<const UnitSnapshot> enemy,
    const StrategicPlan& plan,
    const Position fallbackRetreat) const {
    std::vector<Squad> result;
    std::unordered_set<UnitId> assigned;
    auto nextId = 1;

    // Build a defense detachment only for a real, visible threat near an owned
    // base. Size it by combat value rather than headcount: three tanks require
    // a very different response from three Zerglings.
    struct BaseThreat {
        const BaseSnapshot* base{};
        std::vector<UnitSnapshot> enemies;
        double power{};
    };
    std::vector<BaseThreat> threats;
    for (const auto& base : state.bases) {
        if (base.ownerId != state.self.id) continue;
        std::vector<UnitSnapshot> nearby;
        auto threatPower = 0.0;
        for (const auto& unit : enemy) {
            if (!unit.visible || !unit.position.valid() || !unit.completed ||
                unit.disabled || (unit.groundWeapon.damage <= 0 &&
                                  unit.role != UnitRole::spellcaster) ||
                nearestOwnedBase(state, unit.position) != &base ||
                distanceSquared(unit.position, base.center) > 800 * 800) {
                continue;
            }
            nearby.push_back(unit);
            threatPower += allocationPower(unit);
        }
        if (threatPower > 0.0) threats.push_back({&base, std::move(nearby), threatPower});
    }
    std::ranges::sort(threats, [](const BaseThreat& left, const BaseThreat& right) {
        if (left.power != right.power) return left.power > right.power;
        return left.base->id < right.base->id;
    });
    for (const auto& baseThreat : threats) {
        const auto* threatenedBase = baseThreat.base;
        const auto& baseThreats = baseThreat.enemies;
        const auto highestThreat = baseThreat.power;
        std::vector<UnitSnapshot> candidates;
        std::vector<UnitSnapshot> staticSupport;
        for (const auto& unit : friendly) {
            if (assigned.contains(unit.id) || !unit.completed || unit.disabled ||
                (unitStats(unit.kind).requiresPsi && !unit.powered)) continue;
            if (isStaticDefense(unit.kind)) {
                const auto useful = distanceSquared(unit.position, threatenedBase->center) <=
                                        576 * 576 &&
                                    std::ranges::any_of(
                                        baseThreats, [&unit](const UnitSnapshot& threat) {
                                            return unit.canAttack(threat);
                                        });
                if (useful) staticSupport.push_back(unit);
            } else if (isCombatUnit(unit.kind)) {
                candidates.push_back(unit);
            }
        }
        std::ranges::sort(candidates, [threatenedBase, &baseThreats](
                                          const UnitSnapshot& left,
                                          const UnitSnapshot& right) {
            const auto contributes = [&baseThreats](const UnitSnapshot& unit) {
                return std::ranges::any_of(baseThreats, [&unit](const UnitSnapshot& threat) {
                    return unit.canAttack(threat);
                });
            };
            const auto leftContributes = contributes(left);
            const auto rightContributes = contributes(right);
            if (leftContributes != rightContributes) return leftContributes > rightContributes;
            const auto leftDistance = distanceSquared(left.position, threatenedBase->center);
            const auto rightDistance = distanceSquared(right.position, threatenedBase->center);
            if (leftDistance != rightDistance) return leftDistance < rightDistance;
            return left.id < right.id;
        });
        Squad defense;
        defense.id = nextId++;
        defense.role = SquadRole::baseDefense;
        defense.objective = centroid(baseThreats);
        // Screen on the side of the Nexus opposite the mineral line. A losing
        // defender should not drag melee units through the worker economy.
        defense.retreat = defensiveScreen(state, *threatenedBase);
        defense.requiredRatio = 0.55;
        defense.units = std::move(staticSupport);
        auto committedPower = 0.0;
        for (const auto& unit : defense.units) committedPower += allocationPower(unit);
        const auto targetPower = highestThreat *
                                     (plan.posture == Posture::defend ? 1.45 : 1.30) +
                                 0.35;
        const auto minimumMobile = std::min<std::size_t>(2, candidates.size());
        auto mobileCount = std::size_t{0};
        for (const auto& candidate : candidates) {
            if (mobileCount >= minimumMobile && committedPower >= targetPower) break;
            defense.units.push_back(candidate);
            ++mobileCount;
            assigned.insert(candidate.id);
            if (std::ranges::any_of(baseThreats, [&candidate](const UnitSnapshot& threat) {
                    return candidate.canAttack(threat);
                })) {
                committedPower += allocationPower(candidate);
            }
        }
        if (defense.units.empty()) continue;
        for (const auto& unit : defense.units) assigned.insert(unit.id);
        defense.center = centroid(defense.units);
        finishSquad(defense);
        defense.enemies = localEnemies(enemy, defense.units, defense.objective, 900);
        defense.needsDetection = std::ranges::any_of(defense.enemies, detectionThreat);
        result.push_back(std::move(defense));
    }

    std::vector<UnitSnapshot> harassment;
    std::vector<UnitSnapshot> main;
    for (const auto& unit : friendly) {
        if (assigned.contains(unit.id) || isStaticDefense(unit.kind) ||
            !isCombatUnit(unit.kind)) continue;
        (harassmentUnit(unit) ? harassment : main).push_back(unit);
    }

    if (!harassment.empty()) {
        Squad squad;
        squad.id = nextId++;
        squad.role = SquadRole::harassment;
        squad.units = std::move(harassment);
        squad.objective = plan.attackTarget;
        squad.center = centroid(squad.units);
        const auto home = nearestOwnedBase(state, squad.center);
        squad.retreat = home != nullptr ? home->center : fallbackRetreat;
        squad.requiredRatio = 1.38;
        finishSquad(squad);
        squad.enemies = localEnemies(enemy, squad.units, squad.objective, 720);
        squad.needsDetection = std::ranges::any_of(squad.enemies, detectionThreat);
        result.push_back(std::move(squad));
    }

    // Disconnected army components make independent local decisions until they
    // regroup. This prevents a fight on one side of the map from ordering a
    // retreat on the other side.
    for (auto& group : connectedGroups(main, 576)) {
        Squad squad;
        squad.id = nextId++;
        squad.role = SquadRole::mainArmy;
        squad.units = std::move(group);
        squad.objective = plan.attackTarget.valid() ? plan.attackTarget : plan.rallyPoint;
        squad.center = centroid(squad.units);
        const auto home = nearestOwnedBase(state, squad.center);
        squad.retreat = home != nullptr ? home->center : fallbackRetreat;
        squad.requiredRatio = plan.attackThreshold;
        finishSquad(squad);
        squad.enemies = localEnemies(enemy, squad.units, squad.objective, 820);
        squad.needsDetection = std::ranges::any_of(squad.enemies, detectionThreat);
        result.push_back(std::move(squad));
    }

    std::ranges::sort(result, {}, &Squad::id);
    return result;
}

std::vector<Command> SquadPlanner::detectorEscorts(
    const GameState& state,
    const std::span<const Squad> squads,
    const InfluenceMap& influence) const {
    std::vector<const UnitSnapshot*> observers;
    for (const auto& unit : state.self.units) {
        if (unit.kind == UnitKind::observer && unit.completed) observers.push_back(&unit);
    }
    std::ranges::sort(observers, {}, [](const UnitSnapshot* unit) { return unit->id; });

    std::vector<const Squad*> priorities;
    for (const auto& squad : squads) {
        if (!squad.units.empty()) priorities.push_back(&squad);
    }
    std::ranges::sort(priorities, [](const Squad* left, const Squad* right) {
        if (left->needsDetection != right->needsDetection)
            return left->needsDetection > right->needsDetection;
        if (left->role != right->role)
            return left->role == SquadRole::baseDefense;
        return left->units.size() > right->units.size();
    });

    std::vector<Command> result;
    const auto count = std::min(observers.size(), priorities.size());
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto* observer = observers[i];
        const auto* squad = priorities[i];
        const auto anchor = moveToward(squad->center, squad->retreat, 96.0);
        const auto destination = influence.safestStep(observer->position, anchor, true);
        result.push_back({observer->id, CommandType::move, -1, destination,
                          UnitKind::unknown, squad->needsDetection ? 96 : 72, 0,
                          "detector-escort"});
    }
    return result;
}

const Squad* SquadPlanner::selectVanguard(
    const std::span<const Squad> squads,
    const Position objective) noexcept {
    const Squad* best = nullptr;
    auto bestPower = -1.0;
    auto bestDistance = std::numeric_limits<int>::max();
    for (const auto& squad : squads) {
        if (squad.role != SquadRole::mainArmy || squad.units.empty()) continue;
        auto power = 0.0;
        for (const auto& unit : squad.units) power += allocationPower(unit);
        const auto objectiveDistance = objective.valid()
                                           ? distanceSquared(squad.center, objective)
                                           : 0;
        if (power > bestPower + 0.001 ||
            (std::abs(power - bestPower) <= 0.001 &&
             (objectiveDistance < bestDistance ||
              (objectiveDistance == bestDistance &&
               (best == nullptr || squad.signature < best->signature))))) {
            best = &squad;
            bestPower = power;
            bestDistance = objectiveDistance;
        }
    }
    return best;
}

bool SquadPlanner::mustHoldDefensiveScreen(const Squad& squad) noexcept {
    if (squad.role != SquadRole::baseDefense || !squad.retreat.valid()) return false;
    constexpr auto breachRadius = 384;
    return std::ranges::any_of(squad.enemies, [&squad](const UnitSnapshot& enemy) {
        return enemy.visible && enemy.detected && !enemy.flying &&
               enemy.groundWeapon.damage > 0 &&
               distanceSquared(enemy.position, squad.retreat) <=
                   breachRadius * breachRadius;
    });
}

Position SquadPlanner::centroid(const std::span<const UnitSnapshot> units) noexcept {
    if (units.empty()) return {-1, -1};
    long long x = 0;
    long long y = 0;
    for (const auto& unit : units) {
        x += unit.position.x;
        y += unit.position.y;
    }
    return {static_cast<int>(x / static_cast<long long>(units.size())),
            static_cast<int>(y / static_cast<long long>(units.size()))};
}

std::vector<std::vector<UnitSnapshot>> SquadPlanner::connectedGroups(
    const std::span<const UnitSnapshot> units,
    const int linkDistance) {
    std::vector<std::vector<UnitSnapshot>> groups;
    std::vector<bool> used(units.size(), false);
    for (std::size_t seed = 0; seed < units.size(); ++seed) {
        if (used[seed]) continue;
        used[seed] = true;
        std::vector<std::size_t> frontier{seed};
        std::vector<UnitSnapshot> group;
        for (std::size_t cursor = 0; cursor < frontier.size(); ++cursor) {
            const auto current = frontier[cursor];
            group.push_back(units[current]);
            for (std::size_t candidate = 0; candidate < units.size(); ++candidate) {
                if (!used[candidate] &&
                    distanceSquared(units[current].position, units[candidate].position) <=
                        linkDistance * linkDistance) {
                    used[candidate] = true;
                    frontier.push_back(candidate);
                }
            }
        }
        std::ranges::sort(group, {}, &UnitSnapshot::id);
        groups.push_back(std::move(group));
    }
    std::ranges::sort(groups, [](const auto& left, const auto& right) {
        if (left.empty() || right.empty()) return left.size() > right.size();
        return left.front().id < right.front().id;
    });
    return groups;
}

std::vector<UnitSnapshot> SquadPlanner::localEnemies(
    const std::span<const UnitSnapshot> enemies,
    const std::span<const UnitSnapshot> units,
    const Position /*objective*/,
    const int radius) {
    std::vector<UnitSnapshot> result;
    for (const auto& enemy : enemies) {
        if (!enemy.visible) continue;
        const auto nearMember = std::ranges::any_of(units, [&enemy, radius](const UnitSnapshot& unit) {
            return distanceSquared(enemy.position, unit.position) <= radius * radius;
        });
        // A distant target's defenses must not enter a local fight until the
        // army approaches them; otherwise reinforcements can retreat at home.
        if (nearMember) result.push_back(enemy);
    }
    std::ranges::sort(result, {}, &UnitSnapshot::id);
    return result;
}

std::string_view squadRoleName(const SquadRole role) noexcept {
    switch (role) {
        case SquadRole::mainArmy: return "MainArmy";
        case SquadRole::baseDefense: return "BaseDefense";
        case SquadRole::harassment: return "Harassment";
    }
    return "Invalid";
}

}  // namespace astra
