// Local integration tests require the user's local MPQs, terrain and replay.
#define main replay_extraction_entry
#include "extract.cpp"
#undef main
#include <sstream>

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    try {
        bwgame::replay_player player;
        auto mpqs = bwgame::data_loading::data_files_directory(argv[1]);
        player.init([&](bwgame::a_vector<uint8_t>& dst, bwgame::a_string name) {
            std::ifstream file(std::filesystem::path(argv[2]) / name.c_str(), std::ios::binary);
            if (file) dst.assign(std::istreambuf_iterator<char>(file), {}); else mpqs(dst, name);
        });
        std::ifstream input(argv[3], std::ios::binary);
        std::vector<uint8_t> data{std::istreambuf_iterator<char>(input), {}};
        player.load_replay_data(data.data(), data.size());
        std::vector<int> slots;
        for (int i = 0; i < 8; ++i) if (player.st().players[i].controller == 2) slots.push_back(i);
        if (slots.size() != 2) throw std::runtime_error("bad fixture");
        Perspective p; p.owner = slots[0]; p.enemy = slots[1]; p.index = 0;
        auto& f = player.funcs();
        updateMemory(f, p);
        if (!p.memory.snapshot().empty()) throw std::runtime_error("initial fog leaked enemy");
        const auto before = p.encoder.encode(observe(f, p));
        player.st().current_minerals[p.enemy] = 999999;
        player.st().tech_researched[p.enemy][bwgame::TechTypes::Psionic_Storm] = true;
        bwgame::unit_t* unseen = nullptr;
        for (auto& u : player.st().player_units[p.enemy]) if (!f.ut_building(&u)) { unseen = &u; break; }
        if (!unseen) throw std::runtime_error("missing test enemy");
        unseen->unit_type = f.get_unit_type(bwgame::UnitTypes::Protoss_Arbiter);
        unseen->sprite->position = {2000, 2000};
        p.encoder.reset(); updateMemory(f, p);
        if (before != p.encoder.encode(observe(f, p))) throw std::runtime_error("hidden perturbation affected features");
        unseen->sprite->visibility_flags |= 1 << p.owner;
        f.u_set_status_flag(unseen, bwgame::unit_t::status_flag_requires_detector);
        unseen->detected_flags = 0;
        updateMemory(f, p);
        if (!p.memory.snapshot().empty()) throw std::runtime_error("undetected unit leaked");
        unseen->detected_flags |= 1 << p.owner;
        updateMemory(f, p);
        if (p.memory.snapshot().size() != 1) throw std::runtime_error("detected unit not observed");
        bwgame::unit_t* probe = nullptr;
        bwgame::unit_t* nexus = nullptr;
        for (auto& u : player.st().player_units[p.owner]) {
            if (u.unit_type->id == bwgame::UnitTypes::Protoss_Probe) probe = &u;
            if (u.unit_type->id == bwgame::UnitTypes::Protoss_Nexus) nexus = &u;
        }
        if (!probe || !nexus) throw std::runtime_error("fixture needs a Protoss first player");
        p.active = true; p.mask = ~LearnedActionMask{0};
        std::vector<Perspective> perspectives; perspectives.push_back(std::move(p));
        const auto command = [&](bwgame::unit_t* selected, int code, std::vector<uint8_t> payload) {
            player.action_st.selection[slots[0]].clear(); player.action_st.selection[slots[0]].push_back(selected);
            auto& actions = player.replay_st.actions_data_buffer;
            actions = {0,0,0,0,static_cast<uint8_t>(payload.size()+2),static_cast<uint8_t>(player.action_st.player_id[slots[0]]),static_cast<uint8_t>(code)};
            actions.insert(actions.end(), payload.begin(), payload.end());
            player.action_st.actions_data_position = 0; player.action_st.next_action_frame = 0;
            std::ostringstream audit;
            execute(player, perspectives, audit);
        };
        player.st().current_minerals[slots[0]] = 0;
        command(nexus, 31, {64,0});
        if (perspectives[0].accepted || perspectives[0].rejected != 1) throw std::runtime_error("unaffordable train labeled");
        player.st().current_minerals[slots[0]] = 1000;
        command(nexus, 31, {64,0});
        if (perspectives[0].accepted != 1 || nexus->build_queue.empty()) throw std::runtime_error("accepted train missing");
        auto* pylon = f.get_unit_type(bwgame::UnitTypes::Protoss_Pylon);
        bwgame::xy site{}; bool found = false;
        for (int y = 1; y < 127 && !found; ++y) for (int x = 1; x < 127 && !found; ++x) {
            bwgame::xy pos{x * 32 + pylon->placement_size.x / 2, y * 32 + pylon->placement_size.y / 2};
            if (f.player_position_is_visible(slots[0], pos) && f.can_place_building(probe, slots[0], pylon, pos, false, false)) { site = {x,y}; found = true; }
        }
        if (!found) throw std::runtime_error("missing valid build site");
        std::vector<uint8_t> build{static_cast<uint8_t>(bwgame::Orders::PlaceProtossBuilding),
            static_cast<uint8_t>(site.x),0,static_cast<uint8_t>(site.y),0,static_cast<uint8_t>(bwgame::UnitTypes::Protoss_Pylon),0};
        player.st().current_minerals[slots[0]] = 0;
        command(probe, 12, build);
        if (perspectives[0].accepted != 1) throw std::runtime_error("unaffordable build labeled despite true wrapper return");
        player.st().current_minerals[slots[0]] = 1000;
        command(probe, 12, build);
        if (perspectives[0].accepted != 2) throw std::runtime_error("accepted build missing");
        command(probe, 12, build);
        if (perspectives[0].accepted != 2 || perspectives[0].repeated != 1) throw std::runtime_error("repeated build relabeled");
        std::cout << "PASS: native fog, detection, private-state perturbation, accepted/rejected/repeated commands\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
