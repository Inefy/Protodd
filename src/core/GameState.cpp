#include "protodd/GameState.hpp"
#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <cmath>

namespace protodd {

void UnitSnapshot::inheritObservationHistory(const UnitSnapshot& previous) noexcept {
    lastPosition = previous.position.valid() ? previous.position : previous.lastPosition;
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

void UnitSnapshot::updateMemoryConfidence(const Frame currentFrame) noexcept {
    if (visible && detected) {
        existenceConfidence = 1.0;
        locationConfidence = 1.0;
        healthConfidence = 1.0;
        return;
    }
    if (currentFrame < lastSeen || lastSeen < 0) {
        existenceConfidence = 0.0;
        locationConfidence = 0.0;
        healthConfidence = 0.0;
        return;
    }

    const auto age = static_cast<double>(currentFrame - lastSeen);
    const auto mobileExistenceConfidence = std::exp(-age / (24.0 * 90.0));
    const auto mobileLocationConfidence = std::exp(-age / (24.0 * 12.0));
    const auto healthFreshness = std::exp(-age / (24.0 * 5.0 * 60.0));
    if (isBuilding(kind)) {
        existenceConfidence = position.valid() ? 1.0 : mobileExistenceConfidence;
        locationConfidence = position.valid() ? 1.0 : 0.0;
    } else {
        existenceConfidence = mobileExistenceConfidence;
        locationConfidence = mobileLocationConfidence;
    }
    healthConfidence = healthFreshness;
}

double UnitSnapshot::healthFraction() const noexcept {
    const auto maximum = maxHitPoints + maxShields;
    if (maximum <= 0) {
        return 0.0;
    }
    return static_cast<double>(durability()) / static_cast<double>(maximum);
}

double UnitSnapshot::estimatedHealthFraction() const noexcept {
    const auto observed = healthFraction();
    const auto confidence = std::clamp(healthConfidence, 0.0, 1.0);
    return observed * confidence + (1.0 - confidence);
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

}  // namespace protodd
