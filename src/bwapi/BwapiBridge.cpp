#include "BwapiBridge.hpp"

#include "protodd/Combat.hpp"
#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace protodd::bwapi {
namespace {

using namespace BWAPI;

Position fromBwapi(const BWAPI::Position position) noexcept {
    return {position.x, position.y};
}

bool closeTo(const Position left, const Position right, const int radius) noexcept {
    return left.valid() && right.valid() && distanceSquared(left, right) <= radius * radius;
}

}  // namespace

void BwapiBridge::onStart() {
    enemyMemory_.clear();
    mineralAllocator_.reset();
    baseLastScouted_.clear();
    baseLastConfirmedEmpty_.clear();
    pendingBuilds_.clear();
    failedBuildSites_.clear();
    unitCommandLocks_.clear();
    recentAreaSpells_.clear();
    lastMacroStatus_ = "idle";
    discoverResourceClusters();
    const auto terrain = navigationGrid();
    std::vector<Position> approaches{{Broodwar->mapWidth() * 16, Broodwar->mapHeight() * 16}};
    for (const auto start : Broodwar->getStartLocations())
        approaches.push_back({start.x * 32 + 64, start.y * 32 + 48});
    for (auto& site : resourceSites_) {
        for (const auto approach : approaches) {
            if (distanceSquared(site.depotCenter, approach) < 384 * 384) continue;
            const auto defense = terrain.defensivePosition(site.depotCenter, approach);
            if (!defense.valid() || std::ranges::any_of(site.defenses,
                [&defense](const DefensivePosition& prior) {
                    return distanceSquared(prior.entrance, defense.entrance) < 160 * 160;
                })) continue;
            site.defenses.push_back(defense);
        }
    }
}

GameState BwapiBridge::observe() {
    GameState state;
    state.frame = Broodwar->getFrameCount();
    state.latencyFrames = Broodwar->getLatencyFrames();
    state.mapWidthPixels = Broodwar->mapWidth() * 32;
    state.mapHeightPixels = Broodwar->mapHeight() * 32;
    state.mapName = Broodwar->mapName();
    state.self = snapshotPlayer(Broodwar->self(), true);
    state.enemy = snapshotPlayer(Broodwar->enemy(), false);

    if (const auto enemy = Broodwar->enemy()) {
        for (const auto unit : enemy->getUnits()) {
            if (unit != nullptr && unit->exists() && unit->isVisible() &&
                !enemyMemory_.contains(unit->getID())) {
                remember(unit);
            }
        }
    }
    state.enemy.units.clear();
    state.enemy.units.reserve(enemyMemory_.size());
    for (auto iterator = enemyMemory_.begin(); iterator != enemyMemory_.end();) {
        auto& memory = iterator->second;
        const auto live = Broodwar->getUnit(memory.id);
        if (live != nullptr && live->exists() && live->isVisible()) {
            auto current = snapshotUnit(live, false);
            current.inheritObservationHistory(memory);
            memory = std::move(current);
        } else {
            const auto tile = BWAPI::TilePosition(memory.position.x / 32,
                                                  memory.position.y / 32);
            if (isBuilding(memory.kind) && tile.isValid() && Broodwar->isVisible(tile)) {
                iterator = enemyMemory_.erase(iterator);
                continue;
            }
            memory.visible = false;
            memory.underAttack = false;
        }
        state.enemy.units.push_back(memory);
        ++iterator;
    }
    std::ranges::sort(state.enemy.units, {}, &UnitSnapshot::id);

    // Only count visible, still-travelling shots whose damage semantics we
    // can identify unambiguously. Hitscan effects and impact animations must
    // never reserve damage that has already been applied to observed HP.
    std::vector<IncomingProjectile> incoming;
    for (const auto bullet : Broodwar->getBullets()) {
        if (bullet == nullptr || !bullet->exists() || !bullet->isVisible() ||
            bullet->getPlayer() != Broodwar->self()) continue;
        const auto source = bullet->getSource();
        const auto target = bullet->getTarget();
        if (source == nullptr || target == nullptr || !source->exists() ||
            !target->exists() || !target->isVisible() || !target->isDetected() ||
            target->getPlayer() != Broodwar->enemy()) continue;
        const auto supported =
            (source->getType() == BWAPI::UnitTypes::Protoss_Dragoon &&
             bullet->getType() == BWAPI::BulletTypes::Phase_Disruptor) ||
            (source->getType() == BWAPI::UnitTypes::Protoss_Photon_Cannon &&
             bullet->getType() == BWAPI::BulletTypes::STA_STS_Cannon_Overlay);
        if (!supported || !bullet->getPosition().isValid()) continue;
        const auto speed = std::hypot(bullet->getVelocityX(), bullet->getVelocityY());
        if (speed < 0.5 || bullet->getRemoveTimer() <= 0) continue;
        const auto flightFrames = distance(fromBwapi(bullet->getPosition()),
                                           fromBwapi(target->getPosition())) / speed;
        if (flightFrames > std::min(24, bullet->getRemoveTimer())) continue;
        incoming.push_back({bullet->getID(), source->getID(), target->getID()});
    }
    accountIncomingDamage(state, incoming);

    const auto frame = state.frame;
    std::erase_if(failedBuildSites_, [frame](const FailedBuildSite& site) {
        return site.expires <= frame;
    });
    std::erase_if(unitCommandLocks_, [frame](const auto& entry) {
        return entry.second <= frame;
    });
    for (auto& [kind, pending] : pendingBuilds_) {
        const auto builder = Broodwar->getUnit(pending.builder);
        if (builder != nullptr && builder->exists() &&
            (!pending.lastPosition.valid() ||
             distanceSquared(pending.lastPosition, fromBwapi(builder->getPosition())) >= 16 * 16)) {
            pending.lastPosition = fromBwapi(builder->getPosition());
            pending.lastProgress = frame;
        }
    }
    std::erase_if(pendingBuilds_, [this, frame](const auto& entry) {
        const auto kind = entry.first;
        const auto& pending = entry.second;
        const auto started = std::ranges::any_of(
            Broodwar->self()->getUnits(), [kind, &pending](const Unit unit) {
            if (unit == nullptr || !unit->exists() || toKind(unit->getType()) != kind) {
                return false;
            }
            // A nearby older Pylon/Gateway cannot fulfill this reservation.
            return fromBwapi(BWAPI::Position(unit->getTilePosition())) == pending.target;
        });
        if (started) return true;

        const auto rememberFailure = [this, kind, frame, &pending] {
            failedBuildSites_.push_back(
                {kind, pending.target, frame + 20 * 24});
        };

        const auto builder = Broodwar->getUnit(pending.builder);
        if (builder == nullptr || !builder->exists()) {
            rememberFailure();
            return true;
        }
        const auto age = frame - pending.issued;
        const auto expectedType = toBwapi(kind);
        const auto lastCommand = builder->getLastCommand();
        const auto commandedBuild = lastCommand.getType() == BWAPI::UnitCommandTypes::Build &&
            lastCommand.getUnitType() == expectedType &&
            fromBwapi(lastCommand.getTargetPosition()) == pending.target;
        if (kind != UnitKind::nexus && age > std::max(2 * 24, Broodwar->getLatencyFrames() + 12) &&
            frame - pending.lastProgress > 2 * 24) {
            // Keep travelling builders leased. A stalled order must be
            // cancelled before its resources and worker can be reassigned,
            // otherwise it may complete after a replacement is already sent.
            if (builder->getBuildType() == expectedType || commandedBuild) builder->stop();
            rememberFailure();
            return true;
        }
        // A pre-positioned expansion Probe is intentionally only moving until
        // its fogged footprint becomes commandable, so BWAPI reports no
        // build type during that interval. Ordinary construction leases still
        // require the explicit build type to prevent a redirected Probe from
        // blocking another macro action.
        const auto stillAssigned = pending.prepositioned || commandedBuild ||
                                   builder->getBuildType() == expectedType;
        // Give the command time to cross the latency boundary, then recover
        // quickly if another subsystem or the game rejected the order. Keep a
        // genuinely travelling builder reserved long enough for expansions.
        if (age <= std::max(12, Broodwar->getLatencyFrames() + 6)) return false;
        if (!stillAssigned) {
            rememberFailure();
            return true;
        }
        if (age >= 45 * 24) {
            if (builder->getBuildType() == expectedType || commandedBuild || pending.prepositioned)
                builder->stop();
            rememberFailure();
            return true;
        }
        return false;
    });
    state.bases = snapshotBases(state);
    return state;
}

NavigationGrid BwapiBridge::navigationGrid() const {
    const auto width = Broodwar->mapWidth();
    const auto height = Broodwar->mapHeight();
    std::vector<std::uint8_t> cells(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0U);
    auto elevation = cells;
    for (auto tileY = 0; tileY < height; ++tileY) {
        for (auto tileX = 0; tileX < width; ++tileX) {
            auto passable = 0;
            auto centerPassable = 0;
            for (auto walkY = 0; walkY < 4; ++walkY) {
                for (auto walkX = 0; walkX < 4; ++walkX) {
                    if (!Broodwar->isWalkable(tileX * 4 + walkX, tileY * 4 + walkY)) continue;
                    ++passable;
                    if (walkX >= 1 && walkX <= 2 && walkY >= 1 && walkY <= 2) {
                        ++centerPassable;
                    }
                }
            }
            const auto index = static_cast<std::size_t>(tileY) *
                                   static_cast<std::size_t>(width) +
                               static_cast<std::size_t>(tileX);
            cells[index] = passable >= 10 && centerPassable >= 3 ? 1U : 0U;
            elevation[index] = static_cast<std::uint8_t>(Broodwar->getGroundHeight(tileX, tileY) / 2);
        }
    }
    return {width, height, 32, std::move(cells), std::move(elevation)};
}

void BwapiBridge::remember(const BWAPI::Unit unit) {
    if (unit == nullptr || !unit->exists() || unit->getPlayer() != Broodwar->enemy() ||
        !unit->isVisible()) {
        return;
    }
    auto current = snapshotUnit(unit, false);
    if (const auto previous = enemyMemory_.find(unit->getID());
        previous != enemyMemory_.end()) {
        current.inheritObservationHistory(previous->second);
    } else {
        current.firstSeen = current.lastSeen;
    }
    enemyMemory_.insert_or_assign(unit->getID(), std::move(current));
}

void BwapiBridge::forget(const BWAPI::Unit unit) {
    if (unit != nullptr) {
        enemyMemory_.erase(unit->getID());
    }
}

std::vector<UnitId> BwapiBridge::reservedBuilders() const {
    std::vector<UnitId> result;
    result.reserve(pendingBuilds_.size());
    for (const auto& [kind, pending] : pendingBuilds_) {
        static_cast<void>(kind);
        if (pending.builder >= 0) result.push_back(pending.builder);
    }
    std::ranges::sort(result);
    return result;
}

bool BwapiBridge::execute(const Command& command) {
    const auto actor = Broodwar->getUnit(command.actor);
    if (actor == nullptr || !actor->exists() || actor->getPlayer() != Broodwar->self() ||
        !actor->isCompleted() || actor->isLoaded() || actor->isLockedDown() ||
        actor->isMaelstrommed() || actor->isStasised()) {
        return false;
    }
    if (const auto lock = unitCommandLocks_.find(command.actor);
        lock != unitCommandLocks_.end() &&
        lock->second > Broodwar->getFrameCount()) {
        return false;
    }

    switch (command.type) {
        case CommandType::move:
            return command.targetPosition.valid() &&
                   actor->move(toBwapiPosition(command.targetPosition));
        case CommandType::attackMove:
            return command.targetPosition.valid() &&
                   actor->attack(toBwapiPosition(command.targetPosition));
        case CommandType::attackUnit: {
            const auto target = Broodwar->getUnit(command.targetUnit);
            return target != nullptr && target->exists() && target->isVisible() &&
                   actor->attack(target);
        }
        case CommandType::train:
            return actor->train(toBwapi(command.targetKind));
        case CommandType::hold: return actor->holdPosition();
        case CommandType::stop: return actor->stop();
        case CommandType::recharge: {
            const auto battery = Broodwar->getUnit(command.targetUnit);
            return battery != nullptr && battery->exists() &&
                   battery->getPlayer() == Broodwar->self() &&
                   battery->getType() == UnitTypes::Protoss_Shield_Battery &&
                   actor->rightClick(battery);
        }
        case CommandType::load: {
            const auto target = Broodwar->getUnit(command.targetUnit);
            return target != nullptr && actor->load(target);
        }
        case CommandType::unload:
            return command.targetPosition.valid() &&
                   actor->unloadAll(toBwapiPosition(command.targetPosition));
        case CommandType::useTech: {
            const auto tech = toBwapiTech(command.technology);
            if (tech == TechTypes::None || !command.targetPosition.valid()) return false;
            if (command.technology == TechnologyKind::psionicStorm &&
                std::ranges::any_of(recentAreaSpells_, [&command](const SpellZone& zone) {
                    return closeTo(zone.center, command.targetPosition, 112);
                })) {
                return false;
            }
            const auto target = toBwapiPosition(command.targetPosition);
            if (!actor->canUseTech(tech, target) || !actor->useTech(tech, target)) {
                return false;
            }
            const auto frame = Broodwar->getFrameCount();
            unitCommandLocks_.insert_or_assign(
                command.actor, frame + std::max(12, Broodwar->getLatencyFrames() + 8));
            if (command.technology == TechnologyKind::psionicStorm) {
                recentAreaSpells_.push_back({command.targetPosition, frame + 72});
            }
            return true;
        }
        case CommandType::build:
        case CommandType::gather:
            return false;  // These require richer adapter-specific arguments.
    }
    return false;
}

