#include "protodd/Technology.hpp"

#include <algorithm>
#include <array>

namespace protodd {
namespace {

constexpr TechnologyStats unknown{"None", 0, 0, 0, 0, 0, UnitKind::unknown, false};

}  // namespace

int TechnologyStats::mineralCost(const int level) const noexcept {
    return mineralBase + std::max(0, level - 1) * mineralFactor;
}

int TechnologyStats::gasCost(const int level) const noexcept {
    return gasBase + std::max(0, level - 1) * gasFactor;
}

const TechnologyStats& technologyStats(const TechnologyKind kind) noexcept {
    // Costs use Brood War's next-level pricing. Adapter execution still asks
    // BWAPI for legality, so a stale plan cannot start an illegal upgrade.
    static constexpr std::array table{
        unknown,
        TechnologyStats{"Singularity Charge", 150, 150, 0, 0, 1,
                        UnitKind::cyberneticsCore, false},
        TechnologyStats{"Leg Enhancements", 150, 150, 0, 0, 1,
                        UnitKind::citadelOfAdun, false},
        TechnologyStats{"Psionic Storm", 200, 200, 0, 0, 1,
                        UnitKind::templarArchives, true},
        TechnologyStats{"Stasis Field", 150, 150, 0, 0, 1,
                        UnitKind::arbiterTribunal, true},
        TechnologyStats{"Recall", 150, 150, 0, 0, 1,
                        UnitKind::arbiterTribunal, true},
        TechnologyStats{"Khaydarin Amulet", 150, 150, 0, 0, 1,
                        UnitKind::templarArchives, false},
        TechnologyStats{"Gravitic Drive", 200, 200, 0, 0, 1,
                        UnitKind::roboticsSupportBay, false},
        TechnologyStats{"Gravitic Boosters", 150, 150, 0, 0, 1,
                        UnitKind::observatory, false},
        TechnologyStats{"Sensor Array", 150, 150, 0, 0, 1,
                        UnitKind::observatory, false},
        TechnologyStats{"Reaver Capacity", 200, 200, 0, 0, 1,
                        UnitKind::roboticsSupportBay, false},
        TechnologyStats{"Scarab Damage", 200, 200, 0, 0, 1,
                        UnitKind::roboticsSupportBay, false},
        TechnologyStats{"Carrier Capacity", 100, 100, 0, 0, 1,
                        UnitKind::fleetBeacon, false},
        TechnologyStats{"Protoss Ground Weapons", 100, 100, 50, 50, 3,
                        UnitKind::forge, false},
        TechnologyStats{"Protoss Ground Armor", 100, 100, 75, 75, 3,
                        UnitKind::forge, false},
        TechnologyStats{"Protoss Plasma Shields", 200, 200, 100, 100, 3,
                        UnitKind::forge, false},
        TechnologyStats{"Protoss Air Weapons", 100, 100, 75, 75, 3,
                        UnitKind::cyberneticsCore, false},
        TechnologyStats{"Protoss Air Armor", 150, 150, 75, 75, 3,
                        UnitKind::cyberneticsCore, false},
    };
    const auto index = static_cast<std::size_t>(kind);
    return index < table.size() ? table[index] : unknown;
}

UnitKind technologyPrerequisite(const TechnologyKind kind, const int level) noexcept {
    if (level < 2 || level > technologyStats(kind).maximumLevel) return UnitKind::unknown;
    switch (kind) {
        case TechnologyKind::protossGroundWeapons:
        case TechnologyKind::protossGroundArmor: return UnitKind::templarArchives;
        case TechnologyKind::protossAirWeapons:
        case TechnologyKind::protossAirArmor: return UnitKind::fleetBeacon;
        case TechnologyKind::protossPlasmaShields: return UnitKind::cyberneticsCore;
        default: return UnitKind::unknown;
    }
}

int technologyLevel(
    const PlayerSnapshot& player,
    const TechnologyKind kind) noexcept {
    const auto found = std::ranges::find(player.technologies, kind,
                                         &TechnologySnapshot::kind);
    return found != player.technologies.end() ? found->level : 0;
}

bool technologyInProgress(
    const PlayerSnapshot& player,
    const TechnologyKind kind) noexcept {
    const auto found = std::ranges::find(player.technologies, kind,
                                         &TechnologySnapshot::kind);
    return found != player.technologies.end() && found->inProgress;
}

}  // namespace protodd
