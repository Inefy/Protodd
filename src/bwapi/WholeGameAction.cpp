#include "WholeGameAction.hpp"

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

std::vector<LegalWholeGameCommand> legalWholeGameCommands(
    const cpu::Intent& intent, const whole_observation::Snapshot& observation,
    const std::map<int, BWAPI::Unit>& unitByToken, BWAPI::Game* game,
    std::size_t maximumCommands) {
    std::vector<LegalWholeGameCommand> accepted;
    if (!game || maximumCommands == 0 || intent.kind >= cpu::kindNames.size()) return accepted;
    BWAPI::Unit target = nullptr;
    if (intent.targetEntityId) {
        const auto entity = observation.entities.find(*intent.targetEntityId);
        const auto unit = unitByToken.find(*intent.targetEntityId);
        if (entity != observation.entities.end() && entity->second.visible &&
            unit != unitByToken.end() && unit->second && unit->second->exists())
            target = unit->second;
    }
    BWAPI::Position position = BWAPI::Positions::Invalid;
    if (intent.targetPixel) {
        position = BWAPI::Position(intent.targetPixel->first, intent.targetPixel->second);
        if (position.x < 0 || position.y < 0 || position.x >= game->mapWidth() * 32 ||
            position.y >= game->mapHeight() * 32)
            position = BWAPI::Positions::Invalid;
    }
    for (const auto token : intent.actorIds) {
        if (accepted.size() >= maximumCommands) break;
        const auto entity = observation.entities.find(token);
        const auto lookup = unitByToken.find(token);
        if (entity == observation.entities.end() || entity->second.relation != 0 ||
            lookup == unitByToken.end()) continue;
        const auto actor = lookup->second;
        if (!actor || !actor->exists() || actor->getPlayer() != game->self() ||
            !actor->isCompleted() || actor->isLoaded() || actor->isLockedDown() ||
            actor->isMaelstrommed() || actor->isStasised()) continue;
        const auto command = makeCommand(intent, actor, target, position);
        if (command && actor->canIssueCommand(*command))
            accepted.push_back({token, *command});
    }
    return accepted;
}

}  // namespace protodd::bwapi
