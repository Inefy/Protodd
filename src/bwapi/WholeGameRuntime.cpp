#include "WholeGameRuntime.hpp"
#include "WholeGameAction.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>
#ifdef PROTODD_EMBED_WHOLE_GAME_WEIGHTS
#include <Windows.h>
#endif

namespace protodd::bwapi {
namespace {

bool nativeCommandObserved(const BWAPI::UnitCommand expected) {
    const auto actor = expected.getUnit();
    if (actor == nullptr || !actor->exists()) return false;
    const auto observed = actor->getLastCommand();
    if (observed.getType() != expected.getType()) return false;
    const auto expectedTarget = expected.getTarget();
    if (expectedTarget != nullptr && observed.getTarget() != expectedTarget) return false;
    const auto expectedPosition = expected.getTargetPosition();
    if (expectedPosition.isValid() && observed.getTargetPosition() != expectedPosition) return false;
    if (expected.getUnitType() != BWAPI::UnitTypes::None &&
        observed.getUnitType() != expected.getUnitType()) return false;
    if (expected.getTechType() != BWAPI::TechTypes::None &&
        observed.getTechType() != expected.getTechType()) return false;
    if (expected.getUpgradeType() != BWAPI::UpgradeTypes::None &&
        observed.getUpgradeType() != expected.getUpgradeType()) return false;
    return true;
}

}  // namespace

#ifdef PROTODD_EMBED_WHOLE_GAME_WEIGHTS
extern HINSTANCE moduleInstance;
#endif

void WholeGameRuntime::logActionAudit(
    const WholeGameActionIdentity& identity, const int frame, const int actorToken,
    const std::string_view stage, const std::string_view outcome,
    const std::string_view reason) {
    if (!actionAuditOutput_) return;
    actionAuditOutput_ << identity.attemptId << ',' << identity.proposalFrame << ','
                       << identity.slot << ',' << identity.dueFrame << ',' << frame << ','
                       << actorToken << ',' << identity.actorOrdinal << ','
                       << identity.actorCount << ',' << identity.intentKind << ','
                       << identity.targetMode << ',' << identity.targetEntity << ','
                       << identity.targetX << ',' << identity.targetY << ',' << stage << ','
                       << outcome << ',';
    for (const auto character : reason)
        if (character != ',' && character != '\r' && character != '\n')
            actionAuditOutput_ << character;
    actionAuditOutput_ << '\n';
}

void WholeGameRuntime::recordApiResult(
    const LegalWholeGameCommand& action, const bool accepted, const int frame,
    const std::string_view reason) {
    logActionAudit(action.identity, frame, action.actorToken, "api",
                   accepted ? "accepted" : "rejected", reason);
    if (!accepted) return;
    constexpr auto maximumPendingExecutions = std::size_t{256};
    if (pendingExecutions_.size() >= maximumPendingExecutions) {
        logActionAudit(action.identity, frame, action.actorToken, "execution",
                       "censored", "pending-observation-limit");
        return;
    }
    pendingExecutions_.push_back({action, frame});
}

void WholeGameRuntime::reconcileActionExecutions(const int frame) {
    constexpr auto observationWindow = 24;
    for (auto pending = pendingExecutions_.begin(); pending != pendingExecutions_.end();) {
        const auto actor = pending->action.command.getUnit();
        const auto identity = pending->action.identity;
        const auto actorToken = pending->action.actorToken;
        if (actor == nullptr || !actor->exists()) {
            logActionAudit(identity, frame, actorToken, "execution", "censored",
                           "actor-no-longer-exists");
            pending = pendingExecutions_.erase(pending);
            continue;
        }
        if (nativeCommandObserved(pending->action.command)) {
            logActionAudit(identity, frame, actorToken, "execution", "observed",
                           "native-last-command-matched");
            pending = pendingExecutions_.erase(pending);
            continue;
        }
        const auto target = pending->action.command.getTarget();
        if (target != nullptr && !target->exists()) {
            logActionAudit(identity, frame, actorToken, "execution", "censored",
                           "target-no-longer-exists");
            pending = pendingExecutions_.erase(pending);
            continue;
        }
        if (frame - pending->acceptedFrame >= observationWindow) {
            logActionAudit(identity, frame, actorToken, "execution", "unobserved",
                           "native-last-command-not-matched-within-window");
            pending = pendingExecutions_.erase(pending);
            continue;
        }
        ++pending;
    }
}

int WholeGameRuntime::known(const BWAPI::Unit unit) const {
    if (!unit) return -1;
    const auto found = ids_.find(unit->getID());
    return found == ids_.end() ? -1 : found->second;
}

std::vector<LegalWholeGameCommand> WholeGameRuntime::dispatchDue(
    const int frame, const std::vector<BWAPI::Unit>& current) {
    std::vector<LegalWholeGameCommand> selected;
    auto due = schedule_.takeDue(frame);
    if (due.empty()) return selected;
    std::map<int, BWAPI::Unit> currentByToken;
    for (const auto unit : current) {
        const auto token = known(unit);
        if (token >= 0) currentByToken.emplace(token, unit);
    }
    whole_observation::Snapshot live;
    live.entities = entities_;
    std::set<int> issuedActors;
    for (auto& item : due) {
        const auto identityFor = [&item](const std::size_t ordinal) {
            const auto& intent = item.intent;
            return WholeGameActionIdentity{
                .attemptId = item.attemptId,
                .proposalFrame = item.proposalFrame,
                .dueFrame = item.dueFrame,
                .slot = item.slot,
                .actorOrdinal = item.actorOrdinals.size() > ordinal
                    ? item.actorOrdinals[ordinal] : ordinal,
                .actorCount = item.originalActorCount,
                .intentKind = intent.kind,
                .targetMode = intent.targetMode,
                .targetEntity = intent.targetEntityId.value_or(-1),
                .targetX = intent.targetPixel ? intent.targetPixel->first : -1,
                .targetY = intent.targetPixel ? intent.targetPixel->second : -1,
            };
        };
        if (selected.size() >= 8) {
            for (std::size_t index = 0; index < item.intent.actorIds.size(); ++index)
                logActionAudit(identityFor(index), frame, item.intent.actorIds[index],
                               "dispatch", "deferred", "frame-command-capacity");
            const auto identity = identityFor(0);
            const auto actors = item.intent.actorIds;
            if (!schedule_.defer(std::move(item), frame + 1))
                for (std::size_t index = 0; index < actors.size(); ++index)
                    logActionAudit(identity, frame, actors[index], "dispatch", "canceled",
                                   "cadence-window-expired");
            continue;
        }
        const auto assessments = assessWholeGameCommands(
            item.intent, live, currentByToken, BWAPI::BroodwarPtr);
        std::vector<int> deferredActors;
        std::vector<std::size_t> deferredOrdinals;
        for (std::size_t index = 0; index < assessments.size(); ++index) {
            const auto& assessment = assessments[index];
            const auto identity = identityFor(index);
            if (!assessment.legal()) {
                logActionAudit(identity, frame, assessment.actorToken, "legality",
                               "rejected", assessment.reason);
                continue;
            }
            logActionAudit(identity, frame, assessment.actorToken, "legality",
                           "legal", "all-checks-passed");
            if (selected.size() >= 8) {
                deferredActors.push_back(assessment.actorToken);
                if (index < item.actorOrdinals.size())
                    deferredOrdinals.push_back(item.actorOrdinals[index]);
                logActionAudit(identity, frame, assessment.actorToken, "dispatch",
                               "deferred", "frame-command-capacity");
            } else if (!issuedActors.insert(assessment.actorToken).second) {
                deferredActors.push_back(assessment.actorToken);
                if (index < item.actorOrdinals.size())
                    deferredOrdinals.push_back(item.actorOrdinals[index]);
                logActionAudit(identity, frame, assessment.actorToken, "arbitration",
                               "deferred", "actor-already-claimed-by-earlier-slot");
            } else {
                auto actionIdentity = identity;
                actionIdentity.dueFrame = item.dueFrame;
                selected.push_back({assessment.actorToken, *assessment.command,
                                    actionIdentity});
                logActionAudit(actionIdentity, frame, assessment.actorToken, "arbitration",
                               "selected", "first-eligible-slot-for-actor");
            }
        }
        if (!deferredActors.empty()) {
            item.intent.actorIds = std::move(deferredActors);
            item.actorOrdinals = std::move(deferredOrdinals);
            const auto identity = identityFor(0);
            const auto actors = item.intent.actorIds;
            if (!schedule_.defer(std::move(item), frame + 1))
                for (std::size_t index = 0; index < actors.size(); ++index)
                    logActionAudit(identity, frame, actors[index], "dispatch", "canceled",
                                   "cadence-window-expired");
        }
        if (assessments.empty()) {
            const auto identity = identityFor(0);
            logActionAudit(identity, frame, -1, "legality", "rejected", "empty-actor-set");
        }
    }
    return selected;
}

void WholeGameRuntime::start() {
    output_.close(); inferenceOutput_.close(); intentOutput_.close();
    actionAuditOutput_.close(); model_.reset(); memory_.clear();
    schedule_.clear();
    enabled_ = false; terrain_ = {};
    ids_.clear(); entities_.clear(); published_.clear();
    nextId_ = sequence_ = 0;
    lastFrame_ = -1;
    nextActionAttemptId_ = 0;
    pendingExecutions_.clear();
    const bool trace = std::filesystem::exists("bwapi-data/read/WholeGame-observe.txt");
    const auto weights = std::filesystem::path("bwapi-data/read/WholeGame-weights.bin");
#ifdef PROTODD_EMBED_WHOLE_GAME_WEIGHTS
    const bool hasWeights = true;
#else
    const bool hasWeights = std::filesystem::exists(weights);
#endif
    if (hasWeights) {
        try {
#ifdef PROTODD_EMBED_WHOLE_GAME_WEIGHTS
            const auto resource = FindResourceW(moduleInstance, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10));
            if (!resource) throw std::runtime_error("embedded whole-game weights resource missing");
            const auto loaded = LoadResource(moduleInstance, resource);
            const auto size = SizeofResource(moduleInstance, resource);
            const auto data = loaded ? LockResource(loaded) : nullptr;
            if (!data || size == 0) throw std::runtime_error("cannot read embedded whole-game weights");
            model_ = std::make_unique<cpu::WholeGameCpu>(
                std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data), size));
#else
            model_ = std::make_unique<cpu::WholeGameCpu>(weights);
#endif
#ifdef PROTODD_WHOLE_GAME_CONTROL
            inferenceOutput_.open("bwapi-data/write/WholeGame-inference.csv", std::ios::trunc);
            std::ofstream controller("bwapi-data/write/WholeGame-controller.txt", std::ios::trunc);
            controller << "compiled-control\n";
#else
            inferenceOutput_.open("bwapi-data/write/WholeGame-shadow.csv", std::ios::trunc);
#endif
            intentOutput_.open("bwapi-data/write/WholeGame-intents.csv", std::ios::trunc);
            actionAuditOutput_.open("bwapi-data/write/WholeGame-actions.csv", std::ios::trunc);
            if (actionAuditOutput_)
                actionAuditOutput_ << "attempt_id,proposal_frame,slot,due_frame,frame,actor_token,actor_ordinal,actor_count,intent_kind,target_mode,target_entity,target_x,target_y,stage,outcome,reason\n";
        } catch (const std::exception& error) {
            std::ofstream failure("bwapi-data/write/WholeGame-model-error.txt", std::ios::trunc);
            failure << error.what() << '\n';
            model_.reset();
        }
    }
    if (trace) output_.open("bwapi-data/write/WholeGame-observations.jsonl", std::ios::trunc);
    enabled_ = output_.is_open() || model_ != nullptr;
    if (!enabled_) return;
    auto& game = BWAPI::Broodwar;
    terrain_.width = game->mapWidth(); terrain_.height = game->mapHeight();
    terrain_.walkableFraction.resize(static_cast<std::size_t>(terrain_.width) * terrain_.height);
    for (int y = 0; y < game->mapHeight(); ++y)
        for (int x = 0; x < game->mapWidth(); ++x) {
            int walkable = 0;
            for (int dy = 0; dy < 4; ++dy)
                for (int dx = 0; dx < 4; ++dx)
                    walkable += game->isWalkable(x * 4 + dx, y * 4 + dy) ? 1 : 0;
            terrain_.walkableFraction[static_cast<std::size_t>(y) * terrain_.width + x]
                = walkable / 16.0f;
        }
    if (!output_) return;
    std::ofstream terrain("bwapi-data/write/WholeGame-terrain.json", std::ios::trunc);
    terrain << "{\"schema\":\"" << whole_observation::terrainSchema << "\",\"width_walktiles\":" << game->mapWidth() * 4
            << ",\"height_walktiles\":" << game->mapHeight() * 4 << ",\"walkability\":\"";
    for (int y = 0; y < game->mapHeight() * 4; ++y)
        for (int x = 0; x < game->mapWidth() * 4; ++x)
            terrain << (game->isWalkable(x, y) ? '1' : '0');
    terrain << "\",\"height\":\"";
    // BWAPI exposes build-tile height, whereas the replay adapter reads VF4
    // height at walk-tile resolution. Keep this diagnostic only until parity.
    for (int y = 0; y < game->mapHeight() * 4; ++y)
        for (int x = 0; x < game->mapWidth() * 4; ++x)
            terrain << std::clamp(game->getGroundHeight(x / 4, y / 4) / 2, 0, 2);
    terrain << "\",\"height_comparable\":false,\"walkability_border_applied\":true}\n";
}

