#include "protodd/HybridPolicy.hpp"

#include <algorithm>

namespace protodd {

std::optional<Command> hybridCombatProposal(
    Command proposal, const Frame proposedAt, const Frame frame, const Squad& squad,
    const CombatEstimate& estimate, const std::span<const UnitSnapshot> targets,
    const DefenseArea& defense) {
    if (proposedAt > frame || frame - proposedAt >= 24 ||
        proposal.type != CommandType::attackUnit || squad.withdrawing ||
        squad.emergencyDefense || estimate.decision != FightDecision::engage ||
        estimate.advanceBlocked || estimate.holdScreen) return std::nullopt;
    const auto actor = std::ranges::find(squad.units, proposal.actor, &UnitSnapshot::id);
    const auto target = std::ranges::find(targets, proposal.targetUnit, &UnitSnapshot::id);
    if (actor == squad.units.end() || target == targets.end()) return std::nullopt;
    if ((actor->kind != UnitKind::zealot && actor->kind != UnitKind::dragoon &&
         actor->kind != UnitKind::archon && actor->kind != UnitKind::darkTemplar) ||
        !actor->ours || !actor->completed || actor->loaded || actor->disabled ||
        actor->hallucination || actor->attackFrame || actor->attackWindup || actor->underStorm ||
        actor->weaponCooldown > 0 || target->ours || !target->visible || !target->detected ||
        target->invincible || target->loaded || target->durability() <= target->incomingDamage ||
        !actor->position.valid() || !target->position.valid() || !actor->canAttack(*target))
        return std::nullopt;
    const auto& weapon = target->flying ? actor->airWeapon : actor->groundWeapon;
    const auto range = weaponDistance(*actor, *target);
    // Only immediate shots: a learned target cannot initiate a chase or leave a screen.
    if (range < weapon.minRange || range > weapon.maxRange ||
        (defense.active() && !defense.contains(target->position))) return std::nullopt;
    proposal.priority = 81; // Below screen (82), kite (84), Storm (109), and retreat.
    proposal.earliestFrame = frame;
    proposal.source = "hybrid-trained";
    proposal.alreadyActive = false;
    return proposal;
}

}  // namespace protodd
