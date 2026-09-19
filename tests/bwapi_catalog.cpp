#include "protodd/UnitCatalog.hpp"
#include "../src/bwapi/ProductionCount.hpp"
#include "../src/bwapi/TerranDetection.hpp"
#include "../src/bwapi/SupplyPlanning.hpp"

#include <BWAPI/UnitType.h>
#include <BWAPI/UnitCommand.h>
#include <BWAPI/WeaponType.h>

#include <array>
#include <iostream>
#include <utility>

int main() {
    using protodd::UnitKind;
    using namespace BWAPI::UnitTypes;
    const std::array pairs{
        std::pair{UnitKind::probe, Protoss_Probe},
        std::pair{UnitKind::nexus, Protoss_Nexus},
        std::pair{UnitKind::pylon, Protoss_Pylon},
        std::pair{UnitKind::assimilator, Protoss_Assimilator},
        std::pair{UnitKind::gateway, Protoss_Gateway},
        std::pair{UnitKind::forge, Protoss_Forge},
        std::pair{UnitKind::photonCannon, Protoss_Photon_Cannon},
        std::pair{UnitKind::cyberneticsCore, Protoss_Cybernetics_Core},
        std::pair{UnitKind::shieldBattery, Protoss_Shield_Battery},
        std::pair{UnitKind::roboticsFacility, Protoss_Robotics_Facility},
        std::pair{UnitKind::observatory, Protoss_Observatory},
        std::pair{UnitKind::roboticsSupportBay, Protoss_Robotics_Support_Bay},
        std::pair{UnitKind::stargate, Protoss_Stargate},
        std::pair{UnitKind::citadelOfAdun, Protoss_Citadel_of_Adun},
        std::pair{UnitKind::templarArchives, Protoss_Templar_Archives},
        std::pair{UnitKind::fleetBeacon, Protoss_Fleet_Beacon},
        std::pair{UnitKind::arbiterTribunal, Protoss_Arbiter_Tribunal},
        std::pair{UnitKind::zealot, Protoss_Zealot},
        std::pair{UnitKind::dragoon, Protoss_Dragoon},
        std::pair{UnitKind::highTemplar, Protoss_High_Templar},
        std::pair{UnitKind::darkTemplar, Protoss_Dark_Templar},
        std::pair{UnitKind::reaver, Protoss_Reaver},
        std::pair{UnitKind::observer, Protoss_Observer},
        std::pair{UnitKind::shuttle, Protoss_Shuttle},
        std::pair{UnitKind::scout, Protoss_Scout},
        std::pair{UnitKind::corsair, Protoss_Corsair},
        std::pair{UnitKind::carrier, Protoss_Carrier},
        std::pair{UnitKind::arbiter, Protoss_Arbiter},
    };
    auto failures = 0;
    using protodd::bwapi::supplyPlanningBuffer;
    if (supplyPlanningBuffer(true, 8) != 12 ||
        supplyPlanningBuffer(true, 30) != 12 ||
        supplyPlanningBuffer(true, 192) != 20 ||
        supplyPlanningBuffer(true, 400) != 20 ||
        supplyPlanningBuffer(false, 8) != 4 ||
        supplyPlanningBuffer(false, 36) != 7) {
        std::cerr << "Supply planning buffer mismatch\n";
        ++failures;
    }
    using protodd::bwapi::wantsTerranDetection;
    if (!wantsTerranDetection(true, 5760, 14, 8) ||
        wantsTerranDetection(false, 5760, 14, 8) ||
        wantsTerranDetection(true, 5759, 14, 8) ||
        wantsTerranDetection(true, 5760, 13, 8) ||
        wantsTerranDetection(true, 5760, 14, 7)) {
        std::cerr << "Terran detection insurance guard mismatch\n";
        ++failures;
    }
    const auto checkPending = [&](BWAPI::UnitType producer, BWAPI::UnitType target,
                                  BWAPI::UnitType morph, int queued, int expected) {
        if (protodd::bwapi::pendingProductionCount(producer, target, morph, queued) != expected) {
            std::cerr << "Pending production count mismatch\n";
            ++failures;
        }
    };
    checkPending(Zerg_Drone, Zerg_Spawning_Pool, Zerg_Spawning_Pool, 1, 0);
    checkPending(Zerg_Egg, Zerg_Zergling, Zerg_Zergling, 1, 2);
    checkPending(Zerg_Egg, Zerg_Drone, Zerg_Drone, 1, 1);
    checkPending(Zerg_Egg, Zerg_Drone, Zerg_Overlord, 1, 0);
    checkPending(Terran_Barracks, Terran_Marine, None, 3, 3);
    checkPending(Terran_SCV, Terran_Barracks, Terran_Barracks, 1, 0);
    checkPending(Zerg_Hatchery, Zerg_Drone, None, 1, 0);
    for (const auto type : {Protoss_Pylon, Protoss_Gateway, Protoss_Nexus}) {
        const BWAPI::TilePosition tile{17, 23};
        const auto command = BWAPI::UnitCommand::build(nullptr, tile, type);
        if (command.getTargetPosition() != BWAPI::Position{17 * 32, 23 * 32} ||
            command.getTargetTilePosition() != tile || command.getUnitType() != type) {
            std::cerr << "Construction command coordinate contract mismatch\n";
            ++failures;
        }
    }
    for (const auto& [kind, type] : pairs) {
        const auto& stats = protodd::unitStats(kind);
        if (stats.minerals != type.mineralPrice() || stats.gas != type.gasPrice() ||
            stats.supply != type.supplyRequired() || stats.buildTime != type.buildTime()) {
            std::cerr << "Catalog mismatch: " << stats.name << " (BWAPI "
                      << type.mineralPrice() << '/' << type.gasPrice() << '/' << type.supplyRequired()
                      << '/' << type.buildTime() << ")\n";
            ++failures;
        }
    }
    std::cout << "BWAPI weapon contracts: Zealot hits=" << Protoss_Zealot.maxGroundHits()
              << ", Goliath hits=" << Terran_Goliath.maxAirHits()
              << ", Goliath factor=" << Terran_Goliath.airWeapon().damageFactor()
              << ", Scarab range=" << BWAPI::WeaponTypes::Scarab.maxRange() << '\n';
    return failures == 0 ? 0 : 1;
}
