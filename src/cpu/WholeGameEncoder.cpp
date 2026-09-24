#include "WholeGameEncoder.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace protodd::cpu {

EncodedObservation encode(const whole_observation::Snapshot& source,
                          const Terrain& terrain, const std::size_t limit) {
    const auto pixels = static_cast<std::size_t>(terrain.height) * terrain.width;
    if (terrain.height == 0 || terrain.width == 0 || source.vision.size() != pixels
        || terrain.walkableFraction.size() != pixels || limit < 1
        || source.technologyCompleted.size() != 44 || source.technologyInProgress.size() != 44
        || source.upgradeLevels.size() != 61 || source.upgradeInProgress.size() != 61)
        throw std::runtime_error("invalid whole-game feature source");
    std::vector<const whole_observation::Entity*> own, visible, memory, neutral;
    double meanX = 0, meanY = 0;
    for (const auto& [id, entity] : source.entities) {
        if (entity.relation == 0) {
            own.push_back(&entity);
            meanX += entity.x; meanY += entity.y;
        } else if (entity.relation == 1 && entity.visible) visible.push_back(&entity);
        else if (entity.relation == 1) memory.push_back(&entity);
        else if (entity.relation == 2) neutral.push_back(&entity);
        else throw std::runtime_error("unknown whole-game entity relation");
    }
    if (own.empty() || own.size() > limit)
        throw std::runtime_error("all own entities must fit whole-game capacity");
    meanX /= own.size(); meanY /= own.size();
    const auto distance = [&](const whole_observation::Entity* entity) {
        const auto dx = entity->x - meanX, dy = entity->y - meanY;
        return dx * dx + dy * dy;
    };
    std::sort(visible.begin(), visible.end(), [&](const auto* a, const auto* b) {
        if (distance(a) != distance(b)) return distance(a) < distance(b);
        return a->id < b->id;
    });
    std::sort(memory.begin(), memory.end(), [&](const auto* a, const auto* b) {
        if (a->lastSeen != b->lastSeen) return a->lastSeen > b->lastSeen;
        if (distance(a) != distance(b)) return distance(a) < distance(b);
        return a->id < b->id;
    });
    std::sort(neutral.begin(), neutral.end(), [&](const auto* a, const auto* b) {
        if (distance(a) != distance(b)) return distance(a) < distance(b);
        return a->id < b->id;
    });
    std::vector<const whole_observation::Entity*> selected;
    selected.reserve(std::min(limit, source.entities.size()));
    for (const auto& group : {own, visible, memory, neutral})
        for (const auto* entity : group)
            if (selected.size() < limit) selected.push_back(entity);
    EncodedObservation result;
    result.overflow = source.entities.size() - selected.size();
    auto& input = result.input;
    input.width = terrain.width; input.height = terrain.height;
    input.type.reserve(selected.size()); input.relation.reserve(selected.size());
    input.order.reserve(selected.size()); input.entityNumeric.reserve(selected.size() * 16);
    result.entityIds.reserve(selected.size());
    for (const auto* entity : selected) {
        const bool isOwn = entity->relation == 0;
        const auto x = static_cast<float>(entity->x), y = static_cast<float>(entity->y);
        input.type.push_back(entity->type);
        input.relation.push_back(entity->relation);
        input.order.push_back(isOwn ? entity->order + 1 : 0);
        result.entityIds.push_back(entity->id);
        const auto add = [&](float value) { input.entityNumeric.push_back(value); };
        add(x / (terrain.width * 32.0f)); add(y / (terrain.height * 32.0f));
        add(std::log1p(static_cast<float>(std::max(0, entity->hp))) / 12.0f);
        add(std::log1p(static_cast<float>(std::max(0, entity->shields))) / 12.0f);
        add(entity->visible ? 1.0f : 0.0f); add(entity->completed ? 1.0f : 0.0f);
        add(std::min(1.0f, (source.frame - entity->lastSeen) / 3000.0f));
        add(isOwn ? entity->energy / 250.0f : 0.0f);
        add(isOwn ? entity->groundCooldown / 60.0f : 0.0f);
        add(isOwn ? entity->airCooldown / 60.0f : 0.0f);
        add(isOwn && entity->orderX >= 0 ? entity->orderX / (terrain.width * 32.0f) : 0.0f);
        add(isOwn && entity->orderY >= 0 ? entity->orderY / (terrain.height * 32.0f) : 0.0f);
        add(isOwn && entity->loaded ? 1.0f : 0.0f);
        add(isOwn ? std::min(1.0f, entity->queue.size() / 5.0f) : 0.0f);
        add(isOwn ? std::min(1.0f, entity->cargo.size() / 8.0f) : 0.0f);
        add(entity->firstSeen / 100000.0f);
    }
    input.spatial.resize(pixels * 3);
    for (std::size_t index = 0; index < pixels; ++index) {
        const char vision = source.vision[index];
        if (vision < '0' || vision > '2') throw std::runtime_error("invalid whole-game vision state");
        input.spatial[index] = vision >= '1' ? 1.0f : 0.0f;
        input.spatial[pixels + index] = vision == '2' ? 1.0f : 0.0f;
        input.spatial[2 * pixels + index] = terrain.walkableFraction[index];
    }
    input.global.reserve(216);
    input.global.push_back(std::log1p(static_cast<float>(source.minerals)) / 10.0f);
    input.global.push_back(std::log1p(static_cast<float>(source.gas)) / 10.0f);
    input.global.push_back(source.supplyUsed / 400.0f);
    input.global.push_back(source.supplyTotal / 400.0f);
    input.global.push_back(source.frame / 100000.0f);
    input.global.push_back(static_cast<float>(result.overflow) / limit);
    for (const auto& group : {source.technologyCompleted, source.technologyInProgress,
                              source.upgradeLevels, source.upgradeInProgress})
        for (const auto value : group) input.global.push_back(static_cast<float>(value));
    return result;
}

}  // namespace protodd::cpu
