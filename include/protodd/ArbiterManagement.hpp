#pragma once

#include "protodd/Strategy.hpp"

#include <algorithm>
#include <cstddef>

namespace protodd {

inline constexpr int arbiterStasisEnergyCost = 100;
inline constexpr int arbiterRecallEnergyCost = 150;
inline constexpr int arbiterRecallMinimumForceUnits = 4;
inline constexpr double arbiterRecallMinimumForceValue = 8.0;
inline constexpr double arbiterRecallMinimumFollowThroughValue = 4.0;
inline constexpr double arbiterStasisMinimumValue = 4.0;

[[nodiscard]] constexpr bool arbiterHasOffensiveMission(
    const Posture posture, const Position attackTarget) noexcept {
    return attackTarget.valid() &&
           (posture == Posture::attack || posture == Posture::pressure);
}

[[nodiscard]] constexpr int arbiterRecallRequiredLandingSlots(
    const int forceUnits) noexcept {
    return forceUnits <= 0 ? 0 : std::min(forceUnits, 8);
}

struct ArbiterRecallOpportunity {
    int energy{};
    int forceUnits{};
    int landingSlots{};
    double objectiveValue{};
    double forceValue{};
    double followThroughValue{};
    bool offensiveMission{};
    bool objectiveActionable{};
    bool stagingSafe{};
};

[[nodiscard]] constexpr bool arbiterRecallIsWorthwhile(
    const ArbiterRecallOpportunity& opportunity) noexcept {
    return opportunity.energy >= arbiterRecallEnergyCost &&
           opportunity.offensiveMission && opportunity.objectiveActionable &&
           opportunity.stagingSafe &&
           opportunity.objectiveValue >= 2.0 &&
           opportunity.forceUnits >= arbiterRecallMinimumForceUnits &&
           opportunity.forceValue >= arbiterRecallMinimumForceValue &&
           opportunity.followThroughValue >= arbiterRecallMinimumFollowThroughValue &&
           opportunity.landingSlots >=
               arbiterRecallRequiredLandingSlots(opportunity.forceUnits);
}

struct ArbiterStasisOpportunity {
    double enemyValueAffected{};
    double friendlyValueAffected{};
    double friendlyFollowThroughValue{};
    double actionableEnemyValueAfterCast{};
    bool protectsThreatenedAlly{};
};

[[nodiscard]] constexpr double arbiterStasisValue(
    const ArbiterStasisOpportunity& opportunity) noexcept {
    if (opportunity.enemyValueAffected < arbiterStasisMinimumValue ||
        opportunity.friendlyValueAffected * 2.0 >= opportunity.enemyValueAffected ||
        opportunity.friendlyFollowThroughValue < 1.0 ||
        (opportunity.actionableEnemyValueAfterCast < 2.0 &&
         !opportunity.protectsThreatenedAlly)) {
        return 0.0;
    }
    return opportunity.enemyValueAffected - opportunity.friendlyValueAffected * 2.0 +
           std::min(6.0, opportunity.friendlyFollowThroughValue * 0.25) +
           (opportunity.protectsThreatenedAlly ? 2.0 : 0.0);
}

}  // namespace protodd
