#include "protodd/Squads.hpp"

#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_set>

namespace protodd {
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
    if (base.defense.valid()) return base.defense.anchor;
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
    squad.engagementKey = (static_cast<std::uint64_t>(squad.role) << 32U) |
        static_cast<std::uint32_t>(squad.units.empty() ? -1 : squad.units.front().id);
}

}  // namespace

std::vector<Squad> SquadPlanner::form(
    const GameState& state,
    const std::span<const UnitSnapshot> friendly,
    const std::span<const UnitSnapshot> enemy,
    const StrategicPlan& plan,
    const Position fallbackRetreat, const NavigationGrid* navigation) const {
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
                (distanceSquared(unit.position, base.center) > 800 * 800 &&
                 (!base.defense.valid() || distanceSquared(unit.position, base.defense.entrance) > 640 * 640))) {
                continue;
            }
            // A stabilized field army contests a perimeter contain as one
            // group. Only an actual base breach creates a tethered detachment.
            if (plan.breakContainment &&
                distanceSquared(unit.position, base.center) > 320 * 320) continue;
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
        // Pull the mobile screen far enough forward to meet a rush before it
        // reaches the mineral line.  The old 448-pixel ring left a small gap
        // where incoming Zealots were visible but every defender was still
        // ordered back to the Nexus; that let the first contact happen on top
        // of workers.  Static support keeps its tighter weapon-radius area.
        defense.defense = {threatenedBase->center, 600, threatenedBase->center};
        if (!staticSupport.empty()) {
            // Fight within the actual weapons' support instead of assuming
            // every point around a Nexus is covered by rear-placed Cannons.
            defense.defense = {centroid(staticSupport), 256, threatenedBase->center};
        }
        const auto breached = std::ranges::any_of(baseThreats, [threatenedBase](const UnitSnapshot& enemyUnit) {
            return distanceSquared(enemyUnit.position, threatenedBase->center) <= 320 * 320;
        });
        if (staticSupport.empty() && threatenedBase->defense.valid() && !breached) {
            const auto& terrain = threatenedBase->defense;
            defense.defense = {terrain.anchor, std::clamp(terrain.width, 192, 320),
                               threatenedBase->center, terrain.entrance};
        } else if (breached && threatenedBase->defense.valid()) {
            defense.retreat = threatenedBase->center;
        }
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
        defense.enemies = localEnemies(enemy, defense.units, defense.objective, 900, state.frame);
        defense.needsDetection = std::ranges::any_of(defense.enemies, detectionThreat);
        result.push_back(std::move(defense));
    }

    // Keep a small home guard while a sizeable army is moving across the
    // map. Without this reserve, a cleared perimeter immediately sends every
    // fighter toward the enemy and a hidden reinforcement wave can walk into
    // the Nexus before the next threat snapshot forms a defense squad.
    if (threats.empty() && !plan.breakContainment &&
        (plan.posture == Posture::pressure || plan.posture == Posture::attack)) {
        const auto* homeBase = nearestOwnedBase(state, fallbackRetreat);
        if (homeBase != nullptr) {
            std::vector<UnitSnapshot> candidates;
            for (const auto& unit : friendly) {
                if (assigned.contains(unit.id) || !unit.completed || unit.disabled ||
                    isStaticDefense(unit.kind) || !isCombatUnit(unit.kind) ||
                    (unitStats(unit.kind).requiresPsi && !unit.powered)) {
                    continue;
                }
                candidates.push_back(unit);
            }
            std::ranges::sort(candidates, [homeBase](const UnitSnapshot& left,
                                                      const UnitSnapshot& right) {
                const auto leftDistance = distanceSquared(left.position, homeBase->center);
                const auto rightDistance = distanceSquared(right.position, homeBase->center);
                if (leftDistance != rightDistance) return leftDistance < rightDistance;
                return left.id < right.id;
            });
            // Keep a reserve, but do not let the home guard consume the whole
            // first pressure wave once the strategy has explicitly declared a
            // compact timing.  Before that checkpoint the larger four-unit
            // guard is still needed to absorb a real Zealot flood; releasing
            // it early made the worker line die while only three defenders
            // were present.  Two bodies cover the Nexus after the strategy
            // lowers its attack size and commits the remaining group forward.
            const auto guardLimit = plan.minimumAttackSize <= 8 ? 2U : 4U;
            const auto spare = candidates.size() > static_cast<std::size_t>(plan.minimumAttackSize)
                ? candidates.size() - static_cast<std::size_t>(plan.minimumAttackSize) : 0U;
            const auto guardCount = std::min<std::size_t>(guardLimit, spare);
            if (guardCount > 0) {
                Squad guard;
                guard.id = nextId++;
                guard.role = SquadRole::baseDefense;
                guard.objective = defensiveScreen(state, *homeBase);
                guard.retreat = defensiveScreen(state, *homeBase);
                guard.defense = {homeBase->center, 448, homeBase->center};
                if (homeBase->defense.valid())
                    guard.defense = {homeBase->defense.anchor, 256, homeBase->center,
                                     homeBase->defense.entrance};
                guard.requiredRatio = 1.15;
                guard.units.assign(candidates.begin(), candidates.begin() +
                    static_cast<std::ptrdiff_t>(guardCount));
                for (const auto& unit : guard.units) assigned.insert(unit.id);
                finishSquad(guard);
                guard.enemies = localEnemies(enemy, guard.units, guard.objective, 900, state.frame);
                guard.needsDetection = std::ranges::any_of(guard.enemies, detectionThreat);
                result.push_back(std::move(guard));
            }
        }
    }

    std::vector<UnitSnapshot> raidCandidates;
    for (const auto& unit : friendly)
        if (!assigned.contains(unit.id) && isCombatUnit(unit.kind) && !unit.loaded && !unit.disabled)
            raidCandidates.push_back(unit);
    const auto raid = harassment_.update(state, raidCandidates, plan, fallbackRetreat, !threats.empty(), navigation);
    if (!raid.members.empty()) {
        Squad squad;
        squad.id = nextId++;
        squad.role = SquadRole::harassment;
        for (const auto id : raid.members) {
            const auto member = std::ranges::find(raidCandidates, id, &UnitSnapshot::id);
            if (member != raidCandidates.end()) { squad.units.push_back(*member); assigned.insert(id); }
        }
        squad.center = centroid(squad.units);
        squad.objective = raid.withdrawing ? fallbackRetreat : raid.waypoint;
        squad.retreat = fallbackRetreat;
        squad.withdrawing = raid.withdrawing;
        squad.missionReason = raid.reason;
        squad.requiredRatio = 1.50;
        squad.enemies = localEnemies(enemy, squad.units, squad.objective, 720, state.frame);
        squad.needsDetection = std::ranges::any_of(squad.enemies, detectionThreat);
        finishSquad(squad);
        result.push_back(std::move(squad));
    }

    std::vector<UnitSnapshot> groundHarassment;
    std::vector<UnitSnapshot> airHarassment;
    std::vector<UnitSnapshot> main;
    for (const auto& unit : friendly) {
        if (assigned.contains(unit.id) || isStaticDefense(unit.kind) ||
            !isCombatUnit(unit.kind)) continue;
        if (harassmentUnit(unit)) {
            (unit.flying ? airHarassment : groundHarassment).push_back(unit);
        } else {
            main.push_back(unit);
        }
    }

    // Corsairs and Dark Templar cannot support one another's targets. Each
    // movement domain also splits by locality, just like the main army.
    for (const auto* harassment : {&groundHarassment, &airHarassment}) {
      for (auto& group : connectedGroups(*harassment, 576)) {
        Squad squad;
        squad.id = nextId++;
        squad.role = SquadRole::harassment;
        squad.units = std::move(group);
        const auto opportunity = harassmentOpportunity(state, squad.units.front(), false, navigation);
        squad.objective = opportunity.target.valid() ? opportunity.waypoint : fallbackRetreat;
        squad.withdrawing = !opportunity.target.valid();
        squad.missionReason = opportunity.target.valid() ?
            (squad.units.front().flying ? "Raid exposed air logistics" : "Raid exposed worker line") :
            "Raid waiting: no observed safe economic target";
        squad.center = centroid(squad.units);
        const auto home = nearestOwnedBase(state, squad.center);
        squad.retreat = home != nullptr ? defensiveScreen(state, *home) : fallbackRetreat;
        squad.requiredRatio = 1.38;
        finishSquad(squad);
        squad.enemies = localEnemies(enemy, squad.units, squad.objective, 720, state.frame);
        squad.needsDetection = std::ranges::any_of(squad.enemies, detectionThreat);
        result.push_back(std::move(squad));
      }
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
        squad.retreat = home != nullptr ? defensiveScreen(state, *home) : fallbackRetreat;
        squad.requiredRatio = plan.attackThreshold;
        finishSquad(squad);
        squad.enemies = localEnemies(enemy, squad.units, squad.objective, 820, state.frame);
        squad.needsDetection = std::ranges::any_of(squad.enemies, detectionThreat);
        result.push_back(std::move(squad));
    }

    // Strategic cloak risk should accelerate Observer production, not tether
    // every ground squad to one. A squad requests an escort only after its own
    // local contact list contains a cloaked, burrowed, or undetected threat.
    std::ranges::sort(result, {}, &Squad::id);
    return result;
}

