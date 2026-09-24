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

#ifdef PROTODD_EMBED_WHOLE_GAME_WEIGHTS
extern HINSTANCE moduleInstance;
#endif

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
        if (selected.size() >= 8) {
            static_cast<void>(schedule_.defer(std::move(item), frame + 1));
            continue;
        }
        const auto legal = legalWholeGameCommands(
            item.intent, live, currentByToken, BWAPI::BroodwarPtr, 8 - selected.size());
        std::vector<int> deferredActors;
        for (const auto& command : legal) {
            if (issuedActors.insert(command.actorToken).second)
                selected.push_back(command);
            else
                deferredActors.push_back(command.actorToken);
        }
        if (!deferredActors.empty()) {
            item.intent.actorIds = std::move(deferredActors);
            static_cast<void>(schedule_.defer(std::move(item), frame + 1));
        }
    }
    return selected;
}

void WholeGameRuntime::start() {
    output_.close(); inferenceOutput_.close(); intentOutput_.close(); model_.reset(); memory_.clear();
    schedule_.clear();
    enabled_ = false; terrain_ = {};
    ids_.clear(); entities_.clear(); published_.clear();
    nextId_ = sequence_ = 0; lastFrame_ = -1;
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
                for (const auto& slot : slotPrediction->slots) {
                    const auto intent = cpu::decodeIntent(slot, encoded, row,
                        {.eventProbability = 0.0f, .actorLogit = -1e8f, .maxActors = 1});
                    if (!intent) continue;
                    const auto& delay = slot.heads.at("delay");
                    const auto delayFrames = static_cast<int>(std::distance(
                        delay.begin(), std::max_element(delay.begin(), delay.end())));
                    plans.push_back({delayFrames, *intent});
                    const auto legal = legalWholeGameCommands(
                        *intent, row, currentByToken, BWAPI::BroodwarPtr, 8);
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
                                      << intent->queued << ',' << legal.size() << '\n';
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
                    const auto legal = legalWholeGameCommands(
                        *intent, row, currentByToken, BWAPI::BroodwarPtr, 8);
#ifdef PROTODD_WHOLE_GAME_CONTROL
                    schedule_.replace(frame, {{0, *intent}});
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
                                      << intent->queued << ',' << legal.size() << '\n';
                }
            }
            }
            if (inferenceOutput_ && frame % 240 == 7 &&
                prediction.heads.contains("domain") && prediction.heads.contains("kind")) {
                const auto top = [](const std::vector<float>& logits) {
                    return std::distance(logits.begin(), std::max_element(logits.begin(), logits.end()));
                };
                inferenceOutput_ << frame << ',' << encoded.entityIds.size() << ',' << encoded.overflow
                                 << ',' << elapsed << ',' << top(prediction.heads.at("domain"))
                                 << ',' << top(prediction.heads.at("kind")) << '\n';
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
    }
    return selectedCommands;
}

void WholeGameRuntime::end() {
    if (output_) { output_.flush(); output_.close(); }
    if (inferenceOutput_) { inferenceOutput_.flush(); inferenceOutput_.close(); }
    if (intentOutput_) { intentOutput_.flush(); intentOutput_.close(); }
    model_.reset(); memory_.clear(); enabled_ = false;
    schedule_.clear();
}

}  // namespace protodd::bwapi
