#pragma once

#include "protodd/Squads.hpp"

namespace protodd {

// Learned target choices never own economy, travel, or emergency decisions.
[[nodiscard]] std::optional<Command> hybridCombatProposal(
    Command proposal, Frame proposedAt, Frame frame, const Squad& squad,
    const CombatEstimate& estimate, std::span<const UnitSnapshot> targets,
    const DefenseArea& defense);

}  // namespace protodd
