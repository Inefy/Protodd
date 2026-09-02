#include "BwapiBridge.hpp"

#include "astra/Combat.hpp"
#include "astra/UnitCatalog.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace astra::bwapi {
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
    baseLastScouted_.clear();
    pendingBuilds_.clear();
    recentAreaSpells_.clear();
    discoverResourceClusters();
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
            if (unit != nullptr && unit->exists() && unit->isVisible()) {
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
            memory = snapshotUnit(live, false);
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

    const auto frame = state.frame;
    std::erase_if(pendingBuilds_, [frame](const auto& entry) {
        const auto kind = entry.first;
        const auto& pending = entry.second;
        if (pending.issued + 144 <= frame) return true;
        const auto builder = Broodwar->getUnit(pending.builder);
        if (builder == nullptr || !builder->exists()) return true;
        return std::ranges::any_of(Broodwar->self()->getUnits(), [kind, &pending](const Unit unit) {
            return unit != nullptr && unit->exists() && toKind(unit->getType()) == kind &&
                   closeTo(fromBwapi(unit->getPosition()), pending.target, 96);
        });
    });
    state.bases = snapshotBases(state);
    return state;
}

NavigationGrid BwapiBridge::navigationGrid() const {
    const auto width = Broodwar->mapWidth();
    const auto height = Broodwar->mapHeight();
    std::vector<std::uint8_t> cells(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0U);
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
        }
    }
    return {width, height, 32, std::move(cells)};
}

void BwapiBridge::remember(const BWAPI::Unit unit) {
    if (unit == nullptr || !unit->exists() || unit->getPlayer() != Broodwar->enemy() ||
        !unit->isVisible()) {
        return;
    }
    enemyMemory_.insert_or_assign(unit->getID(), snapshotUnit(unit, false));
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
        case CommandType::load: {
            const auto target = Broodwar->getUnit(command.targetUnit);
            return target != nullptr && actor->load(target);
        }
        case CommandType::unload:
            return command.targetPosition.valid() &&
                   actor->unloadAll(toBwapiPosition(command.targetPosition));
        case CommandType::build:
        case CommandType::gather:
        case CommandType::useTech:
            return false;  // These require richer adapter-specific arguments.
    }
    return false;
}

int BwapiBridge::executeMacro(
    const std::span<const MacroAction> actions,
    const StrategicPlan& plan,
    const int maximumCommands) {
    auto issued = 0;
    for (const auto& action : actions) {
        if (issued >= maximumCommands || !action.reserved) {
            break;
        }
        bool success = false;
        switch (action.action) {
            case MacroActionKind::build:
            case MacroActionKind::expand: success = build(action, plan); break;
            case MacroActionKind::train: success = train(action); break;
            case MacroActionKind::research:
            case MacroActionKind::upgrade: success = executeTechnology(action); break;
        }
        if (success) {
            ++issued;
        }
    }
    return issued;
}