int BwapiBridge::executeMacro(
    const std::span<const MacroAction> actions,
    const StrategicPlan& plan,
    const std::span<const UnitId> unavailableBuilders,
    const int maximumCommands) {
    auto issued = 0;
    std::string firstFailure;
    lastMacroStatus_ = actions.empty() ? "idle" : "saving";
    for (const auto& action : actions) {
        if (issued >= maximumCommands) break;
        // A blocking goal may be present solely to protect a future bank
        // (for example, a Forge or Reaver checkpoint whose prerequisite is
        // still under construction).  It is not executable this pass, but it
        // must not terminate the queue: lower-priority actions that already
        // have disjoint reservations still need to reach their producers.
        if (!action.reserved) continue;
        // A non-executable action is a deliberate reservation for a target
        // whose prerequisite is already under construction. Its resources
        // remain protected by the planner, but it must not block valid
        // commands funded from the remaining surplus.
        if (!action.executable) continue;
        lastMacroStatus_ = "command-rejected-a" +
                           std::to_string(static_cast<int>(action.action)) + "-" +
                           std::string(unitStats(action.target).name);
        bool success = false;
        switch (action.action) {
            case MacroActionKind::build:
            case MacroActionKind::expand:
                success = build(action, plan, unavailableBuilders);
                break;
            case MacroActionKind::train: success = train(action); break;
            case MacroActionKind::research:
            case MacroActionKind::upgrade: success = executeTechnology(action); break;
        }
        if (success) {
            ++issued;
            lastMacroStatus_ = "issued-" + std::string(unitStats(action.target).name);
        } else if (firstFailure.empty() || lastMacroStatus_.starts_with("build-")) {
            firstFailure = lastMacroStatus_;
        }
        // Every emitted reserved action has a disjoint allocation in the
        // ledger. A rejected placement keeps its allocation, but must not
        // freeze unrelated producers funded from the remaining surplus.
    }
    if (issued == 0 && !firstFailure.empty()) lastMacroStatus_ = firstFailure;
    return issued;
}

void BwapiBridge::executeWorkers(const std::span<const WorkerAssignment> assignments) {
    std::vector<MineralWorker> mineralWorkers;
    std::vector<MineralPatchCandidate> patches;
    for (const auto& assignment : assignments) {
        if (assignment.job != WorkerJob::minerals && assignment.job != WorkerJob::transfer) continue;
        const auto worker = Broodwar->getUnit(assignment.worker);
        if (worker == nullptr || !worker->exists() || !worker->isCompleted() ||
            worker->isConstructing()) continue;
        const auto target = worker->getOrderTarget();
        mineralWorkers.push_back({assignment.worker, fromBwapi(worker->getPosition()),
                                  assignment.targetPosition,
                                  target != nullptr && target->getType().isMineralField()
                                      ? target->getID() : -1});
    }
    for (const auto patch : Broodwar->getMinerals()) {
        if (patch != nullptr && patch->exists() && patch->getResources() > 0) {
            patches.push_back({patch->getID(), fromBwapi(patch->getPosition()), 0});
        }
    }
    const auto& mineralTargets = mineralAllocator_.assign(mineralWorkers, patches);
    std::unordered_map<UnitId, int> escapePatchLoad;
    for (const auto& [workerId, patchId] : mineralTargets) ++escapePatchLoad[patchId];

    for (const auto& assignment : assignments) {
        const auto worker = Broodwar->getUnit(assignment.worker);
        if (worker == nullptr || !worker->exists() || !worker->isCompleted() ||
            worker->isConstructing() || assignment.job == WorkerJob::build) {
            continue;
        }
        if (assignment.job == WorkerJob::evacuate && assignment.targetPosition.valid()) {
            // Mineral walking removes unit collision, which is crucial when a
            // damaged Probe must escape a melee surround. Prefer a patch that
            // increases separation from the unit attacking it; fall back to a
            // normal safe-step move when no useful patch exists.
            const auto threat = Broodwar->getUnit(assignment.targetUnit);
            Unit escapePatch = nullptr;
            auto bestScore = std::numeric_limits<long long>::max();
            if (threat != nullptr && threat->exists()) {
                for (const auto patch : Broodwar->getMinerals()) {
                    if (patch == nullptr || !patch->exists() ||
                        patch->getResources() <= 0 || worker->getDistance(patch) > 640) {
                        continue;
                    }
                    if (threat->getDistance(patch) <= threat->getDistance(worker) + 32) continue;
                    const auto unsafe = std::ranges::any_of(enemyMemory_, [patch](const auto& entry) {
                        const auto& enemy = entry.second;
                        if (!enemy.visible || !enemy.position.valid() ||
                            enemy.groundWeapon.damage <= 0) return false;
                        const auto margin = std::max(96, enemy.groundWeapon.maxRange + 64);
                        return distanceSquared(fromBwapi(patch->getPosition()), enemy.position) <
                               margin * margin;
                    });
                    if (unsafe) continue;
                    const auto score = static_cast<long long>(escapePatchLoad[patch->getID()]) *
                                           1'000'000LL + worker->getDistance(patch) * 64LL -
                                       (worker->getOrderTarget() == patch ? 4096LL : 0LL);
                    if (score < bestScore) {
                        bestScore = score;
                        escapePatch = patch;
                    }
                }
            }
            if (escapePatch != nullptr) ++escapePatchLoad[escapePatch->getID()];
            // Wait for the previous command to arrive before issuing another.
            // The old branch alternated move/gather inside the latency window.
            if (worker->getLastCommandFrame() + std::max(6, Broodwar->getLatencyFrames()) >=
                Broodwar->getFrameCount()) continue;
            if (escapePatch != nullptr) {
                if (worker->getOrderTarget() != escapePatch ||
                    !worker->isGatheringMinerals()) {
                    worker->gather(escapePatch);
                }
                continue;
            }
            worker->move(toBwapiPosition(assignment.targetPosition));
            continue;
        }
        if (assignment.job == WorkerJob::defend) {
            const auto target = Broodwar->getUnit(assignment.targetUnit);
            if (target != nullptr && target->exists() && target->isVisible()) {
                const auto race = Broodwar->enemy()->getRace();
                const auto healthLimit = race == Races::Protoss ? 16 :
                                         (race == Races::Terran ? 12 : 10);
                const auto shouldMineralWalk =
                    worker->getGroundWeaponCooldown() >
                        Broodwar->getRemainingLatencyFrames() + 6 ||
                    worker->getHitPoints() + worker->getShields() <= healthLimit;
                Unit escapePatch = nullptr;
                auto furthestDistance = 0;
                if (shouldMineralWalk) {
                    for (const auto patch : Broodwar->getMinerals()) {
                        if (patch == nullptr || !patch->exists() ||
                            patch->getResources() <= 0) {
                            continue;
                        }
                        const auto workerDistance = worker->getDistance(patch);
                        if (workerDistance < 10 || workerDistance > 480 ||
                            workerDistance < furthestDistance ||
                            target->getDistance(patch) < workerDistance) {
                            continue;
                        }
                        escapePatch = patch;
                        furthestDistance = workerDistance;
                    }
                }
                if (escapePatch != nullptr) {
                    if (worker->getOrderTarget() != escapePatch ||
                        !worker->isGatheringMinerals()) {
                        worker->gather(escapePatch);
                    }
                    continue;
                }
                // Attack orders are persistent. Reissuing the same order every
                // worker tick interrupts the short Probe attack animation and
                // turns a militia surround into harmless chasing.
                if (worker->isIdle() || worker->getOrderTarget() != target) {
                    worker->attack(target);
                }
            }
            continue;
        }
        if (worker->isCarryingGas() || worker->isCarryingMinerals()) {
            if (worker->isIdle()) {
                worker->returnCargo();
            }
            continue;
        }
        const auto anchor = assignment.targetPosition.valid()
                                ? toBwapiPosition(assignment.targetPosition)
                                : worker->getPosition();
        const auto currentTarget = worker->getOrderTarget();
        Unit target = nullptr;
        if (assignment.job == WorkerJob::gas) {
            target = Broodwar->getClosestUnit(
                anchor,
                Filter::IsOwned && Filter::IsCompleted && Filter::IsRefinery);
        } else if (assignment.job == WorkerJob::minerals ||
                   assignment.job == WorkerJob::transfer) {
            const auto selected = mineralTargets.find(assignment.worker);
            if (selected != mineralTargets.end()) {
                target = Broodwar->getUnit(selected->second);
            } else {
                target = Broodwar->getClosestUnit(anchor, Filter::IsMineralField);
            }
        }
        const auto atAssignedBase = currentTarget != nullptr &&
                                    closeTo(fromBwapi(currentTarget->getPosition()),
                                            assignment.targetPosition, 384);
        const auto wrongJob = assignment.job == WorkerJob::gas
                                  ? !worker->isGatheringGas() || !atAssignedBase
                                  : !worker->isGatheringMinerals() ||
                                        currentTarget != target;
        if (target != nullptr && (worker->isIdle() || wrongJob) &&
            worker->getLastCommandFrame() + 12 < Broodwar->getFrameCount()) {
            worker->gather(target);
        }
    }
}

void BwapiBridge::executeScouts(const std::span<const ScoutOrder> orders) {
    for (const auto& order : orders) {
        const auto scout = Broodwar->getUnit(order.scout);
        if (scout == nullptr || !scout->exists() || !scout->isCompleted() ||
            !order.target.valid()) {
            continue;
        }
        if (scout->getLastCommandFrame() + std::max(8, Broodwar->getLatencyFrames()) <
            Broodwar->getFrameCount()) {
            scout->move(toBwapiPosition(order.target));
        }
    }
}

