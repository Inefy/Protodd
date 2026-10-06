#pragma once

#include <cstddef>

namespace protodd {

// Action 18 is cancel_build in the versioned whole-game action schema.
inline constexpr std::size_t wholeGameCancelBuildAction = 18;
inline constexpr std::size_t wholeGameResearchAction = 15;
inline constexpr std::size_t wholeGameUpgradeAction = 16;
inline constexpr std::size_t wholeGameCancelResearchAction = 20;
inline constexpr std::size_t wholeGameCancelUpgradeAction = 21;

#ifdef PROTODD_WHOLE_GAME_RESEARCH_AUTHORITY
inline constexpr bool learnedWholeGameResearchAuthorityEnabled = true;
#else
inline constexpr bool learnedWholeGameResearchAuthorityEnabled = false;
#endif

[[nodiscard]] constexpr bool wholeGameActionRequiresResearchAuthority(
    const std::size_t actionKind) noexcept {
    return actionKind == wholeGameResearchAction || actionKind == wholeGameUpgradeAction ||
           actionKind == wholeGameCancelResearchAction || actionKind == wholeGameCancelUpgradeAction;
}

[[nodiscard]] constexpr bool wholeGameActionAuthorityAllowed(
    const std::size_t actionKind,
    const bool learnedResearchAuthority = learnedWholeGameResearchAuthorityEnabled) noexcept {
    return learnedResearchAuthority || !wholeGameActionRequiresResearchAuthority(actionKind);
}

[[nodiscard]] constexpr bool wholeGameActorEligible(
    const std::size_t actionKind, const std::size_t targetMode, const bool completed,
    const bool building, const bool underConstruction) noexcept {
    return completed || (actionKind == wholeGameCancelBuildAction && targetMode == 0 &&
                         building && underConstruction);
}

}  // namespace protodd