bool SquadPlanner::mobileDetectionReady(const GameState& state, const Squad& squad) noexcept {
    if (!squad.needsDetection) return true;
    const auto ahead = squad.objective.valid() ? moveToward(squad.center, squad.objective, 128.0) : squad.center;
    return std::ranges::any_of(state.self.units, [&squad, ahead](const UnitSnapshot& observer) {
        if (observer.kind != UnitKind::observer || !observer.completed || observer.disabled ||
            observer.loaded || observer.hallucination || observer.healthFraction() < 0.25 ||
            !observer.position.valid()) return false;
        const auto radius = std::max(0, (observer.sightRange > 0 ? observer.sightRange : 288) - 32);
        return distanceSquared(observer.position, squad.center) <= radius * radius &&
               distanceSquared(observer.position, ahead) <= radius * radius;
    });
}

std::vector<Command> SquadPlanner::detectorEscorts(
    const GameState& state,
    const std::span<const Squad> squads,
    const InfluenceMap& influence) const {
    std::vector<const UnitSnapshot*> observers;
    for (const auto& unit : state.self.units) {
        if (unit.kind == UnitKind::observer && unit.completed && !unit.loaded &&
            !unit.disabled && !unit.hallucination) observers.push_back(&unit);
    }
    std::ranges::sort(observers, {}, [](const UnitSnapshot* unit) { return unit->id; });

    std::vector<const Squad*> priorities;
    for (const auto& squad : squads) {
        if (squad.role == SquadRole::baseDefense && squad.enemies.empty() &&
            !squad.needsDetection) continue;
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
    // Once a second Observer exists, keep one available for strategic
    // scouting. Fragmented armies can otherwise lease every Observer as an
    // escort, leaving no unit to watch a siege push before it reaches a base.
    const auto escortCapacity = observers.size() > 1 ? observers.size() - 1 : observers.size();
    const auto count = std::min(escortCapacity, priorities.size());
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto* squad = priorities[i];
        const auto anchor = moveToward(squad->center, squad->retreat, 96.0);
        const auto closest = std::min_element(observers.begin() + static_cast<std::ptrdiff_t>(i),
            observers.end(), [anchor](const UnitSnapshot* left, const UnitSnapshot* right) {
                const auto leftReady = left->healthFraction() >= 0.25;
                const auto rightReady = right->healthFraction() >= 0.25;
                if (leftReady != rightReady) return leftReady;
                const auto a = distanceSquared(left->position, anchor);
                const auto b = distanceSquared(right->position, anchor);
                return a != b ? a < b : left->id < right->id;
            });
        std::iter_swap(observers.begin() + static_cast<std::ptrdiff_t>(i), closest);
        const auto* observer = observers[i];
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

std::vector<Command> SquadPlanner::supportEscorts(
    const Squad& squad, const Position objective) {
    if (!objective.valid() || squad.role != SquadRole::mainArmy ||
        squad.withdrawing || !squad.enemies.empty()) return {};
    const UnitSnapshot* support = nullptr;
    auto rearDistance = -1.0;
    for (const auto& reaver : squad.units) {
        if (reaver.kind != UnitKind::reaver || !reaver.completed || reaver.loaded ||
            reaver.disabled || reaver.hallucination || !reaver.position.valid() ||
            reaver.healthFraction() < 0.35) continue;
        const auto remaining = distance(reaver.position, objective);
        if (remaining > rearDistance) { rearDistance = remaining; support = &reaver; }
    }
    if (support == nullptr) return {};
    std::vector<const UnitSnapshot*> escorts;
    for (const auto& unit : squad.units) {
        if ((unit.kind != UnitKind::dragoon && unit.kind != UnitKind::zealot) ||
            !unit.completed || unit.loaded || unit.disabled || unit.hallucination ||
            unit.attackFrame || unit.underAttack || unit.underStorm || unit.healthFraction() < 0.65 ||
            distanceSquared(unit.position, support->position) > 768 * 768 ||
            distance(unit.position, objective) + 192 >= rearDistance) continue;
        escorts.push_back(&unit);
    }
    std::ranges::sort(escorts, [support](const UnitSnapshot* a, const UnitSnapshot* b) {
        const auto da = distanceSquared(a->position, support->position);
        const auto db = distanceSquared(b->position, support->position);
        return da != db ? da < db : a->id < b->id;
    });
    // A lagging support unit may slow at most two bodyguards, never reverse
    // the destination of the entire army. Detached/new/transport-owned Reavers
    // are not members of this squad and cannot recall it.
    if (escorts.size() > 2) escorts.resize(2);
    std::vector<Command> result;
    const auto anchor = moveToward(support->position, objective, 128);
    for (const auto* escort : escorts)
        result.push_back({escort->id, CommandType::move, -1, anchor,
                          UnitKind::unknown, 64, 0, "support-escort"});
    return result;
}

bool SquadPlanner::canCounterattack(
    const Squad& squad, const CombatEstimate& estimate, const StrategicPlan& plan) {
    if (plan.posture != Posture::defend || squad.role != SquadRole::mainArmy ||
        squad.enemies.empty() || estimate.decision != FightDecision::engage ||
        estimate.ratio < std::max(1.6, plan.attackThreshold * 1.35)) return false;
    const auto fighters = std::ranges::count_if(squad.units, [&squad](const UnitSnapshot& unit) {
        return isCombatUnit(unit.kind) && unit.completed && !unit.disabled && !unit.loaded &&
               std::ranges::any_of(squad.enemies, [&unit](const UnitSnapshot& target) {
                   return target.visible && unit.canAttack(target);
               });
    });
    // These are unassigned mobile reserves. Static support and the units
    // already allocated to base defense cannot inflate this breakout force.
    return fighters >= std::max(8, plan.minimumAttackSize);
}

std::vector<UnitSnapshot> SquadPlanner::tacticalTargets(
    const Squad& squad, const std::span<const UnitSnapshot> hostiles) {
    auto targets = squad.enemies;
    for (const auto& candidate : hostiles) {
        if (!candidate.visible || !candidate.detected || !candidate.position.valid() ||
            candidate.loaded || candidate.invincible || candidate.hallucination ||
            std::ranges::find(targets, candidate.id, &UnitSnapshot::id) != targets.end()) {
            continue;
        }
        const auto reachableTarget = std::ranges::any_of(squad.units,
            [&candidate](const UnitSnapshot& unit) {
                return isCombatUnit(unit.kind) && unit.canAttack(candidate) &&
                       distanceSquared(unit.position, candidate.position) <= 416 * 416;
            });
        if (reachableTarget) targets.push_back(candidate);
    }
    std::ranges::sort(targets, {}, &UnitSnapshot::id);
    return targets;
}

std::vector<UnitSnapshot> SquadPlanner::combatSupport(
    const Squad& squad, const std::span<const UnitSnapshot> friendly,
    const NavigationGrid* navigation) {
    auto result = squad.units;
    if (squad.enemies.empty() || squad.role == SquadRole::harassment || squad.withdrawing) return result;
    for (const auto& ally : friendly) {
        if (!ally.completed || ally.disabled || ally.loaded || ally.hallucination ||
            !ally.position.valid() || !ally.powered ||
            (!isCombatUnit(ally.kind) && !isStaticDefense(ally.kind)) ||
            std::ranges::find(result, ally.id, &UnitSnapshot::id) != result.end()) continue;
        const auto nearby = std::ranges::any_of(squad.units, [&ally, navigation](const UnitSnapshot& member) {
            return distanceSquared(ally.position, member.position) <= 384 * 384 &&
                (ally.flying || navigation == nullptr || navigation->empty() ||
                 navigation->lineWalkable(ally.position, member.position));
        });
        if (!nearby) continue;
        const auto supportsFight = std::ranges::any_of(squad.enemies, [&ally](const UnitSnapshot& enemy) {
            const auto& weapon = enemy.flying ? ally.airWeapon : ally.groundWeapon;
            return ally.canAttack(enemy) && weaponDistance(ally, enemy) >= weapon.minRange &&
                weaponDistance(ally, enemy) <= weapon.maxRange + (isBuilding(ally.kind) ? 0 : 128);
        });
        if (supportsFight) result.push_back(ally);
    }
    return result;
}

DefenseArea SquadPlanner::expansionDefense(
    const Squad& squad, const Position assembly, const Position expansion,
    const DefenseArea currentDefense) noexcept {
    // An expansion is a travel objective, not an emergency recall through an
    // active battle. Keep the current fight/retreat policy until contact ends
    // or the army has actually reached the expansion's defensive area.
    const DefenseArea proposed{assembly, 448, expansion};
    if (!assembly.valid() || (!squad.enemies.empty() && !proposed.contains(squad.center)))
        return currentDefense;
    return proposed;
}

DefenseArea SquadPlanner::defensiveArea(const GameState& state, const Position rally) {
    const auto* base = nearestOwnedBase(state, rally);
    if (base != nullptr && base->defense.valid() &&
        distanceSquared(rally, base->defense.anchor) < 256 * 256)
        return {base->defense.anchor, std::clamp(base->defense.width, 192, 320),
                base->center, base->defense.entrance};
    std::vector<UnitSnapshot> support;
    for (const auto& unit : state.self.units) {
        if (isStaticDefense(unit.kind) && unit.completed && unit.powered && !unit.disabled &&
            distanceSquared(unit.position, rally) <= 576 * 576 &&
            (base == nullptr || nearestOwnedBase(state, unit.position) == base)) {
            support.push_back(unit);
        }
    }
    const auto economy = base != nullptr ? base->center : Position{-1, -1};
    return support.empty() ? DefenseArea{rally, 320, economy}
                           : DefenseArea{centroid(support), 256, economy};
}

bool SquadPlanner::mustHoldDefensiveScreen(const Squad& squad) noexcept {
    if (squad.role != SquadRole::baseDefense || !squad.retreat.valid()) return false;
    if (squad.defense.front.valid()) {
        return std::ranges::any_of(squad.enemies, [&squad](const UnitSnapshot& enemy) {
            return enemy.visible && enemy.detected && !enemy.flying && enemy.groundWeapon.damage > 0 &&
                   distanceSquared(enemy.position, squad.defense.economyCenter) <= 256 * 256;
        });
    }
    // The base-defense squad is formed from a threat already inside the
    // 800-pixel local window. Waiting until 224 pixels leaves melee units
    // standing on the mineral line before the low-confidence combat estimate
    // is forced to engage; hold the outer 448-pixel perimeter instead.
    constexpr auto breachRadius = 448;
    return std::ranges::any_of(squad.enemies, [&squad](const UnitSnapshot& enemy) {
        return enemy.visible && enemy.detected && !enemy.flying &&
               enemy.groundWeapon.damage > 0 &&
               (distanceSquared(enemy.position, squad.retreat) <=
                    breachRadius * breachRadius ||
                (squad.defense.economyCenter.valid() &&
                 distanceSquared(enemy.position, squad.defense.economyCenter) <=
                     256 * 256));
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
    const int radius,
    const Frame frame) {
    std::vector<UnitSnapshot> result;
    for (const auto& enemy : enemies) {
        const auto age = frame - enemy.lastSeen;
        if (!enemy.position.valid() || (!enemy.visible &&
            (age < 0 || (!isBuilding(enemy.kind) && age > 8 * 24)))) continue;
        // Losing vision while retreating is not evidence that the opposing
        // army vanished. Keep its last legal observation briefly in the fight
        // estimate, with a bounded possible approach distance. Target selection
        // still requires visibility; no attack command uses a hidden target.
        const auto possibleApproach = enemy.visible ? 0 :
            static_cast<int>(std::clamp(enemy.topSpeed * age, 0.0, 256.0));
        const auto reach = radius + possibleApproach;
        const auto nearMember = std::ranges::any_of(units, [&enemy, reach](const UnitSnapshot& unit) {
            return distanceSquared(enemy.position, unit.position) <= reach * reach;
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

}  // namespace protodd
