#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <ostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace protodd::whole_observation {

inline constexpr auto schema = "protodd-whole-game-pilot-v3.2";
inline constexpr auto terrainSchema = "protodd-terrain-v2";
inline constexpr std::size_t maximumRememberedEntities = 4096;

// IDs are local, causal tokens. Adapters must never place engine IDs in these
// fields or populate an enemy's private state.
struct Entity {
    int id{}, type{}, relation{}, x{}, y{}, hp{}, shields{}, firstSeen{}, lastSeen{};
    bool visible{}, completed{}, building{};
    int energy{}, groundCooldown{}, airCooldown{}, order{}, orderX{}, orderY{}, orderTarget{-1};
    bool loaded{};
    std::vector<int> queue, cargo;
};

struct Snapshot {
    int perspective{}, sequence{}, frame{}, minerals{}, gas{}, supplyUsed{}, supplyTotal{};
    std::string reason, vision;
    std::vector<int> technologyCompleted, technologyInProgress, upgradeLevels, upgradeInProgress;
    std::map<int, Entity> entities;
};

struct EntityPruneResult {
    bool withinLimit{true};
    std::vector<int> removedIds;
};

// Keep recent and currently observed entities. Forgotten own actors must be
// removed by the adapter's lifecycle reconciliation; this cap only evicts
// stale non-own observations when the learned runtime's retained history grows.
inline EntityPruneResult pruneStaleEntities(
    std::map<int, Entity>& entities,
    const std::size_t maximumSize = maximumRememberedEntities) {
    EntityPruneResult result;
    if (entities.size() <= maximumSize) return result;

    std::vector<std::pair<int, int>> stale;
    stale.reserve(entities.size());
    for (const auto& [id, entity] : entities) {
        if (entity.relation != 0 && !entity.visible)
            stale.emplace_back(entity.lastSeen, id);
    }
    std::ranges::sort(stale);
    for (const auto& [lastSeen, id] : stale) {
        static_cast<void>(lastSeen);
        if (entities.size() <= maximumSize) break;
        entities.erase(id);
        result.removedIds.push_back(id);
    }
    result.withinLimit = entities.size() <= maximumSize;
    return result;
}

inline void forgetUnretainedTokens(
    std::map<int, int>& engineIds,
    std::set<int>& published,
    const std::map<int, Entity>& entities) {
    std::erase_if(engineIds, [&entities](const auto& entry) {
        return !entities.contains(entry.second);
    });
    std::erase_if(published, [&entities](const int id) {
        return !entities.contains(id);
    });
}

inline void maskUnpublishedReferences(Snapshot& view, const std::set<int>& published) {
    // Observation adapters update memory every frame, but publish only on
    // cadence/commands. A unit may disappear before its allocated local ID was
    // ever emitted. Never expose such an ID through another unit's own state.
    const auto legal = [&](int id) { return published.contains(id) || view.entities.contains(id); };
    for (auto& [id, entity] : view.entities) if (entity.relation == 0) {
        if (entity.orderTarget >= 0 && !legal(entity.orderTarget)) entity.orderTarget = -1;
        std::erase_if(entity.cargo, [&](int cargo) { return !legal(cargo); });
    }
}

inline void integers(std::ostream& out, const std::vector<int>& values) {
    out << '[';
    for (size_t i = 0; i < values.size(); ++i) { if (i) out << ','; out << values[i]; }
    out << ']';
}

inline void writeObservation(std::ostream& out, const Snapshot& view) {
    out << "{\"schema\":\"" << schema << "\",\"perspective\":" << view.perspective
        << ",\"sequence\":" << view.sequence << ",\"frame\":" << view.frame
        << ",\"reason\":\"" << view.reason << "\",\"minerals\":" << view.minerals
        << ",\"gas\":" << view.gas << ",\"supply_used\":" << view.supplyUsed
        << ",\"supply_total\":" << view.supplyTotal;
    out << ",\"technology_completed\":"; integers(out, view.technologyCompleted);
    out << ",\"technology_in_progress\":"; integers(out, view.technologyInProgress);
    out << ",\"upgrade_levels\":"; integers(out, view.upgradeLevels);
    out << ",\"upgrade_in_progress\":"; integers(out, view.upgradeInProgress);
    out << ",\"entities\":[";
    bool first = true;
    for (const auto& [id, e] : view.entities) {
        if (!first) out << ','; first = false;
        out << "{\"id\":" << id << ",\"type\":" << e.type << ",\"relation\":" << e.relation
            << ",\"position\":[" << e.x << ',' << e.y << "],\"hp\":" << e.hp
            << ",\"shields\":" << e.shields << ",\"visible\":" << int(e.visible)
            << ",\"completed\":" << int(e.completed) << ",\"first_seen\":" << e.firstSeen
            << ",\"last_seen\":" << e.lastSeen << ",\"own_state\":";
        if (e.relation != 0) out << "null";
        else {
            out << "{\"energy\":" << e.energy << ",\"ground_cooldown\":" << e.groundCooldown
                << ",\"air_cooldown\":" << e.airCooldown << ",\"order\":" << e.order
                << ",\"order_position\":[" << e.orderX << ',' << e.orderY
                << "],\"order_target\":" << e.orderTarget << ",\"loaded\":" << int(e.loaded)
                << ",\"queue\":";
            integers(out, e.queue); out << ",\"cargo\":"; integers(out, e.cargo); out << '}';
        }
        out << '}';
    }
    out << "],\"vision\":\"" << view.vision << "\"}\n";
}

inline bool staticWalktile(const int x, const int y, const int width, const int height, const bool tileWalkable) {
    // StarCraft applies a gameplay border even when the VF4 terrain is walkable.
    if (y >= height - 4) return false;
    if (y >= height - 8 && (x < 20 || x >= width - 20)) return false;
    return tileWalkable;
}

}  // namespace protodd::whole_observation