void BwapiBridge::runMaintenance(const int mineralReserve, const int gasReserve) {
    const auto self = Broodwar->self();
    if (self == nullptr) {
        return;
    }
    const auto frame = Broodwar->getFrameCount();
    std::erase_if(recentAreaSpells_, [frame](const SpellZone& zone) {
        return zone.expires <= frame;
    });

    auto mineralBank = self->minerals();
    auto freeMinerals = std::max(0, mineralBank - mineralReserve);
    auto freeGas = std::max(0, self->gas() - gasReserve);
    std::unordered_set<UnitId> spellcastersCommitted;
    const auto reaverCapacity = self->getUpgradeLevel(UpgradeTypes::Reaver_Capacity) > 0
                                    ? 10
                                    : 5;
    const auto carrierCapacity = self->getUpgradeLevel(UpgradeTypes::Carrier_Capacity) > 0
                                     ? 8
                                     : 4;

    for (const auto unit : self->getUnits()) {
        if (unit == nullptr || !unit->exists() || !unit->isCompleted()) {
            continue;
        }
        const auto type = unit->getType();
        if (type == UnitTypes::Protoss_Reaver &&
            unit->getScarabCount() < reaverCapacity && unit->getTrainingQueue().empty()) {
            const auto ammo = UnitTypes::Protoss_Scarab;
            // The first payload makes an expensive unit useful immediately.
            // Repeated army reservations must not leave an empty Reaver unable
            // to buy a fifteen-mineral Scarab during the fight it was built for.
            const auto ammoBudget = unit->getScarabCount() < 2 ? mineralBank : freeMinerals;
            if (ammoBudget >= ammo.mineralPrice() && freeGas >= ammo.gasPrice() &&
                unit->canTrain(ammo) && unit->train(ammo)) {
                mineralBank -= ammo.mineralPrice();
                freeMinerals = std::max(0, mineralBank - mineralReserve);
                freeGas -= ammo.gasPrice();
            }
        } else if (type == UnitTypes::Protoss_Carrier &&
                   unit->getInterceptorCount() < carrierCapacity &&
                   unit->getTrainingQueue().empty() &&
                   (unit->getInterceptorCount() < 4 ? mineralBank : freeMinerals) >=
                       UnitTypes::Protoss_Interceptor.mineralPrice() &&
                   freeGas >= UnitTypes::Protoss_Interceptor.gasPrice() &&
                   unit->canTrain(UnitTypes::Protoss_Interceptor) &&
                   unit->train(UnitTypes::Protoss_Interceptor)) {
            mineralBank -= UnitTypes::Protoss_Interceptor.mineralPrice();
            freeMinerals = std::max(0, mineralBank - mineralReserve);
            freeGas -= UnitTypes::Protoss_Interceptor.gasPrice();
        }
    }

    if (self->hasResearched(TechTypes::Stasis_Field)) {
        for (const auto arbiter : self->getUnits()) {
            if (arbiter == nullptr || arbiter->getType() != UnitTypes::Protoss_Arbiter ||
                arbiter->getEnergy() < 100 || !arbiter->isCompleted()) continue;
            Unit best = nullptr;
            auto bestScore = 4.0;
            for (const auto enemy : Broodwar->enemy()->getUnits()) {
                if (enemy == nullptr || !enemy->exists() || !enemy->isVisible() ||
                    enemy->isStasised() || enemy->getType().isBuilding()) continue;
                if (std::ranges::any_of(recentAreaSpells_, [enemy](const SpellZone& zone) {
                        return closeTo(zone.center, fromBwapi(enemy->getPosition()), 112);
                    })) continue;
                auto enemyValue = 0.0;
                auto friendlyValue = 0.0;
                for (const auto nearby : Broodwar->getUnitsInRadius(enemy->getPosition(), 96)) {
                    if (nearby == nullptr || !nearby->exists() ||
                        nearby->getType().isBuilding()) continue;
                    const auto value = unitStats(toKind(nearby->getType())).combatValue;
                    if (nearby->getPlayer() == Broodwar->enemy()) enemyValue += value;
                    if (nearby->getPlayer() == self) friendlyValue += value;
                }
                const auto score = enemyValue - friendlyValue * 2.0;
                if (score > bestScore) {
                    bestScore = score;
                    best = enemy;
                }
            }
            if (best != nullptr &&
                arbiter->canUseTech(TechTypes::Stasis_Field, best->getPosition()) &&
                arbiter->useTech(TechTypes::Stasis_Field, best->getPosition())) {
                recentAreaSpells_.push_back(
                    {fromBwapi(best->getPosition()), frame + 100});
                spellcastersCommitted.insert(arbiter->getID());
                break;
            }
        }
    }

    // Recall a remote reinforcement ball onto an Arbiter that has established
    // a live position near valuable enemy units or structures. This is bounded
    // to one cast and requires enough recalled army value to justify 150 energy.
    if (self->hasResearched(TechTypes::Recall)) {
        auto recallIssued = false;
        for (const auto arbiter : self->getUnits()) {
            if (recallIssued || arbiter == nullptr || !arbiter->exists() ||
                !arbiter->isCompleted() ||
                arbiter->getType() != UnitTypes::Protoss_Arbiter ||
                arbiter->getEnergy() < 150 || arbiter->isUnderAttack() ||
                spellcastersCommitted.contains(arbiter->getID())) {
                continue;
            }
            auto destinationValue = 0.0;
            for (const auto enemy : Broodwar->getUnitsInRadius(arbiter->getPosition(), 640)) {
                if (enemy == nullptr || !enemy->exists() ||
                    enemy->getPlayer() != Broodwar->enemy()) continue;
                const auto kind = toKind(enemy->getType());
                destinationValue += enemy->getType().isBuilding()
                                        ? (enemy->getType().isResourceDepot() ? 2.0 : 0.35)
                                        : unitStats(kind).combatValue;
            }
            if (destinationValue < 2.0) continue;

            Unit bestCenter = nullptr;
            auto bestValue = 6.0;
            for (const auto candidate : self->getUnits()) {
                if (candidate == nullptr || !candidate->exists() ||
                    !candidate->isCompleted() || candidate->isFlying() ||
                    !isCombatUnit(toKind(candidate->getType())) ||
                    candidate->getDistance(arbiter) < 900) {
                    continue;
                }
                auto clusterValue = 0.0;
                for (const auto nearby : Broodwar->getUnitsInRadius(
                         candidate->getPosition(), 128, Filter::IsOwned)) {
                    if (nearby != nullptr && nearby->exists() && nearby->isCompleted() &&
                        !nearby->isFlying() && isCombatUnit(toKind(nearby->getType()))) {
                        clusterValue += unitStats(toKind(nearby->getType())).combatValue;
                    }
                }
                if (clusterValue > bestValue ||
                    (std::abs(clusterValue - bestValue) < 0.001 &&
                     (bestCenter == nullptr || candidate->getID() < bestCenter->getID()))) {
                    bestValue = clusterValue;
                    bestCenter = candidate;
                }
            }
            if (bestCenter != nullptr &&
                arbiter->canUseTech(TechTypes::Recall, bestCenter->getPosition()) &&
                arbiter->useTech(TechTypes::Recall, bestCenter->getPosition())) {
                spellcastersCommitted.insert(arbiter->getID());
                recallIssued = true;
            }
        }
    }

    // Feedback converts enemy caster energy directly into damage and is most
    // valuable before those units can cast. Prefer lethal, high-energy hits.
    for (const auto darkArchon : self->getUnits()) {
        if (darkArchon == nullptr || !darkArchon->isCompleted() ||
            darkArchon->getType() != UnitTypes::Protoss_Dark_Archon ||
            darkArchon->getEnergy() < 50) continue;
        Unit best = nullptr;
        auto bestScore = 49;
        for (const auto enemy : Broodwar->enemy()->getUnits()) {
            if (enemy == nullptr || !enemy->exists() || !enemy->isVisible() ||
                !enemy->getType().isSpellcaster() || enemy->getEnergy() <= 0) continue;
            const auto lethalBonus = enemy->getEnergy() >= enemy->getHitPoints() ? 200 : 0;
            const auto score = enemy->getEnergy() + lethalBonus;
            if (score > bestScore &&
                darkArchon->canUseTech(TechTypes::Feedback, enemy)) {
                bestScore = score;
                best = enemy;
            }
        }
        if (best != nullptr && darkArchon->useTech(TechTypes::Feedback, best)) break;
    }

    // Convert pairs of spent templar into durable splash units while retaining
    // at least two casters for the next energy cycle.
    std::vector<Unit> spentTemplar;
    for (const auto unit : self->getUnits()) {
        if (unit != nullptr && unit->exists() && unit->isCompleted() &&
            unit->getType() == UnitTypes::Protoss_High_Templar && unit->getEnergy() < 50) {
            spentTemplar.push_back(unit);
        }
    }
    std::ranges::sort(spentTemplar, {}, [](const Unit unit) { return unit->getID(); });
    if (spentTemplar.size() >= 4) {
        const auto first = spentTemplar[0];
        const auto second = *std::ranges::min_element(
            spentTemplar.begin() + 1, spentTemplar.end(), {}, [first](const Unit unit) {
                return first->getDistance(unit);
            });
        if (first->canUseTech(TechTypes::Archon_Warp, second)) {
            first->useTech(TechTypes::Archon_Warp, second);
        }
    }
}

void BwapiBridge::drawDebug(
    const GameState& state,
    const StrategicPlan& plan,
    const ThreatAssessment& threat,
    const DebugOverlay& debug) const {
    if (debug.level == 0) return;
    Broodwar->drawBoxScreen(4, 4, 636, debug.level == 1 ? 94 : 216, Colors::Black, true);
    auto y = 8;
    const auto row = [&y](const char* format, auto... args) {
        Broodwar->drawTextScreen(10, y, format, args...);
        y += 12;
    };
    const auto count = [&state](const UnitKind kind) {
        return static_cast<int>(std::ranges::count_if(state.self.units, [kind](const UnitSnapshot& unit) {
            return unit.kind == kind && unit.completed;
        }));
    };
    const auto weight = [&plan](const UnitKind kind) {
        const auto found = std::ranges::find(plan.composition, kind, &CompositionTarget::kind);
        return found == plan.composition.end() ? 0 : static_cast<int>(found->weight * 100.0 + 0.5);
    };
    row("Protodd | %.85s", plan.name.c_str());
    row("%s | Enemy: %s | uncertainty %.0f%%", postureName(plan.posture).data(),
        enemyPlanName(threat.mostLikely).data(), threat.uncertainty * 100.0);
    row("Army D/Z/R %d/%d/%d | mix %d/%d/%d%% | Probes %d/%d", count(UnitKind::dragoon),
        count(UnitKind::zealot), count(UnitKind::reaver), weight(UnitKind::dragoon),
        weight(UnitKind::zealot), weight(UnitKind::reaver), count(UnitKind::probe), plan.desiredWorkers);
    row("Rally %d,%d | Bases %d/%d | Expansion %s | Gas workers target %d",
        plan.rallyPoint.x, plan.rallyPoint.y, count(UnitKind::nexus), plan.desiredBases,
        plan.expansionTarget.valid() ? "COVERING SITE" : plan.sustainEconomy ? "GROWTH ENABLED" : "WAIT",
        plan.desiredGasWorkers);
    row("Macro: %.90s", lastMacroStatus_.c_str());
    const auto macroLimit = debug.level == 1 ? 1U : 3U;
    for (std::size_t i = 0; i < std::min<std::size_t>(macroLimit, debug.macro.size()); ++i) {
        const auto& action = debug.macro[i];
        const auto label = action.technology != TechnologyKind::none
            ? technologyStats(action.technology).name.data() : unitStats(action.target).name.data();
        row("%s %s (%dM %dG): %.52s", !action.executable ? "PREREQ" : action.reserved ? "READY" : "SAVING",
            label, action.minerals, action.gas, action.reason.c_str());
    }
    if (debug.level > 1) {
        for (std::size_t i = 0; i < std::min<std::size_t>(3, debug.squads.size()); ++i) {
            const auto& squad = debug.squads[i];
            if (squad.enemies > 0)
                row("%s %d vs %d | ratio %.2f / need %.2f", squad.role.c_str(),
                    squad.units, squad.enemies, squad.ratio, squad.required);
            else row("%s %d | no local enemy", squad.role.c_str(), squad.units);
            row("  %.72s -> %d,%d", squad.reason.c_str(), squad.objective.x, squad.objective.y);
        }
    }
    row("/debug: cycle detail/off/compact | /debug 0, 1, 2: select display");

    const auto marker = [](const Position point, const Color color, const char* label) {
        if (!point.valid()) return;
        Broodwar->drawCircleMap(point.x, point.y, 24, color);
        Broodwar->drawTextMap(point.x + 26, point.y, "%s", label);
    };
    marker(plan.rallyPoint, Colors::Cyan, "RALLY");
    marker(plan.attackTarget, Colors::Red, "STRATEGIC TARGET");
    marker(plan.expansionTarget, Colors::Green, "EXPANSION COVER");
    for (const auto& base : state.bases) {
        if (base.ownerId != state.self.id || !base.defense.valid()) continue;
        const auto& defense = base.defense;
        Broodwar->drawLineMap(defense.left.x, defense.left.y, defense.right.x, defense.right.y, Colors::Yellow);
        Broodwar->drawCircleMap(defense.anchor.x, defense.anchor.y,
            std::clamp(defense.width, 192, 320), Colors::Cyan);
        marker(defense.anchor, Colors::Cyan, defense.highGround ? "HIGH-GROUND HOLD" : "CHOKE HOLD");
        Broodwar->drawTextMap(defense.entrance.x, defense.entrance.y, "FRONT / no pursuit (%d px)", defense.width);
    }
    for (const auto& squad : debug.squads) {
        if (!squad.center.valid() || !squad.objective.valid()) continue;
        const auto color = squad.decision == FightDecision::retreat ? Colors::Red :
                           squad.decision == FightDecision::kite ? Colors::Yellow : Colors::Green;
        Broodwar->drawLineMap(squad.center.x, squad.center.y, squad.objective.x, squad.objective.y, color);
        Broodwar->drawTextMap(squad.center.x, squad.center.y - 20, "%s: %s", squad.role.c_str(), squad.reason.c_str());
    }
    for (const auto selected : Broodwar->getSelectedUnits()) {
        if (selected->getPlayer() != Broodwar->self()) continue;
        const auto found = debug.orders.find(selected->getID());
        if (found != debug.orders.end())
            Broodwar->drawTextMap(selected->getPosition().x, selected->getPosition().y + 20,
                                 "ORDER: %s", found->second.c_str());
    }
}

Race BwapiBridge::toRace(const BWAPI::Race race) noexcept {
    if (race == Races::Protoss) return Race::protoss;
    if (race == Races::Terran) return Race::terran;
    if (race == Races::Zerg) return Race::zerg;
    if (race == Races::Random) return Race::random;
    return Race::unknown;
}

DamageType BwapiBridge::toDamageType(const BWAPI::DamageType type) noexcept {
    if (type == DamageTypes::Explosive) return DamageType::explosive;
    if (type == DamageTypes::Concussive) return DamageType::concussive;
    if (type == DamageTypes::Ignore_Armor) return DamageType::ignoreArmor;
    return DamageType::normal;
}

UnitSize BwapiBridge::toUnitSize(const BWAPI::UnitSizeType type) noexcept {
    if (type == UnitSizeTypes::Small) return UnitSize::small;
    if (type == UnitSizeTypes::Medium) return UnitSize::medium;
    if (type == UnitSizeTypes::Large) return UnitSize::large;
    return UnitSize::unknown;
}

UnitRole BwapiBridge::roleOf(const BWAPI::UnitType type, const UnitKind kind) noexcept {
    if (type.isWorker()) return UnitRole::worker;
    if (type.isResourceDepot()) return UnitRole::resourceDepot;
    if (type.isDetector()) return UnitRole::detector;
    if (isStaticDefense(kind)) return UnitRole::staticDefense;
    if (type.spaceProvided() > 0) return UnitRole::transport;
    if (kind == UnitKind::highTemplar || kind == UnitKind::darkArchon ||
        kind == UnitKind::arbiter || kind == UnitKind::defiler ||
        kind == UnitKind::scienceVessel || kind == UnitKind::ghost ||
        kind == UnitKind::queen) return UnitRole::spellcaster;
    if (type.isBuilding() && type.canProduce()) return UnitRole::production;
    if (type.isBuilding()) return UnitRole::other;
    if (type.isFlyer()) return UnitRole::airArmy;
    return isCombatUnit(kind) ? UnitRole::groundArmy : UnitRole::other;
}

WeaponSnapshot BwapiBridge::weapon(const BWAPI::WeaponType type) noexcept {
    if (type == WeaponTypes::None || type == WeaponTypes::Unknown) {
        return {};
    }
    return {type.damageAmount(), type.damageCooldown(), type.minRange(), type.maxRange(),
            toDamageType(type.damageType()), type.targetsAir(), type.targetsGround(),
            type.damageFactor()};
}

