#pragma once
#include "replay.h"
#include <iostream>
namespace {
void snapshot(bwgame::replay_player& player) {
    auto& st = player.st();
    auto& f = player.funcs();
    std::cout << "{\"frame\":" << st.current_frame << ",\"players\":[";
    for (int p = 0; p < 12; ++p) {
        if (p) std::cout << ',';
        std::cout << "{\"owner\":" << p << ",\"minerals\":" << st.current_minerals[p]
                  << ",\"gas\":" << st.current_gas[p] << ",\"usedSupplyRaw\":[";
        for (int race = 0; race < 3; ++race) {
            if (race) std::cout << ',';
            std::cout << st.supply_used[p][race].raw_value;
        }
        std::cout << "],\"maxSupplyRaw\":[";
        for (int race = 0; race < 3; ++race) {
            if (race) std::cout << ',';
            std::cout << st.supply_available[p][race].raw_value;
        }
        std::cout << "]}";
    }
    std::cout << "],\"units\":[";
    bool first = true;
    for (size_t index = 0; index < st.units_container.max_size; ++index) {
        auto* u = f.get_unit(index);
        if (!u || !u->sprite) continue;
        // Match the documented bwsim HUD membership. Turret subunits and
        // Scanner Sweep are absent from that diagnostic interface, so this
        // comparison cannot validate their internal state.
        if (f.ut_turret(u) || int(u->unit_type->id) == 33) continue;
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"index\":" << index << ",\"type\":" << int(u->unit_type->id)
                  << ",\"owner\":" << u->owner << ",\"completed\":" << (f.u_completed(u) ? "true" : "false")
                  << ",\"hidden\":" << (f.us_hidden(u) ? "true" : "false")
                  << ",\"x\":" << u->sprite->position.x << ",\"y\":" << u->sprite->position.y
                  << ",\"hpRaw\":" << u->hp.raw_value << ",\"shieldsRaw\":" << u->shield_points.raw_value
                  << ",\"energyRaw\":" << u->energy.raw_value
                  << ",\"remainingBuildTime\":" << u->remaining_build_time << ",\"queue\":[";
        bool firstQueued = true;
        for (auto* type : u->build_queue) {
            if (!firstQueued) std::cout << ',';
            firstQueued = false;
            std::cout << int(type->id);
        }
        std::cout << "]}";
    }
    std::cout << "]}\n";
}
}

