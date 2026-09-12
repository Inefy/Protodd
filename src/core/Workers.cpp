#include "protodd/Workers.hpp"

#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace protodd {
namespace {

int militiaDemand(const UnitSnapshot& enemy, const Frame frame) {
    if (!enemy.visible || !enemy.detected || enemy.flying || enemy.hallucination) return 0;
    if (isWorker(enemy.kind)) return 1;
    if (isBuilding(enemy.kind)) {
        if (enemy.completed) return 0;
        if (enemy.kind == UnitKind::photonCannon || enemy.kind == UnitKind::bunker ||
            enemy.kind == UnitKind::sunkenColony) {
            return 4;
        }
        if (enemy.kind == UnitKind::pylon) return 2;
        if (enemy.kind == UnitKind::gateway || enemy.kind == UnitKind::barracks ||
            enemy.kind == UnitKind::forge) {
            return 3;
        }
        return 1;
    }

    // Worker surrounds are an emergency bridge until the first combat units
    // arrive, never a general answer to ranged or high-tier armies.
    if (frame < 7 * 60 * 24 && enemy.kind == UnitKind::zergling) return 2;
    if (frame < 6 * 60 * 24 && enemy.kind == UnitKind::zealot) return 3;
    // A detected Dark Templar is still a short-range melee unit, and it can
    // erase the entire mineral line after the mobile screen has been traded
    // away.  Treat it like a small emergency surround target rather than
    // allowing healthy Probes to keep mining underneath the cloak alarm.
    if (enemy.kind == UnitKind::darkTemplar && frame < 16 * 60 * 24) return 3;
    if (frame < 5 * 60 * 24 && enemy.kind == UnitKind::marine) return 1;
    return 0;
}

int targetPriority(const UnitSnapshot& enemy) {
    if (!enemy.completed && (enemy.kind == UnitKind::photonCannon ||
                             enemy.kind == UnitKind::bunker ||
                             enemy.kind == UnitKind::sunkenColony)) {
        return 4;
    }
    if (isWorker(enemy.kind)) return 3;
    if (!enemy.completed && isBuilding(enemy.kind)) return 2;
    return 1;
}

}  // namespace

UnitId selectMineralPatch(
    const std::span<const MineralPatchCandidate> candidates,
    const Position mineralLine,
    const Position workerPosition,
    const UnitId currentTarget) noexcept {
    auto selected = UnitId{-1};
    auto bestScore = std::numeric_limits<long long>::max();
    for (const auto& patch : candidates) {
        if (patch.id < 0 || !patch.position.valid()) continue;
        const auto load = std::max(0, patch.assignedWorkers);
        const auto anchorDistance = distanceSquared(patch.position, mineralLine);
        const auto workerDistance = distanceSquared(patch.position, workerPosition);
        const auto stabilityBonus = patch.id == currentTarget ? 5'000'000LL : 0LL;
        const auto score = static_cast<long long>(load) * 1'000'000'000LL +
                           static_cast<long long>(anchorDistance) * 4LL +
                           static_cast<long long>(workerDistance) - stabilityBonus;
        if (score < bestScore || (score == bestScore &&
                                  (selected < 0 || patch.id < selected))) {
            bestScore = score;
            selected = patch.id;
        }
    }
    return selected;
}

const std::unordered_map<UnitId, UnitId>& MineralAllocator::assign(
    const std::span<const MineralWorker> workers,
    const std::span<const MineralPatchCandidate> patches) {
    std::unordered_map<UnitId, UnitId> retained;
    std::unordered_map<UnitId, int> load;
    for (const auto& worker : workers) {
        const auto previous = targets_.find(worker.id);
        const auto target = previous != targets_.end() ? previous->second : worker.currentTarget;
        const auto patch = std::ranges::find(patches, target, &MineralPatchCandidate::id);
        if (patch != patches.end() &&
            distanceSquared(patch->position, worker.mineralLine) <= 480 * 480) {
            retained[worker.id] = target;
            ++load[target];
        }
    }
    for (const auto& worker : workers) {
        const auto previous = retained.find(worker.id);
        const auto current = previous != retained.end() ? previous->second : -1;
        if (current >= 0) --load[current];
        std::vector<MineralPatchCandidate> candidates;
        for (const auto& patch : patches) {
            if (distanceSquared(patch.position, worker.mineralLine) <= 480 * 480) {
                candidates.push_back({patch.id, patch.position, load[patch.id]});
            }
        }
        const auto selected = selectMineralPatch(candidates, worker.mineralLine,
                                                 worker.position, current);
        if (selected >= 0) {
            retained[worker.id] = selected;
            ++load[selected];
        }
    }
    targets_ = std::move(retained);
    return targets_;
}

