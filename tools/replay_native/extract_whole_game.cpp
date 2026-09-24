// Separate pilot executable: v2 extraction and frozen binaries stay unchanged.
// Reuse the pinned initialization/catalog/checkpoint helpers, not macro labels.
#define main legacy_macro_extraction_entry
#include "extract.cpp"
#undef main
#include "whole_game.hpp"
#include "whole_game_actions.hpp"

namespace {
struct WholePerspective {
    whole_game::View view;
    int sequence{}, observations{}, commands{};
};

void executeWhole(bwgame::replay_player& player, std::vector<WholePerspective>& perspectives,
                  std::ostream& observations, std::ostream& commandsOut) {
    auto& a = player.action_st;
    auto& data = player.replay_st.actions_data_buffer;
    auto& f = *player.opt_funcs;
    if (f.st.current_frame != a.next_action_frame) return;
    while (a.actions_data_position != data.size()) {
        bwgame::data_loading::data_reader_le r(data.data() + a.actions_data_position, data.data() + data.size());
        const int frame = r.get<int32_t>();
        if (frame != f.st.current_frame) { a.next_action_frame = frame; return; }
        const auto count = r.get<uint8_t>();
        const auto* ptr = r.get_n(count); const auto* end = ptr + count;
        bwgame::data_loading::data_reader_le commands(ptr, end);
        while (commands.ptr != end) {
            int id = commands.get<uint8_t>();
            if (id == 128) id = a.player_id.at(0);
            const auto found = std::find(a.player_id.begin(), a.player_id.end(), id);
            if (found == a.player_id.end() || commands.ptr == end) throw std::runtime_error("Invalid command owner/body");
            const int owner = int(found - a.player_id.begin());
            const auto* start = commands.ptr;
            const int code = *start;
            auto p = std::find_if(perspectives.begin(), perspectives.end(),
                [&](const auto& v) { return v.view.owner == owner; });
            std::vector<int> selected;
            whole_game::Intent intent;
            std::vector<whole_game::ActorBefore> actors;
            if (p != perspectives.end()) {
                p->view.update(f);
                for (const auto* u : a.selection[owner]) {
                    const int local = p->view.known(u);
                    if (u && u->owner == owner && local >= 0 && p->view.entities.contains(local)) selected.push_back(local);
                }
                whole_game::writeObservation(observations, f, p->view, p->sequence++, "before_command");
                ++p->observations;
                intent = whole_game::decodeIntent(f, p->view, start, end);
                actors = whole_game::captureActors(player, p->view, intent);
            }
            const bool returned = f.read_action(owner, commands);
            if (p == perspectives.end()) continue;
            if (intent.decoded && intent.consumed != size_t(commands.ptr - start))
                throw std::runtime_error("Semantic decoder disagrees with engine packet length");
            const auto effects = whole_game::effects(f, actors);
            ++p->commands;
            // Replay payloads are issued-action diagnostics, NOT policy inputs.
            // They contain raw target IDs and can include clicks that were rejected.
            // Transition evidence confirms an immediate command-state change,
            // not eventual movement, build completion, damage or spell success.
            commandsOut << "{\"schema\":\"" << whole_game::commandSchema << "\",\"perspective\":" << p->view.perspective << ",\"frame\":" << frame
                << ",\"observation_sequence\":" << p->sequence - 1 << ",\"code\":" << code
                << ",\"selected_own\":";
            whole_game::integers(commandsOut, selected);
            commandsOut << ",\"engine_returned\":" << (returned ? "true" : "false")
                << ",\"acceptance\":\"" << whole_game::acceptance(intent, returned, effects) << "\",\"semantic\":";
            whole_game::writeIntent(commandsOut, intent, effects);
            commandsOut << ",\"payload_hex\":\"";
            constexpr char hex[] = "0123456789abcdef";
            for (const auto* b = start + 1; b < commands.ptr; ++b) commandsOut << hex[*b >> 4] << hex[*b & 15];
            commandsOut << "\"}\n";
        }
        a.actions_data_position = end - data.data();
    }
}
}

