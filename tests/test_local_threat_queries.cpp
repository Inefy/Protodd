#include "protodd/LocalThreatQueries.hpp"

#include <iostream>
#include <string_view>

namespace {

using namespace protodd;
int failures{};

void check(const bool condition, const std::string_view message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

UnitSnapshot enemy(
    const UnitKind kind,
    const Position position,
    const bool visible = true,
    const bool completed = true,
    const bool flying = false) {
    UnitSnapshot result;
    result.kind = kind;
    result.position = position;
    result.visible = visible;
    result.completed = completed;
    result.flying = flying;
    return result;
}

}  // namespace

int main() {
    check(movedCloserByAtLeast({100, 0}, {98, 0}, {0, 0}, PixelDistance{2.0}),
          "movement progress uses a pixel-typed threshold");
    check(!movedCloserByAtLeast({100, 0}, {99, 0}, {0, 0}, PixelDistance{2.0}),
          "movement progress excludes movement below the pixel threshold");

    GameState state;
    state.enemy.units = {enemy(UnitKind::marine, {800, 0})};
    check(hasVisibleGroundCombatContact(state, Position{0, 0}, PixelRadius{800}),
          "ground contact includes the exact pixel-radius boundary");

    state.enemy.units[0].position = {801, 0};
    check(!hasVisibleGroundCombatContact(state, Position{0, 0}, PixelRadius{800}),
          "ground contact excludes positions beyond the pixel-radius boundary");
    check(hasVisibleGroundCombatContact(state, std::nullopt, PixelRadius{800}),
          "an absent anchor preserves map-wide contact behavior");

    state.enemy.units = {
        enemy(UnitKind::marine, {100, 0}, false),
        enemy(UnitKind::marine, {100, 0}, true, false),
        enemy(UnitKind::marine, {100, 0}, true, true, true),
        enemy(UnitKind::probe, {100, 0}),
        enemy(UnitKind::marine, {-1, -1}),
    };
    check(!hasVisibleGroundCombatContact(state, Position{0, 0}, PixelRadius{800}),
          "ground contact excludes unseen, incomplete, flying, noncombat, and invalid units");

    state.enemy.units = {enemy(UnitKind::zealot, {800, 0})};
    check(hasVisibleMeleeContact(state, {0, 0}, PixelRadius{800}),
          "melee contact includes a nearby Zealot at the radius boundary");
    state.enemy.units = {enemy(UnitKind::darkTemplar, {1, 0})};
    check(hasVisibleMeleeContact(state, {0, 0}, PixelRadius{800}),
          "melee contact includes Dark Templar");
    state.enemy.units = {enemy(UnitKind::marine, {1, 0})};
    check(!hasVisibleMeleeContact(state, {0, 0}, PixelRadius{800}),
          "melee contact excludes non-melee ground combatants");
    check(!hasVisibleMeleeContact(state, {-1, -1}, PixelRadius{800}),
          "melee contact requires a valid anchor");

    if (failures == 0) {
        std::cout << "Local threat query checks passed\n";
        return 0;
    }
    std::cerr << failures << " local threat query check(s) failed\n";
    return 1;
}
