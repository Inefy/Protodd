#pragma once
#include "replay.h"
#include "protodd/WholeGameObservation.hpp"
#include <map>
#include <ostream>
#include <set>
#include <vector>

namespace whole_game {
inline constexpr auto schema = protodd::whole_observation::schema;
using protodd::whole_observation::Entity;
using protodd::whole_observation::integers;

struct View {
    int owner{}, enemy{}, perspective{}, nextId{};
    std::map<std::uint64_t, int> ids;
    std::map<int, Entity> entities;
    std::set<int> published;
    int frame{-1};
    static std::uint64_t key(const bwgame::unit_t* u) {
        return std::uint64_t(u->unit_id_generation) * 3400 + u->index;
    }
    int known(const bwgame::unit_t* u) const {
        if (!u) return -1;
        const auto it = ids.find(key(u));
        return it == ids.end() ? -1 : it->second;
    }
    bool observable(bwgame::state_functions& f, const bwgame::unit_t* u) const {
        return u && u->sprite && !f.ut_turret(u) &&
            (u->owner == owner || ((u->sprite->visibility_flags & (1 << owner)) &&
             !f.unit_is_undetected(u, owner) && !f.us_hidden(u)));
    }
    void update(bwgame::state_functions& f) {
        if (f.st.current_frame < frame) throw std::runtime_error("v3 memory moved backwards");
        frame = f.st.current_frame;
        for (auto& [id, e] : entities) e.visible = false;
        std::vector<bwgame::unit_t*> visible;
        std::set<int> own;
        for (size_t i = 0; i < f.st.units_container.max_size; ++i) {
            auto* u = f.st.units_container.try_get(i);
            if (!observable(f, u)) continue;
            if (f.unit_dead(u) || f.unit_dying(u)) { entities.erase(known(u)); continue; }
            // Neutral resources are included; other non-player entities retain
            // a neutral relation, never an enemy-private observation.
            const auto k = key(u);
            if (!ids.contains(k)) ids.emplace(k, nextId++);
            const int id = ids.at(k);
            const bool ours = u->owner == owner;
            if (ours) own.insert(id);
            const auto old = entities.find(id);
            Entity e;
            e.relation = ours ? 0 : (u->owner == enemy ? 1 : 2);
            e.id = id; e.type = int(u->unit_type->id);
            // BWAPI exposes mineral-field variants as the same public type.
            if (e.relation == 2 && e.type >= 176 && e.type <= 178) e.type = 176;
            e.x = u->sprite->position.x; e.y = u->sprite->position.y;
            e.hp = u->hp.ceil().integer_part();
            e.shields = u->unit_type->has_shield ? u->shield_points.integer_part() : 0;
            e.firstSeen = old == entities.end() ? frame : old->second.firstSeen;
            e.lastSeen = frame; e.visible = true; e.completed = f.u_completed(u);
            e.building = f.ut_building(u);
            if (ours) {
                e.energy = f.ut_has_energy(u) ? u->energy.integer_part() : 0;
                e.groundCooldown = u->ground_weapon_cooldown; e.airCooldown = u->air_weapon_cooldown;
                e.order = u->order_type ? int(u->order_type->id) : -1;
                // A unit-target order can internally follow a hidden target.
                // Do not copy its live private position through order_target.pos.
                const auto* target = u->order_target.unit;
                const auto last = entities.find(known(target));
                if (!target || observable(f, target)) {
                    e.orderX = u->order_target.pos.x; e.orderY = u->order_target.pos.y;
                } else if (last != entities.end()) {
                    e.orderX = last->second.x; e.orderY = last->second.y;
                } else { e.orderX = e.orderY = -1; }
                e.loaded = f.u_loaded(u);
                for (auto* type : u->build_queue) e.queue.push_back(int(type->id));
            }
            entities[id] = std::move(e);
            visible.push_back(u);
        }
        // Resolve references only after IDs have been allocated by observations.
        for (const auto* u : visible) if (u->owner == owner) {
            auto& e = entities.at(known(u));
            e.orderTarget = known(u->order_target.unit);
            for (auto* cargo : f.loaded_units(u)) {
                const int id = known(cargo);
                if (id >= 0 && cargo->owner == owner) e.cargo.push_back(id);
            }
        }
        std::erase_if(entities, [&](const auto& pair) {
            const auto& e = pair.second;
            if (e.relation == 0) return !own.contains(e.id);
            // Never use a hidden unit's current position or death state here.
            return !e.visible && e.building &&
                f.player_position_is_visible(owner, {e.x / 32 * 32, e.y / 32 * 32});
        });
    }
};

inline void writeTerrain(std::ostream& out, const bwgame::state_functions& f) {
    // Use immutable map graphics/tileset terrain, not current occupancy, creep,
    // unseen units or any other spectator-only simulation state.
    out << "{\"schema\":\"" << protodd::whole_observation::terrainSchema << "\",\"width_walktiles\":" << f.game_st.map_width / 8
        << ",\"height_walktiles\":" << f.game_st.map_height / 8 << ",\"walkability\":\"";
    const auto flags = [&](int x, int y) {
        const auto tile = f.game_st.gfx_tiles.at(f.tile_index({x,y}));
        const auto mega = f.game_st.cv5.at(tile.group_index()).mega_tile_index[tile.subtile_index()];
        return f.game_st.vf4.at(mega).flags[y / 8 % 4 * 4 + x / 8 % 4];
    };
    for (int y = 0; y < int(f.game_st.map_height); y += 8)
        for (int x = 0; x < int(f.game_st.map_width); x += 8)
            out << (protodd::whole_observation::staticWalktile(x / 8, y / 8,
                f.game_st.map_width / 8, f.game_st.map_height / 8,
                flags(x,y) & bwgame::vf4_entry::flag_walkable) ? '1' : '0');
    out << "\",\"height\":\"";
    for (int y = 0; y < int(f.game_st.map_height); y += 8)
        for (int x = 0; x < int(f.game_st.map_width); x += 8) {
            const auto value = flags(x,y);
            out << ((value & bwgame::vf4_entry::flag_high) ? '2' :
                    (value & bwgame::vf4_entry::flag_middle) ? '1' : '0');
        }
    out << "\"}\n";
}

inline void writeObservation(std::ostream& out, bwgame::state_functions& f,
                             View& view, int sequence, const char* reason) {
    protodd::whole_observation::Snapshot snapshot;
    snapshot.perspective = view.perspective; snapshot.sequence = sequence;
    snapshot.frame = f.st.current_frame; snapshot.reason = reason;
    snapshot.minerals = f.st.current_minerals[view.owner]; snapshot.gas = f.st.current_gas[view.owner];
    snapshot.supplyUsed = f.st.supply_used[view.owner][2].raw_value;
    snapshot.supplyTotal = std::min(400, f.st.supply_available[view.owner][2].raw_value);
    for (auto v : f.st.tech_researched[view.owner]) snapshot.technologyCompleted.push_back(int(v));
    for (auto v : f.st.tech_researching[view.owner]) snapshot.technologyInProgress.push_back(int(v));
    for (auto v : f.st.upgrade_levels[view.owner]) snapshot.upgradeLevels.push_back(int(v));
    for (auto v : f.st.upgrade_upgrading[view.owner]) snapshot.upgradeInProgress.push_back(int(v));
    snapshot.entities = view.entities;
    protodd::whole_observation::maskUnpublishedReferences(snapshot, view.published);
    // Row-major build-tile vision: 0 unexplored, 1 explored, 2 currently visible.
    for (int y = 0; y < int(f.game_st.map_height); y += 32)
        for (int x = 0; x < int(f.game_st.map_width); x += 32)
            snapshot.vision += f.player_position_is_visible(view.owner, {x,y}) ? '2' :
                f.player_position_is_explored(view.owner, {x,y}) ? '1' : '0';
    protodd::whole_observation::writeObservation(out, snapshot);
    for (const auto& [id, entity] : snapshot.entities) view.published.insert(id);
}
} // namespace whole_game
