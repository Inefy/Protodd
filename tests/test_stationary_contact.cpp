#include "protodd/Combat.hpp"
#include "protodd/UnitCatalog.hpp"

#include <cmath>
#include <iostream>
#include <vector>

namespace {
int failures{};
void expect(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
protodd::UnitSnapshot fighter(protodd::UnitId id, protodd::UnitKind kind,
                              bool ours, protodd::Position position) {
    protodd::UnitSnapshot result;
    result.id = id;
    result.kind = kind;
    result.ours = ours;
    result.position = position;
    result.visible = result.completed = true;
    result.hitPoints = result.maxHitPoints = 200;
    return result;
}

// Unupgraded BWAPI 4.4 catalog values, transcribed from the installed
// BWAPILIB/Source/{UnitType,WeaponType}.cpp tables. UnitCatalog supplies
// combat values; it does not contain HP, collision dimensions or weapons.
protodd::UnitSnapshot catalogFighter(protodd::UnitId id, protodd::UnitKind kind,
                                     bool ours, protodd::Position position) {
    using namespace protodd;
    auto result = fighter(id, kind, ours, position);
    switch (kind) {
    case UnitKind::photonCannon:
        result.hitPoints = result.maxHitPoints = 100;
        result.shields = result.maxShields = 100;
        result.size = UnitSize::large;
        result.dimensionLeft = result.dimensionRight = 20;
        result.dimensionUp = result.dimensionDown = 16;
        result.groundWeapon = {20, 22, 0, 224, DamageType::normal, false, true};
        result.airWeapon = {20, 22, 0, 224, DamageType::normal, true, false};
        break;
    case UnitKind::zealot:
        result.hitPoints = result.maxHitPoints = 100;
        result.shields = result.maxShields = 60;
        result.armor = 1;
        result.topSpeed = 4.0;
        result.size = UnitSize::small;
        result.dimensionLeft = result.dimensionRight = 11;
        result.dimensionUp = 5;
        result.dimensionDown = 13;
        result.groundWeapon = {8, 22, 0, 15, DamageType::normal, false, true, 2};
        break;
    case UnitKind::guardian:
    case UnitKind::mutalisk:
        result.flying = true;
        result.dimensionLeft = result.dimensionUp = 22;
        result.dimensionRight = result.dimensionDown = 21;
        if (kind == UnitKind::guardian) {
            result.hitPoints = result.maxHitPoints = 150;
            result.armor = 2;
            result.topSpeed = 2.5;
            result.size = UnitSize::large;
            result.groundWeapon = {20, 30, 0, 256, DamageType::normal, false, true};
        } else {
            result.hitPoints = result.maxHitPoints = 120;
            result.topSpeed = 6.67;
            result.size = UnitSize::small;
            result.groundWeapon = {9, 30, 0, 96, DamageType::normal, true, true};
            result.airWeapon = result.groundWeapon;
        }
        break;
    case UnitKind::sunkenColony:
        result.hitPoints = result.maxHitPoints = 300;
        result.armor = 2;
        result.size = UnitSize::large;
        result.dimensionLeft = result.dimensionUp = 24;
        result.dimensionRight = result.dimensionDown = 23;
        result.groundWeapon = {40, 32, 0, 224, DamageType::explosive, false, true};
        break;
    default:
        expect(false, "catalog fixture kind must have explicitly verified statistics");
    }
    return result;
}

void realisticCatalogFixtures() {
    using namespace protodd;
    const auto evaluate = [](UnitSnapshot friendly, UnitSnapshot enemy) {
        return CombatEvaluator{}.evaluate(std::vector{friendly}, std::vector{enemy}, 1.0, 0.0);
    };
    auto cannon = catalogFighter(101, UnitKind::photonCannon, true, {100, 100});
    auto zealot = catalogFighter(102, UnitKind::zealot, false, {500, 100});
    expect(weaponDistance(cannon, zealot) == 369.0,
           "catalog Cannon and Zealot collision dimensions produce the expected gap");
    // First Cannon shot at ceil((369-224)/4)=37, melee at
    // ceil((369-15)/4)=89. Nine Cannon shots kill at frame 213.
    // Six preceding Zealot volleys leave the Cannon on 100 HP / 4 shields.
    const auto duel = evaluate(cannon, zealot);
    const auto expectedCannon = unitStats(cannon.kind).combatValue * 104.0 / 200.0;
    std::cout << "catalog-cannon-zealot friendlyRemaining=" << duel.simulatedFriendlyRemaining
              << " enemyRemaining=" << duel.simulatedEnemyRemaining << '\n';
    expect(duel.simulatedEnemyRemaining == 0.0 &&
           std::abs(duel.simulatedFriendlyRemaining - expectedCannon) < 1e-9,
           "realistic stationary Cannon fires before charging Zealot contact");

    cannon.ours = false;
    zealot.ours = true;
    const auto enemyDefense = evaluate(zealot, cannon);
    expect(enemyDefense.simulatedFriendlyRemaining == 0.0 &&
           std::abs(enemyDefense.simulatedEnemyRemaining - expectedCannon) < 1e-9 &&
           std::abs(enemyDefense.ratio * duel.ratio - 1.0) < 1e-9,
           "realistic enemy Cannon receives symmetric contact timing and power blending");

    auto guardian = catalogFighter(103, UnitKind::guardian, true, {500, 100});
    const auto siege = evaluate(guardian, cannon);
    expect(guardian.groundWeapon.maxRange > cannon.airWeapon.maxRange &&
           siege.simulatedEnemyRemaining == 0.0 &&
           siege.simulatedFriendlyRemaining == unitStats(guardian.kind).combatValue,
           "enemy Cannon cannot invent an approach by an already outranging Guardian");
    guardian.ours = false;
    cannon.ours = true;
    const auto defending = evaluate(cannon, guardian);
    expect(defending.simulatedFriendlyRemaining == 0.0 &&
           defending.simulatedEnemyRemaining == unitStats(guardian.kind).combatValue,
           "friendly Cannon retains the same no-approach rule against an outranging flyer");

    auto mutalisk = catalogFighter(104, UnitKind::mutalisk, false, {500, 100});
    const auto antiAir = evaluate(cannon, mutalisk);
    // Edge distance 358: first Cannon shot at 21, Mutalisk shot at 40.
    // Six Cannon hits kill at 131, after four nine-damage Mutalisk hits.
    expect(antiAir.simulatedEnemyRemaining == 0.0 &&
           std::abs(antiAir.simulatedFriendlyRemaining -
                    unitStats(cannon.kind).combatValue * 164.0 / 200.0) < 1e-9,
           "stationary anti-air contact selects the eligible Cannon air weapon");
    auto groundOnly = cannon;
    groundOnly.airWeapon = {};
    const auto noAirWeapon = evaluate(groundOnly, mutalisk);
    expect(noAirWeapon.simulatedEnemyRemaining == unitStats(mutalisk.kind).combatValue,
           "a ground weapon cannot damage a flying target when the air weapon is absent");
    groundOnly.airWeapon = cannon.airWeapon;
    groundOnly.airWeapon.targetsAir = false;
    expect(evaluate(groundOnly, mutalisk).simulatedEnemyRemaining == unitStats(mutalisk.kind).combatValue,
           "positive air-weapon damage still requires air-target eligibility");

    auto sunken = catalogFighter(105, UnitKind::sunkenColony, false, {100, 100});
    mutalisk.ours = true;
    const auto groundDefense = evaluate(mutalisk, sunken);
    expect(groundDefense.enemyPower == 0.0 &&
           groundDefense.simulatedFriendlyRemaining == unitStats(mutalisk.kind).combatValue &&
           groundDefense.simulatedEnemyRemaining < unitStats(sunken.kind).combatValue,
           "enemy ground-only static defense cannot return fire against a flying attacker");
}
}

int main() {
    using namespace protodd;
    auto cannon = fighter(1, UnitKind::photonCannon, true, {100, 100});
    cannon.groundWeapon = {200, 22, 0, 224, DamageType::normal, false, true};
    auto melee = fighter(2, UnitKind::zealot, false, {500, 100});
    melee.topSpeed = 4.0;
    melee.groundWeapon = {10, 1000, 0, 32, DamageType::normal, false, true};
    const auto fullCannon = unitStats(cannon.kind).combatValue;
    const auto fullMelee = unitStats(melee.kind).combatValue;
    const auto evaluate = [&](UnitSnapshot friendly, UnitSnapshot enemy) {
        return CombatEvaluator{}.evaluate(std::vector{friendly}, std::vector{enemy}, 1.0, 0.0);
    };

    // No dimensions: the approach to Cannon range takes 44 frames, while
    // melee contact takes 92. The lethal Cannon shot must precede that hit.
    const auto approach = evaluate(cannon, melee);
    std::cout << "approach friendlyRemaining=" << approach.simulatedFriendlyRemaining
              << " enemyRemaining=" << approach.simulatedEnemyRemaining << '\n';
    expect(std::abs(approach.simulatedFriendlyRemaining - fullCannon) < 1e-9 &&
           approach.simulatedEnemyRemaining == 0.0,
           "stationary defense fires as a shorter-range enemy enters its range");

    auto cooling = cannon;
    cooling.weaponCooldown = 50;
    const auto cooldown = evaluate(cooling, melee);
    expect(std::abs(cooldown.simulatedFriendlyRemaining - fullCannon) < 1e-9,
           "an initial cooldown still permits a shot before melee contact");
    cooling.weaponCooldown = 120;
    const auto late = evaluate(cooling, melee);
    expect(std::abs(late.simulatedFriendlyRemaining - fullCannon * 0.95) < 1e-9,
           "approach does not bypass the building's initial weapon cooldown");

    auto outranger = melee;
    outranger.kind = UnitKind::dragoon;
    outranger.groundWeapon.maxRange = 448;
    expect(evaluate(cannon, outranger).simulatedEnemyRemaining == unitStats(outranger.kind).combatValue,
           "a stationary defense cannot chase an outranging mobile target");
    auto equalRange = melee;
    equalRange.groundWeapon.maxRange = cannon.groundWeapon.maxRange;
    expect(evaluate(cannon, equalRange).simulatedEnemyRemaining == fullMelee,
           "equal-range enemies outside the building's range are not assumed to approach");

    auto stationary = melee;
    stationary.topSpeed = 0.0;
    expect(evaluate(cannon, stationary).simulatedEnemyRemaining == fullMelee,
           "two immobile units outside range cannot acquire one another");
    auto distantBuilding = melee;
    distantBuilding.kind = UnitKind::photonCannon;
    expect(evaluate(cannon, distantBuilding).simulatedEnemyRemaining == fullCannon,
           "a distant building cannot approach even with a malformed speed");

    auto harmless = melee;
    harmless.groundWeapon = {};
    expect(evaluate(cannon, harmless).simulatedEnemyRemaining == fullMelee,
           "a harmless out-of-range opponent is not assumed to charge a defense");
    auto alreadyInRange = melee;
    alreadyInRange.position = {300, 100};
    expect(evaluate(cannon, alreadyInRange).simulatedEnemyRemaining == 0.0 &&
           evaluate(cannon, alreadyInRange).simulatedFriendlyRemaining == fullCannon,
           "in-range stationary fire remains immediate");

    auto mobile = cannon;
    mobile.kind = UnitKind::dragoon;
    mobile.topSpeed = 4.0;
    expect(evaluate(mobile, melee).simulatedEnemyRemaining == 0.0,
           "mobile approach remains able to reach a compatible target");

    // Mirroring ownership must preserve contact timing. Values follow the
    // kinds rather than which side is friendly.
    cannon.ours = false;
    melee.ours = true;
    const auto mirrored = evaluate(melee, cannon);
    expect(mirrored.simulatedFriendlyRemaining == 0.0 &&
           std::abs(mirrored.simulatedEnemyRemaining - fullCannon) < 1e-9,
           "enemy static defense receives the same approach timing");
    realisticCatalogFixtures();
    std::cout << "stationary-contact failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