void BwapiBridge::executeWorkers(const std::span<const WorkerAssignment> assignments) {
    std::unordered_map<UnitId, int> mineralLoad;
    for (const auto candidate : Broodwar->self()->getUnits()) {
        if (candidate == nullptr || !candidate->exists() || !candidate->getType().isWorker()) {
            continue;
        }
        const auto target = candidate->getOrderTarget();
        if (target != nullptr && target->exists() && target->getType().isMineralField()) {
            ++mineralLoad[target->getID()];
        }
    }

    for (const auto& assignment : assignments) {
        const auto worker = Broodwar->getUnit(assignment.worker);
        if (worker == nullptr || !worker->exists() || !worker->isCompleted() ||
            worker->isConstructing() || assignment.job == WorkerJob::build) {
            continue;
        }
        if (assignment.job == WorkerJob::evacuate && assignment.targetPosition.valid()) {
            worker->move(toBwapiPosition(assignment.targetPosition));
            continue;
        }
        if (assignment.job == WorkerJob::defend) {
            const auto target = Broodwar->getUnit(assignment.targetUnit);
            if (target != nullptr && target->exists() && target->isVisible()) {
                worker->attack(target);
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
            const auto currentMineral = currentTarget != nullptr && currentTarget->exists() &&
                                        currentTarget->getType().isMineralField()
                                            ? currentTarget->getID()
                                            : -1;
            if (currentMineral >= 0 && mineralLoad[currentMineral] > 0) {
                --mineralLoad[currentMineral];
            }
            std::vector<MineralPatchCandidate> candidates;
            for (const auto mineral : Broodwar->getMinerals()) {
                if (mineral == nullptr || !mineral->exists() || mineral->getResources() <= 0 ||
                    !closeTo(fromBwapi(mineral->getInitialPosition()),
                             assignment.targetPosition, 480)) {
                    continue;
                }
                candidates.push_back({mineral->getID(), fromBwapi(mineral->getPosition()),
                                      mineralLoad[mineral->getID()]});
            }
            const auto targetId = selectMineralPatch(
                candidates, assignment.targetPosition,
                fromBwapi(worker->getPosition()), currentMineral);
            if (targetId >= 0) {
                target = Broodwar->getUnit(targetId);
                ++mineralLoad[targetId];
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

    auto freeMinerals = std::max(0, self->minerals() - mineralReserve);
    auto freeGas = std::max(0, self->gas() - gasReserve);
    std::unordered_set<UnitId> spellcastersCommitted;

    for (const auto unit : self->getUnits()) {
        if (unit == nullptr || !unit->exists() || !unit->isCompleted()) {
            continue;
        }
        const auto type = unit->getType();
        if (type == UnitTypes::Protoss_Reaver && unit->getScarabCount() < 5) {
            const auto ammo = UnitTypes::Protoss_Scarab;
            if (freeMinerals >= ammo.mineralPrice() && freeGas >= ammo.gasPrice() &&
                unit->canTrain(ammo) && unit->train(ammo)) {
                freeMinerals -= ammo.mineralPrice();
                freeGas -= ammo.gasPrice();
            }
        } else if (type == UnitTypes::Protoss_Carrier && unit->getInterceptorCount() < 8 &&
                   freeMinerals >= UnitTypes::Protoss_Interceptor.mineralPrice() &&
                   freeGas >= UnitTypes::Protoss_Interceptor.gasPrice() &&
                   unit->canTrain(UnitTypes::Protoss_Interceptor) &&
                   unit->train(UnitTypes::Protoss_Interceptor)) {
            freeMinerals -= UnitTypes::Protoss_Interceptor.mineralPrice();
            freeGas -= UnitTypes::Protoss_Interceptor.gasPrice();
        }
    }

    if (self->hasResearched(TechTypes::Psionic_Storm)) {
        auto stormIssued = false;
        for (const auto templar : self->getUnits()) {
            if (templar == nullptr || templar->getType() != UnitTypes::Protoss_High_Templar ||
                templar->getEnergy() < 75 || !templar->isCompleted()) {
                continue;
            }
            Unit best = nullptr;
            auto bestScore = 1.8;
            for (const auto enemy : Broodwar->enemy()->getUnits()) {
                if (enemy == nullptr || !enemy->exists() || !enemy->isVisible() ||
                    enemy->isFlying() || enemy->isUnderStorm() ||
                    enemy->getType().isBuilding()) {
                    continue;
                }
                const auto recentlyCovered = std::ranges::any_of(
                    recentAreaSpells_, [enemy](const SpellZone& zone) {
                        return closeTo(zone.center, fromBwapi(enemy->getPosition()), 112);
                    });
                if (recentlyCovered) continue;
                auto enemyValue = 0.0;
                auto friendlyValue = 0.0;
                for (const auto nearby : Broodwar->getUnitsInRadius(enemy->getPosition(), 80)) {
                    if (nearby == nullptr || !nearby->exists() || nearby->isFlying() ||
                        nearby->getType().isBuilding()) {
                        continue;
                    }
                    const auto value = unitStats(toKind(nearby->getType())).combatValue *
                                       std::clamp(static_cast<double>(nearby->getHitPoints() +
                                                                      nearby->getShields()) /
                                                      static_cast<double>(std::max(
                                                          1, nearby->getType().maxHitPoints() +
                                                                 nearby->getType().maxShields())),
                                                  0.2, 1.0);
                    if (nearby->getPlayer() == Broodwar->enemy()) enemyValue += value;
                    if (nearby->getPlayer() == self) friendlyValue += value;
                }
                const auto score = enemyValue - friendlyValue * 1.5;
                if (score > bestScore) {
                    bestScore = score;
                    best = enemy;
                }
            }
            if (best != nullptr &&
                templar->canUseTech(TechTypes::Psionic_Storm, best->getPosition()) &&
                templar->useTech(TechTypes::Psionic_Storm, best->getPosition())) {
                recentAreaSpells_.push_back(
                    {fromBwapi(best->getPosition()), frame + 72});
                spellcastersCommitted.insert(templar->getID());
                stormIssued = true;
            }
            if (stormIssued) break;
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
    const StrategicPlan& plan,
    const ThreatAssessment& threat,
    const CombatEstimate& combat) const {
    Broodwar->drawTextScreen(8, 8, "AstraBot | %s", plan.name.c_str());
    Broodwar->drawTextScreen(8, 22, "Posture: %s | Enemy: %s (%.0f%% uncertainty)",
                            postureName(plan.posture).data(),
                            enemyPlanName(threat.mostLikely).data(), threat.uncertainty * 100.0);
    Broodwar->drawTextScreen(8, 36, "Fight ratio %.2f | %d goals | %d bases target",
                            combat.ratio, static_cast<int>(plan.goals.size()), plan.desiredBases);
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
        kind == UnitKind::scienceVessel) return UnitRole::spellcaster;
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
    if (kind == UnitKind::reaver) result.ammo = unit->getScarabCount();
    if (kind == UnitKind::carrier) result.ammo = unit->getInterceptorCount();
    if (ours && unit->getPlayer() != nullptr) {
        if (type.groundWeapon() != WeaponTypes::None) {
            result.groundWeapon.damage = unit->getPlayer()->damage(type.groundWeapon());
        }
        if (type.airWeapon() != WeaponTypes::None) {
            result.airWeapon.damage = unit->getPlayer()->damage(type.airWeapon());
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
                for (const auto queuedType : unit->getTrainingQueue()) {
                    const auto queuedKind = toKind(queuedType);
                    if (queuedKind != UnitKind::unknown) {
                        result.queuedUnits.push_back(queuedKind);
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
        const auto start = std::ranges::any_of(
            Broodwar->getStartLocations(),
            [center](const TilePosition startTile) {
                return closeTo(center, fromBwapi(BWAPI::Position(startTile)), 256);
            });
        const auto startPosition = BWAPI::Position(Broodwar->self()->getStartLocation());
        const auto island = !Broodwar->hasPath(startPosition, toBwapiPosition(center));
        bases.push_back({id, center, site.mineralLine, minerals, gas, owner,
                         baseLastScouted_[id],
                         start, island, mineralPatches, geysers});
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
    const BWAPI::Position near) const {
    BWAPI::Unit best = nullptr;
    auto bestDistance = std::numeric_limits<int>::max();
    const auto builderType = type.whatBuilds().first;
    for (const auto unit : Broodwar->self()->getUnits()) {
        if (unit == nullptr || !unit->exists() || !unit->isCompleted() ||
            unit->getType() != builderType || unit->isConstructing() || unit->isTraining()) {
            continue;
        }
        const auto distance = unit->getDistance(near);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = unit;
        }
    }
    return best;
}

BWAPI::TilePosition BwapiBridge::buildLocation(
    const UnitKind kind,
    const BWAPI::UnitType type,
    const BWAPI::Unit builder,
    const StrategicPlan& plan) const {
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
        for (const auto& site : resourceSites_) {
            const auto center = site.depotCenter;
            if (!builder->hasPath(toBwapiPosition(center))) continue;
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
            const auto score = travel + danger - static_cast<double>(resources) / 40.0;
            if (score < bestScore) {
                bestScore = score;
                best = &site;
            }
        }
        if (best != nullptr) {
            if (best->depotTile.isValid()) return best->depotTile;
            return Broodwar->getBuildLocation(
                type, TilePosition(best->depotCenter.x / 32, best->depotCenter.y / 32), 12);
        }
    }

    auto anchorPosition = plan.rallyPoint.valid()
                              ? toBwapiPosition(plan.rallyPoint)
                              : BWAPI::Position(Broodwar->self()->getStartLocation());
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
            anchorPosition = leastPoweredBase->getPosition();
        }
    }
    if (type.requiresPsi()) {
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
            const auto localPylon = Broodwar->getClosestUnit(
                forwardNexus->getPosition(),
                Filter::GetType == UnitTypes::Protoss_Pylon && Filter::IsCompleted &&
                    Filter::IsOwned);
            if (localPylon == nullptr || localPylon->getDistance(forwardNexus) > 384)
                return TilePositions::None;
            anchorPosition = localPylon->getPosition();
        }
    }

    static const std::array layout{
        TilePosition{4, 2}, TilePosition{-4, 2}, TilePosition{4, -3},
        TilePosition{-4, -3}, TilePosition{7, 1}, TilePosition{-7, 1},
        TilePosition{2, 6}, TilePosition{-2, 6}, TilePosition{2, -6},
        TilePosition{-2, -6},
    };
    const auto existing = static_cast<std::size_t>(std::ranges::count_if(
        Broodwar->self()->getUnits(), [type](const Unit unit) {
            return unit != nullptr && unit->exists() && unit->getType() == type;
        }));
    const auto anchor = TilePosition(anchorPosition);
    for (std::size_t attempt = 0; attempt < layout.size(); ++attempt) {
        const auto offset = layout[(existing + attempt) % layout.size()];
        const auto location = Broodwar->getBuildLocation(type, anchor + offset, 8);
        if (location.isValid() && !blocksMiningLane(location, type)) return location;
    }
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

bool BwapiBridge::build(const MacroAction& action, const StrategicPlan& plan) {
    const auto type = toBwapi(action.target);
    if (type == UnitTypes::None || !type.isBuilding()) {
        return false;
    }
    const auto pending = pendingBuilds_.find(action.target);
    if (pending != pendingBuilds_.end() &&
        pending->second.issued + 144 > Broodwar->getFrameCount()) {
        return false;
    }
    const auto near = plan.rallyPoint.valid() ? toBwapiPosition(plan.rallyPoint)
                                              : BWAPI::Position(Broodwar->self()->getStartLocation());
    const auto builder = findBuilder(type, near);
    if (builder == nullptr || !Broodwar->canMake(type, builder)) {
        return false;
    }
    const auto location = buildLocation(action.target, type, builder, plan);
    if (!location.isValid() ||
        (type.requiresPsi() && !Broodwar->hasPower(location, type)) ||
        !Broodwar->canBuildHere(location, type, builder, false)) {
        return false;
    }
    if (builder->build(type, location)) {
        pendingBuilds_[action.target] = {
            builder->getID(), Broodwar->getFrameCount(), fromBwapi(BWAPI::Position(location)),
        };
        return true;
    }
    return false;
}

bool BwapiBridge::train(const MacroAction& action) {
    const auto type = toBwapi(action.target);
    if (type == UnitTypes::None) {
        return false;
    }
    const auto producerType = type.whatBuilds().first;
    Unit selected = nullptr;
    for (const auto producer : Broodwar->self()->getUnits()) {
        if (producer == nullptr || !producer->exists() || !producer->isCompleted() ||
            producer->getType() != producerType || producer->isTraining() ||
            !producer->isPowered() || !producer->canTrain(type)) {
            continue;
        }
        if (selected == nullptr || producer->getID() < selected->getID()) {
            selected = producer;
        }
    }
    return selected != nullptr && Broodwar->canMake(type, selected) && selected->train(type);
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
    if (type == Zerg_Drone) return UnitKind::drone;
    if (type == Zerg_Hatchery) return UnitKind::hatchery;
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

}  // namespace astra::bwapi
