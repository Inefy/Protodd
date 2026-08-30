#include "astra/GameState.hpp"

#include <algorithm>

namespace astra {

double UnitSnapshot::healthFraction() const noexcept {
    const auto maximum = maxHitPoints + maxShields;
    if (maximum <= 0) {
        return 0.0;
    }
    return static_cast<double>(durability()) / static_cast<double>(maximum);
}

bool UnitSnapshot::canAttack(const UnitSnapshot& target) const noexcept {
    const auto& weapon = target.flying ? airWeapon : groundWeapon;
    return weapon.damage > 0 &&
           (target.flying ? weapon.targetsAir : weapon.targetsGround);
}

std::optional<UnitSnapshot> GameState::findUnit(const UnitId id) const {
    const auto findIn = [id](const std::vector<UnitSnapshot>& units)
        -> std::optional<UnitSnapshot> {
        const auto found = std::ranges::find(units, id, &UnitSnapshot::id);
        if (found != units.end()) {
            return *found;
        }
        return std::nullopt;
    };

    if (auto unit = findIn(self.units)) {
        return unit;
    }
    return findIn(enemy.units);
}

}  // namespace astra

