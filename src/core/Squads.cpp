#include "astra/Squads.hpp"

#include "astra/UnitCatalog.hpp"

#include <algorithm>
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
           unit.kind == UnitKind::wraith;
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
    // base. Allocation is capped so a small run-by cannot freeze the main army.
    const BaseSnapshot* threatenedBase = nullptr;
    auto highestThreat = 0;
    for (const auto& base : state.bases) {
        if (base.ownerId != state.self.id) continue;
        const auto nearby = static_cast<int>(std::ranges::count_if(
            enemy,
            [&base](const UnitSnapshot& unit) {
                return unit.visible && distanceSquared(unit.position, base.center) <= 800 * 800;
            }));
        if (nearby > highestThreat) {
            highestThreat = nearby;
            threatenedBase = &base;
        }
    }
    if (threatenedBase != nullptr && highestThreat > 0 && !friendly.empty()) {
        std::vector<UnitSnapshot> candidates(friendly.begin(), friendly.end());
        std::ranges::sort(candidates, {}, [threatenedBase](const UnitSnapshot& unit) {
            return distanceSquared(unit.position, threatenedBase->center);
        });
        const auto desired = std::min(
            candidates.size(),
            static_cast<std::size_t>(std::max(3, highestThreat + 2)));
        Squad defense;
        defense.id = nextId++;
        defense.role = SquadRole::baseDefense;
        defense.objective = threatenedBase->center;
        defense.retreat = threatenedBase->mineralLine.valid()
                              ? threatenedBase->mineralLine
                              : threatenedBase->center;
        defense.requiredRatio = 0.82;
        for (std::size_t i = 0; i < desired; ++i) {
            defense.units.push_back(candidates[i]);
            assigned.insert(candidates[i].id);
        }
        defense.center = centroid(defense.units);
        defense.enemies = localEnemies(enemy, defense.units, defense.objective, 900);
        defense.needsDetection = std::ranges::any_of(defense.enemies, detectionThreat);
        result.push_back(std::move(defense));
    }

    std::vector<UnitSnapshot> harassment;
    std::vector<UnitSnapshot> main;
    for (const auto& unit : friendly) {
        if (assigned.contains(unit.id)) continue;
        (harassmentUnit(unit) ? harassment : main).push_back(unit);
    }

    if (!harassment.empty()) {
        Squad squad;
        squad.id = nextId++;
        squad.role = SquadRole::harassment;
        squad.units = std::move(harassment);
        squad.center = centroid(squad.units);
        squad.objective = plan.attackTarget;
        const auto home = nearestOwnedBase(state, squad.center);
        squad.retreat = home != nullptr ? home->center : fallbackRetreat;
        squad.requiredRatio = 1.38;
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
        squad.center = centroid(squad.units);
        squad.objective = plan.attackTarget.valid() ? plan.attackTarget : plan.rallyPoint;
        const auto home = nearestOwnedBase(state, squad.center);
        squad.retreat = home != nullptr ? home->center : fallbackRetreat;
        squad.requiredRatio = plan.attackThreshold;
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
    return groups;
}

std::vector<UnitSnapshot> SquadPlanner::localEnemies(
    const std::span<const UnitSnapshot> enemies,
    const std::span<const UnitSnapshot> units,
    const Position objective,
    const int radius) {
    std::vector<UnitSnapshot> result;
    for (const auto& enemy : enemies) {
        if (!enemy.visible) continue;
        const auto nearMember = std::ranges::any_of(units, [&enemy, radius](const UnitSnapshot& unit) {
            return distanceSquared(enemy.position, unit.position) <= radius * radius;
        });
        const auto guardingObjective = isStaticDefense(enemy.kind) && objective.valid() &&
                                       distanceSquared(enemy.position, objective) <= radius * radius;
        if (nearMember || guardingObjective) result.push_back(enemy);
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
