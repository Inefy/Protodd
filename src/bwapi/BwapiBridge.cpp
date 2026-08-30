#include "BwapiBridge.hpp"

#include "astra/Combat.hpp"
#include "astra/UnitCatalog.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
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
    state.bases = snapshotBases(state);
    return state;
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
            case MacroActionKind::upgrade: break;
        }
        if (success) {
            ++issued;
        }
    }
    return issued;
}

void BwapiBridge::executeWorkers(const std::span<const WorkerAssignment> assignments) {
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
        Unit target = nullptr;
        if (assignment.job == WorkerJob::gas) {
            target = Broodwar->getClosestUnit(
                worker->getPosition(),
                Filter::IsOwned && Filter::IsCompleted && Filter::IsRefinery);
        } else if (assignment.job == WorkerJob::minerals) {
            target = Broodwar->getClosestUnit(worker->getPosition(), Filter::IsMineralField);
        }
        const auto wrongJob = assignment.job == WorkerJob::gas
                                  ? !worker->isGatheringGas()
                                  : !worker->isGatheringMinerals();
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

void BwapiBridge::runMaintenance() {
    const auto self = Broodwar->self();
    if (self == nullptr) {
        return;
    }
    for (const auto unit : self->getUnits()) {
        if (unit == nullptr || !unit->exists() || !unit->isCompleted()) {
            continue;
        }
        const auto type = unit->getType();
        if (type == UnitTypes::Protoss_Reaver && unit->getScarabCount() < 5 &&
            unit->canTrain(UnitTypes::Protoss_Scarab)) {
            unit->train(UnitTypes::Protoss_Scarab);
        } else if (type == UnitTypes::Protoss_Carrier && unit->getInterceptorCount() < 8 &&
                   unit->canTrain(UnitTypes::Protoss_Interceptor)) {
            unit->train(UnitTypes::Protoss_Interceptor);
        } else if (type == UnitTypes::Protoss_Cybernetics_Core &&
                   unit->canUpgrade(UpgradeTypes::Singularity_Charge)) {
            unit->upgrade(UpgradeTypes::Singularity_Charge);
        } else if (type == UnitTypes::Protoss_Citadel_of_Adun &&
                   unit->canUpgrade(UpgradeTypes::Leg_Enhancements)) {
            unit->upgrade(UpgradeTypes::Leg_Enhancements);
        } else if (type == UnitTypes::Protoss_Templar_Archives &&
                   unit->canResearch(TechTypes::Psionic_Storm)) {
            unit->research(TechTypes::Psionic_Storm);
        } else if (type == UnitTypes::Protoss_Forge &&
                   unit->canUpgrade(UpgradeTypes::Protoss_Ground_Weapons)) {
            unit->upgrade(UpgradeTypes::Protoss_Ground_Weapons);
        }
    }

    if (!self->hasResearched(TechTypes::Psionic_Storm)) {
        return;
    }
    auto stormIssued = false;
    for (const auto templar : self->getUnits()) {
        if (templar == nullptr || templar->getType() != UnitTypes::Protoss_High_Templar ||
            templar->getEnergy() < 75 || !templar->isCompleted()) {
            continue;
        }
        Unit best = nullptr;
        auto bestScore = 2;
        for (const auto enemy : Broodwar->enemy()->getUnits()) {
            if (enemy == nullptr || !enemy->exists() || !enemy->isVisible() || enemy->isFlying()) {
                continue;
            }
            const auto clustered = static_cast<int>(Broodwar->getUnitsInRadius(
                enemy->getPosition(), 80,
                !Filter::IsOwned && !Filter::IsNeutral && !Filter::IsFlying).size());
            const auto friendly = static_cast<int>(Broodwar->getUnitsInRadius(
                enemy->getPosition(), 80, Filter::IsOwned).size());
            const auto score = clustered * 2 - friendly * 3;
            if (score > bestScore) {
                bestScore = score;
                best = enemy;
            }
        }
        if (best != nullptr && templar->canUseTech(TechTypes::Psionic_Storm, best->getPosition())) {
            templar->useTech(TechTypes::Psionic_Storm, best->getPosition());
            stormIssued = true;
        }
        if (stormIssued) {
            break;
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
            toDamageType(type.damageType()), type.targetsAir(), type.targetsGround()};
}

UnitSnapshot BwapiBridge::snapshotUnit(const BWAPI::Unit unit, const bool ours) {
    const auto type = unit->getType();
    const auto kind = toKind(type);
    const auto buildTime = std::max(1, type.buildTime());
    return {
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
        result.units.reserve(player->getUnits().size());
        for (const auto unit : player->getUnits()) {
            if (unit != nullptr && unit->exists()) {
                result.units.push_back(snapshotUnit(unit, true));
            }
        }
        std::ranges::sort(result.units, {}, &UnitSnapshot::id);
    }
    return result;
}

std::vector<BaseSnapshot> BwapiBridge::snapshotBases(const GameState& state) {
    std::vector<BaseSnapshot> bases;
    bases.reserve(resourceClusters_.size());
    auto id = 0;
    for (const auto center : resourceClusters_) {
        ++id;
        auto minerals = 0;
        auto gas = 0;
        for (const auto patch : Broodwar->getMinerals()) {
            if (closeTo(center, fromBwapi(patch->getInitialPosition()), 320)) {
                minerals += patch->getResources();
            }
        }
        for (const auto geyser : Broodwar->getGeysers()) {
            if (closeTo(center, fromBwapi(geyser->getInitialPosition()), 320)) {
                gas += geyser->getResources();
            }
        }

        auto owner = -1;
        for (const auto& depot : state.self.units) {
            if (depot.role == UnitRole::resourceDepot && closeTo(center, depot.position, 320)) {
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
        bases.push_back({id, center, center, minerals, gas, owner, baseLastScouted_[id],
                         start, false});
    }
    return bases;
}

void BwapiBridge::discoverResourceClusters() {
    resourceClusters_.clear();
    std::vector<Position> resources;
    for (const auto mineral : Broodwar->getMinerals()) {
        resources.push_back(fromBwapi(mineral->getInitialPosition()));
    }
    for (const auto geyser : Broodwar->getGeysers()) {
        resources.push_back(fromBwapi(geyser->getInitialPosition()));
    }
    std::ranges::sort(resources, {}, &Position::x);
    std::vector<bool> claimed(resources.size(), false);
    for (std::size_t seed = 0; seed < resources.size(); ++seed) {
        if (claimed[seed]) continue;
        long long sumX = 0;
        long long sumY = 0;
        auto count = 0;
        for (std::size_t i = 0; i < resources.size(); ++i) {
            if (!claimed[i] && closeTo(resources[seed], resources[i], 320)) {
                claimed[i] = true;
                sumX += resources[i].x;
                sumY += resources[i].y;
                ++count;
            }
        }
        if (count >= 4) {
            resourceClusters_.push_back({static_cast<int>(sumX / count),
                                         static_cast<int>(sumY / count)});
        }
    }
    for (const auto start : Broodwar->getStartLocations()) {
        const auto center = fromBwapi(BWAPI::Position(start));
        if (std::ranges::none_of(resourceClusters_, [center](const Position candidate) {
                return closeTo(center, candidate, 320);
            })) {
            resourceClusters_.push_back(center);
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
        const auto geyser = Broodwar->getClosestUnit(
            builder->getPosition(), Filter::GetType == UnitTypes::Resource_Vespene_Geyser);
        return geyser != nullptr ? geyser->getTilePosition() : TilePositions::None;
    }
    if (kind == UnitKind::nexus) {
        Position best{-1, -1};
        auto bestScore = std::numeric_limits<double>::infinity();
        for (const auto center : resourceClusters_) {
            const auto occupied = std::ranges::any_of(
                Broodwar->getAllUnits(),
                [center](const Unit unit) {
                    return unit != nullptr && unit->exists() &&
                           unit->getType().isResourceDepot() &&
                           closeTo(center, fromBwapi(unit->getPosition()), 320);
                });
            if (occupied) continue;
            const auto riskBias = plan.posture == Posture::defend ? 1.4 : 1.0;
            const auto score = distance(fromBwapi(builder->getPosition()), center) * riskBias;
            if (score < bestScore) {
                bestScore = score;
                best = center;
            }
        }
        if (best.valid()) {
            return Broodwar->getBuildLocation(type, TilePosition(best.x / 32, best.y / 32), 10);
        }
    }

    auto anchor = Broodwar->self()->getStartLocation();
    if (plan.rallyPoint.valid()) {
        anchor = TilePosition(plan.rallyPoint.x / 32, plan.rallyPoint.y / 32);
    }
    const auto offset = kind == UnitKind::photonCannon ? TilePosition(5, 2)
                                                       : TilePosition(3, 5);
    return Broodwar->getBuildLocation(type, anchor + offset, 18);
}

bool BwapiBridge::build(const MacroAction& action, const StrategicPlan& plan) {
    const auto type = toBwapi(action.target);
    if (type == UnitTypes::None || !type.isBuilding() || !Broodwar->canMake(type)) {
        return false;
    }
    const auto pending = pendingBuilds_.find(action.target);
    if (pending != pendingBuilds_.end() && pending->second + 96 > Broodwar->getFrameCount()) {
        return false;
    }
    const auto near = plan.rallyPoint.valid() ? toBwapiPosition(plan.rallyPoint)
                                              : BWAPI::Position(Broodwar->self()->getStartLocation());
    const auto builder = findBuilder(type, near);
    if (builder == nullptr) {
        return false;
    }
    const auto location = buildLocation(action.target, type, builder, plan);
    if (!location.isValid() || !Broodwar->canBuildHere(location, type, builder, false)) {
        return false;
    }
    if (builder->build(type, location)) {
        pendingBuilds_[action.target] = Broodwar->getFrameCount();
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
    for (const auto producer : Broodwar->self()->getUnits()) {
        if (producer != nullptr && producer->exists() && producer->isCompleted() &&
            producer->getType() == producerType && producer->getTrainingQueue().size() < 2 &&
            producer->canTrain(type) && producer->train(type)) {
            return true;
        }
    }
    return false;
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

}  // namespace astra::bwapi
