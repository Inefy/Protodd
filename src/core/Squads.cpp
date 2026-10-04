#include "protodd/Squads.hpp"

#include "protodd/Scouting.hpp"
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
    // Match combat evaluation: zero health on an undetected enemy is an
    // unavailable observation, not evidence of a wounded attacker.
    const auto vitality = !unit.ours && !unit.detected && unit.durability() == 0
        ? 1.0 : unit.healthFraction();
    return unitStats(unit.kind).combatValue *
           std::clamp(vitality, 0.15, 1.0);
}

Position defensiveScreen(const GameState& state, const BaseSnapshot& base) {
    // The terrain anchor can be hundreds of pixels ahead of this economy.
    // It is an assembly position, not a safe destination for a losing army.
    if (base.defense.valid()) return moveToward(base.center, base.defense.anchor, 144.0);
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
    const Position fallbackRetreat, const NavigationGrid* navigation,
    const bool emergencyConsolidation, const bool limitStaticCoverage) const {
    std::vector<Squad> result;
    std::unordered_set<UnitId> assigned;
    auto nextId = 1;

    // Size economic defense by recently observed combat value. Briefly losing
    // sight of a ranged contain must not release its defenders across the map.
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
            if ((!unit.visible && state.frame - unit.lastSeen > 3 * 24) ||
                !unit.position.valid() || !unit.completed ||
                unit.disabled || (unit.groundWeapon.damage <= 0 &&
                                  unit.role != UnitRole::spellcaster) ||
                nearestOwnedBase(state, unit.position) != &base ||
                (distanceSquared(unit.position, base.center) > 800 * 800 &&
                 (!base.defense.valid() || distanceSquared(unit.position, base.defense.entrance) > 640 * 640))) {
                continue;
            }
            nearby.push_back(unit);
            threatPower += allocationPower(unit);
        }
        // The breakout exemption requires an army actually contesting this
        // perimeter. A strategic flag cannot leave a natural undefended while
        // that army travels to a distant expansion or fights somewhere else.
        if (plan.breakContainment && !nearby.empty() &&
            std::ranges::none_of(nearby, [&base](const UnitSnapshot& unit) {
                return distanceSquared(unit.position, base.center) <= 320 * 320;
            })) {
            auto fieldPower = 0.0;
            auto mobilePower = 0.0;
            for (const auto& ally : friendly) {
                if (!ally.completed || ally.disabled || ally.loaded || ally.hallucination ||
                    isStaticDefense(ally.kind) || !isCombatUnit(ally.kind)) continue;
                const auto power = allocationPower(ally);
                mobilePower += power;
                // An army behind its own choke is present even when no shot
                // is in range yet. Requiring enemy proximity here would make
                // the defensive leash prevent its own breakout condition.
                if (distanceSquared(ally.position, base.center) <= 960 * 960 ||
                    std::ranges::any_of(nearby, [&ally](const UnitSnapshot& unit) {
                        return ally.canAttack(unit) &&
                            distanceSquared(ally.position, unit.position) <= 640 * 640;
                    })) fieldPower += power;
            }
            // In PvT the same nearby Tanks and mines can be a real contain.
            // Release the field group only when it also has a substantial
            // local power lead; otherwise retain the base-defense response.
            const auto localBreakoutLead = state.enemy.race != Race::terran ||
                fieldPower >= threatPower * 1.60;
            if (fieldPower > 0.0 && fieldPower >= mobilePower * 0.60 &&
                localBreakoutLead) continue;
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
        const auto defenseMargin = plan.posture == Posture::defend ? 1.45 : 1.30;
        const auto targetPower = highestThreat * defenseMargin + 0.35;
        auto committedPower = 0.0;
        for (const auto& unit : defense.units) committedPower += allocationPower(unit);
        if (limitStaticCoverage) {
            // The isolated coverage experiment limits static credit to the
            // attackers in range, leaving mobile units for uncovered threats.
            auto coveredThreatPower = 0.0;
            for (const auto& threat : baseThreats) {
                if (!threat.visible || !threat.detected || threat.invincible ||
                    threat.loaded || threat.hallucination) continue;
                const auto covered = std::ranges::any_of(defense.units,
                    [&threat](const UnitSnapshot& defender) {
                        const auto& weapon = threat.flying ? defender.airWeapon : defender.groundWeapon;
                        const auto separation = weaponDistance(defender, threat);
                        return defender.canAttack(threat) && separation >= weapon.minRange &&
                            separation <= weapon.maxRange;
                    });
                if (covered) coveredThreatPower += allocationPower(threat);
            }
            committedPower = std::min(committedPower, coveredThreatPower * defenseMargin);
        }
        const auto minimumMobile = std::min<std::size_t>(2, candidates.size());
        const auto visibleAttackers = std::ranges::count_if(baseThreats,
            [](const UnitSnapshot& threat) {
                return threat.visible && threat.groundWeapon.damage > 0 &&
                    isCombatUnit(threat.kind);
            });
        // A small static screen can satisfy the power quota while the mobile
        // army remains detached. When a large observed group is contesting
        // the economy, commit the field force to the same defense authority.
        const auto emergency = emergencyConsolidation && baseThreats.size() >= 6 &&
            visibleAttackers >= 2 && candidates.size() >= 8;
        const auto committedMinimum = emergency
            ? std::min(candidates.size(),
                       std::max<std::size_t>(8, (candidates.size() * 3 + 3) / 4))
            : minimumMobile;
        defense.emergencyDefense = emergency;
        auto mobileCount = std::size_t{0};
        for (const auto& candidate : candidates) {
            if (mobileCount >= committedMinimum && committedPower >= targetPower) break;
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
                guard.center = centroid(guard.units);
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

    // The adapter's army span excludes workers. Add only workers that can
    // immediately surround a member or are visibly pursuing one, after squad
    // allocation, so a remote mineral line cannot absorb defensive reserves.
    for (auto& squad : result) {
        for (const auto& worker : state.enemy.units) {
            if (!isWorker(worker.kind) || !worker.visible || !worker.detected ||
                !worker.completed || worker.disabled || worker.loaded || worker.hallucination ||
                worker.invincible || !worker.position.valid() || worker.durability() <= 0 ||
                std::ranges::find(squad.enemies, worker.id, &UnitSnapshot::id) != squad.enemies.end()) continue;
            const auto defending = std::ranges::any_of(squad.units, [&worker, navigation](const UnitSnapshot& member) {
                if (!member.completed || member.disabled || member.loaded || member.hallucination ||
                    member.invincible || !member.position.valid() || !worker.canAttack(member)) return false;
                const auto& weapon = member.flying ? worker.airWeapon : worker.groundWeapon;
                const auto range = weaponDistance(worker, member);
                if (range < weapon.minRange ||
                    (range > weapon.maxRange + 32 &&
                     (worker.orderTargetId != member.id || range > 160))) return false;
                // A known cliff separates two close positions. If either
                // coarse origin is unavailable, retain the visible danger.
                return navigation == nullptr || navigation->empty() ||
                    !navigation->walkable(worker.position) || !navigation->walkable(member.position) ||
                    navigation->lineWalkable(worker.position, member.position);
            });
            if (defending) squad.enemies.push_back(worker);
        }
        std::ranges::sort(squad.enemies, {}, &UnitSnapshot::id);
    }

    // Strategic cloak risk should accelerate Observer production, not tether
    // every ground squad to one. A squad requests an escort only after its own
    // local contact list contains a cloaked, burrowed, or undetected threat.
    std::ranges::sort(result, {}, &Squad::id);
    return result;
}

std::optional<Position> SquadPlanner::threatenedNaturalRally(
    const GameState& state, const StrategicPlan& plan) {
    if ((plan.posture != Posture::hold && plan.posture != Posture::defend) ||
        !plan.rallyPoint.valid()) return std::nullopt;
    const auto home = std::ranges::find_if(state.bases, [&state](const BaseSnapshot& base) {
        return base.ownerId == state.self.id && base.startLocation && base.center.valid();
    });
    if (home == state.bases.end()) return std::nullopt;

    const BaseSnapshot* threatened = nullptr;
    auto mostAttackers = 0;
    for (const auto& base : state.bases) {
        if (base.ownerId != state.self.id || base.startLocation || !base.center.valid() ||
            distanceSquared(base.center, plan.rallyPoint) <= 640 * 640) continue;
        const auto attackers = std::ranges::count_if(state.enemy.units, [&state, &base](
            const UnitSnapshot& enemy) {
            return enemy.completed && !enemy.loaded && !enemy.hallucination &&
                !enemy.invincible && enemy.position.valid() && isCombatUnit(enemy.kind) &&
                (enemy.visible || (enemy.lastSeen > 0 &&
                                   state.frame - enemy.lastSeen <= 8 * 24)) &&
                distanceSquared(enemy.position, base.center) <= 900 * 900;
        });
        if (attackers >= 3 && attackers > mostAttackers) {
            threatened = &base;
            mostAttackers = static_cast<int>(attackers);
        }
    }
    if (threatened == nullptr) return std::nullopt;
    // Assemble behind the attacked Nexus, toward our main. The base-defense
    // squad still owns the immediate interception, so this moves only the
    // otherwise uncommitted main force toward reinforcement range.
    return moveToward(threatened->center, home->center, 128.0);
}

bool SquadPlanner::mobileDetectionReady(
    const GameState& state, const Squad& squad,
    const InfluenceMap* influence, const UnitId assignedObserver) noexcept {
    if (!squad.needsDetection) return true;
    const auto ahead = squad.objective.valid() ? moveToward(squad.center, squad.objective, 128.0) : squad.center;
#ifdef PROTODD_SAFE_OBSERVER_COVERAGE
    if (assignedObserver < 0 || influence == nullptr) return false;
    return std::ranges::any_of(state.self.units, [&state, &squad, ahead, influence, assignedObserver](const UnitSnapshot& observer) {
        if (observer.id != assignedObserver || observer.kind != UnitKind::observer ||
            !observer.completed || observer.disabled || observer.loaded ||
            observer.hallucination || !observer.position.valid() ||
            ScoutManager::observerInDanger(state, observer, *influence)) return false;
        const auto radius = std::max(0, (observer.sightRange > 0 ? observer.sightRange : 288) - 32);
        return distanceSquared(observer.position, squad.center) <= radius * radius &&
               distanceSquared(observer.position, ahead) <= radius * radius;
    });
#else
    (void)influence;
    (void)assignedObserver;
    return std::ranges::any_of(state.self.units, [&squad, ahead](const UnitSnapshot& observer) {
        if (observer.kind != UnitKind::observer || !observer.completed || observer.disabled ||
            observer.loaded || observer.hallucination || observer.healthFraction() < 0.25 ||
            !observer.position.valid()) return false;
        const auto radius = std::max(0, (observer.sightRange > 0 ? observer.sightRange : 288) - 32);
        return distanceSquared(observer.position, squad.center) <= radius * radius &&
               distanceSquared(observer.position, ahead) <= radius * radius;
    });
#endif
}

bool SquadPlanner::mobileDetectionBlocksAdvance(
    const GameState& state, const Squad& squad,
    const InfluenceMap* influence, const UnitId assignedObserver) noexcept {
    return squad.role != SquadRole::baseDefense &&
        !mobileDetectionReady(state, squad, influence, assignedObserver);
}

std::vector<Command> SquadPlanner::detectorEscorts(
    const GameState& state,
    const std::span<const Squad> squads,
    const InfluenceMap& influence,
    const bool mobilizeReserveAgainstLurkers,
    const bool centerBlockedMainEscort,
    const bool mobilizeContestedReserve,
    const bool directSafeRendezvous) const {
    const auto assignments = detectorEscortAssignments(
        state, squads, influence, mobilizeReserveAgainstLurkers,
        centerBlockedMainEscort, mobilizeContestedReserve,
        directSafeRendezvous, {}, false);
    std::vector<Command> commands;
    commands.reserve(assignments.size());
    for (const auto& assignment : assignments) commands.push_back(assignment.order);
    return commands;
}

std::vector<DetectorEscortAssignment> SquadPlanner::detectorEscortAssignments(
    const GameState& state,
    const std::span<const Squad> squads,
    const InfluenceMap& influence,
    const bool mobilizeReserveAgainstLurkers,
    const bool centerBlockedMainEscort,
    const bool mobilizeContestedReserve,
    const bool directSafeRendezvous,
    const std::span<const UnitId> unavailableObservers,
    const bool blockedSquadsOnly) const {
    std::vector<const UnitSnapshot*> observers;
    for (const auto& unit : state.self.units) {
        if (unit.kind == UnitKind::observer && unit.completed && !unit.loaded &&
            !unit.disabled && !unit.hallucination && unit.position.valid() &&
            std::ranges::find(unavailableObservers, unit.id) == unavailableObservers.end() &&
            !ScoutManager::observerInDanger(state, unit, influence))
            observers.push_back(&unit);
    }
    std::ranges::sort(observers, {}, [](const UnitSnapshot* unit) { return unit->id; });

    const auto lurkerAtHome = mobilizeReserveAgainstLurkers &&
        std::ranges::any_of(state.enemy.units, [&state](const UnitSnapshot& enemy) {
            if (enemy.kind != UnitKind::lurker || !enemy.completed ||
                !enemy.position.valid() ||
                (!enemy.visible && state.frame - enemy.lastSeen > 3 * 24))
                return false;
            return std::ranges::any_of(state.bases, [&state, &enemy](const BaseSnapshot& base) {
                return base.ownerId == state.self.id && base.center.valid() &&
                       distanceSquared(enemy.position, base.center) <= 900 * 900;
            });
        });

    std::vector<const Squad*> priorities;
    for (const auto& squad : squads) {
        if (blockedSquadsOnly && !squad.needsDetection) continue;
        if (squad.role == SquadRole::baseDefense && squad.enemies.empty() &&
            !squad.needsDetection) continue;
        if (!squad.units.empty()) priorities.push_back(&squad);
    }
    std::ranges::sort(priorities, [lurkerAtHome](const Squad* left, const Squad* right) {
        if (left->needsDetection != right->needsDetection)
            return left->needsDetection > right->needsDetection;
        // In a home Lurker fight, give the larger force its detection before
        // a small remote guard. A static Cannon can still cover the guard's
        // base, while an undetected main army cannot advance at all.
        if (lurkerAtHome && left->needsDetection &&
            left->units.size() != right->units.size())
            return left->units.size() > right->units.size();
        if (left->role != right->role)
            return left->role == SquadRole::baseDefense;
        return left->units.size() > right->units.size();
    });

    std::vector<DetectorEscortAssignment> result;
    // Once a second Observer exists, keep one available for strategic
    // scouting. Fragmented armies can otherwise lease every Observer as an
    // escort, leaving no unit to watch a siege push before it reaches a base.
    const auto detectionDemand = static_cast<std::size_t>(std::ranges::count_if(
        priorities, [](const Squad* squad) { return squad->needsDetection; }));
    const auto blockedMainGroups = std::ranges::count_if(priorities, [](const Squad* squad) {
        return squad->role == SquadRole::mainArmy && squad->needsDetection &&
               !squad->enemies.empty();
    });
    // A scout reserve has less value than two active main groups unable to
    // fire through mines or cloak. Keep the reserve when one escort suffices.
    const auto contestedReserve = mobilizeContestedReserve &&
        blockedMainGroups >= 2 && detectionDemand >= observers.size();
    const auto escortCapacity = lurkerAtHome || contestedReserve
        ? std::min(observers.size(), detectionDemand)
        : (observers.size() > 1 ? observers.size() - 1 : observers.size());
    const auto count = blockedSquadsOnly
        ? std::min(escortCapacity, detectionDemand)
        : std::min(escortCapacity, priorities.size());
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto* squad = priorities[i];
        // A blocked main army needs the Observer to cover its next 128px step.
        // Parking 96px behind the center can leave that step just outside the
        // conservative sight radius and hold the army under enemy fire.
        const auto anchor = centerBlockedMainEscort &&
                            squad->role == SquadRole::mainArmy && squad->needsDetection
            ? squad->center : moveToward(squad->center, squad->retreat, 96.0);
#ifndef PROTODD_SAFE_OBSERVER_COVERAGE
        auto atAnchor = *observers.front();
        atAnchor.position = anchor;
        if (!anchor.valid() ||
            ScoutManager::observerInDanger(state, atAnchor, influence)) continue;
#else
        if (!anchor.valid()) continue;
#endif
        const auto closest = std::min_element(observers.begin() + static_cast<std::ptrdiff_t>(i),
            observers.end(), [anchor](const UnitSnapshot* left, const UnitSnapshot* right) {
#ifdef PROTODD_SAFE_OBSERVER_COVERAGE
                const auto leftReady = left->healthFraction() >= 0.50;
                const auto rightReady = right->healthFraction() >= 0.50;
#else
                const auto leftReady = left->healthFraction() >= 0.25;
                const auto rightReady = right->healthFraction() >= 0.25;
#endif
                if (leftReady != rightReady) return leftReady;
                const auto a = distanceSquared(left->position, anchor);
                const auto b = distanceSquared(right->position, anchor);
                return a != b ? a < b : left->id < right->id;
            });
        std::iter_swap(observers.begin() + static_cast<std::ptrdiff_t>(i), closest);
        const auto* observer = observers[i];
#ifdef PROTODD_SAFE_OBSERVER_COVERAGE
        auto atAnchor = *observer;
        atAnchor.position = anchor;
        if (ScoutManager::observerInDanger(state, atAnchor, influence)) continue;
#endif
        auto destination = influence.safestStep(
            observer->position, anchor, true, directSafeRendezvous);
        // A local gradient can keep a cloaked Observer circling just outside
        // coverage while the army waits for detection. On a clear air corridor,
        // issue the actual rendezvous point and let pathing close the gap.
        if (directSafeRendezvous && squad->role == SquadRole::mainArmy &&
            squad->needsDetection && anchor.valid() && observer->position.valid() &&
            distanceSquared(observer->position, anchor) <= 384 * 384) {
            auto clearCorridor = true;
            const auto steps = std::max(1, static_cast<int>(std::ceil(
                distance(observer->position, anchor) / 64.0)));
            for (auto step = 0; step <= steps; ++step) {
                const auto fraction = static_cast<double>(step) / steps;
                const Position point{
                    observer->position.x + static_cast<int>(std::lround(
                        (anchor.x - observer->position.x) * fraction)),
                    observer->position.y + static_cast<int>(std::lround(
                        (anchor.y - observer->position.y) * fraction)),
                };
                const auto cell = influence.at(point);
#ifdef PROTODD_SAFE_OBSERVER_COVERAGE
                if ((cell.airThreat > 0.05F &&
                     (!observer->cloaked || cell.detection > 0.05F)) ||
                    influence.stormDanger(point) > 0.05F) {
#else
                if (cell.airThreat > 0.05F || cell.detection > 0.05F ||
                    influence.stormDanger(point) > 0.05F) {
#endif
                    clearCorridor = false;
                    break;
                }
            }
            if (clearCorridor) destination = anchor;
        }
        result.push_back({squad->id, {observer->id, CommandType::move, -1, destination,
                          UnitKind::unknown, squad->needsDetection ? 96 : 72, 0,
                          "detector-escort"}});
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

MainArmyTravelMode SquadPlanner::mainArmyTravelMode(
    const Squad& squad,
    const Squad* vanguard,
    const bool aggressive,
    const int minimumAttackSize) noexcept {
    // One squad must meet the commitment size. Summing disconnected groups
    // would send each fragment into contact as if the full force were there.
    if (!aggressive || vanguard == nullptr ||
        vanguard->units.size() < static_cast<std::size_t>(std::max(1, minimumAttackSize)))
        return MainArmyTravelMode::assemble;
    return vanguard == &squad ? MainArmyTravelMode::attack
                              : MainArmyTravelMode::joinVanguard;
}

Position SquadPlanner::favorableTerranFrontTarget(
    const GameState& state, const Squad& squad,
    const CombatEstimate& estimate) noexcept {
    // The frozen empty policy can turn an advantaged PvT field army back to
    // its rally while Terran units occupy a mining-base approach.
    // Commit only the formed, unassigned force after its actual local fight
    // estimate accepts contact; the separately allocated base guards remain.
    if (state.enemy.race != Race::terran ||
        squad.role != SquadRole::mainArmy || squad.withdrawing ||
        !squad.center.valid() || squad.units.size() < 16 ||
        estimate.advanceBlocked || estimate.decision != FightDecision::engage ||
        estimate.ratio < 1.8 ||
        std::ranges::count_if(state.self.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::nexus && unit.completed;
        }) < 1) return {-1, -1};

    const auto visibleContact = std::ranges::count_if(
        squad.enemies, [&squad](const UnitSnapshot& enemy) {
            return enemy.visible && enemy.detected && enemy.completed &&
                enemy.kind != UnitKind::spiderMine && isCombatUnit(enemy.kind) &&
                enemy.position.valid() &&
                distanceSquared(squad.center, enemy.position) <= 640 * 640;
        });
    const UnitSnapshot* best = nullptr;
    auto bestScore = std::numeric_limits<double>::infinity();
    for (const auto& enemy : squad.enemies) {
        if (!enemy.visible || !enemy.detected || !enemy.completed ||
            enemy.loaded || enemy.invincible || enemy.hallucination ||
            !enemy.position.valid() || enemy.kind == UnitKind::spiderMine ||
            (!isCombatUnit(enemy.kind) && enemy.kind != UnitKind::scienceVessel) ||
            distanceSquared(squad.center, enemy.position) > 640 * 640 ||
            !std::ranges::any_of(state.bases, [&state, &enemy](const BaseSnapshot& base) {
                return base.ownerId == state.self.id && base.center.valid() &&
                    distanceSquared(base.center, enemy.position) <= 800 * 800;
            }) ||
            (enemy.kind != UnitKind::siegeTank && visibleContact < 2)) continue;
        const auto targeters = std::ranges::count_if(
            squad.units, [&enemy](const UnitSnapshot& ally) {
                return ally.completed && !ally.disabled && !ally.loaded &&
                    ally.canAttack(enemy);
            });
        if (targeters < std::max<std::ptrdiff_t>(8, squad.units.size() / 3))
            continue;
        const auto priority = enemy.kind == UnitKind::siegeTank ? 240.0 :
            enemy.kind == UnitKind::goliath ? 120.0 :
            enemy.kind == UnitKind::vulture ? 100.0 :
            enemy.kind == UnitKind::wraith ? 80.0 : 0.0;
        const auto score = distance(squad.center, enemy.position) - priority;
        if (score < bestScore ||
            (score == bestScore && (best == nullptr || enemy.id < best->id))) {
            bestScore = score;
            best = &enemy;
        }
    }
    return best != nullptr ? best->position : Position{-1, -1};
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
            unit.attackFrame || unit.attackWindup || unit.underAttack || unit.underStorm || unit.healthFraction() < 0.65 ||
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

Position SquadPlanner::reinforcementDestination(
    const Squad& squad, const Squad& vanguard, const Position attackTarget) {
    const auto fighters = std::ranges::count_if(squad.units, [](const UnitSnapshot& unit) {
        return isCombatUnit(unit.kind) && !isBuilding(unit.kind) && unit.completed &&
            !unit.disabled && !unit.loaded && !unit.hallucination &&
            (unit.groundWeapon.damage > 0 || unit.airWeapon.damage > 0);
    });
    // Reinforcement growth can make a rear component the largest army. That
    // must not recall an already viable assault group from the enemy doorstep.
    // Tiny detachments still regroup; contact is evaluated before this policy.
    if (fighters >= 6 && attackTarget.valid() && squad.center.valid() &&
        vanguard.center.valid() && distance(squad.center, attackTarget) + 192 <
            distance(vanguard.center, attackTarget)) return attackTarget;
    return vanguard.center;
}

DefenseArea SquadPlanner::defensiveEngagementArea(
    const Squad& squad, const CombatEstimate& estimate) {
    if (squad.role != SquadRole::baseDefense || !squad.defense.economyCenter.valid() ||
        squad.enemies.empty() || estimate.decision != FightDecision::engage ||
        estimate.advanceBlocked || estimate.ratio < 1.5) return squad.defense;
    auto mobileCount = 0;
    auto mobilePower = 0.0;
    auto staticPower = 0.0;
    for (const auto& member : squad.units) {
        if (!member.completed || member.disabled || member.loaded || member.hallucination) continue;
        if (isBuilding(member.kind)) {
            staticPower += allocationPower(member);
        } else if (isCombatUnit(member.kind) &&
            distanceSquared(member.position, squad.defense.economyCenter) <= 960 * 960 &&
            std::ranges::any_of(squad.enemies, [&member](const UnitSnapshot& target) {
                return target.visible && target.detected && member.canAttack(target);
            })) {
            ++mobileCount;
            mobilePower += allocationPower(member);
        }
    }
    // A large mobile army must be able to clear its own perimeter. Otherwise
    // fresh vision reallocates the advancing force into a 256px Cannon leash,
    // pulling it home and leaving the remaining main squad too weak to fight.
    // Small guards and forces relying on static fire retain their tight screen.
    if (mobileCount < 12 || mobilePower < staticPower * 3.0) return squad.defense;
    return {squad.defense.economyCenter, 960, squad.defense.economyCenter};
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
    const auto targetable = [](const UnitSnapshot& candidate) {
        // The engagement estimate may remember a unit after vision is lost.
        // Only an identified, currently visible BWAPI unit can receive an
        // Attack_Unit order. Unknown spell effects such as Scanner Sweep can
        // appear as visible enemy units but reject attack commands.
        return candidate.kind != UnitKind::unknown && candidate.visible &&
            candidate.detected && candidate.position.valid() &&
            !candidate.loaded && !candidate.invincible && !candidate.hallucination &&
            candidate.durability() > 0;
    };
    std::vector<UnitSnapshot> targets;
    for (const auto& candidate : squad.enemies) {
        if (targetable(candidate)) targets.push_back(candidate);
    }
    for (const auto& candidate : hostiles) {
        if (!targetable(candidate) ||
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

bool SquadPlanner::shouldCoverExpansion(
    const GameState& state, const StrategicPlan& plan,
    const bool coverForwardThird) noexcept {
    // Establishing the natural moves the whole defensive line out of the main.
    // Subsequent economic requests must not recall that field army, especially
    // to an unbuilt third behind an already secured front.
    if (!plan.expansionTarget.valid() || plan.posture == Posture::attack)
        return false;
    const auto completedNexuses = std::ranges::count_if(
        state.self.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::nexus && unit.completed;
        });
    if (completedNexuses < 2) return true;
    const auto thirdUnderConstruction = std::ranges::any_of(
        state.self.units, [&plan](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::nexus && !unit.completed &&
                unit.position.valid() &&
                distanceSquared(unit.position, plan.expansionTarget) <= 128 * 128;
        });
    if (!coverForwardThird || completedNexuses != 2 ||
        (plan.desiredBases < 3 && !thirdUnderConstruction) ||
        !plan.attackTarget.valid() ||
        plan.posture == Posture::defend || plan.posture == Posture::recover)
        return false;
    const auto mobileArmy = std::ranges::count_if(state.self.units,
        [](const UnitSnapshot& unit) {
            return unit.completed && !unit.disabled && !unit.loaded &&
                !unit.hallucination && !isBuilding(unit.kind) &&
                !isWorker(unit.kind) && isCombatUnit(unit.kind);
        });
    if (mobileArmy < 14) return false;
    const auto siteToEnemy = distance(plan.expansionTarget, plan.attackTarget);
    const auto alreadyForward = std::ranges::count_if(state.self.units,
        [&plan, siteToEnemy](const UnitSnapshot& unit) {
            return unit.completed && !unit.disabled && !unit.loaded &&
                !unit.hallucination && !isBuilding(unit.kind) &&
                !isWorker(unit.kind) && isCombatUnit(unit.kind) &&
                unit.position.valid() &&
                distance(unit.position, plan.attackTarget) + 128.0 < siteToEnemy;
        });
    if (alreadyForward * 2 >= mobileArmy) return false;
    auto frontDistance = std::numeric_limits<double>::infinity();
    for (const auto& nexus : state.self.units) {
        if (nexus.kind == UnitKind::nexus && nexus.completed &&
            nexus.position.valid())
            frontDistance = std::min(frontDistance,
                distance(nexus.position, plan.attackTarget));
    }
    // A third farther behind the existing front is an economy request, not a
    // reason to pull the main army off its current attack or defensive screen.
    return siteToEnemy + 256.0 < frontDistance;
}

bool SquadPlanner::survivingBaseUnderThreat(const GameState& state) noexcept {
    for (const auto& base : state.bases) {
        if (base.ownerId != state.self.id || !base.center.valid()) continue;
        auto nearbyAttackers = 0;
        for (const auto& enemy : state.enemy.units) {
            if (!enemy.position.valid() || !enemy.completed || enemy.disabled ||
                enemy.invincible || enemy.hallucination || enemy.loaded ||
                (!enemy.visible && (enemy.lastSeen <= 0 ||
                                    state.frame - enemy.lastSeen > 3 * 24)) ||
                (enemy.groundWeapon.damage <= 0 && enemy.role != UnitRole::spellcaster))
                continue;
            const auto separation = distanceSquared(enemy.position, base.center);
            if (separation <= 320 * 320) return true;
            if (separation <= 800 * 800 && ++nearbyAttackers >= 2) return true;
        }
    }
    return false;
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
        // A ranged contain remains dangerous when its rear units step out of
        // sight. Dropping them after eight seconds repeatedly made the front
        // look beatable, then reversed the army as soon as it gained vision.
        const auto memoryFrames = enemy.groundWeapon.maxRange >= 96 ? 20 * 24 : 8 * 24;
        if (!enemy.position.valid() || (!enemy.visible &&
            (age < 0 || (!isBuilding(enemy.kind) && age > memoryFrames)))) continue;
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
