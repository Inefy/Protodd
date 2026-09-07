#include "protodd/UnitCatalog.hpp"

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