UnitSnapshot BwapiBridge::snapshotUnit(const BWAPI::Unit unit, const bool ours) {
    const auto type = unit->getType();
    const auto kind = toKind(type);
    const auto buildTime = std::max(1, type.buildTime());
    auto result = UnitSnapshot{
        unit->getID(), type.getID(), kind, toRace(type.getRace()), roleOf(type, kind),
        fromBwapi(unit->getPosition()), fromBwapi(unit->getPosition()),
        Broodwar->getFrameCount(), unit->getHitPoints(), type.maxHitPoints(),
        unit->getShields(), type.maxShields(), unit->getEnergy(), type.armor(),
        std::clamp((buildTime - unit->getRemainingBuildTime()) * 100 / buildTime, 0, 100),
        std::max(unit->getGroundWeaponCooldown(), unit->getAirWeaponCooldown()),
        type.topSpeed(), weapon(type.groundWeapon()), weapon(type.airWeapon()), ours,
        unit->isCompleted(), unit->isFlying(), unit->isVisible(), unit->isDetected(),
        unit->isBurrowed(), unit->isCloaked(),
        unit->isCarryingGas() || unit->isCarryingMinerals(), unit->isUnderAttack(),
        unit->isHallucination(),
    };
    result.size = toUnitSize(type.size());
    result.powered = unit->isPowered();
    result.loaded = unit->isLoaded();
    const auto transport = unit->getTransport();
    result.transportId = transport != nullptr ? transport->getID() : -1;
    result.cargoSpace = type.spaceProvided() > 0 ? unit->getSpaceRemaining() : 0;
    result.sightRange = ours && unit->getPlayer() != nullptr
                            ? unit->getPlayer()->sightRange(type)
                            : type.sightRange();
    result.attackFrame = unit->isAttackFrame();
    const auto orderTarget = unit->getOrderTarget();
    result.orderTargetId = orderTarget != nullptr ? orderTarget->getID() : -1;
    result.underStorm = unit->isUnderStorm();
    result.firstSeen = result.lastSeen;
    result.dimensionLeft = type.dimensionLeft();
    result.dimensionRight = type.dimensionRight();
    result.dimensionUp = type.dimensionUp();
    result.dimensionDown = type.dimensionDown();
    result.disabled = unit->isLockedDown() || unit->isMaelstrommed() || unit->isStasised();
    result.invincible = unit->isInvincible() || unit->isStasised();
    result.gatheringGas = ours && unit->isGatheringGas();
    if (!ours && !result.completed) {
        // BWAPI's inside-only remaining-build-time field is zero for enemies.
        // Zero here means unavailable, not a construction that is 100% done.
        result.buildProgress = -1;
    }
    if (isBuilding(kind)) {
        const auto elapsed = result.completed ? buildTime :
                             (ours ? buildTime * result.buildProgress / 100 : 0);
        result.constructionStartUpperBound = std::max(0, result.lastSeen - elapsed);
    }
    result.groundWeapon.hits = std::max(result.groundWeapon.hits, type.maxGroundHits());
    result.airWeapon.hits = std::max(result.airWeapon.hits, type.maxAirHits());
    if (kind == UnitKind::reaver) result.ammo = unit->getScarabCount();
    if (kind == UnitKind::carrier) result.ammo = unit->getInterceptorCount();

    // BWAPI exposes the payload weapons on Scarabs/Interceptors rather than
    // their parent unit types. Model them on the controllable parent so combat
    // evaluation, influence, and focus fire do not treat these expensive units
    // as harmless. Bunkers similarly inherit four Marines' Gauss Rifles.
    if (kind == UnitKind::reaver) {
        result.groundWeapon = weapon(WeaponTypes::Scarab);
        // Scarab's BWAPI weapon range is 128; the controllable Reaver launches
        // from eight tiles and fires once per 60 frames.
        result.groundWeapon.maxRange = 8 * 32;
        result.groundWeapon.cooldown = 60;
    } else if (kind == UnitKind::carrier) {
        auto payload = weapon(WeaponTypes::Pulse_Cannon);
        payload.maxRange = 8 * 32;
        payload.hits = std::max(1, result.ammo);
        result.groundWeapon = payload;
        result.airWeapon = payload;
    } else if (kind == UnitKind::bunker) {
        auto garrison = weapon(WeaponTypes::Gauss_Rifle);
        garrison.maxRange += 2 * 32;
        garrison.hits = 4;
        result.groundWeapon = garrison;
        result.airWeapon = garrison;
    }
    if (ours && unit->getPlayer() != nullptr) {
        const auto owner = unit->getPlayer();
        result.armor = owner->armor(type);
        result.shieldArmor = owner->getUpgradeLevel(UpgradeTypes::Protoss_Plasma_Shields);
        result.topSpeed = owner->topSpeed(type);
        if (kind == UnitKind::reaver) {
            result.groundWeapon.damage = unit->getPlayer()->damage(WeaponTypes::Scarab);
        } else if (kind == UnitKind::carrier) {
            const auto damage = unit->getPlayer()->damage(WeaponTypes::Pulse_Cannon);
            result.groundWeapon.damage = damage;
            result.airWeapon.damage = damage;
        }
        if (type.groundWeapon() != WeaponTypes::None) {
            result.groundWeapon.damage = owner->damage(type.groundWeapon()) /
                                         std::max(1, type.groundWeapon().damageFactor());
            result.groundWeapon.maxRange = owner->weaponMaxRange(type.groundWeapon());
        }
        if (type.airWeapon() != WeaponTypes::None) {
            result.airWeapon.damage = owner->damage(type.airWeapon()) /
                                      std::max(1, type.airWeapon().damageFactor());
            result.airWeapon.maxRange = owner->weaponMaxRange(type.airWeapon());
        }
    }
    return result;
}

PlayerSnapshot BwapiBridge::snapshotPlayer(const BWAPI::Player player, const bool ours) {
    PlayerSnapshot result;
    if (player == nullptr) {
        return result;
    }
    result.id = player->getID();
    result.race = toRace(player->getRace());
    result.minerals = ours ? player->minerals() : 0;
    result.gas = ours ? player->gas() : 0;
    result.supplyUsed = ours ? player->supplyUsed() : 0;
    result.supplyTotal = ours ? player->supplyTotal() : 0;
    result.gatheredMinerals = ours ? player->gatheredMinerals() : 0;
    result.gatheredGas = ours ? player->gatheredGas() : 0;
    if (ours) {
        result.technologies.reserve(
            static_cast<std::size_t>(TechnologyKind::count) - 1U);
        for (auto value = static_cast<int>(TechnologyKind::none) + 1;
             value < static_cast<int>(TechnologyKind::count); ++value) {
            const auto kind = static_cast<TechnologyKind>(value);
            const auto tech = toBwapiTech(kind);
            const auto upgrade = toBwapiUpgrade(kind);
            if (tech != TechTypes::None) {
                result.technologies.push_back(
                    {kind, player->hasResearched(tech) ? 1 : 0,
                     player->isResearching(tech)});
            } else if (upgrade != UpgradeTypes::None) {
                result.technologies.push_back(
                    {kind, player->getUpgradeLevel(upgrade),
                     player->isUpgrading(upgrade)});
            }
        }
        result.units.reserve(player->getUnits().size());
        for (const auto unit : player->getUnits()) {
            if (unit != nullptr && unit->exists()) {
                result.units.push_back(snapshotUnit(unit, true));
                // BWAPI's training queue is only meaningful on production
                // buildings. Reading it from mobile units can expose stale
                // order bytes (observed as a permanently queued Pylon on an
                // idle Probe), which suppresses the real construction goal.
                if (unit->getType().isBuilding()) {
                    const auto trainingQueue = unit->getTrainingQueue();
                    const auto buildUnit = unit->getBuildUnit();
                    const auto lastCommand = unit->getLastCommand();
                    // BWAPI updates isTraining()/the queue asynchronously. A
                    // just-issued train command can therefore look idle for
                    // one latency window. Treat it as occupied immediately so
                    // the planner neither double-orders nor leaves a producer
                    // reservation out of the next snapshot.
                    const auto recentTrainingCommand =
                        lastCommand.getType() == BWAPI::UnitCommandTypes::Train &&
                        unit->getLastCommandFrame() +
                                std::max(1, Broodwar->getLatencyFrames()) >=
                            Broodwar->getFrameCount();
                    const auto activeTraining = unit->isTraining() ||
                                                unit->getRemainingTrainTime() > 0 ||
                                                recentTrainingCommand ||
                                                (buildUnit != nullptr &&
                                                 buildUnit->exists() &&
                                                 !buildUnit->isCompleted());
                    auto skippedActiveQueueEntry = false;
                    for (const auto queuedType : trainingQueue) {
                        const auto queuedKind = toKind(queuedType);
                        if (queuedKind != UnitKind::unknown) {
                            // The exposed in-progress Protoss unit is already
                            // present in player->getUnits(). Count the producer
                            // as busy and retain only entries waiting behind it.
                            if (activeTraining && !skippedActiveQueueEntry) {
                                skippedActiveQueueEntry = true;
                            } else {
                                result.queuedUnits.push_back(queuedKind);
                            }
                        }
                    }
                    // In live BWAPI 4.4 games a Protoss producer can report an
                    // active, non-empty transitional queue whose UnitType is
                    // not yet usable by the adapter. isTraining() can also
                    // trail the underlying build timer. In either case the
                    // producer is occupied and must not reserve another unit
                    // ahead of throughput structures or workers.
                    if (activeTraining && !trainingSlotAvailable(
                            activeTraining, static_cast<int>(trainingQueue.size()),
                            unit->getRemainingTrainTime(),
                            Broodwar->getRemainingLatencyFrames(), recentTrainingCommand)) {
                        const auto producer = toKind(unit->getType());
                        if (producer != UnitKind::unknown) {
                            result.busyProducers.push_back(producer);
                        }
                    }
                }
            }
        }
        std::ranges::sort(result.units, {}, &UnitSnapshot::id);
    }
    return result;
}

std::vector<BaseSnapshot> BwapiBridge::snapshotBases(const GameState& state) {
    std::vector<BaseSnapshot> bases;
    bases.reserve(resourceSites_.size());
    auto id = 0;
    for (const auto& site : resourceSites_) {
        ++id;
        const auto center = site.depotCenter;
        auto minerals = 0;
        auto gas = 0;
        auto mineralPatches = 0;
        auto geysers = 0;
        for (const auto patch : Broodwar->getMinerals()) {
            if (closeTo(site.resourceCenter, fromBwapi(patch->getInitialPosition()), 352)) {
                minerals += patch->getResources();
                ++mineralPatches;
            }
        }
        for (const auto geyser : Broodwar->getGeysers()) {
            if (closeTo(site.resourceCenter, fromBwapi(geyser->getInitialPosition()), 352)) {
                gas += geyser->getResources();
                ++geysers;
            }
        }

        auto owner = -1;
        for (const auto& depot : state.self.units) {
            if (depot.role == UnitRole::resourceDepot && depot.completed &&
                closeTo(center, depot.position, 320)) {
                owner = state.self.id;
            }
        }
        for (const auto& depot : state.enemy.units) {
            if (depot.role == UnitRole::resourceDepot && closeTo(center, depot.position, 320)) {
                owner = state.enemy.id;
            }
        }
        const TilePosition tile(center.x / 32, center.y / 32);
        if (tile.isValid() && Broodwar->isVisible(tile)) {
            baseLastScouted_[id] = state.frame;
        }
        auto footprintVisible = true;
        for (auto x = -2; x < 2; ++x) {
            for (auto y = -1; y < 2; ++y) {
                const TilePosition footprintTile(tile.x + x, tile.y + y);
                footprintVisible = footprintVisible && footprintTile.isValid() &&
                                   Broodwar->isVisible(footprintTile);
            }
        }
        if (owner == -1 && footprintVisible) baseLastConfirmedEmpty_[id] = state.frame;
        if (owner != -1) baseLastConfirmedEmpty_.erase(id);
        const auto start = std::ranges::any_of(
            Broodwar->getStartLocations(),
            [center](const TilePosition startTile) {
                return closeTo(center, fromBwapi(BWAPI::Position(startTile)), 256);
            });
        const auto startPosition = BWAPI::Position(Broodwar->self()->getStartLocation());
        const auto island = !Broodwar->hasPath(startPosition, toBwapiPosition(center));
        bases.push_back({id, center, site.mineralLine, minerals, gas, owner,
                         baseLastScouted_[id],
                         start, island, mineralPatches, geysers,
                         baseLastConfirmedEmpty_.contains(id) ? baseLastConfirmedEmpty_.at(id) : -1});
        const UnitSnapshot* approach = nullptr;
        auto closest = 1600 * 1600;
        for (const auto& enemy : state.enemy.units) {
            if (!enemy.visible || enemy.flying || !isCombatUnit(enemy.kind) ||
                enemy.groundWeapon.damage <= 0 || !enemy.position.valid()) continue;
            const auto separation = distanceSquared(center, enemy.position);
            if (separation < closest) { closest = separation; approach = &enemy; }
        }
        auto bestScore = std::numeric_limits<double>::infinity();
        for (const auto& defense : site.defenses) {
            const auto score = (approach ? distance(defense.entrance, approach->position) : 0.0) +
                defense.width * (approach ? 0.25 : 1.5) +
                distance(center, defense.anchor) * 0.25 - (defense.highGround ? 180.0 : 0.0);
            if (score < bestScore) { bestScore = score; bases.back().defense = defense; }
        }
    }
    return bases;
}