int GasBankController::target(const GameState& state, const StrategicPlan& plan) {
    if (state.frame < lastFrame_) paused_ = false;
    lastFrame_ = state.frame;
    auto highWater = 300;
    for (const auto& goal : plan.goals) {
        if (!goal.blocking || goal.target == UnitKind::unknown) continue;
        const auto existing = std::ranges::count(state.self.units, goal.target, &UnitSnapshot::kind) +
            std::ranges::count(state.self.queuedUnits, goal.target);
        if (existing < goal.desiredCount)
            highWater = std::max(highWater, unitStats(goal.target).gas + 100);
    }
    // Separate stop/resume thresholds prevent every 50-mineral spend or
    // strategic plan flip from shuffling workers between gas and minerals.
    if (paused_ && (state.self.gas < highWater / 2 || state.self.minerals >= 300)) paused_ = false;
    if (!paused_ && state.self.minerals < 150 && state.self.gas >= highWater) paused_ = true;
    return paused_ ? 0 : std::max(0, plan.desiredGasWorkers);
}

std::vector<WorkerAssignment> WorkerManager::assign(
    const GameState& state,
    const StrategicPlan& plan,
    const InfluenceMap& influence,
    const std::span<const UnitId> reservedBuilders) const {
    std::vector<const UnitSnapshot*> workers;
    for (const auto& unit : state.self.units) {
        if (isWorker(unit.kind) && unit.completed) {
            workers.push_back(&unit);
        }
    }
    std::ranges::sort(workers, {}, [](const UnitSnapshot* worker) { return worker->id; });

    std::unordered_set<UnitId> builders(reservedBuilders.begin(), reservedBuilders.end());
    std::vector<WorkerAssignment> result;
    result.reserve(workers.size());

    std::vector<const BaseSnapshot*> ownedBases;
    for (const auto& base : state.bases) {
        if (base.ownerId == state.self.id && base.center.valid()) {
            ownedBases.push_back(&base);
        }
    }
    if (ownedBases.empty() && !workers.empty()) {
        // After the last Nexus is destroyed, surviving Probes must keep mining
        // while the recovery Nexus is constructed. Use the nearest safe-ish
        // resource cluster as a temporary economy anchor.
        const BaseSnapshot* fallback = nullptr;
        auto bestDistance = std::numeric_limits<int>::max();
        for (const auto& base : state.bases) {
            if (!base.center.valid() || base.mineralsRemaining <= 0) continue;
            const auto candidate = distanceSquared(workers.front()->position, base.center);
            if (candidate < bestDistance) {
                bestDistance = candidate;
                fallback = &base;
            }
        }
        if (fallback != nullptr) ownedBases.push_back(fallback);
    }
    std::ranges::sort(ownedBases, {}, [](const BaseSnapshot* base) { return base->id; });
    const auto safeBase = safestOwnedBase(state, influence);
    const auto hasCompletedStaticScreen = std::ranges::any_of(
        state.self.units, [](const UnitSnapshot& unit) {
            return unit.completed && (unit.kind == UnitKind::photonCannon ||
                                      unit.kind == UnitKind::shieldBattery);
        });
    std::vector<const UnitSnapshot*> available;
    available.reserve(workers.size());

    for (const auto* worker : workers) {
        if (builders.contains(worker->id)) {
            result.push_back({worker->id, WorkerJob::build, -1, -1, {-1, -1}, 100});
            continue;
        }

        const auto rangedBio = std::ranges::min_element(
            state.enemy.units, {}, [worker, &ownedBases](const UnitSnapshot& enemy) {
                const auto relevant = enemy.visible && enemy.detected && enemy.completed &&
                                      enemy.kind == UnitKind::marine &&
                                      enemy.position.valid() &&
                                      std::ranges::any_of(
                                          ownedBases, [&enemy](const BaseSnapshot* base) {
                                              return distanceSquared(enemy.position,
                                                                     base->center) <
                                                     512 * 512;
                                          });
                return relevant
                           ? distanceSquared(worker->position, enemy.position)
                           : std::numeric_limits<int>::max();
            });
        if (hasCompletedStaticScreen && rangedBio != state.enemy.units.end() &&
            rangedBio->visible && rangedBio->kind == UnitKind::marine &&
            distanceSquared(worker->position, rangedBio->position) < 288 * 288) {
            const Position away{
                worker->position.x + worker->position.x - rangedBio->position.x,
                worker->position.y + worker->position.y - rangedBio->position.y,
            };
            result.push_back({worker->id, WorkerJob::evacuate,
                              safeBase != nullptr ? safeBase->id : -1,
                              rangedBio->id,
                              influence.safestStep(worker->position, away, false), 99});
            continue;
        }

        const auto local = influence.at(worker->position);
        const auto woundedNearThreat = worker->healthFraction() < 0.75;
        if ((worker->healthFraction() < 0.35 || woundedNearThreat) &&
            local.groundThreat > 0.25F && safeBase != nullptr) {
            const auto threat = std::ranges::min_element(
                state.enemy.units, {}, [worker](const UnitSnapshot& enemy) {
                    return enemy.visible && enemy.position.valid() &&
                                   enemy.groundWeapon.damage > 0
                               ? distanceSquared(worker->position, enemy.position)
                               : std::numeric_limits<int>::max();
                });
            const auto threatId = threat != state.enemy.units.end() &&
                                          threat->visible &&
                                          threat->groundWeapon.damage > 0
                                      ? threat->id
                                      : -1;
            const auto escape = influence.safestStep(
                worker->position, safeBase->mineralLine, false);
            result.push_back({worker->id, WorkerJob::evacuate, safeBase->id,
                              threatId, escape, 98});
            continue;
        }

        // An undetected DT cannot be a militia target. Only workers within
        // its approach radius escape; safe workers keep the detection funded.
        const auto cloakedNearBase = std::ranges::min_element(
            state.enemy.units, {}, [worker](const UnitSnapshot& enemy) {
                return enemy.visible && enemy.position.valid() &&
                               enemy.kind == UnitKind::darkTemplar &&
                               !enemy.detected
                           ? distanceSquared(enemy.position, worker->position)
                           : std::numeric_limits<int>::max();
            });
        if (safeBase != nullptr && cloakedNearBase != state.enemy.units.end() &&
            cloakedNearBase->visible && cloakedNearBase->position.valid() &&
            cloakedNearBase->kind == UnitKind::darkTemplar &&
            !cloakedNearBase->detected &&
            distanceSquared(worker->position, cloakedNearBase->position) <= 160 * 160) {
            const Position away{2 * worker->position.x - cloakedNearBase->position.x,
                                2 * worker->position.y - cloakedNearBase->position.y};
            const auto escape = influence.safestStep(
                worker->position, away, false);
            result.push_back({worker->id, WorkerJob::evacuate, safeBase->id,
                              cloakedNearBase->id, escape, 99});
            continue;
        }
        available.push_back(worker);
    }

    // A worker militia only answers threats for which worker contact is useful:
    // workers, unfinished proxies, and tiny opening-unit groups. In particular,
    // never feed Probes into tanks or completed static defenses.
    struct MilitiaTarget {
        const UnitSnapshot* unit{};
        int demand{};
    };
    std::vector<MilitiaTarget> baseThreats;
    const auto localEnemyWorkers = std::ranges::count_if(
        state.enemy.units, [&ownedBases](const UnitSnapshot& enemy) {
            return enemy.visible && isWorker(enemy.kind) && enemy.position.valid() &&
                   std::ranges::any_of(ownedBases, [&enemy](const BaseSnapshot* base) {
            return distanceSquared(enemy.position, base->center) < 640 * 640;
                   });
        });
    for (const auto& enemy : state.enemy.units) {
        auto demand = militiaDemand(enemy, state.frame);
        // A militia time limit must not also switch off worker protection.
        const auto meleeDanger = enemy.visible && enemy.completed && !enemy.hallucination &&
                                 (enemy.kind == UnitKind::zealot || enemy.kind == UnitKind::zergling ||
                                  enemy.kind == UnitKind::darkTemplar);
        if ((demand == 0 && !meleeDanger) || !enemy.position.valid()) continue;
        // Probes cannot close on ranged bio efficiently. Once a Cannon/Battery
        // screen exists, charging Marines only donates the economy and blocks
        // the combat units that should be using that screen.
        if (enemy.kind == UnitKind::marine && hasCompletedStaticScreen) continue;
        if (isWorker(enemy.kind) && localEnemyWorkers < 3) {
            const auto attackingProbe = enemy.orderTargetId >= 0 &&
                std::ranges::any_of(workers, [&enemy](const UnitSnapshot* worker) {
                    return worker->id == enemy.orderTargetId;
                });
            const auto hurtingMineralLine = std::ranges::any_of(
                workers, [&enemy](const UnitSnapshot* worker) {
                    return worker->underAttack &&
                           distanceSquared(worker->position, enemy.position) <= 96 * 96;
                });
            // Ignore a harmless scouting worker, but immediately surround one
            // that has started attacking the mineral line. One defending Probe
            // merely trades hits with an SCV; two can prevent the repeat kills
            // seen while the rest of the line continued mining.
            if (!attackingProbe && !hurtingMineralLine) continue;
            demand = std::max(demand, 2);
        }
        if (std::ranges::any_of(ownedBases, [&enemy](const BaseSnapshot* base) {
            return distanceSquared(enemy.position, base->center) < 640 * 640;
            })) {
            baseThreats.push_back({&enemy, demand});
        }
    }
    const auto localArmy = std::ranges::count_if(
        state.self.units, [&baseThreats](const UnitSnapshot& unit) {
            return unit.completed && isCombatUnit(unit.kind) && !unit.flying &&
                   std::ranges::any_of(baseThreats, [&unit](const MilitiaTarget& target) {
                       return distanceSquared(unit.position, target.unit->position) < 576 * 576;
                   });
        });
    // Do not turn a defended mineral line into a second melee squad.  The
    // previous demand calculation sent up to eight Probes into every visible
    // Zealot wave even when four-to-six Zealots/Dragoons were already in
    // contact.  Those workers were then lost, the mineral income collapsed,
    // and the next reinforcement cycle never arrived.  Keep militia as a
    // true last resort: it becomes eligible again if the mobile screen has
    // been wiped down to one or fewer nearby combat units.
    const auto requestedDefenders = std::accumulate(
        baseThreats.begin(), baseThreats.end(), 0,
        [localArmy](const int total, const MilitiaTarget& target) {
            const auto melee = target.unit->kind == UnitKind::zealot ||
                               target.unit->kind == UnitKind::zergling ||
                               target.unit->kind == UnitKind::darkTemplar;
            return total + (melee && localArmy >= 2 ? 0 : target.demand);
        });
    // Protect workers close to melee even after militia recruitment expires.
    // The adapter mineral-walks toward safer patches and balances their load.
    const auto visibleMeleeThreats = std::ranges::count_if(
        baseThreats, [](const MilitiaTarget& target) {
            return target.unit->kind == UnitKind::zealot ||
                   target.unit->kind == UnitKind::zergling ||
                   target.unit->kind == UnitKind::darkTemplar;
        });
    // Do not wait for a perfect two-unit surround before moving the workers.
    // In live games the first Zealot often occupies the home screen while the
    // second and third are still crossing the ramp; by the time localArmy
    // reaches two, the mineral line has already been trapped.  After five
    // minutes, even one visible melee unit is enough evidence to evacuate if
    // only one of our mobile units is nearby.  The local-army check keeps a
    // healthy screen mining while it can actually contest the contact.
    const auto evacuationScreen = localArmy >= 2 ||
                                  (state.frame >= 5 * 60 * 24 &&
                                   visibleMeleeThreats >= 1 &&
                                   localArmy <= 1);
    if (evacuationScreen && safeBase != nullptr) {
        const UnitSnapshot* closestMelee = nullptr;
        auto closestDistance = std::numeric_limits<int>::max();
        for (const auto& target : baseThreats) {
            const auto melee = target.unit->kind == UnitKind::zealot ||
                               target.unit->kind == UnitKind::zergling ||
                               target.unit->kind == UnitKind::darkTemplar;
            if (!melee) continue;
            const auto toBase = distanceSquared(target.unit->position, safeBase->center);
            if (toBase < closestDistance) {
                closestDistance = toBase;
                closestMelee = target.unit;
            }
        }
        if (closestMelee != nullptr && closestDistance <= 640 * 640) {
            // Evacuate only the exposed edge of the line.  A full-line
            // evacuation looks safe for one frame but strands the bot with no
            // minerals for replacement Zealots, Cannons, or Robotics.  Keep
            // a mining floor behind the mobile screen, just as Stardust's
            // worker manager does while its vanguard holds the ramp.
            const auto keepMining = localArmy >= 2 ? 6 : 4;
            auto evacuationBudget = std::max(
                0, static_cast<int>(available.size()) - keepMining);
            for (auto worker = available.begin(); worker != available.end();) {
                if (evacuationBudget <= 0) break;
                // Select the danger for this worker, not the unit nearest
                // the safest base. Distant perimeter sightings leave mining alone.
                const UnitSnapshot* workerThreat = nullptr;
                auto workerThreatDistance = 160 * 160 + 1;
                for (const auto& target : baseThreats) {
                    if (target.unit->kind != UnitKind::zealot &&
                        target.unit->kind != UnitKind::zergling &&
                        target.unit->kind != UnitKind::darkTemplar) continue;
                    const auto separation = distanceSquared((*worker)->position, target.unit->position);
                    if (separation < workerThreatDistance) {
                        workerThreatDistance = separation;
                        workerThreat = target.unit;
                    }
                }
                if (workerThreat == nullptr) {
                    ++worker;
                    continue;
                }
                // A mineral-line target is not an escape target: on Python the
                // safest patch is often still inside the Zealot's path. Move
                // directly away from the closest attacker and let the BWAPI
                // adapter resume mining only after separation is restored.
                const Position away{
                    (*worker)->position.x +
                        ((*worker)->position.x - workerThreat->position.x),
                    (*worker)->position.y +
                        ((*worker)->position.y - workerThreat->position.y),
                };
                const auto escape = influence.safestStep(
                    (*worker)->position, away, false);
                result.push_back({(*worker)->id, WorkerJob::evacuate, safeBase->id,
                                  workerThreat->id, escape, 97});
                worker = available.erase(worker);
                --evacuationBudget;
            }
        }
    }
    const auto meleeBreach = std::ranges::any_of(
        baseThreats, [&ownedBases](const MilitiaTarget& target) {
            if (target.unit->kind != UnitKind::zealot &&
                target.unit->kind != UnitKind::zergling &&
                target.unit->kind != UnitKind::darkTemplar) {
                return false;
            }
            return std::ranges::any_of(ownedBases, [&target](const BaseSnapshot* base) {
                return distanceSquared(target.unit->position, base->center) < 320 * 320;
            });
        });
    const auto economyCap = meleeBreach
                                ? std::min(10, std::max(
                                      4, static_cast<int>(available.size()) - 2))
                                : (workers.size() >= 10U
                                       ? static_cast<int>(available.size() / 2U)
                                       : std::min(6, std::max(
                                             4, static_cast<int>(available.size()) - 2)));
    auto defendersRemaining = std::clamp(
        requestedDefenders - static_cast<int>(localArmy) * 2, 0,
        std::min(8, economyCap));
    while (defendersRemaining > 0 && !available.empty()) {
        auto bestWorker = available.end();
        const UnitSnapshot* bestTarget = nullptr;
        auto bestScore = std::numeric_limits<long long>::max();
        for (auto worker = available.begin(); worker != available.end(); ++worker) {
            // When an actual rush is already inside the base, carrying a
            // mineral must not exempt a healthy Probe from the emergency
            // surround. Cargo is disposable; the Nexus and worker line are
            // not. Wounded Probes are still handled by the evacuation pass.
            if ((*worker)->healthFraction() < 0.5) continue;
            for (const auto& target : baseThreats) {
                if (target.demand <= 0) continue;
                if (!isBuilding(target.unit->kind)) {
                    const auto isMelee = target.unit->groundWeapon.maxRange <= 32;
                    // Once a melee threat is inside the base perimeter, a
                    // Probe that is still mining can reach it before the
                    // mineral line is erased.  The old 320px gate left the
                    // militia idle while a Zealot pack fought the last
                    // standing Zealots just outside the Nexus; use a wider
                    // 640px contact window for short-range attackers while
                    // keeping ranged units on their weapon-range leash.
                    const auto contactRange = isMelee
                                                  ? 640
                                                  : target.unit->groundWeapon.maxRange + 128;
                    if (distanceSquared((*worker)->position, target.unit->position) >
                        contactRange * contactRange) {
                        continue;
                    }
                }
                const auto priorityBias = 5 - targetPriority(*target.unit);
                const auto score = static_cast<long long>(priorityBias) * 1'000'000LL +
                                   distanceSquared((*worker)->position,
                                                   target.unit->position);
                if (score < bestScore) {
                    bestScore = score;
                    bestWorker = worker;
                    bestTarget = target.unit;
                }
            }
        }
        if (bestWorker == available.end() || bestTarget == nullptr) break;
        result.push_back({(*bestWorker)->id, WorkerJob::defend, -1, bestTarget->id,
                          bestTarget->position, 90});
        available.erase(bestWorker);
        const auto assignedTarget = std::ranges::find(
            baseThreats, bestTarget, &MilitiaTarget::unit);
        if (assignedTarget != baseThreats.end()) --assignedTarget->demand;
        --defendersRemaining;
    }

    // Each completed assimilator has exactly three efficient worker slots.
    // Pair gas workers with a concrete base so multi-base economies do not
    // repeatedly drag probes across the map.
    struct GasSlot {
        const BaseSnapshot* base{};
        const UnitSnapshot* refinery{};
    };
    std::vector<GasSlot> gasSlots;
    for (const auto& building : state.self.units) {
        if (building.kind != UnitKind::assimilator || !building.completed) continue;
        const auto base = std::ranges::min_element(
            ownedBases, {}, [&building](const BaseSnapshot* candidate) {
                return distanceSquared(building.position, candidate->center);
        });
        // A refinery can survive after its Nexus falls. It no longer belongs
        // to the nearest surviving economy across the map: doing that sends
        // replacement gas workers back through the army that destroyed it.
        const auto exposed = std::ranges::any_of(state.enemy.units,
            [&state, &building](const UnitSnapshot& enemy) {
                return enemy.position.valid() && enemy.completed && !isWorker(enemy.kind) &&
                    enemy.groundWeapon.damage > 0 && !enemy.disabled &&
                    (enemy.visible || state.frame - enemy.lastSeen <= 8 * 24) &&
                    distanceSquared(enemy.position, building.position) <=
                        (enemy.groundWeapon.maxRange + 128) * (enemy.groundWeapon.maxRange + 128);
            });
        if (base != ownedBases.end() &&
            distanceSquared(building.position, (*base)->center) <= 384 * 384 && !exposed) {
            for (auto slot = 0; slot < 3; ++slot) {
                gasSlots.push_back({*base, &building});
            }
        }
    }
    const auto effectiveDesiredGas = gasBank_.target(state, plan);
    const auto desiredGas = std::min(
        {effectiveDesiredGas, static_cast<int>(gasSlots.size()),
         // Gas cannot replace lost Probes. Preserve enough unleased workers
         // on minerals even when the strategic gas request predates a raid.
         std::max(0, static_cast<int>(available.size()) - 6)});
    auto gasAssigned = 0;
    const auto safeGasRoute = [&influence](const UnitSnapshot* worker, const GasSlot& slot) {
        return distanceSquared(worker->position, slot.refinery->position) <= 384 * 384 ||
            influence.maximumGroundThreat(worker->position, slot.refinery->position) <= 0.25F;
    };

    // Keep workers that are already on the requested refinery. Re-selecting
    // the probes nearest the Nexus every worker tick used to rotate mineral
    // workers onto gas and gas workers back to minerals, losing mining time.
    for (auto worker = available.begin(); worker != available.end() &&
                                    gasAssigned < desiredGas;) {
        auto slot = std::ranges::find_if(
            gasSlots, [worker, &safeGasRoute](const GasSlot& candidate) {
                return candidate.refinery->id == (*worker)->orderTargetId && safeGasRoute(*worker, candidate);
            });
        if (slot == gasSlots.end() && (*worker)->gatheringGas) {
            // ReturnGas targets the Nexus, and workers inside a refinery can
            // temporarily have no target. Preserve their mining cycle too.
            slot = std::ranges::min_element(gasSlots, {}, [worker](const GasSlot& candidate) {
                return distanceSquared(candidate.refinery->position, (*worker)->position);
            });
            if (slot != gasSlots.end() &&
                (distanceSquared(slot->refinery->position, (*worker)->position) > 480 * 480 ||
                 !safeGasRoute(*worker, *slot))) {
                slot = gasSlots.end();
            }
        }
        if (slot == gasSlots.end()) {
            ++worker;
            continue;
        }
        result.push_back({(*worker)->id, WorkerJob::gas, slot->base->id, slot->refinery->id,
                          slot->refinery->position, 55});
        worker = available.erase(worker);
        gasSlots.erase(slot);
        ++gasAssigned;
    }

    while (gasAssigned < desiredGas && !gasSlots.empty()) {
        const auto slot = gasSlots.begin();
        const auto worker = std::ranges::min_element(
            available, {}, [slot, &safeGasRoute](const UnitSnapshot* candidate) {
                return safeGasRoute(candidate, *slot)
                    ? distanceSquared(candidate->position, slot->refinery->position)
                    : std::numeric_limits<int>::max();
            });
        if (worker == available.end()) break;
        if (!safeGasRoute(*worker, *slot)) {
            gasSlots.erase(slot);
            continue;
        }
        result.push_back({(*worker)->id, WorkerJob::gas, slot->base->id, slot->refinery->id,
                          slot->refinery->position, 55});
        available.erase(worker);
        gasSlots.erase(slot);
        ++gasAssigned;
    }

    // Greedily equalize mineral saturation while retaining a small distance
    // bias. A transfer order is explicit so the adapter retargets workers that
    // are already gathering at an oversaturated base.
    std::unordered_map<int, int> assignedPerBase;
    for (const auto* worker : available) {
        const BaseSnapshot* bestBase = nullptr;
        auto bestScore = std::numeric_limits<double>::infinity();
        for (const auto* base : ownedBases) {
            const auto patches = base->mineralPatches > 0 ? base->mineralPatches : 8;
            const auto capacity = std::clamp(patches * 2, 4, 16);
            const auto saturation = static_cast<double>(assignedPerBase[base->id] + 1) /
                                    static_cast<double>(capacity);
            const auto travel = distance(worker->position, base->center) / 2048.0;
            const auto remote = distanceSquared(worker->position, base->center) > 640 * 640;
            if (remote && influence.maximumGroundThreat(worker->position, base->center) > 0.25F)
                continue;
            const auto local = influence.at(base->center);
            const auto depletion = base->mineralsRemaining > 0 ? 0.0 : 100.0;
            const auto score = saturation + travel * 0.18 +
                               static_cast<double>(local.groundThreat) * 1.5 + depletion;
            if (score < bestScore) {
                bestScore = score;
                bestBase = base;
            }
        }
        if (bestBase == nullptr) {
            result.push_back({worker->id, WorkerJob::idle, -1, -1, {-1, -1}, 0});
            continue;
        }
        ++assignedPerBase[bestBase->id];
        const auto transfer = distanceSquared(worker->position, bestBase->center) > 640 * 640;
        result.push_back({worker->id,
                          transfer ? WorkerJob::transfer : WorkerJob::minerals,
                          bestBase->id, -1, bestBase->mineralLine, 50});
    }

    std::ranges::sort(result, {}, &WorkerAssignment::worker);
    return result;
}

const BaseSnapshot* WorkerManager::safestOwnedBase(
    const GameState& state,
    const InfluenceMap& influence) {
    const BaseSnapshot* best = nullptr;
    auto bestScore = std::numeric_limits<double>::infinity();
    for (const auto& base : state.bases) {
        if (base.ownerId != state.self.id || !base.center.valid()) {
            continue;
        }
        const auto cell = influence.at(base.center);
        const auto score = static_cast<double>(cell.groundThreat + cell.airThreat) -
                           static_cast<double>(base.mineralsRemaining) / 100000.0;
        if (score < bestScore) {
            bestScore = score;
            best = &base;
        }
    }
    return best;
}

}  // namespace protodd
