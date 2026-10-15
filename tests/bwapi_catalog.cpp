#include "../src/bwapi/PlacementFootprint.hpp"
#include "protodd/UnitCatalog.hpp"
#include "protodd/Technology.hpp"
#include "../src/bwapi/ProductionCount.hpp"
#include "../src/bwapi/TerranDetection.hpp"
#include "../src/bwapi/SupplyPlanning.hpp"
#include "../src/bwapi/ObservedCloak.hpp"

#include <BWAPI/UnitType.h>
#include <BWAPI/UnitCommand.h>
#include <BWAPI/WeaponType.h>
#include <BWAPI/UpgradeType.h>
#include <BWAPI/TechType.h>

#include <array>
#include <algorithm>
#include <iostream>
#include <set>
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
    using protodd::bwapi::observedUnitCloaked;
    if (!observedUnitCloaked(Protoss_Dark_Templar, false, true) ||
        !observedUnitCloaked(Protoss_Observer, false, true) ||
        observedUnitCloaked(Protoss_Dark_Templar, false, false) ||
        observedUnitCloaked(Protoss_Observer, false, false) ||
        observedUnitCloaked(Protoss_Zealot, false, true) ||
        observedUnitCloaked(Terran_Ghost, false, true) ||
        observedUnitCloaked(Terran_Wraith, false, true) ||
        !observedUnitCloaked(Terran_Ghost, true, true) ||
        !observedUnitCloaked(Terran_Wraith, true, true)) {
        std::cerr << "Observed cloak must include completed permanent cloakers while preserving toggle state\n";
        ++failures;
    }
    // Exercise the actual BWAPI dimensions, including inclusive edge contact.
    // Native traces supply the expensive path reproduction; these controls
    // guard the occupancy exception and deferred-check decision order.
    const BWAPI::TilePosition footprintTile{10,10};
    const auto verifyFootprint = [&](const BWAPI::UnitType type) {
        using protodd::bwapi::PlacementBuilderBounds;
        using protodd::bwapi::placementFootprintBuildable;
        const auto center = BWAPI::Position(footprintTile) + BWAPI::Position(type.tileSize()) / 2;
        const auto right = center.x + type.dimensionRight();
        int mapCalls{}, builderCalls{};
        const auto mapCheck = [&] { ++mapCalls; return true; };
        const auto builderCheck = [&] { ++builderCalls; return true; };
        if (!placementFootprintBuildable(footprintTile,type,{0,0,16,16},mapCheck,builderCheck) ||
            mapCalls != 1 || builderCalls != 0) {
            std::cerr << "A clear footprint must defer its redundant builder path check\n";
            ++failures;
        }
        mapCalls = builderCalls = 0;
        const auto blockedMap = [&] { ++mapCalls; return false; };
        if (placementFootprintBuildable(footprintTile,type,{0,0,16,16},blockedMap,builderCheck) ||
            mapCalls != 1 || builderCalls != 0) {
            std::cerr << "A remote builder cannot unblock another occupant\n";
            ++failures;
        }
        mapCalls = builderCalls = 0;
        if (!placementFootprintBuildable(footprintTile,type,
                {right,center.y,right+16,center.y+16},blockedMap,builderCheck) ||
            mapCalls != 1 || builderCalls != 1) {
            std::cerr << "A builder touching the footprint must retain its exclusion check\n";
            ++failures;
        }
        builderCalls = 0;
        if (placementFootprintBuildable(footprintTile,type,
                {right+1,center.y,right+17,center.y+16},blockedMap,builderCheck) || builderCalls != 0) {
            std::cerr << "A worker outside the inclusive collision edge cannot unblock the footprint\n";
            ++failures;
        }
        builderCalls = 0;
        if (placementFootprintBuildable(footprintTile,type,
                {center.x,center.y,center.x+16,center.y+16},blockedMap,
                [&] { ++builderCalls; return false; }) || builderCalls != 1) {
            std::cerr << "Builder exclusion must not override another failed placement requirement\n";
            ++failures;
        }
        builderCalls = 0;
        if (!placementFootprintBuildable(footprintTile,type,{},blockedMap,builderCheck) || builderCalls != 1) {
            std::cerr << "Unknown builder bounds must retain conservative native validation\n";
            ++failures;
        }
    };
    for (const auto& [kind,type] : pairs) {
        static_cast<void>(kind);
        if (type.isBuilding() && !type.isRefinery()) verifyFootprint(type);
    }
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
        // Production must use the engine's actual producer and technology
        // requirements. Buildings also need a builder and power, which the
        // planner handles through placement. Archon merges are a separate path.
        if (!type.isBuilding()) {
            std::set<int> expected, actual;
            for (const auto& [required, count] : type.requiredUnits()) {
                expected.insert(required.getID());
                if (count != 1) {
                    std::cerr << "Unexpected prerequisite multiplicity: " << stats.name << '\n';
                    ++failures;
                }
            }
            for (const auto required : protodd::unitPrerequisites(kind)) {
                const auto found = std::ranges::find_if(pairs, [required](const auto& entry) {
                    return entry.first == required;
                });
                if (found == pairs.end()) {
                    std::cerr << "Unmapped prerequisite: " << stats.name << '\n';
                    ++failures;
                } else actual.insert(found->second.getID());
            }
            if (actual != expected) {
                std::cerr << "Training prerequisites differ from BWAPI: " << stats.name << '\n';
                ++failures;
            }
        }
    }
    using protodd::TechnologyKind;
    using namespace BWAPI::UpgradeTypes;
    const std::array upgrades{
        std::pair{TechnologyKind::singularityCharge, Singularity_Charge},
        std::pair{TechnologyKind::legEnhancements, Leg_Enhancements},
        std::pair{TechnologyKind::khaydarinAmulet, Khaydarin_Amulet},
        std::pair{TechnologyKind::graviticDrive, Gravitic_Drive},
        std::pair{TechnologyKind::graviticBoosters, Gravitic_Boosters},
        std::pair{TechnologyKind::sensorArray, Sensor_Array},
        std::pair{TechnologyKind::reaverCapacity, Reaver_Capacity},
        std::pair{TechnologyKind::scarabDamage, Scarab_Damage},
        std::pair{TechnologyKind::carrierCapacity, Carrier_Capacity},
        std::pair{TechnologyKind::protossGroundWeapons, Protoss_Ground_Weapons},
        std::pair{TechnologyKind::protossGroundArmor, Protoss_Ground_Armor},
        std::pair{TechnologyKind::protossPlasmaShields, Protoss_Plasma_Shields},
        std::pair{TechnologyKind::protossAirWeapons, Protoss_Air_Weapons},
        std::pair{TechnologyKind::protossAirArmor, Protoss_Air_Armor},
    };
    const auto engineType = [&pairs](UnitKind kind) {
        const auto entry = std::ranges::find(pairs, kind, &decltype(pairs)::value_type::first);
        return entry == pairs.end() ? BWAPI::UnitTypes::None : entry->second;
    };
    for (const auto& [kind, upgrade] : upgrades) {
        const auto& stats = protodd::technologyStats(kind);
        if (stats.research || stats.maximumLevel != upgrade.maxRepeats() ||
            engineType(stats.producer) != upgrade.whatUpgrades()) {
            std::cerr << "Upgrade producer/levels mismatch: " << stats.name << '\n'; ++failures;
        }
        for (int level = 1; level <= upgrade.maxRepeats(); ++level) {
            if (stats.mineralCost(level) != upgrade.mineralPrice(level) ||
                stats.gasCost(level) != upgrade.gasPrice(level) ||
                engineType(protodd::technologyPrerequisite(kind, level)) != upgrade.whatsRequired(level)) {
                std::cerr << "Upgrade cost/prerequisite mismatch: " << stats.name << " level " << level << '\n';
                ++failures;
            }
        }
    }
    const std::array research{
        std::pair{TechnologyKind::psionicStorm, BWAPI::TechTypes::Psionic_Storm},
        std::pair{TechnologyKind::stasisField, BWAPI::TechTypes::Stasis_Field},
        std::pair{TechnologyKind::recall, BWAPI::TechTypes::Recall},
    };
    for (const auto& [kind, tech] : research) {
        const auto& stats = protodd::technologyStats(kind);
        if (!stats.research || stats.maximumLevel != 1 || engineType(stats.producer) != tech.whatResearches() ||
            stats.mineralCost(1) != tech.mineralPrice() || stats.gasCost(1) != tech.gasPrice()) {
            std::cerr << "Research catalog mismatch: " << stats.name << '\n'; ++failures;
        }
    }
    std::cout << "BWAPI weapon contracts: Zealot hits=" << Protoss_Zealot.maxGroundHits()
              << ", Goliath hits=" << Terran_Goliath.maxAirHits()
              << ", Goliath factor=" << Terran_Goliath.airWeapon().damageFactor()
              << ", Scarab range=" << BWAPI::WeaponTypes::Scarab.maxRange() << '\n';
    return failures == 0 ? 0 : 1;
}
