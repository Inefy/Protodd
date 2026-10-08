#include "protodd/LocalThreatQueries.hpp"

#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <ranges>

namespace protodd {
namespace {

bool withinRadius(
    const Position position,
    const Position anchor,
    const PixelRadius radius) noexcept {
    return withinPixelRadius(position, anchor, radius);
}

bool meleeKind(const UnitKind kind) noexcept {
    return kind == UnitKind::zealot || kind == UnitKind::darkTemplar;
}

}  // namespace

bool hasVisibleGroundCombatContact(
    const GameState& state,
    const std::optional<Position> anchor,
    const PixelRadius radius) noexcept {
    return std::ranges::any_of(state.enemy.units, [&](const UnitSnapshot& enemy) {
        return enemy.visible && enemy.completed && !enemy.flying &&
               isCombatUnit(enemy.kind) && enemy.position.valid() &&
               (!anchor || withinRadius(enemy.position, *anchor, radius));
    });
}

bool hasVisibleMeleeContact(
    const GameState& state,
    const Position anchor,
    const PixelRadius radius) noexcept {
    if (!anchor.valid()) return false;
    return std::ranges::any_of(state.enemy.units, [&](const UnitSnapshot& enemy) {
        return enemy.visible && enemy.completed && !enemy.flying &&
               meleeKind(enemy.kind) && enemy.position.valid() &&
               withinRadius(enemy.position, anchor, radius);
    });
}

}  // namespace protodd
