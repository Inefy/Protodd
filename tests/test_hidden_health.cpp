#include "protodd/Combat.hpp"
#include "protodd/UnitCatalog.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <string_view>

namespace {
int failures{};
void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
bool near(double a, double b) { return std::abs(a - b) < 0.000001; }

protodd::UnitSnapshot dragoon(int id, bool ours, protodd::Position position) {
    using namespace protodd;
    UnitSnapshot result;
    result.id = id;
    result.kind = UnitKind::dragoon;
    result.ours = ours;
    result.position = position;
    result.completed = result.visible = true;
    result.hitPoints = result.maxHitPoints = 100;
    result.shields = result.maxShields = 80;
    result.armor = 1;
    result.size = UnitSize::large;
    result.topSpeed = 5.0;
    result.dimensionLeft = result.dimensionUp = 15;
    result.dimensionRight = result.dimensionDown = 16;
    result.groundWeapon = {20, 30, 0, ours ? 192 : 128,
                           DamageType::explosive, true, true, 1};
    result.airWeapon = result.groundWeapon;
    return result;
}

protodd::UnitSnapshot darkTemplar() {
    using namespace protodd;
    auto result = dragoon(208, false, {3799, 3186});
    result.kind = UnitKind::darkTemplar;
    result.hitPoints = result.maxHitPoints = 80;
    result.shields = result.maxShields = 40;
    result.detected = false;
    result.cloaked = true;
    result.size = UnitSize::small;
    result.topSpeed = 4.92;
    result.dimensionLeft = result.dimensionUp = 12;
    result.dimensionRight = result.dimensionDown = 11;
    result.groundWeapon = {40, 30, 0, 15, DamageType::normal, false, true, 1};
    result.airWeapon = {};
    return result;
}
}

int main() {
    using namespace protodd;
    CombatEvaluator evaluator;
    std::array friendly{dragoon(195, true, {3799, 3230})};
    std::array enemy{darkTemplar()};
    const auto full = evaluator.evaluate(friendly, enemy, 1.18, 0.0);
    enemy[0].hitPoints = enemy[0].shields = 0;
    const auto hidden = evaluator.evaluate(friendly, enemy, 1.18, 0.0);
    std::cout << "one Dragoon vs undetected DT: zero-health ratio=" << hidden.ratio
              << ", full-health ratio=" << full.ratio
              << ", enemyRemaining=" << hidden.simulatedEnemyRemaining << '\n';
    expect(near(hidden.enemyPower, full.enemyPower) && near(hidden.ratio, full.ratio),
           "unavailable enemy health must have the same threat as conservative full health");
    expect(near(hidden.simulatedEnemyRemaining, unitStats(UnitKind::darkTemplar).combatValue),
           "untargetable DT retains full simulated value, not 1/120 of its value");
    expect(hidden.decision == FightDecision::retreat, "a Dragoon retreats from an untargetable DT");
    expect(near(hidden.simulatedFriendlyRemaining, 0.0), "hidden-health DT still attacks and kills");
    expect(enemy[0].durability() == 0 && !enemy[0].detected,
           "evaluation must not rewrite the observed snapshot or grant detection");
    expect(evaluator.selectTarget(friendly[0], enemy) == nullptr,
           "unknown-health enemy remains untargetable");
    for (bool simulate : {false, true}) {
        const auto unknown = evaluator.evaluate(friendly, enemy, 1.18, 0.0, simulate);
        enemy[0].hitPoints = 80;
        enemy[0].shields = 40;
        const auto known = evaluator.evaluate(friendly, enemy, 1.18, 0.0, simulate);
        expect(near(unknown.ratio, known.ratio), "static and simulated evaluation agree on unknown health");
        enemy[0].hitPoints = enemy[0].shields = 0;
    }
    enemy[0].detected = true;
    const auto detectedZero = evaluator.evaluate(friendly, enemy, 1.18, 0.0, false);
    enemy[0].detected = false;
    enemy[0].hitPoints = 1;
    const auto knownWounded = evaluator.evaluate(friendly, enemy, 1.18, 0.0, false);
    expect(near(detectedZero.enemyPower, knownWounded.enemyPower),
           "detected zero health and known low health retain existing vitality floor");
    expect(knownWounded.enemyPower < full.enemyPower * 0.1,
           "positive known enemy health is not replaced with full health");
    enemy[0].hitPoints = 0;
    enemy[0].ours = true;
    expect(near(evaluator.evaluate(friendly, enemy, 1.18, 0.0, false).enemyPower,
                detectedZero.enemyPower), "own undetected zero-health snapshots are not inflated");

    // Frame 7680 entity positions, health, range and squad members plus support
    // from cog-2026 Protodd.log. Unlogged type dimensions/cooldowns use BWAPI values.
    auto zealot = dragoon(180, true, {3855, 3391});
    zealot.kind = UnitKind::zealot;
    zealot.shields = zealot.maxShields = 60;
    zealot.size = UnitSize::small;
    zealot.topSpeed = 4.0;
    zealot.dimensionLeft = zealot.dimensionRight = 11;
    zealot.dimensionUp = zealot.dimensionDown = 19;
    zealot.groundWeapon = {8, 22, 0, 15, DamageType::normal, false, true, 2};
    zealot.airWeapon = {};
    std::array army{zealot, dragoon(195, true, {3827, 3372}),
                    dragoon(205, true, {3882, 3412}), dragoon(212, true, {3795, 3366}),
                    dragoon(188, true, {3827, 3419})};
    std::array opposition{dragoon(185, false, {3633, 3101}), darkTemplar()};
    opposition[0].shields = 52;
    opposition[1].hitPoints = opposition[1].shields = 0;
    const auto logged = evaluator.evaluate(army, opposition, 1.18, 0.954115);
    opposition[1].hitPoints = 80;
    opposition[1].shields = 40;
    const auto conservative = evaluator.evaluate(army, opposition, 1.18, 0.954115);
    std::cout << "frame 7680 fixture: zero-health ratio=" << logged.ratio
              << ", full-health ratio=" << conservative.ratio
              << ", enemyRemaining=" << logged.simulatedEnemyRemaining << '\n';
    expect(near(logged.ratio, conservative.ratio), "logged army cannot exploit unavailable DT health");
    expect(logged.ratio < 2.0, "frame 7680 fixture no longer reports a 7.71 advantage");
    return failures == 0 ? 0 : 1;
}
