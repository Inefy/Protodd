#pragma once

#include "protodd/CommandBus.hpp"
#include "protodd/HybridDiagnostics.hpp"

#include <optional>
#include <utility>

namespace protodd {

[[nodiscard]] constexpr bool hybridRightClickEnemyAttackEligible(
    const bool bridgeEnabled, const bool rightClickUnit, const bool actorPresent,
    const bool actorOwned, const bool actorCompleted, const UnitKind actorKind,
    const bool targetPresent, const bool targetEnemy, const bool targetVisible,
    const bool targetDetected) noexcept {
    const bool supportedActor = actorKind == UnitKind::zealot || actorKind == UnitKind::dragoon ||
        actorKind == UnitKind::archon || actorKind == UnitKind::darkTemplar;
    return bridgeEnabled && rightClickUnit && actorPresent && actorOwned && actorCompleted &&
        supportedActor && targetPresent && targetEnemy && targetVisible && targetDetected;
}
// The BWAPI adapter normalizes commands into this DTO so the production
// classification/translation path can be covered without a running game.
struct HybridCommandAdapterInput {
    HybridDiagnosticCommand command{HybridDiagnosticCommand::other};
    int commandTypeId{-1};
    Frame frame{-1};
    UnitId actorId{-1};
    int actorTypeId{-1};
    UnitKind actorKind{UnitKind::unknown};
    UnitId targetId{-1};
    HybridDiagnosticRelation targetRelation{HybridDiagnosticRelation::unknown};
    bool actorPresent{};
    bool actorExists{};
    bool actorOwned{};
    bool actorCompleted{};
    bool targetPresent{};
    bool targetExists{};
    bool targetEnemy{};
    bool targetVisible{};
    bool targetDetected{};
};

struct HybridCommandAdaptation {
    std::optional<Command> proposal;
    HybridDiagnosticSample diagnostic;
};

// Attack_Unit keeps its original intake rule. Only the gated enemy-unit
// right-click case is translated; all other command types remain unhandled.
[[nodiscard]] inline HybridCommandAdaptation adaptHybridCommand(
    const HybridCommandAdapterInput& input,
    const bool rightClickEnemyAttackEnabled = false) {
    HybridCommandAdaptation result;
    result.diagnostic.stage = HybridDiagnosticStage::intake;
    result.diagnostic.frame = input.frame;
    result.diagnostic.command = input.command;
    result.diagnostic.commandTypeId = input.commandTypeId;
    result.diagnostic.actorTypeId = input.actorTypeId;
    result.diagnostic.targetRelation = input.targetRelation;
    result.diagnostic.targetVisible = input.targetVisible;
    result.diagnostic.targetDetected = input.targetDetected;

    bool accepted = false;
    if (input.command == HybridDiagnosticCommand::attackUnit) {
        accepted = input.actorPresent && input.targetPresent;
        if (!input.actorPresent) result.diagnostic.reason = HybridDiagnosticReason::missingActor;
        else if (!input.targetPresent) result.diagnostic.reason = HybridDiagnosticReason::missingTarget;
    } else if (input.command == HybridDiagnosticCommand::rightClickUnit) {
        accepted = hybridRightClickEnemyAttackEligible(
            rightClickEnemyAttackEnabled, true, input.actorPresent && input.actorExists,
            input.actorOwned, input.actorCompleted, input.actorKind,
            input.targetPresent && input.targetExists, input.targetEnemy,
            input.targetVisible, input.targetDetected);
        if (!rightClickEnemyAttackEnabled)
            result.diagnostic.reason = HybridDiagnosticReason::rightClickBridgeDisabled;
        else if (!accepted)
            result.diagnostic.reason = HybridDiagnosticReason::rightClickIneligible;
    }

    if (accepted) {
        result.diagnostic.reason = HybridDiagnosticReason::candidateQueued;
        Command proposal;
        proposal.actor = input.actorId;
        proposal.type = CommandType::attackUnit;
        proposal.targetUnit = input.targetId;
        result.proposal = std::move(proposal);
    }
    return result;
}

}  // namespace protodd