void BwapiBridge::discoverResourceClusters() {
    resourceSites_.clear();
    struct ResourceEntry {
        Position position{-1, -1};
        bool mineral{};
    };
    std::vector<ResourceEntry> resources;
    for (const auto mineral : Broodwar->getStaticMinerals()) {
        resources.push_back({fromBwapi(mineral->getInitialPosition()), true});
    }
    for (const auto geyser : Broodwar->getStaticGeysers()) {
        resources.push_back({fromBwapi(geyser->getInitialPosition()), false});
    }
    std::ranges::sort(resources, [](const ResourceEntry& left, const ResourceEntry& right) {
        if (left.position.x != right.position.x) return left.position.x < right.position.x;
        return left.position.y < right.position.y;
    });
    std::vector<bool> claimed(resources.size(), false);
    for (std::size_t seed = 0; seed < resources.size(); ++seed) {
        if (claimed[seed]) continue;
        claimed[seed] = true;
        std::vector<std::size_t> members{seed};
        for (std::size_t cursor = 0; cursor < members.size(); ++cursor) {
            for (std::size_t candidate = 0; candidate < resources.size(); ++candidate) {
                if (!claimed[candidate] &&
                    closeTo(resources[members[cursor]].position,
                            resources[candidate].position, 320)) {
                    claimed[candidate] = true;
                    members.push_back(candidate);
                }
            }
        }
        long long sumX = 0;
        long long sumY = 0;
        long long mineralX = 0;
        long long mineralY = 0;
        auto mineralCount = 0;
        for (const auto member : members) {
            const auto& resource = resources[member];
            sumX += resource.position.x;
            sumY += resource.position.y;
            if (resource.mineral) {
                mineralX += resource.position.x;
                mineralY += resource.position.y;
                ++mineralCount;
            }
        }
        if (members.size() < 4U) continue;
        const auto divisor = static_cast<long long>(members.size());
        const Position resourceCenter{static_cast<int>(sumX / divisor),
                                      static_cast<int>(sumY / divisor)};
        const Position mineralLine = mineralCount > 0
                                         ? Position{static_cast<int>(mineralX / mineralCount),
                                                    static_cast<int>(mineralY / mineralCount)}
                                         : resourceCenter;
        auto depotTile = TilePositions::None;
        for (const auto start : Broodwar->getStartLocations()) {
            if (closeTo(resourceCenter, fromBwapi(BWAPI::Position(start)), 384)) {
                depotTile = start;
                break;
            }
        }
        if (!depotTile.isValid()) {
            depotTile = Broodwar->getBuildLocation(
                UnitTypes::Protoss_Nexus,
                TilePosition(resourceCenter.x / 32, resourceCenter.y / 32), 12);
        }
        const auto depotCenter = depotTile.isValid()
                                     ? Position{depotTile.x * 32 + 64,
                                                depotTile.y * 32 + 48}
                                     : resourceCenter;
        resourceSites_.push_back({resourceCenter, depotCenter, mineralLine, depotTile});
    }
    for (const auto start : Broodwar->getStartLocations()) {
        const Position center{start.x * 32 + 64, start.y * 32 + 48};
        if (std::ranges::none_of(resourceSites_, [center](const ResourceSite& site) {
                return closeTo(center, site.depotCenter, 320);
            })) {
            resourceSites_.push_back({center, center, center, start});
        }
    }
}

BWAPI::Unit BwapiBridge::findBuilder(
    const BWAPI::UnitType type,
    const BWAPI::Position near,
    const std::span<const UnitId> unavailableBuilders) const {
    BWAPI::Unit best = nullptr;
    auto bestScore = std::numeric_limits<long long>::max();
    const auto builderType = type.whatBuilds().first;
    for (const auto unit : Broodwar->self()->getUnits()) {
        if (unit == nullptr || !unit->exists() || !unit->isCompleted() ||
            unit->getType() != builderType || unit->isConstructing() || unit->isTraining()) {
            continue;
        }
        if (std::ranges::find(unavailableBuilders, unit->getID()) !=
            unavailableBuilders.end()) {
            continue;
        }
        // Pending Protoss construction is a real lease even while the Probe
        // is merely walking to the tile. Reusing it for a second structure
        // cancels the first order and can create an indefinite supply block.
        if (std::ranges::any_of(pendingBuilds_, [unit](const auto& entry) {
                return entry.second.builder == unit->getID();
            })) {
            continue;
        }
        auto exposed = false;
        if (const auto enemy = Broodwar->enemy()) {
            exposed = std::ranges::any_of(enemy->getUnits(), [unit](const Unit threat) {
                if (threat == nullptr || !threat->exists() || !threat->isVisible() ||
                    !threat->isCompleted()) {
                    return false;
                }
                const auto weapon = threat->getType().groundWeapon();
                return weapon != WeaponTypes::None &&
                       threat->getDistance(unit) <= weapon.maxRange() + 96;
            });
        }
        const auto missingDurability =
            unit->getType().maxHitPoints() + unit->getType().maxShields() -
            unit->getHitPoints() - unit->getShields();
        const auto score = static_cast<long long>(unit->getDistance(near)) +
                           static_cast<long long>(std::max(0, missingDurability)) * 16LL +
                           (exposed ? 1'000'000LL : 0LL);
        if (score < bestScore ||
            (score == bestScore && (best == nullptr || unit->getID() < best->getID()))) {
            bestScore = score;
            best = unit;
        }
    }
    return best;
}