std::vector<LegalWholeGameCommand> WholeGameRuntime::observe() {
    std::vector<LegalWholeGameCommand> selectedCommands;
    if (!enabled_) return selectedCommands;
    auto& game = BWAPI::Broodwar;
    auto* self = game->self();
    auto* enemy = game->enemy();
    if (!self || !enemy || game->isReplay() || game->isPaused()) return selectedCommands;
    const int frame = game->getFrameCount();
    reconcileActionExecutions(frame);
    const bool cadence = frame % 24 == 7;
#ifdef PROTODD_WHOLE_GAME_CONTROL
    if (!cadence && (!controlling() || !schedule_.hasDue(frame)))
        return selectedCommands;
#else
    if (!cadence) return selectedCommands;
#endif
    if (frame < lastFrame_) { ids_.clear(); entities_.clear(); published_.clear(); memory_.clear(); schedule_.clear(); nextId_ = sequence_ = 0; }
    lastFrame_ = frame;
    for (auto& [id, entity] : entities_) entity.visible = false;
    std::vector<BWAPI::Unit> current;
    std::set<int> own;
    const auto collect = [&](BWAPI::Unitset units, int relation) {
        for (const auto unit : units) {
            if (!unit || !unit->exists() || (relation != 0 && (!unit->isVisible() || !unit->isDetected()))) continue;
            const int engineId = unit->getID();
            const auto [at, inserted] = ids_.try_emplace(engineId, nextId_);
            if (inserted) ++nextId_;
            const int id = at->second;
            if (relation == 0) own.insert(id);
            const auto previous = entities_.find(id);
            whole_observation::Entity e;
            e.id = id; e.type = unit->getType().getID(); e.relation = relation;
            e.x = unit->getPosition().x; e.y = unit->getPosition().y;
            e.hp = unit->getHitPoints(); e.shields = unit->getShields();
            e.firstSeen = previous == entities_.end() ? frame : previous->second.firstSeen;
            e.lastSeen = frame; e.visible = true; e.completed = unit->isCompleted();
            e.building = unit->getType().isBuilding();
            if (relation == 0) {
                e.energy = unit->getEnergy();
                e.groundCooldown = unit->getGroundWeaponCooldown(); e.airCooldown = unit->getAirWeaponCooldown();
                e.order = unit->getOrder().getID(); e.loaded = unit->isLoaded();
                const auto target = unit->getOrderTarget();
                const bool legalTarget = !target || target->getPlayer() == self ||
                    (target->exists() && target->isVisible() && target->isDetected());
                if (legalTarget) {
                    e.orderX = unit->getOrderTargetPosition().x;
                    e.orderY = unit->getOrderTargetPosition().y;
                } else if (const auto old = entities_.find(known(target)); old != entities_.end()) {
                    e.orderX = old->second.x; e.orderY = old->second.y;
                } else e.orderX = e.orderY = -1;
                for (auto type : unit->getTrainingQueue()) e.queue.push_back(type.getID());
            }
            entities_[id] = std::move(e);
            current.push_back(unit);
        }
    };
    collect(self->getUnits(), 0);
    collect(enemy->getUnits(), 1);
    collect(game->getNeutralUnits(), 2);
    for (const auto unit : current) if (unit->getPlayer() == self) {
        auto& e = entities_.at(known(unit));
        e.orderTarget = known(unit->getOrderTarget());
        for (const auto cargo : unit->getLoadedUnits())
            if (cargo && cargo->getPlayer() == self && known(cargo) >= 0) e.cargo.push_back(known(cargo));
    }
    std::erase_if(entities_, [&](const auto& pair) {
        const auto& e = pair.second;
        if (e.relation == 0) return !own.contains(e.id);
        return !e.visible && e.building && e.x >= 0 && e.y >= 0 &&
            game->isVisible(BWAPI::TilePosition(e.x / 32, e.y / 32));
    });
    const auto prunedEntities = whole_observation::pruneStaleEntities(entities_);
    if (!prunedEntities.withinLimit) {
        // The learned observer/controller is optional. If the currently live
        // set alone exceeds its bounded memory, stop it and leave native
        // control in charge instead of growing retained state indefinitely.
        std::ofstream failure("bwapi-data/write/WholeGame-model-error.txt", std::ios::app);
        if (failure) failure << "frame=" << frame << ",reason=entity-memory-limit\n";
        end();
        return selectedCommands;
    }
    // Keep the engine-ID registry and published-token set in step with entity
    // memory so destroyed or evicted units do not accumulate for the match and
    // freed engine IDs can be assigned a fresh local token if reused.
    whole_observation::forgetUnretainedTokens(ids_, published_, entities_);
    // Strategy, diagnostics and several other costly legacy phases run on
    // frame 0 of each 24-frame window. Stagger the learned policy so the
    // total BWAPI callback remains below the tournament frame budget.
    if (!cadence) {
#ifdef PROTODD_WHOLE_GAME_CONTROL
        if (controlling()) selectedCommands = dispatchDue(frame, current);
#endif
        return selectedCommands;
    }
#ifdef PROTODD_WHOLE_GAME_CONTROL
    schedule_.replace(frame, {});
#endif
    whole_observation::Snapshot row;
    row.perspective = 0; row.sequence = sequence_++; row.frame = frame; row.reason = "cadence";
    row.minerals = self->minerals(); row.gas = self->gas();
    row.supplyUsed = self->supplyUsed(); row.supplyTotal = std::min(400, self->supplyTotal());
    for (int i = 0; i < 44; ++i) {
        const BWAPI::TechType technology(i);
        const bool researchedProtossTech = technology.getRace() == BWAPI::Races::Protoss &&
            technology.whatResearches() != BWAPI::UnitTypes::None;
        row.technologyCompleted.push_back(researchedProtossTech && self->hasResearched(technology) ? 1 : 0);
        row.technologyInProgress.push_back(self->isResearching(BWAPI::TechType(i)) ? 1 : 0);
    }
    for (int i = 0; i < 61; ++i) {
        row.upgradeLevels.push_back(self->getUpgradeLevel(BWAPI::UpgradeType(i)));
        row.upgradeInProgress.push_back(self->isUpgrading(BWAPI::UpgradeType(i)) ? 1 : 0);
    }
    row.entities = entities_;
    whole_observation::maskUnpublishedReferences(row, published_);
    row.vision.reserve(game->mapWidth() * game->mapHeight());
    for (int y = 0; y < game->mapHeight(); ++y)
        for (int x = 0; x < game->mapWidth(); ++x) {
            const BWAPI::TilePosition tile(x,y);
            row.vision += game->isVisible(tile) ? '2' : game->isExplored(tile) ? '1' : '0';
        }
    if (output_) whole_observation::writeObservation(output_, row);
    for (const auto& [id, entity] : row.entities) published_.insert(id);
    // No legal own actor remains after defeat. Do not turn a normal terminal
    // state into a permanent model failure on the last few game frames.
    if (model_ && self->getRace() == BWAPI::Races::Protoss &&
        !own.empty() && own.size() <= 512) {
        try {
            const auto encoded = cpu::encode(row, terrain_);
            const auto began = std::chrono::steady_clock::now();
            cpu::Output prediction;
            std::optional<cpu::SlotOutput> slotPrediction;
            if (model_->multiSlot()) {
                slotPrediction = model_->inferSlots(encoded.input, memory_);
                prediction.memory = slotPrediction->memory;
                if (!slotPrediction->slots.empty())
                    prediction.heads = slotPrediction->slots.front().heads;
            } else prediction = model_->infer(encoded.input, memory_);
            if (slotPrediction ? !cpu::safeWholeGameOutput(*slotPrediction)
                               : !cpu::safeWholeGameOutput(prediction))
                throw std::runtime_error("nonfinite or out-of-range whole-game inference output");
            const auto elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - began).count();
            memory_ = prediction.memory;
            if (slotPrediction) {
                std::vector<cpu::PlannedIntent> plans;
                std::map<int, BWAPI::Unit> currentByToken;
                for (const auto unit : current) {
                    const auto token = known(unit);
                    if (token >= 0) currentByToken.emplace(token, unit);
                }
                for (std::size_t slotOrdinal = 0;
                     slotOrdinal < slotPrediction->slots.size(); ++slotOrdinal) {
                    const auto& slot = slotPrediction->slots[slotOrdinal];
                    const auto intent = cpu::decodeIntent(slot, encoded, row,
                        {.eventProbability = 0.0f, .actorLogit = -1e8f, .maxActors = 1});
                    if (!intent) continue;
                    const auto& delay = slot.heads.at("delay");
                    const auto delayFrames = static_cast<int>(std::distance(
                        delay.begin(), std::max_element(delay.begin(), delay.end())));
                    const auto attemptId = nextActionAttemptId_++;
                    plans.push_back({delayFrames, *intent, attemptId, slotOrdinal});
                    const auto assessments = assessWholeGameCommands(
                        *intent, row, currentByToken, BWAPI::BroodwarPtr);
                    const auto legalCount = std::ranges::count_if(
                        assessments, [](const WholeGameActorAssessment& item) {
                            return item.legal();
                        });
                    for (std::size_t actorOrdinal = 0;
                         actorOrdinal < intent->actorIds.size(); ++actorOrdinal) {
                        const auto& target = intent->targetEntityId;
                        const auto& pixel = intent->targetPixel;
                        WholeGameActionIdentity identity{
                            .attemptId = attemptId,
                            .proposalFrame = frame,
                            .dueFrame = frame + delayFrames,
                            .slot = slotOrdinal,
                            .actorOrdinal = actorOrdinal,
                            .actorCount = intent->actorIds.size(),
                            .intentKind = intent->kind,
                            .targetMode = intent->targetMode,
                            .targetEntity = target.value_or(-1),
                            .targetX = pixel ? pixel->first : -1,
                            .targetY = pixel ? pixel->second : -1,
                        };
                        logActionAudit(identity, frame, intent->actorIds[actorOrdinal],
                                       "proposal", "emitted", "model-decoded-slot");
#ifndef PROTODD_WHOLE_GAME_CONTROL
                        const auto assessment = actorOrdinal < assessments.size()
                            ? assessments[actorOrdinal]
                            : WholeGameActorAssessment{intent->actorIds[actorOrdinal],
                                                       std::nullopt, "missing-legality-result"};
                        logActionAudit(identity, frame, assessment.actorToken, "legality",
                                       assessment.legal() ? "legal" : "rejected",
                                       assessment.legal() ? "all-checks-passed" : assessment.reason);
#endif
                    }
                    if (intentOutput_) {
                        const auto target = intent->targetEntityId.value_or(-1);
                        const auto x = intent->targetPixel ? intent->targetPixel->first : -1;
                        const auto y = intent->targetPixel ? intent->targetPixel->second : -1;
                        intentOutput_ << frame << ',' << intent->eventProbability << ','
                                      << cpu::kindNames[intent->kind] << ',' << intent->kindProbability << ','
                                      << cpu::domainNames[intent->domain] << ','
                                      << cpu::targetModeNames[intent->targetMode] << ','
                                      << intent->actorIds.size() << ',' << intent->actorIds.front() << ','
                                      << target << ',' << x << ',' << y << ',' << intent->unitType << ','
                                      << intent->technology << ',' << intent->upgrade << ','
                                      << intent->queued << ',' << legalCount << '\n';
                    }
                }
#ifdef PROTODD_WHOLE_GAME_CONTROL
                schedule_.replace(frame, std::move(plans));
                selectedCommands = dispatchDue(frame, current);
#endif
            } else {
            {
#ifdef PROTODD_WHOLE_GAME_CONTROL
                const auto intent = cpu::decodeIntent(prediction, encoded, row);
#else
                const auto intent = cpu::decodeIntent(prediction, encoded, row,
                    {.eventProbability = 0.0f, .actorLogit = -1000.0f});
#endif
                if (intent) {
                    std::map<int, BWAPI::Unit> currentByToken;
                    for (const auto unit : current) {
                        const auto token = known(unit);
                        if (token >= 0) currentByToken.emplace(token, unit);
                    }
                    const auto attemptId = nextActionAttemptId_++;
                    const auto assessments = assessWholeGameCommands(
                        *intent, row, currentByToken, BWAPI::BroodwarPtr);
                    const auto legalCount = std::ranges::count_if(
                        assessments, [](const WholeGameActorAssessment& item) {
                            return item.legal();
                        });
                    for (std::size_t actorOrdinal = 0;
                         actorOrdinal < intent->actorIds.size(); ++actorOrdinal) {
                        WholeGameActionIdentity identity{
                            .attemptId = attemptId,
                            .proposalFrame = frame,
                            .dueFrame = frame,
                            .slot = 0,
                            .actorOrdinal = actorOrdinal,
                            .actorCount = intent->actorIds.size(),
                            .intentKind = intent->kind,
                            .targetMode = intent->targetMode,
                            .targetEntity = intent->targetEntityId.value_or(-1),
                            .targetX = intent->targetPixel ? intent->targetPixel->first : -1,
                            .targetY = intent->targetPixel ? intent->targetPixel->second : -1,
                        };
                        logActionAudit(identity, frame, intent->actorIds[actorOrdinal],
                                       "proposal", "emitted", "model-decoded-action");
#ifndef PROTODD_WHOLE_GAME_CONTROL
                        const auto& assessment = assessments[actorOrdinal];
                        logActionAudit(identity, frame, assessment.actorToken, "legality",
                                       assessment.legal() ? "legal" : "rejected",
                                       assessment.legal() ? "all-checks-passed" : assessment.reason);
#endif
                    }
#ifdef PROTODD_WHOLE_GAME_CONTROL
                    schedule_.replace(frame, {{0, *intent, attemptId}});
                    selectedCommands = dispatchDue(frame, current);
#endif
                    const auto target = intent->targetEntityId.value_or(-1);
                    const auto x = intent->targetPixel ? intent->targetPixel->first : -1;
                    const auto y = intent->targetPixel ? intent->targetPixel->second : -1;
                    if (intentOutput_)
                        intentOutput_ << frame << ',' << intent->eventProbability << ','
                                      << cpu::kindNames[intent->kind] << ',' << intent->kindProbability << ','
                                      << cpu::domainNames[intent->domain] << ','
                                      << cpu::targetModeNames[intent->targetMode] << ','
                                      << intent->actorIds.size() << ',' << intent->actorIds.front() << ','
                                      << target << ',' << x << ',' << y << ',' << intent->unitType << ','
                                      << intent->technology << ',' << intent->upgrade << ','
                                      << intent->queued << ',' << legalCount << '\n';
                }
            }
            }
            if (inferenceOutput_ && frame % 240 == 7) {
                const auto top = [](const std::vector<float>& logits) {
                    return std::distance(logits.begin(), std::max_element(logits.begin(), logits.end()));
                };
                inferenceOutput_ << frame << ',' << encoded.entityIds.size() << ',' << encoded.overflow
                                 << ',' << elapsed << ','
                                 << (prediction.heads.contains("domain") ? top(prediction.heads.at("domain")) : -1)
                                 << ',' << (prediction.heads.contains("kind") ? top(prediction.heads.at("kind")) : -1)
                                 << '\n';
            }
        } catch (const std::exception& error) {
            std::ofstream failure("bwapi-data/write/WholeGame-model-error.txt", std::ios::trunc);
            failure << "frame=" << frame << ',' << error.what() << '\n';
            model_.reset(); memory_.clear(); schedule_.clear();
            enabled_ = output_.is_open();
        }
    }
    if (frame % 240 == 7) {
        if (output_) output_.flush();
        if (inferenceOutput_) inferenceOutput_.flush();
        if (intentOutput_) intentOutput_.flush();
        if (actionAuditOutput_) actionAuditOutput_.flush();
    }
    return selectedCommands;
}

void WholeGameRuntime::end() {
    const auto frame = BWAPI::Broodwar->getFrameCount();
    for (const auto& pending : pendingExecutions_)
        logActionAudit(pending.action.identity, frame, pending.action.actorToken,
                       "execution", "censored", "game-ended-before-observation");
    pendingExecutions_.clear();
    if (output_) { output_.flush(); output_.close(); }
    if (inferenceOutput_) { inferenceOutput_.flush(); inferenceOutput_.close(); }
    if (intentOutput_) { intentOutput_.flush(); intentOutput_.close(); }
    if (actionAuditOutput_) { actionAuditOutput_.flush(); actionAuditOutput_.close(); }
    model_.reset(); memory_.clear(); enabled_ = false;
    schedule_.clear();
    ids_.clear(); entities_.clear(); published_.clear(); terrain_ = {};
    nextId_ = sequence_ = 0; lastFrame_ = -1;
}

}  // namespace protodd::bwapi
