#include "astra/GameState.hpp"
#include "astra/Geometry.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void testGeometry() {
    expect(std::abs(astra::distance({0, 0}, {3, 4}) - 5.0) < 0.001,
           "Euclidean distance");
    expect(astra::moveToward({0, 0}, {100, 0}, 32.0) == astra::Position{32, 0},
           "bounded movement toward a target");
}

void testSnapshots() {
    astra::UnitSnapshot dragoon;
    dragoon.id = 7;
    dragoon.hitPoints = 80;
    dragoon.maxHitPoints = 100;
    dragoon.shields = 40;
    dragoon.maxShields = 80;
    dragoon.groundWeapon = {.damage = 20, .targetsGround = true};

    astra::UnitSnapshot enemy;
    enemy.id = 9;

    expect(std::abs(dragoon.healthFraction() - (120.0 / 180.0)) < 0.001,
           "combined shield and hit point fraction");
    expect(dragoon.canAttack(enemy), "ground target compatibility");

    astra::GameState state;
    state.self.units.push_back(dragoon);
    state.enemy.units.push_back(enemy);
    expect(state.findUnit(9).has_value(), "unit lookup across both players");
    expect(!state.findUnit(42).has_value(), "missing unit lookup");
}

}  // namespace

int main() {
    testGeometry();
    testSnapshots();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All Astra core tests passed\n";
    return EXIT_SUCCESS;
}