BWAPI::TilePosition BwapiBridge::buildLocation(
    const UnitKind kind,
    const BWAPI::UnitType type,
    const BWAPI::Unit builder,
    const StrategicPlan& plan) {
    lastMacroStatus_ = "placement-search";
    if (kind == UnitKind::assimilator) {
        Unit bestGeyser = nullptr;
        auto bestScore = std::numeric_limits<int>::max();
        for (const auto geyser : Broodwar->getGeysers()) {
            if (geyser == nullptr || !geyser->exists() || geyser->getResources() <= 0) continue;
            const auto nexus = Broodwar->getClosestUnit(
                geyser->getPosition(),
                Filter::IsOwned && Filter::GetType == UnitTypes::Protoss_Nexus);
            if (nexus == nullptr || nexus->getDistance(geyser) > 448 ||
                !builder->hasPath(geyser->getPosition())) {
                continue;
            }
            // Taken geysers disappear from getGeysers(), so the remaining
            // candidate nearest an owned base naturally advances from main gas
            // to natural and third without repeatedly selecting an occupied tile.
            const auto score = builder->getDistance(geyser) + nexus->getDistance(geyser) * 2;
            if (score < bestScore || (score == bestScore &&
                                      (bestGeyser == nullptr ||
                                       geyser->getID() < bestGeyser->getID()))) {
                bestScore = score;
                bestGeyser = geyser;
            }
        }
        return bestGeyser != nullptr ? bestGeyser->getTilePosition()
                                     : TilePositions::None;
    }
    if (kind == UnitKind::nexus) {
        const ResourceSite* best = nullptr;
        auto bestScore = std::numeric_limits<double>::infinity();
        auto validSites = 0;
        auto reachableSites = 0;
        auto freeSites = 0;
        for (const auto& site : resourceSites_) {
            if (!site.depotTile.isValid()) continue;
            ++validSites;
            const auto center = site.depotCenter;
            if (!Broodwar->hasPath(builder->getPosition(), toBwapiPosition(center))) continue;
            ++reachableSites;
            const auto occupied = std::ranges::any_of(
                Broodwar->getAllUnits(),
                [center](const Unit unit) {
                    return unit != nullptr && unit->exists() &&
                           unit->getType().isResourceDepot() &&
                           closeTo(center, fromBwapi(unit->getPosition()), 320);
                });
            const auto rememberedDepot = std::ranges::any_of(
                enemyMemory_, [center](const auto& entry) {
                    const auto& unit = entry.second;
                    return unit.role == UnitRole::resourceDepot &&
                           closeTo(center, unit.position, 320);
                });
            if (occupied || rememberedDepot) continue;
            ++freeSites;

            auto resources = 0;
            for (const auto patch : Broodwar->getMinerals()) {
                if (closeTo(site.resourceCenter, fromBwapi(patch->getInitialPosition()), 352))
                    resources += patch->getResources();
            }
            auto nearestEnemy = std::numeric_limits<double>::infinity();
            for (const auto& [id, enemy] : enemyMemory_) {
                static_cast<void>(id);
                if (enemy.position.valid())
                    nearestEnemy = std::min(nearestEnemy, distance(center, enemy.position));
            }
            const auto danger = std::max(0.0, 1200.0 - nearestEnemy) *
                                (plan.posture == Posture::defend ? 1.8 : 0.8);
            const auto travel = distance(fromBwapi(builder->getPosition()), center);
            const auto strategicSite = plan.expansionTarget.valid() &&
                closeTo(center, plan.expansionTarget, 64);
            const auto score = travel + danger - static_cast<double>(resources) / 40.0 -
                (strategicSite ? 100000.0 : 0.0);
            if (score < bestScore) {
                bestScore = score;
                best = &site;
            }
        }
        if (best != nullptr) {
            return best->depotTile;
        }
        // Never fall through to generic placement and warp a Nexus into the
        // main when no real resource site is currently available.
        lastMacroStatus_ = "placement-nexus-s" + std::to_string(resourceSites_.size()) +
                           "-v" + std::to_string(validSites) + "-r" +
                           std::to_string(reachableSites) + "-f" +
                           std::to_string(freeSites);
        return TilePositions::None;
    }

    auto anchorPosition = plan.rallyPoint.valid()
                              ? toBwapiPosition(plan.rallyPoint)
                              : BWAPI::Position(Broodwar->self()->getStartLocation());
    const auto defenseDirectionKnown = Broodwar->getStartLocations().size() == 2U ||
        std::ranges::any_of(enemyMemory_, [](const auto& entry) {
            return entry.second.role == UnitRole::resourceDepot;
        });
    auto useForwardLayout = false;
    auto startLayoutAtCenter = false;
    auto preserveBaseAnchor = false;
    auto avoidEnemyFire = false;
    Unit defendedNexus = nullptr;
    if (kind == UnitKind::pylon) {
        Unit disabledProduction = nullptr;
        for (const auto building : Broodwar->self()->getUnits()) {
            if (building == nullptr || !building->exists() || !building->isCompleted() ||
                !building->getType().isBuilding() || building->isPowered()) {
                continue;
            }
            if (disabledProduction == nullptr ||
                building->getID() < disabledProduction->getID()) {
                disabledProduction = building;
            }
        }
        if (disabledProduction != nullptr) {
            anchorPosition = disabledProduction->getPosition();
        }
        Unit leastPoweredBase = nullptr;
        auto fewestNearbyPylons = std::numeric_limits<int>::max();
        for (const auto nexus : Broodwar->self()->getUnits()) {
            if (nexus == nullptr || !nexus->exists() ||
                nexus->getType() != UnitTypes::Protoss_Nexus) continue;
            const auto nearby = static_cast<int>(Broodwar->getUnitsInRadius(
                nexus->getPosition(), 384,
                Filter::IsOwned && Filter::GetType == UnitTypes::Protoss_Pylon).size());
            if (nearby < fewestNearbyPylons) {
                fewestNearbyPylons = nearby;
                leastPoweredBase = nexus;
            }
        }
        if (disabledProduction == nullptr && leastPoweredBase != nullptr) {
            const auto pylonCount = std::ranges::count_if(
                Broodwar->self()->getUnits(), [](const Unit unit) {
                    return unit != nullptr && unit->exists() &&
                           unit->getType() == UnitTypes::Protoss_Pylon;
                });
            const auto hasForwardDefense = std::ranges::any_of(
                Broodwar->self()->getUnits(), [](const Unit unit) {
                    return unit != nullptr && unit->exists() &&
                           (unit->getType() == UnitTypes::Protoss_Photon_Cannon ||
                            unit->getType() == UnitTypes::Protoss_Shield_Battery);
                });
            const auto needsFirstForwardPower = pylonCount == 0;
            const auto needsRedundantForwardPower = pylonCount == 1 && hasForwardDefense;
            const auto forwardDistance = plan.rallyPoint.valid()
                                             ? distance(fromBwapi(leastPoweredBase->getPosition()),
                                                        plan.rallyPoint)
                                             : 0.0;
            if (defenseDirectionKnown &&
                (needsFirstForwardPower || needsRedundantForwardPower) &&
                forwardDistance >= 64.0 && forwardDistance <= 256.0) {
                // Once the enemy-facing direction is known, power the intercept
                // with the first Pylon, then
                // put redundant power halfway back toward the Nexus. The
                // backup still overlaps the Cannon screen without sending its
                // builder through the forward Marine lane.
                if (needsRedundantForwardPower) {
                    const auto basePosition = fromBwapi(leastPoweredBase->getPosition());
                    anchorPosition = toBwapiPosition(
                        {(basePosition.x + plan.rallyPoint.x) / 2,
                         (basePosition.y + plan.rallyPoint.y) / 2});
                    startLayoutAtCenter = true;
                    avoidEnemyFire = true;
                } else {
                    anchorPosition = toBwapiPosition(plan.rallyPoint);
                }
                useForwardLayout = true;
            } else {
                anchorPosition = leastPoweredBase->getPosition();
            }
        }
    }
    const auto vulnerableTech = kind == UnitKind::forge ||
                                kind == UnitKind::cyberneticsCore ||
                                kind == UnitKind::roboticsFacility ||
                                kind == UnitKind::observatory ||
                                kind == UnitKind::roboticsSupportBay ||
                                kind == UnitKind::stargate ||
                                kind == UnitKind::citadelOfAdun ||
                                kind == UnitKind::templarArchives ||
                                kind == UnitKind::fleetBeacon ||
                                kind == UnitKind::arbiterTribunal;
    const auto highValueTech = kind == UnitKind::roboticsFacility ||
                               kind == UnitKind::observatory ||
                               kind == UnitKind::roboticsSupportBay;
    const auto protectProduction = kind == UnitKind::gateway;
    if ((defenseDirectionKnown || protectProduction) && type.requiresPsi() &&
        (vulnerableTech || protectProduction)) {
        const auto base = Broodwar->getClosestUnit(
            builder->getPosition(),
            Filter::GetType == UnitTypes::Protoss_Nexus && Filter::IsCompleted &&
                Filter::IsOwned);
        if (base != nullptr) {
            // The first Pylons intentionally sit on the intercept. Tech built
            // around those Pylons was exposed ahead of the Cannons and had to
            // be rebuilt. Search the powered tiles nearest the Nexus instead.
            anchorPosition = base->getPosition();
            useForwardLayout = true;
            preserveBaseAnchor = true;
            // Robotics and detection are the bridge from a hold to a
            // counterattack.  If an enemy army is already in the home area,
            // reject forward-cluster tiles inside a generous weapon buffer so
            // the building survives long enough to produce its first unit.
            if (highValueTech || protectProduction) {
                Unit danger = nullptr;
                auto dangerDistance = std::numeric_limits<int>::max();
                for (const auto enemy : Broodwar->enemy()->getUnits()) {
                    if (enemy == nullptr || !enemy->exists() ||
                        !enemy->isVisible() || !enemy->isCompleted()) continue;
                    const auto weapon = enemy->getType().groundWeapon();
                    if (weapon == WeaponTypes::None) continue;
                    const auto distanceToBase = enemy->getDistance(base);
                    if (distanceToBase < dangerDistance) {
                        danger = enemy;
                        dangerDistance = distanceToBase;
                    }
                }
                if (danger != nullptr && dangerDistance <= 1024) {
                    const auto basePosition = fromBwapi(base->getPosition());
                    const auto threatPosition = fromBwapi(danger->getPosition());
                    const auto dx = basePosition.x - threatPosition.x;
                    const auto dy = basePosition.y - threatPosition.y;
                    const auto length = std::hypot(static_cast<double>(dx),
                                                   static_cast<double>(dy));
                    if (length > 0.001) {
                        anchorPosition = {
                            basePosition.x + static_cast<int>(std::lround(dx / length * 192.0)),
                            basePosition.y + static_cast<int>(std::lround(dy / length * 192.0)),
                        };
                        startLayoutAtCenter = true;
                    }
                    avoidEnemyFire = true;
                }
            }
        }
    }
    if (type.requiresPsi() && !preserveBaseAnchor) {
        const auto pylon = Broodwar->getClosestUnit(
            anchorPosition,
            Filter::GetType == UnitTypes::Protoss_Pylon && Filter::IsCompleted &&
                Filter::IsOwned);
        if (pylon != nullptr) anchorPosition = pylon->getPosition();
    }
    if (kind == UnitKind::photonCannon || kind == UnitKind::shieldBattery) {
        Unit forwardNexus = nullptr;
        auto fewestNearbyDefenses = std::numeric_limits<int>::max();
        auto bestDistance = std::numeric_limits<double>::infinity();
        for (const auto unit : Broodwar->self()->getUnits()) {
            if (unit == nullptr || !unit->exists() ||
                unit->getType() != UnitTypes::Protoss_Nexus) continue;
            const auto nearby = static_cast<int>(Broodwar->getUnitsInRadius(
                unit->getPosition(), 416,
                Filter::IsOwned && Filter::GetType == type).size());
            const auto candidate = plan.attackTarget.valid()
                                       ? distance(fromBwapi(unit->getPosition()),
                                                  plan.attackTarget)
                                       : distance(fromBwapi(unit->getPosition()),
                                                  fromBwapi(anchorPosition));
            if (nearby < fewestNearbyDefenses ||
                (nearby == fewestNearbyDefenses && candidate < bestDistance)) {
                fewestNearbyDefenses = nearby;
                bestDistance = candidate;
                forwardNexus = unit;
            }
        }
        if (forwardNexus != nullptr) {
            defendedNexus = forwardNexus;
            const auto localPylon = Broodwar->getClosestUnit(
                forwardNexus->getPosition(),
                Filter::GetType == UnitTypes::Protoss_Pylon && Filter::IsCompleted &&
                    Filter::IsOwned);
            if (localPylon == nullptr || localPylon->getDistance(forwardNexus) > 384)
                return TilePositions::None;
            // The Pylon is already offset from the Nexus. Anchoring the layout
            // to it and applying another layout offset placed Cannons beyond
            // useful mineral-line coverage. Search around the defended Nexus
            // instead; the normal hasPower check below still guarantees psi.
            const auto forwardDistance = plan.rallyPoint.valid()
                                             ? distance(fromBwapi(forwardNexus->getPosition()),
                                                        plan.rallyPoint)
                                             : 0.0;
            if (defenseDirectionKnown && forwardDistance >= 64.0 &&
                forwardDistance <= 256.0) {
                // Intercept ranged rushes before they acquire the Probe line.
                // Keeping the anchor within eight tiles of the Nexus preserves
                // compact power coverage and short reinforcement paths.
                const auto groundPressure = std::ranges::any_of(
                    Broodwar->enemy()->getUnits(), [forwardNexus](const Unit enemy) {
                        if (enemy == nullptr || !enemy->exists() || !enemy->isVisible() ||
                            !enemy->isCompleted()) {
                            return false;
                        }
                        const auto weapon = enemy->getType().groundWeapon();
                        return weapon != WeaponTypes::None &&
                               enemy->getDistance(forwardNexus) <= 512;
                    });
                if (groundPressure) {
                    const auto basePosition = fromBwapi(forwardNexus->getPosition());
                    anchorPosition = toBwapiPosition(
                        {(basePosition.x + plan.rallyPoint.x) / 2,
                         (basePosition.y + plan.rallyPoint.y) / 2});
                    startLayoutAtCenter = true;
                    avoidEnemyFire = true;
                } else {
                    anchorPosition = toBwapiPosition(plan.rallyPoint);
                }
                useForwardLayout = true;
            } else {
                anchorPosition = forwardNexus->getPosition();
            }
        }
    }

    static const std::array standardLayout{
        TilePosition{4, 2}, TilePosition{-4, 2}, TilePosition{4, -3},
        TilePosition{-4, -3}, TilePosition{7, 1}, TilePosition{-7, 1},
        TilePosition{2, 6}, TilePosition{-2, 6}, TilePosition{2, -6},
        TilePosition{-2, -6},
    };
    static const std::array forwardLayout{
        TilePosition{0, 0}, TilePosition{2, 0}, TilePosition{-2, 0},
        TilePosition{0, 2}, TilePosition{0, -2}, TilePosition{2, 2},
        TilePosition{-2, 2}, TilePosition{2, -2}, TilePosition{-2, -2},
        TilePosition{4, 0},
    };
    const auto& layout = useForwardLayout ? forwardLayout : standardLayout;
    // Non-producing tech can share edges, but Robotics and Gateway exits must
    // stay open for large units. A legal Probe path does not prove that the
    // Reaver produced there can leave the building cluster.
    const auto structureGap = vulnerableTech && !type.canProduce() ? 0 : 32;
    const auto existing = static_cast<std::size_t>(std::ranges::count_if(
        Broodwar->self()->getUnits(), [type](const Unit unit) {
            return unit != nullptr && unit->exists() && unit->getType() == type;
        }));
    const auto layoutStart = startLayoutAtCenter ? std::size_t{0} : existing;
    const auto anchor = TilePosition(anchorPosition);
    auto miningLaneFallback = TilePositions::None;
    auto validCandidates = 0;
    auto poweredCandidates = 0;
    auto buildableCandidates = 0;
    auto reachableCandidates = 0;
    auto laneCandidates = 0;
    auto enemyFireFallback = TilePositions::None;
    auto enemyFireFallbackScore = -std::numeric_limits<double>::infinity();
    const auto usable = [&](const TilePosition location) {
        if (!location.isValid() || location.x < 0 || location.y < 0 ||
            location.x + type.tileWidth() > Broodwar->mapWidth() ||
            location.y + type.tileHeight() > Broodwar->mapHeight()) {
            return false;
        }
        ++validCandidates;
        if (type.requiresPsi() && !Broodwar->hasPower(location, type)) return false;
        ++poweredCandidates;
        // Unit::build performs this same check with checkExplored=true.  Using
        // false here can select a nominally buildable fogged tile which the
        // command then rejects as Unbuildable_Location.
        // `canBuildHere` with a Probe argument is stricter than the actual
        // placement test on some BWAPI builds: a Probe that is still carrying
        // a mineral or finishing a previous move can make every otherwise
        // legal tile report false for one frame.  Stardust separates the
        // footprint test from the worker schedule.  Accept the location when
        // the map-level check is legal, then keep the explicit path and
        // command-time validation below as the worker-side guard.
        if (!Broodwar->canBuildHere(location, type, builder, true) &&
            !Broodwar->canBuildHere(location, type, nullptr, true)) {
            return false;
        }
        const BuildingFootprint footprint{
            {location.x * 32, location.y * 32}, type.tileWidth() * 32, type.tileHeight() * 32};
        for (const auto building : Broodwar->self()->getUnits()) {
            if (building == nullptr || !building->exists() || !building->getType().isBuilding() ||
                building->isLifted()) continue;
            const auto otherType = building->getType();
            const auto otherTile = building->getTilePosition();
            const BuildingFootprint occupied{
                {otherTile.x * 32, otherTile.y * 32}, otherType.tileWidth() * 32,
                otherType.tileHeight() * 32};
            // Adjacent buildings created sealed pockets of fresh Dragoons.
            // Reserve a full build tile for movement between structures.
            const auto exitGap = type.canProduce() || otherType.canProduce() ? 32 : structureGap;
            if (!separatedByGap(footprint, occupied, exitGap)) return false;
        }
        for (const auto& [pendingKind, pending] : pendingBuilds_) {
            if (pendingKind == kind || pendingKind == UnitKind::nexus) continue;
            const auto pendingType = toBwapi(pendingKind);
            if (!separatedByGap(footprint,
                    {pending.target, pendingType.tileWidth() * 32, pendingType.tileHeight() * 32},
                    type.canProduce() || pendingType.canProduce() ? 32 : structureGap)) return false;
        }
        ++buildableCandidates;
        const Position center{
            location.x * 32 + type.tileWidth() * 16,
            location.y * 32 + type.tileHeight() * 16,
        };
        if (avoidEnemyFire) {
            auto unsafe = false;
            auto nearestThreatDistance = std::numeric_limits<double>::infinity();
            for (const auto enemy : Broodwar->enemy()->getUnits()) {
                if (enemy == nullptr || !enemy->exists() || !enemy->isVisible() ||
                    !enemy->isCompleted()) continue;
                const auto weapon = enemy->getType().groundWeapon();
                if (weapon == WeaponTypes::None) continue;
                const auto threatDistance = enemy->getDistance(toBwapiPosition(center));
                nearestThreatDistance = std::min(nearestThreatDistance,
                                                 static_cast<double>(threatDistance));
                unsafe = unsafe || threatDistance <= weapon.maxRange() +
                                      (highValueTech ? 256 : 96);
            }
            if (unsafe) {
                // Do not deadlock critical tech simply because every powered
                // tile is inside a live ranged weapon's conservative margin.
                // Keep the safest legal fallback (farthest from the nearest
                // threat, then closest to the base anchor) and use it only if
                // the exhaustive safe search finds no alternative.
                const auto anchorDistance = distance(center, fromBwapi(anchorPosition));
                const auto score = nearestThreatDistance * 4.0 - anchorDistance * 0.1;
                if (builder->hasPath(toBwapiPosition(center)) &&
                    score > enemyFireFallbackScore) {
                    enemyFireFallback = location;
                    enemyFireFallbackScore = score;
                }
                return false;
            }
        }
        if (std::ranges::any_of(
                failedBuildSites_, [kind, center](const FailedBuildSite& failed) {
                    return failed.kind == kind &&
                           distanceSquared(failed.target, center) <= 64 * 64;
                })) {
            return false;
        }
        if (!builder->hasPath(toBwapiPosition(center))) return false;
        ++reachableCandidates;
        return true;
    };
    const auto consider = [&](const TilePosition location) {
        if (!usable(location)) return TilePositions::None;
        if (!blocksMiningLane(location, type)) return location;
        ++laneCandidates;
        if (!miningLaneFallback.isValid()) miningLaneFallback = location;
        return TilePositions::None;
    };
    if (protectProduction) {
        // getBuildLocation returns its first legal tile, which can be on the
        // exposed side of a distant Pylon. Rank the compact powered footprint
        // around the economy instead of accepting that search-order accident.
        auto bestLocation = TilePositions::None;
        auto bestScore = std::numeric_limits<double>::infinity();
        for (auto dy = -10; dy <= 10; ++dy) {
            for (auto dx = -10; dx <= 10; ++dx) {
                const auto location = anchor + TilePosition{dx, dy};
                const Position center{location.x * 32 + type.tileWidth() * 16,
                                      location.y * 32 + type.tileHeight() * 16};
                const auto score = distance(center, fromBwapi(anchorPosition)) +
                    distance(center, fromBwapi(builder->getPosition())) * 0.10;
                if (score >= bestScore || !usable(location) || blocksMiningLane(location, type))
                    continue;
                bestScore = score;
                bestLocation = location;
            }
        }
        if (bestLocation.isValid()) {
            lastMacroStatus_ = "placement-protected-production";
            return bestLocation;
        }
    }
    if (defendedNexus != nullptr) {
        // Score actual coverage before consulting the generic layout. A
        // Cannon beside the forward Pylon can leave the entire mineral line
        // outside its weapon range, even though it is close to the Nexus.
        auto bestLocation = TilePositions::None;
        auto bestScore = -std::numeric_limits<double>::infinity();
        const auto nexusPosition = fromBwapi(defendedNexus->getPosition());
        const auto nexusTile = defendedNexus->getTilePosition();
        Position intercept{-1, -1};
        auto nearestApproach = 1100 * 1100;
        for (const auto& [id, enemy] : enemyMemory_) {
            static_cast<void>(id);
            if (!enemy.visible || !enemy.completed || enemy.flying ||
                enemy.groundWeapon.damage <= 0 || !enemy.position.valid() ||
                isWorker(enemy.kind)) continue;
            const auto separation = distanceSquared(enemy.position, nexusPosition);
            if (separation < nearestApproach) {
                nearestApproach = separation;
                intercept = moveToward(nexusPosition, enemy.position, 160.0);
            }
        }
        std::vector<Position> workerLine;
        std::vector<Position> existingCannons;
        for (const auto patch : Broodwar->getMinerals()) {
            if (patch->exists() && patch->getResources() > 0 &&
                patch->getDistance(defendedNexus) <= 288) {
                workerLine.push_back(fromBwapi(patch->getPosition()));
            }
        }
        for (const auto cannon : Broodwar->self()->getUnits()) {
            if (cannon->exists() && cannon->getType() == UnitTypes::Protoss_Photon_Cannon &&
                cannon->isPowered()) existingCannons.push_back(fromBwapi(cannon->getPosition()));
        }
        for (auto dy = -8; dy <= 8; ++dy) {
            for (auto dx = -8; dx <= 8; ++dx) {
                const auto location = nexusTile + TilePosition{dx, dy};
                if (!usable(location) || blocksMiningLane(location, type)) continue;
                const Position center{location.x * 32 + type.tileWidth() * 16,
                                      location.y * 32 + type.tileHeight() * 16};
                auto score = -distance(center, nexusPosition) * 0.04;
                if (intercept.valid()) score -= distance(center, intercept) * 0.30;
                else if (plan.rallyPoint.valid()) score -= distance(center, plan.rallyPoint) * 0.08;
                for (const auto patch : workerLine) {
                    if (distanceSquared(center, patch) > 256 * 256) continue;
                    const auto alreadyCovered = std::ranges::any_of(existingCannons,
                        [patch](const Position cannon) {
                            return distanceSquared(cannon, patch) <= 256 * 256;
                        });
                    score += alreadyCovered ? 2.0 : (intercept.valid() ? 4.0 : 12.0);
                }
                if (score > bestScore) {
                    bestScore = score;
                    bestLocation = location;
                }
            }
        }
        if (bestLocation.isValid()) {
            lastMacroStatus_ = "placement-defensive-coverage";
            return bestLocation;
        }
    }
    for (std::size_t attempt = 0; attempt < layout.size(); ++attempt) {
        const auto offset = layout[(layoutStart + attempt) % layout.size()];
        const auto location = Broodwar->getBuildLocation(type, anchor + offset, 8);
        const auto accepted = consider(location);
        if (accepted.isValid()) {
            lastMacroStatus_ = "placement-safe";
            return accepted;
        }
    }

    const auto broadLocation = Broodwar->getBuildLocation(type, anchor, 32);
    const auto broadAccepted = consider(broadLocation);
    if (broadAccepted.isValid()) {
        lastMacroStatus_ = "placement-safe";
        return broadAccepted;
    }
    if (miningLaneFallback.isValid()) {
        lastMacroStatus_ = "placement-lane-fallback";
        return miningLaneFallback;
    }

    // The fixed layout is only a preference. Search every tile on expanding
    // rings so unusual tournament maps, starting mineral geometries, and
    // partially occupied bases cannot permanently deadlock production.
    constexpr auto maximumRadius = 16;
    for (auto radius = 2; radius <= maximumRadius; ++radius) {
        for (auto dx = -radius; dx <= radius; ++dx) {
            for (const auto dy : {-radius, radius}) {
                const auto accepted = consider(anchor + TilePosition(dx, dy));
                if (accepted.isValid()) {
                    lastMacroStatus_ = "placement-safe";
                    return accepted;
                }
            }
        }
        for (auto dy = -radius + 1; dy < radius; ++dy) {
            for (const auto dx : {-radius, radius}) {
                const auto accepted = consider(anchor + TilePosition(dx, dy));
                if (accepted.isValid()) {
                    lastMacroStatus_ = "placement-safe";
                    return accepted;
                }
            }
        }
    }

    if (enemyFireFallback.isValid()) {
        lastMacroStatus_ = "placement-enemy-fire-fallback";
        return enemyFireFallback;
    }

    // A suboptimal mineral-side building is preferable to a permanent supply
    // block. This is reached only if the exhaustive safe search found no
    // legal alternative.
    if (miningLaneFallback.isValid()) {
        lastMacroStatus_ = "placement-lane-fallback";
        return miningLaneFallback;
    }
    lastMacroStatus_ = "placement-v" + std::to_string(validCandidates) +
                       "-p" + std::to_string(poweredCandidates) +
                       "-b" + std::to_string(buildableCandidates) +
                       "-r" + std::to_string(reachableCandidates) +
                       "-l" + std::to_string(laneCandidates);
    return TilePositions::None;
}

