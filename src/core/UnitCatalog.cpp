#include "astra/UnitCatalog.hpp"

#include <array>

namespace astra {
namespace {

constexpr UnitStats unknown{"Unknown", 0, 0, 0, 0, 0.0, false, false, false};

constexpr UnitStats make(
    const std::string_view name,
    const int minerals,
    const int gas,
    const int supply,
    const int buildTime,
    const double combatValue,
    const bool building = false,
    const bool producer = false,
    const bool requiresPsi = false) {
    return {name, minerals, gas, supply, buildTime, combatValue, building, producer,
            requiresPsi};
}

}  // namespace

const UnitStats& unitStats(const UnitKind kind) noexcept {
    // Indexed by UnitKind. Semantic aliases intentionally have zero cost.
    static constexpr std::array table{
        unknown,
        make("Worker", 0, 0, 2, 0, 0.35),
        make("Resource depot", 0, 0, 0, 0, 0.0, true, true),
        make("Supply provider", 0, 0, 0, 0, 0.0, true),
        make("Refinery", 0, 0, 0, 0, 0.0, true),
        make("Transport", 0, 0, 0, 0, 1.0),
        make("Detector", 0, 0, 0, 0, 0.8),
        make("Probe", 50, 0, 2, 300, 0.35),
        make("Nexus", 400, 0, 0, 1800, 0.0, true, true),
        make("Pylon", 100, 0, 0, 450, 0.0, true),
        make("Assimilator", 100, 0, 0, 600, 0.0, true),
        make("Gateway", 150, 0, 0, 900, 0.0, true, true, true),
        make("Forge", 150, 0, 0, 600, 0.0, true, false, true),
        make("Photon Cannon", 150, 0, 0, 750, 1.6, true, false, true),
        make("Cybernetics Core", 200, 0, 0, 900, 0.0, true, false, true),
        make("Shield Battery", 100, 0, 0, 450, 0.3, true, false, true),
        make("Robotics Facility", 200, 200, 0, 1200, 0.0, true, true, true),
        make("Observatory", 50, 100, 0, 450, 0.0, true, false, true),
        make("Robotics Support Bay", 150, 100, 0, 450, 0.0, true, false, true),
        make("Stargate", 150, 150, 0, 1050, 0.0, true, true, true),
        make("Citadel of Adun", 150, 100, 0, 900, 0.0, true, false, true),
        make("Templar Archives", 150, 200, 0, 900, 0.0, true, false, true),
        make("Fleet Beacon", 300, 200, 0, 900, 0.0, true, false, true),
        make("Arbiter Tribunal", 200, 150, 0, 900, 0.0, true, false, true),
        make("Zealot", 100, 0, 4, 600, 1.0),
        make("Dragoon", 125, 50, 4, 750, 1.45),
        make("High Templar", 50, 150, 4, 750, 1.25),
        make("Dark Templar", 125, 100, 4, 750, 1.7),
        make("Archon", 0, 0, 8, 300, 2.7),
        make("Dark Archon", 0, 0, 8, 300, 1.25),
        make("Reaver", 200, 100, 8, 1050, 3.0),
        make("Observer", 25, 75, 2, 600, 0.7),
        make("Shuttle", 200, 0, 4, 900, 0.8),
        make("Scout", 275, 125, 6, 1200, 1.6),
        make("Corsair", 150, 100, 4, 600, 1.45),
        make("Carrier", 350, 250, 12, 2100, 4.2),
        make("Arbiter", 100, 350, 8, 2400, 3.0),
        make("SCV", 50, 0, 2, 300, 0.35),
        make("Command Center", 400, 0, 0, 1800, 0.0, true, true),
        make("Barracks", 150, 0, 0, 1200, 0.0, true, true),
        make("Factory", 200, 100, 0, 1200, 0.0, true, true),
        make("Starport", 150, 100, 0, 1050, 0.0, true, true),
        make("Bunker", 100, 0, 0, 600, 1.5, true),
        make("Missile Turret", 75, 0, 0, 450, 1.0, true),
        make("Marine", 50, 0, 2, 360, 0.55),
        make("Medic", 50, 25, 2, 450, 0.55),
        make("Firebat", 50, 25, 2, 360, 0.6),
        make("Vulture", 75, 0, 4, 450, 1.05),
        make("Siege Tank", 150, 100, 4, 750, 1.8),
        make("Goliath", 100, 50, 4, 600, 1.1),
        make("Wraith", 150, 100, 4, 900, 1.25),
        make("Science Vessel", 100, 225, 4, 1200, 1.3),
        make("Dropship", 100, 100, 4, 750, 0.8),
        make("Battlecruiser", 400, 300, 12, 2000, 4.0),
        make("Drone", 50, 0, 2, 300, 0.35),
        make("Hatchery", 300, 0, 0, 1800, 0.0, true, true),
        make("Lair", 150, 100, 0, 1500, 0.0, true, true),
        make("Hive", 200, 150, 0, 1800, 0.0, true, true),
        make("Spawning Pool", 200, 0, 0, 1200, 0.0, true),
        make("Hydralisk Den", 100, 50, 0, 600, 0.0, true),
        make("Spire", 200, 150, 0, 1800, 0.0, true),
        make("Greater Spire", 100, 150, 0, 1800, 0.0, true),
        make("Sunken Colony", 125, 0, 0, 300, 1.5, true),
        make("Spore Colony", 125, 0, 0, 300, 1.1, true),
        make("Zergling", 25, 0, 1, 420, 0.45),
        make("Hydralisk", 75, 25, 2, 420, 0.85),
        make("Lurker", 125, 125, 4, 600, 1.8),
        make("Mutalisk", 100, 100, 4, 600, 1.25),
        make("Scourge", 12, 37, 1, 450, 0.5),
        make("Ultralisk", 200, 200, 8, 900, 2.7),
        make("Defiler", 50, 150, 4, 750, 1.5),
        make("Overlord", 100, 0, 0, 600, 0.4),
    };

    const auto index = static_cast<std::size_t>(kind);
    return index < table.size() ? table[index] : unknown;
}

bool isBuilding(const UnitKind kind) noexcept {
    return unitStats(kind).building;
}

bool isWorker(const UnitKind kind) noexcept {
    return kind == UnitKind::worker || kind == UnitKind::probe ||
           kind == UnitKind::scv || kind == UnitKind::drone;
}

bool isStaticDefense(const UnitKind kind) noexcept {
    return kind == UnitKind::photonCannon || kind == UnitKind::bunker ||
           kind == UnitKind::missileTurret || kind == UnitKind::sunkenColony ||
           kind == UnitKind::sporeColony;
}

bool isCombatUnit(const UnitKind kind) noexcept {
    const auto& stats = unitStats(kind);
    const auto supportOnly = kind == UnitKind::observer || kind == UnitKind::shuttle ||
                             kind == UnitKind::dropship || kind == UnitKind::overlord ||
                             kind == UnitKind::transport || kind == UnitKind::detector;
    return !stats.building && !isWorker(kind) && !supportOnly && stats.combatValue > 0.5;
}

std::span<const UnitKind> unitPrerequisites(const UnitKind kind) noexcept {
    static constexpr UnitKind pylon[]{UnitKind::pylon};
    static constexpr UnitKind forge[]{UnitKind::forge, UnitKind::pylon};
    static constexpr UnitKind gateway[]{UnitKind::gateway};
    static constexpr UnitKind core[]{UnitKind::cyberneticsCore};
    static constexpr UnitKind gatewayCore[]{UnitKind::gateway, UnitKind::cyberneticsCore};
    static constexpr UnitKind robotics[]{UnitKind::roboticsFacility};
    static constexpr UnitKind observatory[]{UnitKind::roboticsFacility, UnitKind::observatory};
    static constexpr UnitKind supportBay[]{UnitKind::roboticsFacility,
                                            UnitKind::roboticsSupportBay};
    static constexpr UnitKind stargate[]{UnitKind::stargate};
    static constexpr UnitKind citadel[]{UnitKind::citadelOfAdun};
    static constexpr UnitKind archives[]{UnitKind::gateway, UnitKind::templarArchives};
    static constexpr UnitKind beacon[]{UnitKind::stargate, UnitKind::fleetBeacon};
    static constexpr UnitKind archivesStargate[]{UnitKind::templarArchives,
                                                  UnitKind::stargate};
    static constexpr UnitKind tribunal[]{UnitKind::stargate, UnitKind::arbiterTribunal};
    static constexpr UnitKind nexus[]{UnitKind::nexus};

    switch (kind) {
        case UnitKind::gateway:
        case UnitKind::forge:
        case UnitKind::shieldBattery: return pylon;
        case UnitKind::photonCannon: return forge;
        case UnitKind::cyberneticsCore: return gateway;
        case UnitKind::roboticsFacility:
        case UnitKind::stargate:
        case UnitKind::citadelOfAdun: return core;
        case UnitKind::observatory:
        case UnitKind::roboticsSupportBay: return robotics;
        case UnitKind::templarArchives: return citadel;
        case UnitKind::fleetBeacon: return stargate;
        case UnitKind::arbiterTribunal: return archivesStargate;
        case UnitKind::probe: return nexus;
        case UnitKind::zealot: return gateway;
        case UnitKind::dragoon: return gatewayCore;
        case UnitKind::highTemplar:
        case UnitKind::darkTemplar: return archives;
        case UnitKind::reaver: return supportBay;
        case UnitKind::observer: return observatory;
        case UnitKind::shuttle: return robotics;
        case UnitKind::scout:
        case UnitKind::corsair: return stargate;
        case UnitKind::carrier: return beacon;
        case UnitKind::arbiter: return tribunal;
        default: return {};
    }
}

}  // namespace astra
