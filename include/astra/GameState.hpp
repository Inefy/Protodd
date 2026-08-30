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

struct WeaponSnapshot {
    int damage{};
    int cooldown{};
    int minRange{};
    int maxRange{};
    DamageType damageType{DamageType::normal};
    bool targetsAir{};
    bool targetsGround{};
};

struct UnitSnapshot {
    UnitId id{};
    int typeId{};
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

