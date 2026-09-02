#pragma once

#include "astra/Geometry.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace astra {

using Frame = std::int32_t;
using UnitId = std::int32_t;

enum class Race : std::uint8_t { unknown, protoss, terran, zerg, random };

// Research and upgrades are deliberately separate from UnitKind. Treating a
// technology as a unit made strategic goals impossible to price, observe, or
// execute consistently through BWAPI.
enum class TechnologyKind : std::uint8_t {
    none,
    singularityCharge,
    legEnhancements,
    psionicStorm,
    stasisField,
    recall,
    khaydarinAmulet,
    graviticDrive,
    graviticBoosters,
    sensorArray,
    reaverCapacity,
    scarabDamage,
    carrierCapacity,
    protossGroundWeapons,
    protossGroundArmor,
    protossPlasmaShields,
    protossAirWeapons,
    protossAirArmor,
    count,
};

enum class UnitKind : std::uint16_t {
    unknown,
    // Shared semantic kinds.
    worker,
    resourceDepot,
    supplyProvider,
    refinery,
    transport,
    detector,
    // Protoss economy, technology, and army.
    probe,
    nexus,
    pylon,
    assimilator,
    gateway,
    forge,
    photonCannon,
    cyberneticsCore,
    shieldBattery,
    roboticsFacility,
    observatory,
    roboticsSupportBay,
    stargate,
    citadelOfAdun,
    templarArchives,
    fleetBeacon,
    arbiterTribunal,
    zealot,
    dragoon,
    highTemplar,
    darkTemplar,
    archon,
    darkArchon,
    reaver,
    observer,
    shuttle,
    scout,
    corsair,
    carrier,
    arbiter,
    // Terran units and strategic structures.
    scv,
    commandCenter,
    barracks,
    factory,
    starport,
    bunker,
    missileTurret,
    marine,
    medic,
    firebat,
    vulture,
    siegeTank,
    goliath,
    wraith,
    scienceVessel,
    dropship,
    battlecruiser,
    // Zerg units and strategic structures.
    drone,
    hatchery,
    lair,
    hive,
    spawningPool,
    hydraliskDen,
    spire,
    greaterSpire,
    sunkenColony,
    sporeColony,
    zergling,
    hydralisk,
    lurker,
    mutalisk,
    scourge,
    ultralisk,
    defiler,
    overlord,
};

enum class UnitRole : std::uint8_t {
    worker,
    resourceDepot,
    production,
    supply,
    detector,
    transport,
    groundArmy,
    airArmy,
    staticDefense,
    spellcaster,
    other,
};

enum class DamageType : std::uint8_t { normal, explosive, concussive, ignoreArmor };

enum class UnitSize : std::uint8_t { unknown, small, medium, large };

struct WeaponSnapshot {
    int damage{};
    int cooldown{};
    int minRange{};
    int maxRange{};
    DamageType damageType{DamageType::normal};
    bool targetsAir{};
    bool targetsGround{};
    int hits{1};
};

struct UnitSnapshot {
    UnitId id{};
    int typeId{};
    UnitKind kind{UnitKind::unknown};
    Race race{Race::unknown};
    UnitRole role{UnitRole::other};
    Position position{-1, -1};
    Position lastPosition{-1, -1};
    Frame lastSeen{};
    int hitPoints{};
    int maxHitPoints{};
    int shields{};
    int maxShields{};
    int energy{};
    int armor{};
    int buildProgress{100};
    int weaponCooldown{};
    double topSpeed{};
    WeaponSnapshot groundWeapon{};
    WeaponSnapshot airWeapon{};
    bool ours{};
    bool completed{};
    bool flying{};
    bool visible{};
    bool detected{true};
    bool burrowed{};
    bool cloaked{};
    bool carryingResources{};
    bool underAttack{};
    bool hallucination{};
    UnitSize size{UnitSize::unknown};
    bool powered{true};

    [[nodiscard]] int durability() const noexcept {
        return hitPoints + shields;
    }

    [[nodiscard]] double healthFraction() const noexcept;
    [[nodiscard]] bool canAttack(const UnitSnapshot& target) const noexcept;
};

struct BaseSnapshot {
    int id{};
    Position center{-1, -1};
    Position mineralLine{-1, -1};
    int mineralsRemaining{};
    int gasRemaining{};
    int ownerId{-1};
    Frame lastScouted{};
    bool startLocation{};
    bool island{};
    int mineralPatches{};
    int geysers{};
};

struct TechnologySnapshot {
    TechnologyKind kind{TechnologyKind::none};
    int level{};
    bool inProgress{};
};

struct PlayerSnapshot {
    int id{-1};
    Race race{Race::unknown};
    int minerals{};
    int gas{};
    int supplyUsed{};
    int supplyTotal{};
    int gatheredMinerals{};
    int gatheredGas{};
    std::vector<UnitSnapshot> units;
    std::vector<UnitKind> queuedUnits;
    std::vector<TechnologySnapshot> technologies;
};

struct GameState {
    Frame frame{};
    int latencyFrames{};
    int mapWidthPixels{};
    int mapHeightPixels{};
    std::string mapName;
    PlayerSnapshot self;
    PlayerSnapshot enemy;
    std::vector<BaseSnapshot> bases;

    [[nodiscard]] std::span<const UnitSnapshot> ourUnits() const noexcept {
        return self.units;
    }

    [[nodiscard]] std::span<const UnitSnapshot> enemyUnits() const noexcept {
        return enemy.units;
    }

    [[nodiscard]] std::optional<UnitSnapshot> findUnit(UnitId id) const;
};

}  // namespace astra
