#include "protodd/HybridRightClickBridge.hpp"

#include <array>
#include <iostream>
#include <string_view>

namespace {
using namespace protodd;
int failures{};
void check(const bool condition, const std::string_view message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
HybridCommandAdapterInput enemyRightClick() {
    HybridCommandAdapterInput input;
    input.command = HybridDiagnosticCommand::rightClickUnit;
    input.commandTypeId = 31; // BWAPI UnitCommandTypes::Right_Click_Unit
    input.frame = 1024;
    input.actorId = 17;
    input.actorTypeId = 3;
    input.actorKind = UnitKind::dragoon;
    input.targetId = 29;
    input.targetRelation = HybridDiagnosticRelation::enemy;
    input.actorPresent = input.actorExists = input.actorOwned = input.actorCompleted = true;
    input.targetPresent = input.targetExists = input.targetEnemy = true;
    input.targetVisible = input.targetDetected = true;
    return input;
}
}

int main() {
    using namespace protodd;
    const auto raw = enemyRightClick();
    const auto adapted = adaptHybridCommand(raw, true);
    check(adapted.proposal.has_value(), "enabled eligible enemy-unit right-click becomes a proposal");
    check(adapted.proposal && adapted.proposal->type == CommandType::attackUnit,
          "production adapter emits internal attackUnit");
    check(adapted.proposal && adapted.proposal->actor == 17 && adapted.proposal->targetUnit == 29,
          "production adapter preserves actor and target IDs");
    check(adapted.diagnostic.command == HybridDiagnosticCommand::rightClickUnit &&
          adapted.diagnostic.commandTypeId == 31 && adapted.diagnostic.targetRelation == HybridDiagnosticRelation::enemy,
          "translation preserves original right-click class, BWAPI type ID, and target relation");
    for (const auto kind : {UnitKind::zealot, UnitKind::dragoon, UnitKind::archon, UnitKind::darkTemplar}) {
        auto supported = raw; supported.actorKind = kind;
        check(adaptHybridCommand(supported, true).proposal.has_value(),
              "each supported actor kind converts through the production adapter");
    }
    check(adapted.diagnostic.frame == 1024 &&
          adapted.diagnostic.reason == HybridDiagnosticReason::candidateQueued,
          "adapter preserves input frame and marks accepted candidate");

    const auto defaultOff = adaptHybridCommand(raw);
    check(!defaultOff.proposal && defaultOff.diagnostic.reason == HybridDiagnosticReason::rightClickBridgeDisabled,
          "default-off adapter rejects otherwise eligible right-click");
    check(defaultOff.diagnostic.command == HybridDiagnosticCommand::rightClickUnit &&
          defaultOff.diagnostic.commandTypeId == 31,
          "default-off rejection still records the original command");

    for (const auto kind : {UnitKind::probe, UnitKind::nexus, UnitKind::observer, UnitKind::highTemplar,
                            UnitKind::darkArchon, UnitKind::reaver, UnitKind::shuttle, UnitKind::unknown}) {
        auto input = raw; input.actorKind = kind;
        check(!adaptHybridCommand(input, true).proposal, "unsupported actor rejected");
    }
    const std::array<void(*)(HybridCommandAdapterInput&), 11> rejectedCases{
        +[](HybridCommandAdapterInput& x) { x.actorPresent = false; },
        +[](HybridCommandAdapterInput& x) { x.actorExists = false; },
        +[](HybridCommandAdapterInput& x) { x.actorOwned = false; },
        +[](HybridCommandAdapterInput& x) { x.actorCompleted = false; },
        +[](HybridCommandAdapterInput& x) { x.targetPresent = false; },
        +[](HybridCommandAdapterInput& x) { x.targetExists = false; },
        +[](HybridCommandAdapterInput& x) { x.targetEnemy = false; x.targetRelation = HybridDiagnosticRelation::friendly; },
        +[](HybridCommandAdapterInput& x) { x.targetEnemy = false; x.targetRelation = HybridDiagnosticRelation::neutral; },
        +[](HybridCommandAdapterInput& x) { x.targetEnemy = false; x.targetRelation = HybridDiagnosticRelation::unknown; },
        +[](HybridCommandAdapterInput& x) { x.targetVisible = false; },
        +[](HybridCommandAdapterInput& x) { x.targetDetected = false; },
    };
    for (const auto mutate : rejectedCases) {
        auto input = raw; mutate(input);
        check(!adaptHybridCommand(input, true).proposal, "each right-click admission predicate is enforced");
    }
    auto position = raw;
    position.command = HybridDiagnosticCommand::rightClickPosition;
    position.commandTypeId = 30; // BWAPI UnitCommandTypes::Right_Click_Position
    position.targetRelation = HybridDiagnosticRelation::position;
    check(!adaptHybridCommand(position, true).proposal, "position right-click is never converted");
    auto other = raw; other.command = HybridDiagnosticCommand::other;
    check(!adaptHybridCommand(other, true).proposal, "unrelated commands are never converted");

    // This deliberately mirrors the former intake rule: Attack_Unit needs only
    // non-null actor and target handles, regardless of right-click gate fields.
    auto explicitAttack = raw;
    explicitAttack.command = HybridDiagnosticCommand::attackUnit;
    explicitAttack.commandTypeId = 1; // BWAPI UnitCommandTypes::Attack_Unit
    explicitAttack.actorKind = UnitKind::probe;
    explicitAttack.actorExists = explicitAttack.actorOwned = explicitAttack.actorCompleted = false;
    explicitAttack.targetExists = explicitAttack.targetEnemy = false;
    explicitAttack.targetVisible = explicitAttack.targetDetected = false;
    const auto unchangedAttack = adaptHybridCommand(explicitAttack, false);
    check(unchangedAttack.proposal && unchangedAttack.proposal->type == CommandType::attackUnit &&
          unchangedAttack.proposal->actor == 17 && unchangedAttack.proposal->targetUnit == 29,
          "explicit Attack_Unit handling and IDs remain unchanged with bridge disabled");
    const auto attackWithBridgeEnabled = adaptHybridCommand(explicitAttack, true);
    check(attackWithBridgeEnabled.proposal && attackWithBridgeEnabled.proposal->actor == 17 &&
          attackWithBridgeEnabled.proposal->targetUnit == 29,
          "enabling the right-click bridge does not alter explicit Attack_Unit handling");
    check(unchangedAttack.diagnostic.command == HybridDiagnosticCommand::attackUnit &&
          unchangedAttack.diagnostic.commandTypeId == 1,
          "explicit attack telemetry retains its original command type");
    explicitAttack.actorPresent = false;
    check(!adaptHybridCommand(explicitAttack, true).proposal &&
          adaptHybridCommand(explicitAttack, true).diagnostic.reason == HybridDiagnosticReason::missingActor,
          "explicit Attack_Unit still rejects missing actor handle");
    explicitAttack.actorPresent = true; explicitAttack.targetPresent = false;
    check(!adaptHybridCommand(explicitAttack, true).proposal &&
          adaptHybridCommand(explicitAttack, true).diagnostic.reason == HybridDiagnosticReason::missingTarget,
          "explicit Attack_Unit still rejects missing target handle");

    return failures == 0 ? 0 : 1;
}
