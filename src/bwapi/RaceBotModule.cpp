#include "RaceBotModule.hpp"
#include "ProductionCount.hpp"
#include "TerranDetection.hpp"
#include "SupplyPlanning.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <utility>

namespace protodd::bwapi {
namespace {
using namespace BWAPI;
bool alive(Unit u) { return u && u->exists(); }
bool soldier(Unit u) {
    if (!alive(u) || !u->isCompleted()) return false;
    const auto t = u->getType();
    return t == UnitTypes::Terran_Marine || t == UnitTypes::Terran_Medic ||
           t == UnitTypes::Zerg_Zergling || t == UnitTypes::Zerg_Hydralisk;
}
bool ready(Unit u) {
    return alive(u) && Broodwar->getFrameCount() - u->getLastCommandFrame() >=
        std::max(8, Broodwar->getLatencyFrames() + 2);
}
}

RaceBotModule::RaceBotModule(int brandedRace) : brandedRace_(brandedRace) {}

void RaceBotModule::configure(int opening, int posture) noexcept {
    choice_ = {std::clamp(opening, 0, 2), std::clamp(posture, 0, 2)};
}

void RaceBotModule::setActionChoiceHook(ActionChoiceHook hook) { hook_ = std::move(hook); }

void RaceBotModule::onStart() {
    supported_ = false;
    if (!BWAPI::BroodwarPtr || BWAPI::Broodwar->isReplay() || !BWAPI::Broodwar->self()) return;
    const auto race = BWAPI::Broodwar->self()->getRace();
    terran_ = race == BWAPI::Races::Terran;
    supported_ = terran_ || race == BWAPI::Races::Zerg;
    construction_ = {};
    enemyBuildings_.clear();
    searchPoints_.clear();
    searchIndex_ = 0;
    searchTarget_ = BWAPI::Positions::None;
    attacking_ = false;
    economyMode_ = false;
    lastMacro_ = lastCombat_ = lastPolicy_ = lastSearch_ = -1000;
    lastLogFrame_ = -240;
    lastAcceptedFrame_ = -1;
    acceptedProduction_ = acceptedBuilds_ = acceptedResumes_ = 0;
    observedBuildStarts_ = abandonedBuildOrders_ = 0;
    acceptedByType_.clear();
    buildDiagnostics_.clear();
    for (const auto tile : BWAPI::Broodwar->getStartLocations()) {
        const BWAPI::Position p = BWAPI::Position(tile) + BWAPI::Position(64, 48);
        if (p.getDistance(home()) > 320) searchPoints_.push_back(p);
    }
    // Public map dimensions/start locations; enemies are learned only when visible.
    for (int y = 4; y < BWAPI::Broodwar->mapHeight(); y += 12)
        for (int x = 4; x < BWAPI::Broodwar->mapWidth(); x += 12)
            searchPoints_.push_back(BWAPI::Position(BWAPI::TilePosition(x, y)));
    BWAPI::Broodwar->printf("%s: playable baseline; actual race %s",
        brandedRace_ == 2 ? "ZergTodd" : "TerranTodd", race.c_str());
    if (supported_) {
        policy_.start();
        if (log_.is_open()) log_.close();
        log_.clear();
        log_.open("bwapi-data/write/RaceBot.log", std::ios::trunc);
        log_ << "START," << race.getName() << ',' << BWAPI::Broodwar->getRandomSeed()
             << ",version=1,brand=" << (brandedRace_ == 2 ? "ZergTodd" : "TerranTodd")
             << ",policy_enabled=" << policy_.enabled() << '\n';
        log_.flush();
        if (!log_) BWAPI::Broodwar->printf("RaceBot: unable to write bwapi-data/write/RaceBot.log");
        logSnapshot();
    }
}

void RaceBotModule::onEnd(bool winner) {
    if (supported_ && BWAPI::BroodwarPtr) {
        logSnapshot();
        policy_.end(winner);
        log_ << "END," << BWAPI::Broodwar->getFrameCount() << ',' << (winner ? 1 : 0) << '\n';
        log_.flush();
        log_.close();
    }
}

void RaceBotModule::logSnapshot() noexcept {
    if (!log_ || !BWAPI::BroodwarPtr || !BWAPI::Broodwar->self()) return;
    try {
        const auto o = observation();
        const auto self = BWAPI::Broodwar->self();
        int buildings = 0, completedBuildings = 0, mining = 0, gasWorkers = 0;
        int idleWorkers = 0, constructingWorkers = 0, eggs = 0, training = 0;
        std::map<int, int> completedTypes, incompleteTypes, rawQueueTypes, visibleEnemyTypes;
        int undetectedEnemies = 0, attackingMarines = 0, movingMarines = 0;
        for (auto enemy : BWAPI::Broodwar->getAllUnits()) {
            if (!alive(enemy) || !enemy->isVisible() || !enemy->getPlayer() ||
                !self->isEnemy(enemy->getPlayer())) continue;
            ++visibleEnemyTypes[enemy->getType().getID()];
            if (!enemy->isDetected()) ++undetectedEnemies;
        }
        for (auto u : self->getUnits()) {
            if (!alive(u)) continue;
            if (u->getType().isBuilding()) {
                ++buildings;
                if (u->isCompleted()) ++completedBuildings;
            }
            if (u->isCompleted()) ++completedTypes[u->getType().getID()];
            else ++incompleteTypes[u->getType().getID()];
            for (const auto queued : u->getTrainingQueue()) ++rawQueueTypes[queued.getID()];
            if (u->getType().isWorker() && u->isCompleted()) {
                if (u->isGatheringMinerals()) ++mining;
                if (u->isGatheringGas()) ++gasWorkers;
                if (u->isIdle()) ++idleWorkers;
                if (u->isConstructing()) ++constructingWorkers;
            }
            if (u->getType() == BWAPI::UnitTypes::Zerg_Egg) ++eggs;
            if (u->isTraining()) ++training;
            if (u->getType() == BWAPI::UnitTypes::Terran_Marine && u->isCompleted()) {
                if (u->isAttacking()) ++attackingMarines;
                if (u->isMoving()) ++movingMarines;
                const auto target = u->getOrderTarget();
                log_ << "MARINE," << o.frame << ",id=" << u->getID()
                     << ",hp=" << u->getHitPoints() << ",cooldown=" << u->getGroundWeaponCooldown()
                     << ",home_distance=" << u->getDistance(home())
                     << ",order=" << u->getOrder().getID()
                     << ",target_type=" << (alive(target) && target->isVisible() ? target->getType().getID() : -1)
                     << ",target_distance=" << (alive(target) && target->isVisible() ? u->getDistance(target) : -1)
                     << '\n';
            }
        }
        // Accepted counts are cumulative commands, NOT completions. One ling egg
        // is one accepted production command. Supply is in BWAPI doubled units.
        log_ << "SNAPSHOT," << o.frame << ",workers=" << o.workers
             << ",army=" << o.army << ",bases=" << o.bases
             << ",buildings=" << buildings << ",completed_buildings=" << completedBuildings
             << ",mineral_workers=" << mining << ",gas_workers=" << gasWorkers
             << ",idle_workers=" << idleWorkers << ",constructing_workers=" << constructingWorkers
             << ",minerals=" << o.minerals << ",gas=" << o.gas
             << ",gathered_minerals=" << self->gatheredMinerals() << ",gathered_gas=" << self->gatheredGas()
             << ",supply=" << o.supplyUsed << '/' << o.supplyTotal
             << ",supply_incoming=" << incomingSupply() << ",eggs=" << eggs << ",training=" << training
             << ",enemyVisible=" << o.visibleEnemyArmy << ",base_threats=" << o.visibleBaseThreats
             << ",visible_undetected=" << undetectedEnemies
             << ",attacking_marines=" << attackingMarines << ",moving_marines=" << movingMarines
             << ",opening=" << choice_.opening << ",posture=" << choice_.posture
             << ",economy=" << economyMode_ << ",policy_enabled=" << policy_.enabled()
             << ",accepted_production=" << acceptedProduction_ << ",accepted_builds=" << acceptedBuilds_
             << ",accepted_resumes=" << acceptedResumes_ << ",observed_build_starts=" << observedBuildStarts_
             << ",abandoned_build_orders=" << abandonedBuildOrders_ << ",last_accepted_frame=" << lastAcceptedFrame_
             << ",pending_build_type=" << (construction_.issued >= 0 ? construction_.type.getID() : -1)
             << ",pending_build_age=" << (construction_.issued >= 0 ? o.frame - construction_.issued : 0);
        // Per-type keys use BWAPI UnitType IDs; absent keys mean zero.
        for (const auto& entry : completedTypes)
            log_ << ",completed_type_" << entry.first << '=' << entry.second;
        for (const auto& entry : incompleteTypes)
            log_ << ",incomplete_type_" << entry.first << '=' << entry.second;
        for (const auto& entry : rawQueueTypes)
            log_ << ",raw_queue_type_" << entry.first << '=' << entry.second;
        for (const auto& entry : visibleEnemyTypes)
            log_ << ",visible_enemy_type_" << entry.first << '=' << entry.second;
        for (const auto& entry : acceptedByType_)
            log_ << ",accepted_type_" << entry.first << '=' << entry.second;
        for (const auto& entry : buildDiagnostics_)
            log_ << ",build_" << entry.first << '=' << entry.second;
        log_ << '\n';
        log_.flush();
        lastLogFrame_ = o.frame;
    } catch (...) { /* Telemetry failure must not interrupt play. */ }
}

std::vector<BWAPI::Unit> RaceBotModule::bases() const {
    std::vector<BWAPI::Unit> result;
    for (auto u : BWAPI::Broodwar->self()->getUnits())
        if (alive(u) && u->getType().isResourceDepot() && u->isCompleted() && !u->isLifted())
            result.push_back(u);
    return result;
}

BWAPI::Position RaceBotModule::home() const {
    const auto owned = bases();
    if (!owned.empty()) return owned.front()->getPosition();
    return BWAPI::Position(BWAPI::Broodwar->self()->getStartLocation()) + BWAPI::Position(64, 48);
}

int RaceBotModule::count(BWAPI::UnitType type, bool completedOnly) const {
    int n = 0;
    for (auto u : BWAPI::Broodwar->self()->getUnits()) {
        if (!alive(u)) continue;
        if (u->getType() == type && (!completedOnly || u->isCompleted())) ++n;
        if (completedOnly) continue;
        int matchingQueue = 0;
        for (const auto queued : u->getTrainingQueue()) if (queued == type) ++matchingQueue;
        n += pendingProductionCount(u->getType(), type, u->getBuildType(), matchingQueue);
    }
    if (!completedOnly && construction_.issued >= 0 && construction_.type == type) ++n;
    return n;
}

int RaceBotModule::incomingSupply() const {
    int n = 0;
    for (auto u : BWAPI::Broodwar->self()->getUnits()) {
        if (!alive(u)) continue;
        if (!u->isCompleted()) n += u->getType().supplyProvided();
        if (u->getType() == BWAPI::UnitTypes::Zerg_Egg)
            n += u->getBuildType().supplyProvided();
    }
    if (construction_.issued >= 0) n += construction_.type.supplyProvided();
    return n;
}

bool RaceBotModule::affordable(BWAPI::UnitType type) const {
    const auto self = BWAPI::Broodwar->self();
    // One spending command per macro tick; reserve the exact outstanding build.
    const int minerals = construction_.issued >= 0 ? construction_.type.mineralPrice() : 0;
    const int gas = construction_.issued >= 0 ? construction_.type.gasPrice() : 0;
    const int supply = type.supplyRequired() * (type.isTwoUnitsInOneEgg() ? 2 : 1);
    return self->minerals() >= type.mineralPrice() + minerals &&
        self->gas() >= type.gasPrice() + gas &&
        (type.isBuilding() || self->supplyUsed() + supply <= self->supplyTotal());
}

bool RaceBotModule::reserved(BWAPI::Unit worker) const {
    return construction_.issued >= 0 && construction_.worker == worker;
}

BWAPI::Unit RaceBotModule::workerNear(BWAPI::Position position) const {
    BWAPI::Unit best = nullptr;
    int distance = std::numeric_limits<int>::max();
    for (auto u : BWAPI::Broodwar->self()->getUnits()) {
        if (!alive(u) || !u->getType().isWorker() || !u->isCompleted() ||
            !ready(u) || reserved(u) || u->isConstructing() || u->isMorphing() ||
            u->isRepairing() || u->isGatheringGas() || !u->hasPath(position)) continue;
        const int d = u->getDistance(position) + (u->isCarryingMinerals() ? 96 : 0);
        if (d < distance) { distance = d; best = u; }
    }
    return best;
}

BWAPI::Unit RaceBotModule::threatNear(BWAPI::Position position, int radius) const {
    BWAPI::Unit best = nullptr;
    int distance = radius;
    for (auto u : BWAPI::Broodwar->getAllUnits()) {
        if (!alive(u) || !u->isVisible() || !u->getPlayer() ||
            !BWAPI::Broodwar->self()->isEnemy(u->getPlayer())) continue;
        if (!u->getType().canAttack() && !u->getType().isWorker()) continue;
        const int d = u->getDistance(position);
        if (d < distance) { best = u; distance = d; }
    }
    return best;
}

bool RaceBotModule::wantsGas() const {
    if (terran_) return choice_.opening != 1 && count(BWAPI::UnitTypes::Terran_Marine) >= 8;
    return choice_.opening == 2 || count(BWAPI::UnitTypes::Zerg_Zergling) >= 16;
}

bool RaceBotModule::build(BWAPI::UnitType type) {
    const auto diagnostic = [this, type](const char* reason) {
        ++buildDiagnostics_[std::to_string(type.getID()) + '_' + reason];
    };
    diagnostic("attempt");
    if (construction_.issued >= 0) { diagnostic("leased"); return false; }
    if (!affordable(type)) { diagnostic("unfunded"); return false; }
    auto owned = bases();
    // Allow rebuilding a lost depot at the original base.
    std::vector<BWAPI::Position> anchors;
    for (auto base : owned) anchors.push_back(base->getPosition());
    if (anchors.empty()) anchors.push_back(home());
    for (const auto anchor : anchors) {
        if (threatNear(anchor, 320)) { diagnostic("threat"); continue; }
        auto worker = workerNear(anchor);
        if (!worker || !worker->canBuild(type)) { diagnostic("worker_ineligible"); continue; }
        diagnostic("placement_search");
        auto issue = [&](BWAPI::TilePosition tile) {
            if (!tile.isValid() || !BWAPI::Broodwar->canBuildHere(tile, type, worker, true) ||
                !worker->hasPath(BWAPI::Position(tile)) || !worker->canBuild(type, tile)) return false;
            if (!worker->build(type, tile)) { diagnostic("command_rejected"); return false; }
            construction_ = {worker, type, tile, BWAPI::Broodwar->getFrameCount()};
            ++acceptedBuilds_;
            ++acceptedByType_[type.getID()];
            lastAcceptedFrame_ = BWAPI::Broodwar->getFrameCount();
            return true;
        };
        if (type.isRefinery()) {
            for (auto geyser : BWAPI::Broodwar->getGeysers())
                if (alive(geyser) && geyser->isVisible() && geyser->getDistance(anchor) < 320 &&
                    issue(geyser->getTilePosition())) return true;
            continue;
        }
        const BWAPI::TilePosition center(anchor);
        // Bounded, near-base footprint search. BWAPI checks creep, occupancy and prerequisites.
        for (int r = 2; r <= 12; ++r) {
            for (int dy = -r; dy <= r; ++dy) for (int dx = -r; dx <= r; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                const BWAPI::TilePosition tile(center.x + dx, center.y + dy);
                if (!tile.isValid()) continue;
                const BWAPI::Position p = BWAPI::Position(tile) +
                    BWAPI::Position(type.tileWidth() * 16, type.tileHeight() * 16);
                bool mineralLane = false;
                for (auto mineral : BWAPI::Broodwar->getMinerals()) {
                    if (!alive(mineral) || !mineral->isVisible() || mineral->getDistance(anchor) > 384) continue;
                    // Preserve the harvesting corridor and room around mineral patches.
                    if (p.getDistance(mineral->getPosition()) < 112 ||
                        p.getDistance(anchor) + p.getDistance(mineral->getPosition()) <
                            anchor.getDistance(mineral->getPosition()) + 96) {
                        mineralLane = true;
                        break;
                    }
                }
                if (!mineralLane && issue(tile)) return true;
            }
        }
    }
    diagnostic("no_accepted_location");
    return false;
}

bool RaceBotModule::produce(BWAPI::UnitType type) {
    if (!affordable(type)) return false;
    for (auto u : BWAPI::Broodwar->self()->getUnits()) {
        if (!ready(u) || !u->isCompleted()) continue;
        bool accepted = false;
        if (terran_) {
            if (u->getType() == type.whatBuilds().first && !u->isTraining() &&
                u->getTrainingQueue().empty() && u->canTrain(type)) accepted = u->train(type);
        } else if (u->getType() == BWAPI::UnitTypes::Zerg_Larva &&
                   u->canMorph(type)) accepted = u->morph(type);
        if (accepted) {
            ++acceptedProduction_;
            ++acceptedByType_[type.getID()];
            lastAcceptedFrame_ = BWAPI::Broodwar->getFrameCount();
            return true;
        }
    }
    return false;
}

bool RaceBotModule::maintainConstruction() {
    using namespace BWAPI;
    if (construction_.issued >= 0) {
        bool started = false;
        for (auto u : Broodwar->self()->getUnits())
            if (alive(u) && u->getType() == construction_.type &&
                u->getTilePosition() == construction_.tile) { started = true; break; }
        if (started || !alive(construction_.worker) ||
            Broodwar->getFrameCount() - construction_.issued > 480) {
            const auto worker = construction_.worker;
            if (started) ++observedBuildStarts_;
            else ++abandonedBuildOrders_;
            construction_ = {};
            // Release a timed-out walking builder, but never cancel an active structure.
            if (!started && alive(worker) && worker->getType().isWorker() && ready(worker)) worker->stop();
        }
    }
    if (!terran_) return false;
    for (auto building : Broodwar->self()->getUnits()) {
        if (!alive(building) || !building->getType().isBuilding() || building->isCompleted() ||
            building->getType().getRace() != Races::Terran) continue;
        bool assigned = false;
        for (auto worker : Broodwar->self()->getUnits()) {
            if (!alive(worker) || !worker->getType().isWorker()) continue;
            if (worker->getBuildUnit() == building || worker->getOrderTarget() == building ||
                worker->getTarget() == building) { assigned = true; break; }
        }
        if (assigned) continue;
        auto worker = workerNear(building->getPosition());
        if (worker && worker->canRightClick(building) && worker->rightClick(building)) {
            ++acceptedResumes_;
            lastAcceptedFrame_ = Broodwar->getFrameCount();
            return true;
        }
    }
    return false;
}

void RaceBotModule::macro() {
    using namespace BWAPI;
    const auto self = Broodwar->self();
    const auto workerType = terran_ ? UnitTypes::Terran_SCV : UnitTypes::Zerg_Drone;
    const auto depotType = terran_ ? UnitTypes::Terran_Command_Center : UnitTypes::Zerg_Hatchery;
    const auto supplyType = terran_ ? UnitTypes::Terran_Supply_Depot : UnitTypes::Zerg_Overlord;
    const int workerCount = count(workerType);
    const auto owned = bases();
    if (owned.empty() && count(depotType) == 0 && build(depotType)) return;
    int patches = 0;
    for (auto mineral : Broodwar->getMinerals()) {
        if (!alive(mineral) || !mineral->isVisible() || mineral->getResources() <= 0) continue;
        for (auto base : owned) if (mineral->getDistance(base) < 384) { ++patches; break; }
    }
    const bool gas = wantsGas();
    const int workerGoal = std::clamp(patches * 2 + (gas ? 3 : 0) + (economyMode_ ? 2 : 0), 10, 48);
    const int supplyBuffer = supplyPlanningBuffer(terran_, self->supplyUsed());
    if (self->supplyTotal() < 400 &&
        self->supplyTotal() + incomingSupply() - self->supplyUsed() <= supplyBuffer &&
        (terran_ ? build(supplyType) : produce(supplyType))) return;
    const auto core = terran_ ? UnitTypes::Terran_Barracks : UnitTypes::Zerg_Spawning_Pool;
    const int openingWorkers = choice_.opening == 1 ? 8 : 10;
    if (workerCount >= openingWorkers && count(core) == 0 && build(core)) return;
    // Recover economy even after catastrophic worker losses; normal builds are checked by BWAPI.
    if (workerCount < std::min(workerGoal, 14) && produce(workerType)) return;
    if (economyMode_ && workerCount < workerGoal &&
        (terran_ || count(UnitTypes::Zerg_Zergling) + count(UnitTypes::Zerg_Hydralisk) >= 6) &&
        produce(workerType)) return;
    const auto refinery = terran_ ? UnitTypes::Terran_Refinery : UnitTypes::Zerg_Extractor;
    if (gas && count(core, true) > 0 && workerCount >= 12 && count(refinery) == 0 && build(refinery)) return;
    if (terran_) {
        const auto enemy = Broodwar->enemy();
        if (wantsTerranDetection(enemy && enemy->getRace() == Races::Protoss,
                                Broodwar->getFrameCount(), workerCount,
                                count(UnitTypes::Terran_Marine, true))) {
            if (count(UnitTypes::Terran_Engineering_Bay) == 0 &&
                build(UnitTypes::Terran_Engineering_Bay)) return;
            if (count(UnitTypes::Terran_Engineering_Bay, true) &&
                count(UnitTypes::Terran_Missile_Turret) == 0 &&
                build(UnitTypes::Terran_Missile_Turret)) return;
        }
        if (gas && count(refinery, true) && count(UnitTypes::Terran_Academy) == 0 &&
            build(UnitTypes::Terran_Academy)) return;
        const int desiredBarracks = choice_.opening == 1 ? 4 : 3;
        if (workerCount >= 14 && count(core) < desiredBarracks &&
            self->minerals() >= (count(core) < 2 ? 200 : 350) && build(core)) return;
        if (workerCount < workerGoal && produce(workerType)) return;
        if (count(UnitTypes::Terran_Academy, true) &&
            count(UnitTypes::Terran_Medic) < count(UnitTypes::Terran_Marine) / 6 &&
            produce(UnitTypes::Terran_Medic)) return;
        produce(UnitTypes::Terran_Marine);
    } else {
        if (gas && count(refinery, true) && count(core, true) &&
            count(UnitTypes::Zerg_Hydralisk_Den) == 0 && build(UnitTypes::Zerg_Hydralisk_Den)) return;
        // Extra hatcheries are local production, not claims of an expansion economy.
        if (workerCount >= 14 && count(depotType) < 3 && self->minerals() >= 400 && build(depotType)) return;
        const int army = count(UnitTypes::Zerg_Zergling) + count(UnitTypes::Zerg_Hydralisk);
        if (workerCount < workerGoal && (army >= 6 || !count(core, true)) && produce(workerType)) return;
        if (count(UnitTypes::Zerg_Hydralisk_Den, true) && produce(UnitTypes::Zerg_Hydralisk)) return;
        produce(UnitTypes::Zerg_Zergling);
    }
}

void RaceBotModule::workers() {
    using namespace BWAPI;
    std::vector<Unit> minerals, refineries;
    const auto owned = bases();
    for (auto m : Broodwar->getMinerals()) {
        if (!alive(m) || !m->isVisible() || m->getResources() <= 0) continue;
        for (auto base : owned) if (m->getDistance(base) < 384) { minerals.push_back(m); break; }
    }
    if (wantsGas() && Broodwar->self()->gas() < 300)
        for (auto u : Broodwar->self()->getUnits())
            if (alive(u) && u->isCompleted() && u->getType().isRefinery() && u->getResources() > 0)
                for (auto base : owned) if (u->getDistance(base) < 384) { refineries.push_back(u); break; }
    std::map<int, int> loads;
    for (auto w : Broodwar->self()->getUnits()) {
        if (!alive(w) || !w->getType().isWorker()) continue;
        const auto target = w->getOrderTarget() ? w->getOrderTarget() : w->getTarget();
        if (alive(target)) ++loads[target->getID()];
    }
    int defenders = 0;
    for (auto w : Broodwar->self()->getUnits()) {
        if (!ready(w) || !w->getType().isWorker() || !w->isCompleted() ||
            reserved(w) || w->isConstructing() || w->isMorphing() || w->isRepairing()) continue;
        // A resumed SCV walking toward an unfinished building must keep its assignment.
        const auto target = w->getOrderTarget() ? w->getOrderTarget() : w->getTarget();
        if (alive(target) && target->getPlayer() == Broodwar->self() &&
            target->getType().isBuilding() && !target->isCompleted()) continue;
        auto threat = threatNear(w->getPosition(), 160);
        if (threat && !threat->isFlying() && w->getDistance(home()) < 448 && defenders < 4 &&
            w->getHitPoints() > 20 && w->canAttack(threat)) {
            ++defenders;
            if (w->getOrderTarget() != threat) w->attack(threat);
            continue;
        }
        if (w->isCarryingGas() || w->isCarryingMinerals()) {
            if (w->isIdle()) w->returnCargo();
            continue;
        }
        const bool currentGas = alive(target) &&
            std::find(refineries.begin(), refineries.end(), target) != refineries.end();
        if (currentGas && loads[target->getID()] <= 3 && !w->isIdle()) continue;
        Unit gasTarget = nullptr;
        for (auto gas : refineries)
            if (loads[gas->getID()] < 3 && w->canGather(gas) && w->hasPath(gas)) {
                gasTarget = gas; break;
            }
        if (gasTarget && w->gather(gasTarget)) {
            if (alive(target)) --loads[target->getID()];
            ++loads[gasTarget->getID()];
            continue;
        }
        if (w->isGatheringMinerals() && alive(target) && target->getType().isMineralField() &&
            target->getResources() > 0 && loads[target->getID()] <= 2) continue;
        Unit best = nullptr;
        int score = std::numeric_limits<int>::max();
        for (auto m : minerals) {
            const int cost = w->getDistance(m) + 128 * loads[m->getID()];
            if (cost < score && w->canGather(m) && w->hasPath(m)) { best = m; score = cost; }
        }
        if (best && (target != best || w->isIdle()) && w->gather(best)) {
            if (alive(target)) --loads[target->getID()];
            ++loads[best->getID()];
        }
    }
}

void RaceBotModule::rememberEnemies() {
    using namespace BWAPI;
    std::map<int, TilePosition> visible;
    for (auto u : Broodwar->getAllUnits())
        if (alive(u) && u->isVisible() && u->getPlayer() &&
            Broodwar->self()->isEnemy(u->getPlayer()) && u->getType().isBuilding() && !u->isLifted())
            visible[u->getID()] = u->getTilePosition();
    for (auto it = enemyBuildings_.begin(); it != enemyBuildings_.end();) {
        if (Broodwar->isVisible(it->second) && visible.find(it->first) == visible.end())
            it = enemyBuildings_.erase(it);
        else ++it;
    }
    for (const auto& item : visible) enemyBuildings_[item.first] = item.second;
}

BWAPI::Position RaceBotModule::attackDestination(BWAPI::Unit leader) {
    using namespace BWAPI;
    Position best = Positions::None;
    int distance = std::numeric_limits<int>::max();
    for (const auto& entry : enemyBuildings_) {
        const Position p = Position(entry.second) + Position(32, 32);
        const int d = leader->getDistance(p);
        if (d < distance && leader->hasPath(p)) { best = p; distance = d; }
    }
    if (best.isValid()) return best;
    if (searchTarget_.isValid() && leader->getDistance(searchTarget_) > 160 &&
        Broodwar->getFrameCount() - lastSearch_ < 960) return searchTarget_;
    for (std::size_t i = 0; i < searchPoints_.size(); ++i) {
        const auto p = searchPoints_[searchIndex_++ % searchPoints_.size()];
        if (leader->hasPath(p) && leader->getDistance(p) > 160) {
            searchTarget_ = p;
            lastSearch_ = Broodwar->getFrameCount();
            return p;
        }
    }
    return home();
}

void RaceBotModule::combat() {
    using namespace BWAPI;
    std::vector<Unit> army;
    for (auto u : Broodwar->self()->getUnits()) if (soldier(u)) army.push_back(u);
    Unit threat = nullptr;
    for (auto base : bases()) {
        threat = threatNear(base->getPosition(), 640);
        if (threat) break;
    }
    const int threshold = economyMode_ ? 20 : choice_.opening == 1 ? 8 : 14;
    if (static_cast<int>(army.size()) >= threshold) attacking_ = true;
    if (army.size() < 4) attacking_ = false;
    const bool attack = choice_.posture == 2 || (choice_.posture == 0 && attacking_);
    Position destination = home();
    if (threat) destination = threat->getPosition();
    else if (attack && !army.empty()) destination = attackDestination(army.front());
    for (auto u : army) {
        if (!ready(u)) continue;
        if (u->getType() == UnitTypes::Terran_Medic) {
            Unit wounded = nullptr;
            int distance = 320;
            for (auto ally : army) {
                if (ally == u || ally->getType() == UnitTypes::Terran_Medic ||
                    ally->getHitPoints() >= ally->getType().maxHitPoints()) continue;
                const int d = u->getDistance(ally);
                if (d < distance && u->canUseTech(TechTypes::Healing, ally)) { wounded = ally; distance = d; }
            }
            if (wounded) {
                if (u->getOrderTarget() != wounded) u->useTech(TechTypes::Healing, wounded);
            } else {
                Unit escort = nullptr;
                for (auto ally : army) if (ally->getType() == UnitTypes::Terran_Marine) { escort = ally; break; }
                const Position p = escort ? escort->getPosition() : destination;
                if (u->getDistance(p) > 96 && (u->isIdle() || u->getOrderTargetPosition().getDistance(p) > 96)) u->move(p);
            }
            continue;
        }
        if (u->isAttackFrame()) continue;
        if (threat && u->getDistance(threat) < 384 && u->canAttack(threat)) {
            if (u->getOrderTarget() != threat) u->attack(threat);
        } else if ((u->isIdle() || u->getOrderTargetPosition().getDistance(destination) > 128) &&
                   u->getDistance(destination) > 96 && u->hasPath(destination) && u->canAttack(destination)) {
            u->attack(destination);
        }
    }
    // Supply providers stay behind the army; do not scout them into enemy defenses.
    if (!terran_)
        for (auto u : Broodwar->self()->getUnits())
            if (ready(u) && u->getType() == UnitTypes::Zerg_Overlord && u->isIdle() && u->getDistance(home()) > 192)
                u->move(home());
}

RaceBotModule::Observation RaceBotModule::observation() const {
    Observation o;
    if (!BWAPI::BroodwarPtr || !BWAPI::Broodwar->self()) return o;
    const auto self = BWAPI::Broodwar->self();
    o.frame = BWAPI::Broodwar->getFrameCount();
    o.race = terran_ ? 1 : 2;
    o.minerals = self->minerals(); o.gas = self->gas();
    o.supplyUsed = self->supplyUsed(); o.supplyTotal = self->supplyTotal(); // BWAPI doubled units.
    const auto owned = bases();
    o.bases = static_cast<int>(owned.size());
    for (auto u : self->getUnits()) {
        if (!alive(u)) continue;
        if (u->getType().isWorker() && u->isCompleted()) ++o.workers;
        if (soldier(u)) ++o.army;
    }
    for (auto u : BWAPI::Broodwar->getAllUnits()) {
        if (!alive(u) || !u->isVisible() || !u->getPlayer() || !self->isEnemy(u->getPlayer())) continue;
        if (!u->getType().canAttack() && !u->getType().isWorker()) continue;
        ++o.visibleEnemyArmy;
        for (auto base : owned) if (u->getDistance(base) < 640) { ++o.visibleBaseThreats; break; }
    }
    return o;
}

void RaceBotModule::onFrame() {
    if (!supported_ || !BWAPI::BroodwarPtr || BWAPI::Broodwar->isReplay() ||
        BWAPI::Broodwar->isPaused() || !BWAPI::Broodwar->self()) return;
    const int frame = BWAPI::Broodwar->getFrameCount();
    if (policy_.enabled()) {
        const auto action = policy_.decision();
        economyMode_ = action == PolicyAction::economy;
        switch (action) {
        case PolicyAction::pressure: configure(1, 2); break;
        case PolicyAction::economy: configure(2, 0); break;
        case PolicyAction::defend: configure(0, 1); attacking_ = false; break;
        default: configure(0, 0); break;
        }
    }
    if (frame - lastPolicy_ >= 240) {
        lastPolicy_ = frame;
        if (hook_ && !policy_.enabled()) {
            try { const auto action = hook_(observation()); configure(action.opening, action.posture); }
            catch (...) { /* Keep the last valid policy choice; inference cannot stop macro. */ }
        }
    }
    if (frame - lastMacro_ >= std::max(8, BWAPI::Broodwar->getLatencyFrames() + 2)) {
        lastMacro_ = frame;
        maintainConstruction();
        macro();
        workers();
    }
    if (frame - lastCombat_ >= std::max(12, BWAPI::Broodwar->getLatencyFrames() + 2)) {
        lastCombat_ = frame;
        rememberEnemies();
        combat();
    }
    if (frame - lastLogFrame_ >= 240) logSnapshot();
}

} // namespace protodd::bwapi
