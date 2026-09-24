#include "replay.h"
#include "whole_game.hpp"
#include "whole_game_actions.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

int main(int argc, char** argv) {
    if (argc != 5) return 2;
    try {
        {
            protodd::whole_observation::Snapshot row;
            protodd::whole_observation::Entity own;
            own.id = 0; own.relation = 0; own.orderTarget = 661; own.cargo = {661, 7};
            row.entities.emplace(0, own);
            protodd::whole_observation::Entity current;
            current.id = 7; current.relation = 1;
            row.entities.emplace(7, current);
            protodd::whole_observation::maskUnpublishedReferences(row, {});
            if (row.entities.at(0).orderTarget != -1 || row.entities.at(0).cargo != std::vector<int>{7})
                throw std::runtime_error("unpublished order/cargo ID leaked");
            row.entities.at(0).orderTarget = 661;
            protodd::whole_observation::maskUnpublishedReferences(row, {661});
            if (row.entities.at(0).orderTarget != 661)
                throw std::runtime_error("published historical order ID lost");
        }
        bwgame::replay_player player;
        auto mpqs = bwgame::data_loading::data_files_directory(argv[1]);
        player.init([&](bwgame::a_vector<uint8_t>& dst, bwgame::a_string name) {
            std::string filename(name.c_str());
            std::replace(filename.begin(), filename.end(), '\\', '/');
            std::ifstream file(std::filesystem::path(argv[2]) / filename, std::ios::binary);
            if (file) dst.assign(std::istreambuf_iterator<char>(file), {}); else mpqs(dst, name);
        });
        std::ifstream input(argv[3], std::ios::binary);
        std::vector<uint8_t> data{std::istreambuf_iterator<char>(input), {}};
        player.load_replay_data(data.data(), data.size());
        std::vector<int> slots;
        for (int i = 0; i < 8; ++i) if (player.st().players[i].controller == 2) slots.push_back(i);
        const int index = std::stoi(argv[4]);
        if (slots.size() != 2 || index < 0 || index > 1) throw std::runtime_error("bad fixture");
        whole_game::View view; view.owner = slots[index]; view.enemy = slots[1-index]; view.perspective = index;
        auto& f = *player.opt_funcs;
        const auto encoded = [&] {
            view.update(f); std::ostringstream out;
            whole_game::writeObservation(out, f, view, 0, "test"); return out.str();
        };
        const auto initial = encoded();
        for (const auto& [id,e] : view.entities) if (e.relation == 1) throw std::runtime_error("initial enemy leaked");
        bwgame::unit_t* enemy = nullptr;
        bwgame::unit_t* own = nullptr;
        for (auto& u : f.st.player_units[view.enemy]) if (!f.ut_building(&u)) { enemy = &u; break; }
        for (auto& u : f.st.player_units[view.owner]) if (!f.ut_building(&u)) { own = &u; break; }
        if (!enemy || !own) throw std::runtime_error("fixture has no workers");
        f.st.current_minerals[view.enemy] = 99999;
        enemy->sprite->position = {2000,2000}; enemy->energy = bwgame::fp8::integer(200);
        if (initial != encoded() || view.known(enemy) != -1) throw std::runtime_error("hidden state affected observation or ID allocation");
        enemy->sprite->visibility_flags |= 1 << view.owner;
        f.u_set_status_flag(enemy, bwgame::unit_t::status_flag_requires_detector);
        enemy->detected_flags = 0;
        if (initial != encoded()) throw std::runtime_error("undetected unit leaked");
        enemy->detected_flags |= 1 << view.owner;
        const auto detected = encoded(); const int observedId = view.known(enemy);
        if (detected == initial || observedId < 0) throw std::runtime_error("detected entity missing");
        enemy->energy = bwgame::fp8::integer(99); enemy->ground_weapon_cooldown = 99;
        if (detected != encoded()) throw std::runtime_error("enemy private energy/cooldown leaked");
        enemy->sprite->visibility_flags &= ~(1 << view.owner);
        own->order_target.unit = enemy;
        own->order_target.pos = {2500,2500};
        const auto memory = encoded();
        enemy->sprite->position = {3000,3000}; enemy->hp = bwgame::fp8::integer(1);
        own->order_target.pos = {3000,3000};
        if (memory != encoded()) throw std::runtime_error("fog position/health leaked through memory or own order");
        const auto& frozen = view.entities.at(observedId);
        if (frozen.visible || frozen.x != 2000 || frozen.lastSeen != 0) throw std::runtime_error("memory age/position invalid");
        own->ground_weapon_cooldown = 75;
        if (memory == encoded()) throw std::runtime_error("own state missing");
        own->ground_weapon_cooldown = 0;
        // A single-frame sighting must survive unsampled frames as aged memory.
        f.st.current_frame = 1; enemy->sprite->visibility_flags |= 1 << view.owner; view.update(f);
        f.st.current_frame = 2; enemy->sprite->visibility_flags &= ~(1 << view.owner); view.update(f);
        if (view.entities.at(observedId).lastSeen != 1 || view.entities.at(observedId).visible)
            throw std::runtime_error("brief sighting lost");
        const auto word = [](std::vector<uint8_t>& bytes, int value) {
            bytes.push_back(uint8_t(value & 255)); bytes.push_back(uint8_t((value >> 8) & 255));
        };
        const auto orderPacket = [&](int order, int x, int y, int target, bool queue) {
            std::vector<uint8_t> bytes{97};
            word(bytes,x); word(bytes,y); word(bytes,target); word(bytes,0);
            word(bytes,int(bwgame::UnitTypes::None)); bytes.push_back(uint8_t(order)); bytes.push_back(uint8_t(queue));
            return bytes;
        };
        auto hiddenPacket = orderPacket(int(bwgame::Orders::AttackDefault), 2000, 2000, f.get_unit_id(enemy).raw_value, false);
        const auto hidden = whole_game::decodeIntent(f,view,hiddenPacket.data(),hiddenPacket.data()+hiddenPacket.size());
        if (!hidden.targetRequested || hidden.targetAvailable || hidden.target != -1)
            throw std::runtime_error("raw packet exposed hidden target ID");
        enemy->sprite->visibility_flags |= 1 << view.owner; view.update(f);
        const auto target = whole_game::decodeIntent(f,view,hiddenPacket.data(),hiddenPacket.data()+hiddenPacket.size());
        if (!target.targetAvailable || target.target != observedId || target.kind != "attack")
            throw std::runtime_error("visible target did not resolve to causal ID");

        const auto issue = [&](bwgame::unit_t* selected, const std::vector<uint8_t>& packet, const char* expected) {
            player.action_st.selection[view.owner].clear();
            if (selected) player.action_st.selection[view.owner].push_back(selected);
            view.update(f);
            const auto intent = whole_game::decodeIntent(f, view, packet.data(), packet.data()+packet.size());
            const auto before = whole_game::captureActors(player, view, intent);
            bwgame::data_loading::data_reader_le reader(packet.data(), packet.data()+packet.size());
            const bool returned = f.read_action(view.owner,reader);
            if (!intent.decoded || intent.consumed != size_t(reader.ptr-packet.data()))
                throw std::runtime_error("packet length mismatch");
            const auto after = whole_game::effects(f,before);
            const std::string result = whole_game::acceptance(intent,returned,after);
            if (result != expected) throw std::runtime_error("command " + intent.kind + ": expected " + expected + ", got " + result);
            return intent;
        };
        auto* nexus = static_cast<bwgame::unit_t*>(nullptr);
        for (auto& u : f.st.player_units[view.owner]) if (u.unit_type->id == bwgame::UnitTypes::Protoss_Nexus) { nexus=&u; break; }
        if (!nexus) throw std::runtime_error("fixture needs Protoss perspective");
        f.st.current_minerals[view.owner] = 0;
        issue(nexus, {31,64,0}, "engine_rejected");
        f.st.current_minerals[view.owner] = 1000;
        issue(nexus, {31,64,0}, "confirmed_transition");
        const auto position = own->sprite->position;
        const int x = std::clamp(position.x+64,128,int(f.game_st.map_width)-128);
        const int y = std::clamp(position.y+64,128,int(f.game_st.map_height)-128);
        issue(own, orderPacket(int(bwgame::Orders::Move),x,y,0,false), "confirmed_transition");
        issue(own, orderPacket(int(bwgame::Orders::Move),x-32,y-32,0,true), "confirmed_transition");

        const auto* pylon = f.get_unit_type(bwgame::UnitTypes::Protoss_Pylon);
        bwgame::xy site{}; bool found = false;
        for (int ty=1; ty<int(f.game_st.map_tile_height)-1 && !found; ++ty)
            for (int tx=1; tx<int(f.game_st.map_tile_width)-1 && !found; ++tx) {
                bwgame::xy pos{tx*32+pylon->placement_size.x/2,ty*32+pylon->placement_size.y/2};
                if (f.player_position_is_visible(view.owner,pos) && f.can_place_building(own,view.owner,pylon,pos,false,false)) {
                    site={tx,ty}; found=true;
                }
            }
        if (!found) throw std::runtime_error("no building site");
        std::vector<uint8_t> build{12,uint8_t(bwgame::Orders::PlaceProtossBuilding)};
        word(build,site.x); word(build,site.y); word(build,int(bwgame::UnitTypes::Protoss_Pylon));
        f.st.current_minerals[view.owner] = 0;
        issue(own,build,"unconfirmed_no_change"); // true wrapper return is not acceptance
        f.st.current_minerals[view.owner] = 1000;
        issue(own,build,"confirmed_transition");
        issue(own,build,"unconfirmed_no_change");

        auto* templar = f.create_unit(bwgame::UnitTypes::Protoss_High_Templar,{x,y},view.owner);
        auto* shuttle = f.create_unit(bwgame::UnitTypes::Protoss_Shuttle,{x,y},view.owner);
        if (!templar || !shuttle) throw std::runtime_error("scenario units could not be created");
        f.complete_unit(templar); f.complete_unit(shuttle);
        templar->energy = bwgame::fp8::integer(200);
        const auto ownEnergy = encoded();
        templar->energy = bwgame::fp8::integer(150);
        if (ownEnergy == encoded()) throw std::runtime_error("own caster energy missing");
        templar->energy = bwgame::fp8::integer(200);
        const auto spell = orderPacket(int(bwgame::Orders::CastPsionicStorm),x,y,0,false);
        f.st.tech_researched[view.owner][bwgame::TechTypes::Psionic_Storm] = false;
        issue(templar,spell,"engine_rejected");
        f.st.tech_researched[view.owner][bwgame::TechTypes::Psionic_Storm] = true;
        if (issue(templar,spell,"confirmed_transition").domain != "ability") throw std::runtime_error("spell not classified");
        f.unit_load_target(shuttle,templar);
        issue(shuttle,{40,0},"confirmed_transition"); // this wrapper falsely returns false
        std::vector<uint8_t> unload{98}; word(unload,f.get_unit_id(templar).raw_value); word(unload,0);
        issue(nullptr,unload,"confirmed_transition"); // cargo action does not require selection
        std::ostringstream terrainBefore; whole_game::writeTerrain(terrainBefore,f);
        for (auto& tile : f.st.tiles) tile.flags ^= bwgame::tile_t::flag_has_creep | bwgame::tile_t::flag_occupied;
        std::ostringstream terrainAfter; whole_game::writeTerrain(terrainAfter,f);
        if (terrainBefore.str()!=terrainAfter.str()) throw std::runtime_error("dynamic private state leaked into terrain");
        std::cout << "PASS: v3.2 fog/private-state/terrain perturbation, causal targets, brief sightings, accepted/rejected/repeated build, train, move, queued order, spell, unload-all and selection-independent unload\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