bool BwapiBridge::blocksMiningLane(
    const BWAPI::TilePosition tile,
    const BWAPI::UnitType type) const {
    if (!tile.isValid()) return true;
    const Position buildingCenter{
        tile.x * 32 + type.tileWidth() * 16,
        tile.y * 32 + type.tileHeight() * 16,
    };
    const auto clearance = std::max(type.tileWidth(), type.tileHeight()) * 16 + 40;
    for (const auto& site : resourceSites_) {
        if (!closeTo(site.depotCenter, buildingCenter, 640)) continue;
        for (const auto mineral : Broodwar->getStaticMinerals()) {
            const auto resource = fromBwapi(mineral->getInitialPosition());
            if (!closeTo(resource, site.resourceCenter, 352)) continue;
            if (closeTo(buildingCenter, resource, clearance)) return true;

            const auto segmentX = resource.x - site.depotCenter.x;
            const auto segmentY = resource.y - site.depotCenter.y;
            const auto lengthSquared = segmentX * segmentX + segmentY * segmentY;
            if (lengthSquared <= 0) continue;
            const auto projection = std::clamp(
                static_cast<double>((buildingCenter.x - site.depotCenter.x) * segmentX +
                                    (buildingCenter.y - site.depotCenter.y) * segmentY) /
                    static_cast<double>(lengthSquared),
                0.0, 1.0);
            const Position closest{
                site.depotCenter.x + static_cast<int>(std::lround(segmentX * projection)),
                site.depotCenter.y + static_cast<int>(std::lround(segmentY * projection)),
            };
            if (distanceSquared(buildingCenter, closest) <= clearance * clearance) return true;
        }
    }
    return false;
}

bool BwapiBridge::build(
    const MacroAction& action,
    const StrategicPlan& plan,
    const std::span<const UnitId> unavailableBuilders) {
    const auto type = toBwapi(action.target);
    if (type == UnitTypes::None || !type.isBuilding()) {
        lastMacroStatus_ = "build-invalid-type";
        return false;
    }
    const auto pending = pendingBuilds_.find(action.target);
    if (pending != pendingBuilds_.end()) {
        // Expansions may be pre-positioned into unexplored fog before BWAPI
        // accepts the build command.  The old guard treated that pending
        // lease as terminal: every later macro pass returned here, so the
        // Probe could stand on the natural for 45 seconds and the Nexus was
        // never actually issued.  Retry the exact reserved tile once the
        // worker arrives and the footprint is explored.
        if (action.target == UnitKind::nexus) {
            const auto builder = Broodwar->getUnit(pending->second.builder);
            const auto targetTile = BWAPI::TilePosition(
                pending->second.target.x / 32,
                pending->second.target.y / 32);
            const BWAPI::Position center{
                pending->second.target.x + type.tileWidth() * 16,
                pending->second.target.y + type.tileHeight() * 16,
            };
            if (builder != nullptr && builder->exists() && builder->isCompleted() &&
                builder->getDistance(center) <= 96 &&
                Broodwar->canBuildHere(targetTile, type, builder, true) &&
                builder->build(type, targetTile)) {
                lastMacroStatus_ = "issued-pending-Nexus";
                return true;
            }
        } else {
            // A Protoss build command can be acknowledged while the Probe is
            // still walking to the footprint.  The old lease only retried
            // Nexus placement, so a Forge/Core/Gateway that missed that
            // transient command stayed "pending" until the long lease timed
            // out and silently blocked the strategic checkpoint.  Retry the
            // exact tile as soon as the leased Probe arrives; the lifecycle
            // code still owns the worker until construction is observed.
            const auto builder = Broodwar->getUnit(pending->second.builder);
            const auto targetTile = BWAPI::TilePosition(
                pending->second.target.x / 32,
                pending->second.target.y / 32);
            const BWAPI::Position center{
                targetTile.x * 32 + type.tileWidth() * 16,
                targetTile.y * 32 + type.tileHeight() * 16,
            };
            if (builder != nullptr && builder->exists() && builder->isCompleted() &&
                builder->getDistance(center) <= 96 &&
                Broodwar->canBuildHere(targetTile, type, builder, true) &&
                builder->build(type, targetTile)) {
                lastMacroStatus_ = "issued-pending-" +
                                   std::string(unitStats(action.target).name);
                return true;
            }
        }
        lastMacroStatus_ = "build-pending";
        return false;
    }
    const auto near = plan.rallyPoint.valid() ? toBwapiPosition(plan.rallyPoint)
                                              : BWAPI::Position(Broodwar->self()->getStartLocation());
    const auto builder = findBuilder(type, near, unavailableBuilders);
    if (builder == nullptr) {
        lastMacroStatus_ = "build-no-builder";
        return false;
    }
    if (!Broodwar->canMake(type, builder)) {
        lastMacroStatus_ = "build-cannot-make";
        return false;
    }
    const auto location = buildLocation(action.target, type, builder, plan);
    if (!location.isValid()) {
        lastMacroStatus_ = "build-no-location-" + lastMacroStatus_;
        return false;
    }
    if (type.requiresPsi() && !Broodwar->hasPower(location, type)) {
        lastMacroStatus_ = "build-unpowered-location";
        return false;
    }
    // Match Unit::build's command-time validation; otherwise placement can
    // succeed in unexplored fog and fail forever when the command is issued.
    if (!Broodwar->canBuildHere(location, type, builder, true)) {
        // Expansion footprints are commonly still in fog. BWAPI refuses a
        // build command until every footprint tile is explored, so reserve
        // the worker and reveal the location first; subsequent macro passes
        // will issue the Nexus as soon as the footprint becomes commandable.
        if (action.target == UnitKind::nexus &&
            Broodwar->canBuildHere(location, type, builder, false)) {
            const BWAPI::Position center{
                location.x * 32 + type.tileWidth() * 16,
                location.y * 32 + type.tileHeight() * 16,
            };
            if (builder->move(center)) {
                pendingBuilds_[action.target] = {
                    builder->getID(), Broodwar->getFrameCount(),
                    fromBwapi(BWAPI::Position(location)), true,
                };
                lastMacroStatus_ = "build-preposition-Nexus";
                return false;
            }
        }
        lastMacroStatus_ = "build-location-rejected";
        return false;
    }
    if (builder->build(type, location)) {
        pendingBuilds_[action.target] = {
            builder->getID(), Broodwar->getFrameCount(),
            fromBwapi(BWAPI::Position(location)), false,
        };
        return true;
    }
    failedBuildSites_.push_back({
        action.target, fromBwapi(BWAPI::Position(location)),
        Broodwar->getFrameCount() + 20 * 24,
    });
    lastMacroStatus_ = "build-command-rejected-" + Broodwar->getLastError().toString() +
                       "-u" + std::to_string(builder->getID()) + "-at" +
                       std::to_string(builder->getTilePosition().x) + "x" +
                       std::to_string(builder->getTilePosition().y) + "-to" +
                       std::to_string(location.x) + "x" + std::to_string(location.y);
    return false;
}

bool BwapiBridge::train(const MacroAction& action) {
    const auto type = toBwapi(action.target);
    if (type == UnitTypes::None) {
        lastMacroStatus_ = "train-invalid-type";
        return false;
    }
    const auto producerType = type.whatBuilds().first;
    Unit selected = nullptr;
    for (const auto producer : Broodwar->self()->getUnits()) {
        const auto lastCommand = producer != nullptr ? producer->getLastCommand()
                                                     : BWAPI::UnitCommand{};
        const auto recentTrainingCommand =
            producer != nullptr &&
            lastCommand.getType() == BWAPI::UnitCommandTypes::Train &&
            producer->getLastCommandFrame() +
                    std::max(1, Broodwar->getLatencyFrames()) >=
                Broodwar->getFrameCount();
        if (producer == nullptr || !producer->exists() || !producer->isCompleted() ||
            producer->getType() != producerType ||
            !trainingSlotAvailable(producer->isTraining() || producer->getRemainingTrainTime() > 0,
                                   static_cast<int>(producer->getTrainingQueue().size()),
                                   producer->getRemainingTrainTime(),
                                   Broodwar->getRemainingLatencyFrames(), recentTrainingCommand) ||
            !producer->isPowered() || !producer->canTrain(type)) {
            continue;
        }
        if (selected == nullptr || producer->getID() < selected->getID()) {
            selected = producer;
        }
    }
    if (selected == nullptr) {
        lastMacroStatus_ = "train-no-idle-producer-" + std::string(unitStats(action.target).name);
        return false;
    }
    if (!Broodwar->canMake(type, selected)) {
        lastMacroStatus_ = "train-cannot-make-" + std::string(unitStats(action.target).name);
        return false;
    }
    if (!selected->train(type)) {
        lastMacroStatus_ = "train-command-rejected-" + Broodwar->getLastError().toString();
        return false;
    }
    return true;
}

