#include "astra/GameState.hpp"

#include <algorithm>

namespace astra {

void UnitSnapshot::inheritObservationHistory(const UnitSnapshot& previous) noexcept {
    lastPosition = previous.position;
    // Zerg morphs retain their unit ID, but not the timing of the old type.
    if (kind != previous.kind) return;
    firstSeen = previous.firstSeen;
    if (previous.constructionStartUpperBound >= 0) {
        constructionStartUpperBound = constructionStartUpperBound < 0
                                         ? previous.constructionStartUpperBound
                                         : std::min(constructionStartUpperBound,
                                                    previous.constructionStartUpperBound);
    }
}

double UnitSnapshot::healthFraction() const noexcept {
    const auto maximum = maxHitPoints + maxShields;
    if (maximum <= 0) {
        return 0.0;
    }
    return static_cast<double>(durability()) / static_cast<double>(maximum);
}

bool UnitSnapshot::canAttack(const UnitSnapshot& target) const noexcept {
    if ((kind == UnitKind::reaver || kind == UnitKind::carrier) && ammo <= 0) {
        return false;
    }
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
