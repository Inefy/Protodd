// Spectator diagnostic output only. These are NOT legal policy observations.
#include "replay.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

#include "checkpoints.hpp"

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "Usage: replay_checkpoints MPQ_DIRECTORY MODERN_ASSETS_DIRECTORY DECODED_REPLAY INTERVAL\n";
        return 2;
    }
    try {
        const int interval = std::stoi(argv[4]);
        if (interval < 1 || interval > 2400) throw std::runtime_error("Invalid checkpoint interval");
        auto mpqs = bwgame::data_loading::data_files_directory(argv[1]);
        bwgame::replay_player player;
        player.init([&](bwgame::a_vector<uint8_t>& dst, bwgame::a_string name) {
            std::string filename(name.c_str());
            std::replace(filename.begin(), filename.end(), '\\', '/');
            for (auto& ch : filename) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
            // All six supported ladder tilesets must use the pinned Remastered
            // CV5/VF4 pair. Never silently fall back to classic terrain.
            if (filename.rfind("tileset/", 0) == 0 &&
                filename.find("ashworld.") == std::string::npos &&
                filename.find("install.") == std::string::npos) {
                const auto path = std::filesystem::path(argv[2]) / filename;
                std::ifstream input(path, std::ios::binary);
                if (!input) throw std::runtime_error("Missing Remastered terrain asset: " + path.string());
                dst.assign(std::istreambuf_iterator<char>(input), {});
                const size_t recordSize = path.extension() == ".cv5" ? 52 : 32;
                if (dst.empty() || dst.size() % recordSize) throw std::runtime_error("Invalid terrain asset: " + path.string());
            } else {
                mpqs(dst, name);
            }
        });
        std::ifstream input(argv[3], std::ios::binary);
        if (!input) throw std::runtime_error("Cannot read decoded replay");
        std::vector<uint8_t> data{std::istreambuf_iterator<char>(input), {}};
        player.load_replay_data(data.data(), data.size());
        const int end = player.replay_st.end_frame;
        if (end < 1 || end > 2000000) throw std::runtime_error("Invalid replay end frame");
        for (;;) {
            const int frame = player.st().current_frame;
            if (frame % interval == 0 || frame == end) snapshot(player);
            if (frame == end) break;
            if (player.is_done()) throw std::runtime_error("Playback stopped before declared end frame");
            player.next_frame();
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