bool BwapiBridge::executeTechnology(const MacroAction& action) {
    const auto self = Broodwar->self();
    if (self == nullptr || action.technology == TechnologyKind::none) return false;

    const auto tech = toBwapiTech(action.technology);
    if (tech != TechTypes::None) {
        if (self->hasResearched(tech) || self->isResearching(tech)) return false;
        Unit producer = nullptr;
        for (const auto candidate : self->getUnits()) {
            if (candidate == nullptr || !candidate->exists() || !candidate->isCompleted() ||
                candidate->getType() != tech.whatResearches() || candidate->isResearching()) {
                continue;
            }
            if (producer == nullptr || candidate->getID() < producer->getID()) {
                producer = candidate;
            }
        }
        return producer != nullptr && producer->canResearch(tech) && producer->research(tech);
    }

    const auto upgrade = toBwapiUpgrade(action.technology);
    if (upgrade == UpgradeTypes::None || self->isUpgrading(upgrade) ||
        self->getUpgradeLevel(upgrade) >= upgrade.maxRepeats()) {
        return false;
    }
    Unit producer = nullptr;
    for (const auto candidate : self->getUnits()) {
        if (candidate == nullptr || !candidate->exists() || !candidate->isCompleted() ||
            candidate->getType() != upgrade.whatUpgrades() || candidate->isUpgrading()) {
            continue;
        }
        if (producer == nullptr || candidate->getID() < producer->getID()) {
            producer = candidate;
        }
    }
    return producer != nullptr && producer->canUpgrade(upgrade) && producer->upgrade(upgrade);
}

BWAPI::Position BwapiBridge::toBwapiPosition(const Position position) noexcept {
    return {position.x, position.y};
}

UnitKind BwapiBridge::toKind(const BWAPI::UnitType type) noexcept {
    using namespace UnitTypes;
    if (type == Protoss_Probe) return UnitKind::probe;
    if (type == Protoss_Nexus) return UnitKind::nexus;
    if (type == Protoss_Pylon) return UnitKind::pylon;
    if (type == Protoss_Assimilator) return UnitKind::assimilator;
    if (type == Protoss_Gateway) return UnitKind::gateway;
    if (type == Protoss_Forge) return UnitKind::forge;
    if (type == Protoss_Photon_Cannon) return UnitKind::photonCannon;
    if (type == Protoss_Cybernetics_Core) return UnitKind::cyberneticsCore;
    if (type == Protoss_Shield_Battery) return UnitKind::shieldBattery;
    if (type == Protoss_Robotics_Facility) return UnitKind::roboticsFacility;
    if (type == Protoss_Observatory) return UnitKind::observatory;
    if (type == Protoss_Robotics_Support_Bay) return UnitKind::roboticsSupportBay;
    if (type == Protoss_Stargate) return UnitKind::stargate;
    if (type == Protoss_Citadel_of_Adun) return UnitKind::citadelOfAdun;
    if (type == Protoss_Templar_Archives) return UnitKind::templarArchives;
    if (type == Protoss_Fleet_Beacon) return UnitKind::fleetBeacon;
    if (type == Protoss_Arbiter_Tribunal) return UnitKind::arbiterTribunal;
    if (type == Protoss_Zealot) return UnitKind::zealot;
    if (type == Protoss_Dragoon) return UnitKind::dragoon;
    if (type == Protoss_High_Templar) return UnitKind::highTemplar;
    if (type == Protoss_Dark_Templar) return UnitKind::darkTemplar;
    if (type == Protoss_Archon) return UnitKind::archon;
    if (type == Protoss_Dark_Archon) return UnitKind::darkArchon;
    if (type == Protoss_Reaver) return UnitKind::reaver;
    if (type == Protoss_Observer) return UnitKind::observer;
    if (type == Protoss_Shuttle) return UnitKind::shuttle;
    if (type == Protoss_Scout) return UnitKind::scout;
    if (type == Protoss_Corsair) return UnitKind::corsair;
    if (type == Protoss_Carrier) return UnitKind::carrier;
    if (type == Protoss_Arbiter) return UnitKind::arbiter;
    if (type == Terran_SCV) return UnitKind::scv;
    if (type == Terran_Command_Center) return UnitKind::commandCenter;
    if (type == Terran_Supply_Depot) return UnitKind::supplyProvider;
    if (type == Terran_Refinery) return UnitKind::refinery;
    if (type == Terran_Barracks) return UnitKind::barracks;
    if (type == Terran_Factory) return UnitKind::factory;
    if (type == Terran_Starport) return UnitKind::starport;
    if (type == Terran_Bunker) return UnitKind::bunker;
    if (type == Terran_Missile_Turret) return UnitKind::missileTurret;
    if (type == Terran_Marine) return UnitKind::marine;
    if (type == Terran_Medic) return UnitKind::medic;
    if (type == Terran_Firebat) return UnitKind::firebat;
    if (type == Terran_Vulture) return UnitKind::vulture;
    if (type == Terran_Siege_Tank_Tank_Mode || type == Terran_Siege_Tank_Siege_Mode)
        return UnitKind::siegeTank;
    if (type == Terran_Goliath) return UnitKind::goliath;
    if (type == Terran_Wraith) return UnitKind::wraith;
    if (type == Terran_Science_Vessel) return UnitKind::scienceVessel;
    if (type == Terran_Dropship) return UnitKind::dropship;
    if (type == Terran_Battlecruiser) return UnitKind::battlecruiser;
    if (type == Terran_Ghost) return UnitKind::ghost;
    if (type == Terran_Valkyrie) return UnitKind::valkyrie;
    if (type == Terran_Vulture_Spider_Mine) return UnitKind::spiderMine;
    if (type == Terran_Academy) return UnitKind::academy;
    if (type == Terran_Engineering_Bay) return UnitKind::engineeringBay;
    if (type == Terran_Armory) return UnitKind::armory;
    if (type == Terran_Machine_Shop) return UnitKind::machineShop;
    if (type == Terran_Control_Tower) return UnitKind::controlTower;
    if (type == Terran_Science_Facility) return UnitKind::scienceFacility;
    if (type == Terran_Covert_Ops) return UnitKind::covertOps;
    if (type == Terran_Physics_Lab) return UnitKind::physicsLab;
    if (type == Terran_Comsat_Station) return UnitKind::comsatStation;
    if (type == Terran_Nuclear_Silo) return UnitKind::nuclearSilo;
    if (type == Zerg_Drone) return UnitKind::drone;
    if (type == Zerg_Hatchery) return UnitKind::hatchery;
    if (type == Zerg_Infested_Command_Center) return UnitKind::commandCenter;
    if (type == Zerg_Extractor) return UnitKind::refinery;
    if (type == Zerg_Lair) return UnitKind::lair;
    if (type == Zerg_Hive) return UnitKind::hive;
    if (type == Zerg_Spawning_Pool) return UnitKind::spawningPool;
    if (type == Zerg_Hydralisk_Den) return UnitKind::hydraliskDen;
    if (type == Zerg_Spire) return UnitKind::spire;
    if (type == Zerg_Greater_Spire) return UnitKind::greaterSpire;
    if (type == Zerg_Sunken_Colony) return UnitKind::sunkenColony;
    if (type == Zerg_Spore_Colony) return UnitKind::sporeColony;
    if (type == Zerg_Zergling) return UnitKind::zergling;
    if (type == Zerg_Hydralisk) return UnitKind::hydralisk;
    if (type == Zerg_Lurker) return UnitKind::lurker;
    if (type == Zerg_Mutalisk) return UnitKind::mutalisk;
    if (type == Zerg_Scourge) return UnitKind::scourge;
    if (type == Zerg_Ultralisk) return UnitKind::ultralisk;
    if (type == Zerg_Defiler) return UnitKind::defiler;
    if (type == Zerg_Overlord) return UnitKind::overlord;
    if (type == Zerg_Queen) return UnitKind::queen;
    if (type == Zerg_Guardian) return UnitKind::guardian;
    if (type == Zerg_Devourer) return UnitKind::devourer;
    if (type == Zerg_Broodling) return UnitKind::broodling;
    if (type == Zerg_Infested_Terran) return UnitKind::infestedTerran;
    if (type == Zerg_Creep_Colony) return UnitKind::creepColony;
    if (type == Zerg_Evolution_Chamber) return UnitKind::evolutionChamber;
    if (type == Zerg_Queens_Nest) return UnitKind::queensNest;
    if (type == Zerg_Ultralisk_Cavern) return UnitKind::ultraliskCavern;
    if (type == Zerg_Defiler_Mound) return UnitKind::defilerMound;
    if (type == Zerg_Nydus_Canal) return UnitKind::nydusCanal;
    if (type == Zerg_Lurker_Egg) return UnitKind::lurkerEgg;
    if (type == Zerg_Cocoon) return UnitKind::cocoon;
    return UnitKind::unknown;
}

BWAPI::UnitType BwapiBridge::toBwapi(const UnitKind kind) noexcept {
    using namespace UnitTypes;
    switch (kind) {
        case UnitKind::probe: return Protoss_Probe;
        case UnitKind::nexus: return Protoss_Nexus;
        case UnitKind::pylon: return Protoss_Pylon;
        case UnitKind::assimilator: return Protoss_Assimilator;
        case UnitKind::gateway: return Protoss_Gateway;
        case UnitKind::forge: return Protoss_Forge;
        case UnitKind::photonCannon: return Protoss_Photon_Cannon;
        case UnitKind::cyberneticsCore: return Protoss_Cybernetics_Core;
        case UnitKind::shieldBattery: return Protoss_Shield_Battery;
        case UnitKind::roboticsFacility: return Protoss_Robotics_Facility;
        case UnitKind::observatory: return Protoss_Observatory;
        case UnitKind::roboticsSupportBay: return Protoss_Robotics_Support_Bay;
        case UnitKind::stargate: return Protoss_Stargate;
        case UnitKind::citadelOfAdun: return Protoss_Citadel_of_Adun;
        case UnitKind::templarArchives: return Protoss_Templar_Archives;
        case UnitKind::fleetBeacon: return Protoss_Fleet_Beacon;
        case UnitKind::arbiterTribunal: return Protoss_Arbiter_Tribunal;
        case UnitKind::zealot: return Protoss_Zealot;
        case UnitKind::dragoon: return Protoss_Dragoon;
        case UnitKind::highTemplar: return Protoss_High_Templar;
        case UnitKind::darkTemplar: return Protoss_Dark_Templar;
        case UnitKind::archon: return Protoss_Archon;
        case UnitKind::darkArchon: return Protoss_Dark_Archon;
        case UnitKind::reaver: return Protoss_Reaver;
        case UnitKind::observer: return Protoss_Observer;
        case UnitKind::shuttle: return Protoss_Shuttle;
        case UnitKind::scout: return Protoss_Scout;
        case UnitKind::corsair: return Protoss_Corsair;
        case UnitKind::carrier: return Protoss_Carrier;
        case UnitKind::arbiter: return Protoss_Arbiter;
        default: return None;
    }
}

BWAPI::TechType BwapiBridge::toBwapiTech(const TechnologyKind kind) noexcept {
    using namespace TechTypes;
    switch (kind) {
        case TechnologyKind::psionicStorm: return Psionic_Storm;
        case TechnologyKind::stasisField: return Stasis_Field;
        case TechnologyKind::recall: return Recall;
        default: return None;
    }
}

BWAPI::UpgradeType BwapiBridge::toBwapiUpgrade(const TechnologyKind kind) noexcept {
    using namespace UpgradeTypes;
    switch (kind) {
        case TechnologyKind::singularityCharge: return Singularity_Charge;
        case TechnologyKind::legEnhancements: return Leg_Enhancements;
        case TechnologyKind::khaydarinAmulet: return Khaydarin_Amulet;
        case TechnologyKind::graviticDrive: return Gravitic_Drive;
        case TechnologyKind::graviticBoosters: return Gravitic_Boosters;
        case TechnologyKind::sensorArray: return Sensor_Array;
        case TechnologyKind::reaverCapacity: return Reaver_Capacity;
        case TechnologyKind::scarabDamage: return Scarab_Damage;
        case TechnologyKind::carrierCapacity: return Carrier_Capacity;
        case TechnologyKind::protossGroundWeapons: return Protoss_Ground_Weapons;
        case TechnologyKind::protossGroundArmor: return Protoss_Ground_Armor;
        case TechnologyKind::protossPlasmaShields: return Protoss_Plasma_Shields;
        case TechnologyKind::protossAirWeapons: return Protoss_Air_Weapons;
        case TechnologyKind::protossAirArmor: return Protoss_Air_Armor;
        default: return None;
    }
}

}  // namespace protodd::bwapi
