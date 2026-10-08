#include "WholeGameAction.hpp"
#include "protodd/WholeGameActionPolicy.hpp"

#include <algorithm>
#include <optional>

namespace protodd::bwapi {
namespace {

std::optional<BWAPI::UnitCommand> makeCommand(
    const cpu::Intent& intent, BWAPI::Unit actor, BWAPI::Unit target,
    BWAPI::Position position) {
    using BWAPI::UnitCommand;
    const bool hasEntity = intent.targetMode == 1 && target != nullptr;
    const bool hasPosition = intent.targetMode == 2 && position.isValid();
    switch (intent.kind) {
        case 0:  // right_click
            if (hasEntity) return UnitCommand::rightClick(actor, target, intent.queued);
            if (hasPosition) return UnitCommand::rightClick(actor, position, intent.queued);
            return std::nullopt;
        case 1: // move
            if (hasPosition) return UnitCommand::move(actor, position, intent.queued);
            return std::nullopt;
        case 2: // attack
            if (hasEntity) return UnitCommand::attack(actor, target, intent.queued);
            if (hasPosition) return UnitCommand::attack(actor, position, intent.queued);
            return std::nullopt;
        case 3: // attack_move
            if (hasPosition) return UnitCommand::attack(actor, position, intent.queued);
            return std::nullopt;
        case 4: // patrol
            if (hasPosition) return UnitCommand::patrol(actor, position, intent.queued);
            return std::nullopt;
        case 5: // follow
            if (hasEntity) return UnitCommand::follow(actor, target, intent.queued);
            return std::nullopt;
        case 7: // gather
            if (hasEntity) return UnitCommand::gather(actor, target, intent.queued);
            return std::nullopt;
        case 8: // rally
            if (hasEntity) return UnitCommand::setRallyPoint(actor, target);
            if (hasPosition) return UnitCommand::setRallyPoint(actor, position);
            return std::nullopt;
        case 9: // load
            if (hasEntity) return UnitCommand::load(actor, target, intent.queued);
            return std::nullopt;
        case 10: // unload_position
            if (hasPosition) return UnitCommand::unloadAll(actor, position, intent.queued);
            return std::nullopt;
        case 11: { // cast
            const BWAPI::TechType technology(intent.technology);
            if (technology.getRace() != BWAPI::Races::Protoss) return std::nullopt;
            if (hasEntity) return UnitCommand::useTech(actor, technology, target);
            if (hasPosition) return UnitCommand::useTech(actor, technology, position);
            if (intent.targetMode == 0) return UnitCommand::useTech(actor, technology);
            return std::nullopt;
        }
        case 12: { // build
            const BWAPI::UnitType type(intent.unitType);
            if (!hasPosition || type.getRace() != BWAPI::Races::Protoss || !type.isBuilding())
                return std::nullopt;
            return UnitCommand::build(actor, BWAPI::TilePosition(position), type);
        }
        case 13: // train
        case 29: { // train_fighter
            const BWAPI::UnitType type(intent.unitType);
            if (intent.targetMode != 0 || type.getRace() != BWAPI::Races::Protoss || type.isBuilding())
                return std::nullopt;
            return UnitCommand::train(actor, type);
        }
        case 15: { // research
            const BWAPI::TechType technology(intent.technology);
            if (intent.targetMode != 0 || technology.getRace() != BWAPI::Races::Protoss)
                return std::nullopt;
            return UnitCommand::research(actor, technology);
        }
        case 16: { // upgrade
            const BWAPI::UpgradeType upgrade(intent.upgrade);
            if (intent.targetMode != 0 || upgrade.getRace() != BWAPI::Races::Protoss)
                return std::nullopt;
            return UnitCommand::upgrade(actor, upgrade);
        }
        case 17: // cancel_queue
            if (intent.targetMode == 0) return UnitCommand::cancelTrain(actor, intent.queueSlot);
            return std::nullopt;
        case 18: // cancel_build
            if (intent.targetMode == 0) return UnitCommand::cancelConstruction(actor);
            return std::nullopt;
        case 20: // cancel_research
            if (intent.targetMode == 0) return UnitCommand::cancelResearch(actor);
            return std::nullopt;
        case 21: // cancel_upgrade
            if (intent.targetMode == 0) return UnitCommand::cancelUpgrade(actor);
            return std::nullopt;
        case 23: // stop
            if (intent.targetMode == 0) return UnitCommand::stop(actor, intent.queued);
            return std::nullopt;
        case 24: // return_cargo
            if (intent.targetMode == 0) return UnitCommand::returnCargo(actor, intent.queued);
            return std::nullopt;
        case 30: // unload_all
            if (intent.targetMode == 0) return UnitCommand::unloadAll(actor, intent.queued);
            if (hasPosition) return UnitCommand::unloadAll(actor, position, intent.queued);
            return std::nullopt;
        case 31: // unload_unit
            if (hasEntity) return UnitCommand::unload(actor, target);
            return std::nullopt;
        case 32: // merge_archon
            if (hasEntity) return UnitCommand::useTech(actor, BWAPI::TechTypes::Archon_Warp, target);
            return std::nullopt;
        case 33: // merge_dark_archon
            if (hasEntity) return UnitCommand::useTech(actor, BWAPI::TechTypes::Dark_Archon_Meld, target);
            return std::nullopt;
        case 34: // hold
            if (intent.targetMode == 0) return UnitCommand::holdPosition(actor, intent.queued);
            return std::nullopt;
        default:
            return std::nullopt; // Race-specific and raw order kinds need explicit support.
    }
}

}  // namespace

std::vector<WholeGameActorAssessment> assessWholeGameCommands(
    const cpu::Intent& intent, const whole_observation::Snapshot& observation,
    const std::map<int, BWAPI::Unit>& unitByToken, BWAPI::Game* game) {
    std::vector<WholeGameActorAssessment> results;
    results.reserve(intent.actorIds.size());
    std::string globalRejection;
    if (!game) globalRejection = "game-unavailable";
    else if (intent.kind >= cpu::kindNames.size()) globalRejection = "invalid-action-kind";
    else if (!protodd::wholeGameActionAuthorityAllowed(intent.kind))
        globalRejection = "action-authority-disabled";
    BWAPI::Unit target = nullptr;
    if (globalRejection.empty()) {
        const auto entity = intent.targetEntityId
            ? observation.entities.find(*intent.targetEntityId) : observation.entities.end();
        const auto unit = intent.targetEntityId
            ? unitByToken.find(*intent.targetEntityId) : unitByToken.end();
        const auto entityStatus = protodd::wholeGameEntityTargetStatus(
            intent.targetMode == 1, intent.targetEntityId.has_value(),
            entity != observation.entities.end(),
            entity != observation.entities.end() && entity->second.visible,
            unit != unitByToken.end() && unit->second && unit->second->exists());
        switch (entityStatus) {
            case protodd::WholeGameEntityTargetStatus::notRequired:
            case protodd::WholeGameEntityTargetStatus::available:
                if (entityStatus == protodd::WholeGameEntityTargetStatus::available)
                    target = unit->second;
                break;
            case protodd::WholeGameEntityTargetStatus::missingId:
                globalRejection = "missing-target-entity";
                break;
            case protodd::WholeGameEntityTargetStatus::notInObservation:
                globalRejection = "target-not-in-observation";
                break;
            case protodd::WholeGameEntityTargetStatus::notVisible:
                globalRejection = "target-not-visible";
                break;
            case protodd::WholeGameEntityTargetStatus::noLongerExists:
                globalRejection = "target-no-longer-exists";
                break;
        }
    }
    BWAPI::Position position = BWAPI::Positions::Invalid;
    if (globalRejection.empty() && intent.targetMode == 2) {
        if (!intent.targetPixel) globalRejection = "missing-target-position";
        else {
        position = BWAPI::Position(intent.targetPixel->first, intent.targetPixel->second);
        if (position.x < 0 || position.y < 0 || position.x >= game->mapWidth() * 32 ||
            position.y >= game->mapHeight() * 32)
                globalRejection = "target-position-out-of-map";
        }
    }
    if (globalRejection.empty() && intent.targetMode > 2)
        globalRejection = "invalid-target-mode";
    for (const auto token : intent.actorIds) {
        auto& result = results.emplace_back();
        result.actorToken = token;
        if (!globalRejection.empty()) {
            result.reason = globalRejection;
            continue;
        }
        const auto entity = observation.entities.find(token);
        const auto lookup = unitByToken.find(token);
        if (entity == observation.entities.end()) {
            result.reason = "actor-not-in-observation";
            continue;
        }
        if (entity->second.relation != 0) {
            result.reason = "actor-not-owned";
            continue;
        }
        if (lookup == unitByToken.end()) {
            result.reason = "actor-not-current";
            continue;
        }
        const auto actor = lookup->second;
        if (!actor || !actor->exists()) {
            result.reason = "actor-no-longer-exists";
            continue;
        }
        if (actor->getPlayer() != game->self()) {
            result.reason = "actor-not-self";
            continue;
        }
        if (actor->isLoaded()) {
            result.reason = "actor-loaded";
            continue;
        }
        if (actor->isLockedDown() || actor->isMaelstrommed() || actor->isStasised()) {
            result.reason = "actor-disabled";
            continue;
        }
        if (!wholeGameActorEligible(intent.kind, intent.targetMode, actor->isCompleted(),
                                    actor->getType().isBuilding(), actor->isBeingConstructed())) {
            result.reason = intent.kind == 18 && !actor->getType().isBuilding()
                ? "cancel-build-actor-not-building"
                : intent.kind == 18 && !actor->isBeingConstructed()
                    ? "cancel-build-not-under-construction"
                    : "actor-incomplete";
            continue;
        }
        const auto command = makeCommand(intent, actor, target, position);
        if (!command) {
            result.reason = "unsupported-or-invalid-action-arguments";
            continue;
        }
        if (!actor->canIssueCommand(*command)) {
            result.reason = "bwapi-can-issue-command-false";
            continue;
        }
        result.command = *command;
    }
    return results;
}

std::vector<LegalWholeGameCommand> legalWholeGameCommands(
    const cpu::Intent& intent, const whole_observation::Snapshot& observation,
    const std::map<int, BWAPI::Unit>& unitByToken, BWAPI::Game* game,
    const std::size_t maximumCommands) {
    std::vector<LegalWholeGameCommand> accepted;
    if (maximumCommands == 0) return accepted;
    for (const auto& assessment : assessWholeGameCommands(
             intent, observation, unitByToken, game)) {
        if (accepted.size() >= maximumCommands) break;
        if (assessment.command)
            accepted.push_back({assessment.actorToken, *assessment.command, {}});
    }
    return accepted;
}

}  // namespace protodd::bwapi