int main(int argc, char** argv) {
    if (argc != 7) {
        std::cerr << "Usage: replay_extract_v3 MPQ_DIR ASSET_DIR RAW OUTPUT_DIR VALID_THROUGH_FRAME PERSPECTIVE\n";
        return 2;
    }
    try {
        const std::filesystem::path output(argv[4]);
        const int validThrough = std::stoi(argv[5]), perspective = std::stoi(argv[6]);
        if (validThrough < 1 || validThrough > 2000000 || perspective < 0 || perspective > 1)
            throw std::runtime_error("Invalid prefix/perspective");
        if (!std::filesystem::create_directory(output)) throw std::runtime_error("Output exists");
        auto mpqs = bwgame::data_loading::data_files_directory(argv[1]);
        bwgame::replay_player player;
        player.init([&](bwgame::a_vector<uint8_t>& dst, bwgame::a_string name) {
            std::string filename(name.c_str());
            std::replace(filename.begin(), filename.end(), '\\', '/');
            for (auto& ch : filename) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
            if (filename.rfind("tileset/", 0) == 0 && filename.find("ashworld.") == std::string::npos && filename.find("install.") == std::string::npos) {
                std::ifstream input(std::filesystem::path(argv[2]) / filename, std::ios::binary);
                if (!input) throw std::runtime_error("Missing modern terrain: " + filename);
                dst.assign(std::istreambuf_iterator<char>(input), {});
                if (dst.empty() || dst.size() % (filename.ends_with(".cv5") ? 52 : 32)) throw std::runtime_error("Invalid modern terrain");
            } else mpqs(dst, name);
        });
        std::ifstream input(argv[3], std::ios::binary);
        if (!input) throw std::runtime_error("Missing replay");
        std::vector<uint8_t> data{std::istreambuf_iterator<char>(input), {}};
        player.load_replay_data(data.data(), data.size());
        std::vector<int> humans;
        for (int p = 0; p < 8; ++p) if (player.st().players[p].controller == 2) humans.push_back(p);
        if (humans.size() != 2 || player.st().players[humans[perspective]].race != bwgame::race_t::protoss)
            throw std::runtime_error("Requires qualified Protoss 1v1 perspective");
        if (validThrough > player.replay_st.end_frame) throw std::runtime_error("Prefix exceeds replay");
        std::vector<WholePerspective> perspectives(1);
        auto& p = perspectives.front();
        p.view.owner = humans[perspective]; p.view.enemy = humans[1-perspective]; p.view.perspective = perspective;
        std::ofstream observations(output / "observations.jsonl"), commands(output / "commands.jsonl"), checkpoints(output / "checkpoints.jsonl");
        if (!observations || !commands || !checkpoints) throw std::runtime_error("Cannot create outputs");
        std::ofstream terrain(output / "terrain.json");
        whole_game::writeTerrain(terrain, *player.opt_funcs);
        terrain.close(); if (!terrain) throw std::runtime_error("Terrain write failed");
        for (;;) {
            auto& f = *player.opt_funcs;
            const int frame = f.st.current_frame;
            if (frame % 240 == 0 || frame == validThrough) {
                auto* previous = std::cout.rdbuf(checkpoints.rdbuf()); snapshot(player); std::cout.rdbuf(previous);
            }
            if (f.st.players[p.view.owner].controller == 2) {
                p.view.update(f);
                if (frame % 24 == 0) {
                    whole_game::writeObservation(observations, f, p.view, p.sequence++, "cadence");
                    ++p.observations;
                }
            }
            if (frame == validThrough) break;
            executeWhole(player, perspectives, observations, commands);
            f.bwgame::state_functions::next_frame();
        }
        observations.close(); commands.close(); checkpoints.close();
        if (!observations || !commands || !checkpoints) throw std::runtime_error("Write failed");
        std::ofstream summary(output / "summary.json");
        summary << "{\"schema\":\"" << whole_game::schema << "\",\"complete\":true,\"training_ready\":false,"
            << "\"command_schema\":\"" << whole_game::commandSchema << "\",\"live_parity_verified\":false,\"width_tiles\":" << player.opt_funcs->game_st.map_tile_width
            << ",\"height_tiles\":" << player.opt_funcs->game_st.map_tile_height << ",\"valid_through_frame\":" << validThrough
            << ",\"technology_count\":" << player.st().tech_researched[p.view.owner].size()
            << ",\"upgrade_count\":" << player.st().upgrade_levels[p.view.owner].size()
            << ",\"observations\":" << p.observations << ",\"commands\":" << p.commands << "}\n";
        summary.close(); if (!summary) throw std::runtime_error("Summary write failed");
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
