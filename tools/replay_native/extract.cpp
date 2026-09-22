// Legal Protoss observations and accepted macro intents. Never grants command authority.
#include "replay.h"
#include "protodd/ObservationEncoder.hpp"
#include "protodd/MacroEnemyMemory.hpp"
#include "catalog.hpp"
#include "checkpoints.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>

using namespace protodd;
namespace {
UnitId identity(const bwgame::unit_t* u) {
    const auto id = uint64_t(u->unit_id_generation) * 3400 + u->index;
    if (id > INT32_MAX) throw std::runtime_error("Unit identity overflow");
    return static_cast<UnitId>(id);
}
Race race(bwgame::race_t value) {
    if (value == bwgame::race_t::protoss) return Race::protoss;
    if (value == bwgame::race_t::terran) return Race::terran;
    if (value == bwgame::race_t::zerg) return Race::zerg;
    return Race::unknown;
}
bool gatheringGas(bwgame::state_functions& f, bwgame::unit_t* u) {
    using enum bwgame::Orders;
    if (!f.ut_worker(u) || !f.u_gathering(u)) return false;
    auto order = u->order_type->id;
    if (order == ResetCollision) return (u->carrying_flags & 1) != 0;
    if (order == WaitForGas || order == HarvestGas || order == ReturnGas) return true;
    if (order != Harvest1 && order != Harvest2 && order != MoveToGas) return false;
    for (auto* target : {u->move_target.unit, u->order_target.unit}) {
        if (!target || f.unit_dead(target) || !f.u_completed(target) || target->owner != u->owner) continue;
        const auto kind = replayKind(target->unit_type->id);
        if (f.ut_resource_depot(target) || kind == UnitKind::assimilator || kind == UnitKind::refinery) return true;
    }
    return false;
}
UnitSnapshot unitSnapshot(bwgame::state_functions& f, bwgame::unit_t* u, bool ours) {
    UnitSnapshot out;
    out.id = identity(u); out.kind = replayKind(u->unit_type->id);
    out.race = race(f.unit_race(u)); out.ours = ours;
    out.completed = f.u_completed(u); out.visible = out.detected = true;
    out.position = {u->sprite->position.x, u->sprite->position.y};
    if (ours) { out.hallucination = f.u_hallucination(u); out.gatheringGas = gatheringGas(f, u); }
    return out;
}
struct Perspective {
    int owner{}, enemy{}, index{};
    MacroEnemyMemory memory;
    ObservationEncoder encoder;
    std::vector<float> features;
    LearnedActionMask mask{};
    int observationFrame{}, actionFrame{}, action{};
    bool active{}, invalid{};
    size_t samples{}, positives{}, masked{}, accepted{}, rejected{}, repeated{};
};

// Only this boundary can inspect spectator state. No hidden enemy unit fields
// are copied. Removal needs current vision/detection at the death event.
void updateMemory(bwgame::state_functions& f, Perspective& p) {
    std::vector<UnitSnapshot> observations;
    std::vector<UnitId> removals;
    for (size_t i = 0; i < f.st.units_container.max_size; ++i) {
        auto* u = f.st.units_container.try_get(i);
        if (!u || !u->sprite || u->owner != p.enemy || f.ut_turret(u) ||
            !(u->sprite->visibility_flags & (1 << p.owner)) || f.unit_is_undetected(u, p.owner) || f.us_hidden(u)) continue;
        if (f.unit_dying(u)) removals.push_back(identity(u));
        else observations.push_back(unitSnapshot(f, u, false));
    }
    p.memory.update(f.st.current_frame, observations, removals, [&](Position position) {
        return f.player_position_is_visible(p.owner, {position.x / 32 * 32, position.y / 32 * 32});
    });
}
GameState observe(bwgame::state_functions& f, Perspective& p) {
    GameState state;
    state.frame = f.st.current_frame;
    state.mapWidthPixels = static_cast<int>(f.game_st.map_width);
    state.mapHeightPixels = static_cast<int>(f.game_st.map_height);
    auto& own = state.self;
    own.id = p.owner; own.race = Race::protoss;
    own.minerals = f.st.current_minerals[p.owner]; own.gas = f.st.current_gas[p.owner];
    own.supplyUsed = f.st.supply_used[p.owner][2].raw_value;
    own.supplyTotal = std::min(400, f.st.supply_available[p.owner][2].raw_value);
    own.gatheredMinerals = f.st.total_minerals_gathered[p.owner];
    own.gatheredGas = f.st.total_gas_gathered[p.owner];
    for (auto& item : f.st.player_units[p.owner]) {
        auto* u = &item;
        if (f.unit_dead(u) || !u->sprite || f.ut_turret(u)) continue;
        own.units.push_back(unitSnapshot(f, u, true));
        if (f.ut_building(u)) {
            // The active first item is already represented by its incomplete
            // unit. Match BwapiBridge's own-production queue contract.
            bool first = true;
            for (auto* type : u->build_queue) {
                if (first) { first = false; continue; }
                const auto kind = replayKind(type->id);
                if (kind != UnitKind::unknown) own.queuedUnits.push_back(kind);
            }
        }
    }
    for (int k = 1; k < static_cast<int>(TechnologyKind::count); ++k) {
        auto kind = static_cast<TechnologyKind>(k);
        auto tech = replayTech(kind); auto upgrade = replayUpgrade(kind);
        if (static_cast<int>(tech) >= 0) own.technologies.push_back({kind,
            f.st.tech_researched[p.owner][tech] ? 1 : 0, f.st.tech_researching[p.owner][tech]});
        else if (static_cast<int>(upgrade) >= 0) own.technologies.push_back({kind,
            f.st.upgrade_levels[p.owner][upgrade], f.st.upgrade_upgrading[p.owner][upgrade]});
    }
    state.enemy.units = p.memory.snapshot();
    return state;
}
int macroAction(int code, const uint8_t* payload, size_t size) {
    UnitKind unit = UnitKind::unknown;
    TechnologyKind technology = TechnologyKind::none;
    if ((code == 31 && size >= 2) || (code == 12 && size >= 7)) {
        auto* data = payload + (code == 12 ? 5 : 0);
        unit = replayKind(static_cast<bwgame::UnitTypes>(data[0] | data[1] << 8));
    } else if ((code == 48 || code == 50) && size >= 1) {
        for (int i = 1; i < static_cast<int>(TechnologyKind::count); ++i) {
            auto kind = static_cast<TechnologyKind>(i);
            if ((code == 48 && static_cast<int>(replayTech(kind)) == payload[0]) ||
                (code == 50 && static_cast<int>(replayUpgrade(kind)) == payload[0])) technology = kind;
        }
    }
    for (size_t i = 1; i < learnedIntents().size(); ++i) {
        const auto& intent = learnedIntents()[i];
        if (unit != UnitKind::unknown && intent.unit == unit) return static_cast<int>(i);
        if (technology != TechnologyKind::none && intent.technology == technology) return static_cast<int>(i);
    }
    return -1;
}
struct BuildSignature {
    int order{-1}, x{}, y{};
    std::vector<int> queue;
    bool operator==(const BuildSignature&) const = default;
};
BuildSignature signature(bwgame::unit_t* u) {
    BuildSignature s;
    if (!u) return s;
    s.order = u->order_type ? static_cast<int>(u->order_type->id) : -1;
    s.x = u->order_target.pos.x; s.y = u->order_target.pos.y;
    for (auto* type : u->build_queue) s.queue.push_back(static_cast<int>(type->id));
    return s;
}
// Same framing/execution order as pinned action_functions::execute_actions.
// Intercepts the actual return AND producer transition, not parser click counts.
void execute(bwgame::replay_player& player, std::vector<Perspective>& perspectives, std::ostream& audit) {
    auto& a = player.action_st;
    auto& st = player.st();
    auto& data = player.replay_st.actions_data_buffer;
    if (st.current_frame != a.next_action_frame) return;
    // Action execution and simulation must share the persistent function object:
    // its unit-finder search generation is part of deterministic playback.
    auto& f = *player.opt_funcs;
    while (a.actions_data_position != data.size()) {
        bwgame::data_loading::data_reader_le r(data.data() + a.actions_data_position, data.data() + data.size());
        const int frame = r.get<int32_t>();
        if (frame != st.current_frame) { a.next_action_frame = frame; return; }
        const auto count = r.get<uint8_t>();
        const auto* ptr = r.get_n(count); const auto* end = ptr + count;
        bwgame::data_loading::data_reader_le commands(ptr, end);
        while (commands.ptr != end) {
            int id = commands.get<uint8_t>();
            if (id == 128) id = a.player_id.at(0);
            auto found = std::find(a.player_id.begin(), a.player_id.end(), id);
            if (found == a.player_id.end() || commands.ptr == end) throw std::runtime_error("Invalid command owner/body");
            const int owner = static_cast<int>(found - a.player_id.begin());
            const int code = *commands.ptr;
            const int action = macroAction(code, commands.ptr + 1, end - commands.ptr - 1);
            auto* producer = action >= 0 ? f.get_single_selected_unit(owner) : nullptr;
            const auto producerId = producer ? identity(producer) : -1;
            const auto before = signature(producer);
            const bool returned = f.read_action(owner, commands);
            if (action < 0) continue;
            bool accepted = returned;
            bool repeat = false;
            if (code == 12) {
                const auto after = signature(producer);
                // OpenBW's build wrapper returns true even when placement or
                // affordability rejects it. A changed matching order/queue is required.
                accepted = returned && producer && !after.queue.empty() &&
                    replayKind(static_cast<bwgame::UnitTypes>(after.queue.front())) == learnedIntents()[action].unit;
                repeat = accepted && before == after;
                accepted = accepted && !repeat;
            }
            for (auto& p : perspectives) if (p.owner == owner) {
                if (accepted) ++p.accepted; else if (repeat) ++p.repeated; else ++p.rejected;
                if (accepted && p.active && p.action == 0) {
                    p.action = action; p.actionFrame = frame;
                    if (!(p.mask & (LearnedActionMask{1} << action))) p.invalid = true;
                }
            }
            audit << "{\"frame\":" << frame << ",\"owner\":" << owner << ",\"code\":" << code
                  << ",\"action\":\"" << learnedIntents()[action].name << "\",\"producer\":" << producerId
                  << ",\"accepted\":" << (accepted ? "true" : "false")
                  << ",\"repeated\":" << (repeat ? "true" : "false") << "}\n";
        }
        a.actions_data_position = end - data.data();
    }
}
void emit(Perspective& p, const std::string& game, std::ostream& output) {
    if (!p.active) return;
    if (p.invalid) { ++p.masked; return; }
    output << "{\"game_id\":\"" << game << "\",\"perspective\":" << p.index
           << ",\"frame\":" << p.observationFrame << ",\"action_frame\":" << p.actionFrame
           << ",\"action\":\"" << learnedIntents()[p.action].name << "\",\"confidence\":1,\"features\":[";
    for (size_t i = 0; i < p.features.size(); ++i) { if (i) output << ','; output << p.features[i]; }
    output << "],\"allowed_actions\":[";
    bool first = true;
    for (size_t i = 0; i < learnedIntents().size(); ++i) if (p.mask & (LearnedActionMask{1} << i)) {
        if (!first) output << ','; first = false;
        output << '"' << learnedIntents()[i].name << '"';
    }
    output << "]}\n";
    ++p.samples; if (p.action) ++p.positives;
}
}

int main(int argc, char** argv) {
    if (argc != 8) { std::cerr << "Usage: replay_extract MPQ_DIR ASSET_DIR RAW GAME_ID SAMPLES_JSONL ACTIONS_JSONL CHECKPOINTS_JSONL\n"; return 2; }
    try {
        const std::string game = argv[4];
        if (game.empty() || game.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.:-") != std::string::npos)
            throw std::runtime_error("Invalid game ID");
        if (std::filesystem::exists(argv[5]) || std::filesystem::exists(argv[6]) || std::filesystem::exists(argv[7])) throw std::runtime_error("Output exists");
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
        if (!input) throw std::runtime_error("Missing decoded replay");
        std::vector<uint8_t> data{std::istreambuf_iterator<char>(input), {}};
        player.load_replay_data(data.data(), data.size());
        std::vector<int> humans;
        for (int p = 0; p < 8; ++p) if (player.st().players[p].controller == 2) humans.push_back(p);
        if (humans.size() != 2) throw std::runtime_error("Only 1v1 human replays supported");
        std::vector<Perspective> perspectives;
        for (int i = 0; i < 2; ++i) if (player.st().players[humans[i]].race == bwgame::race_t::protoss) {
            Perspective p; p.owner = humans[i]; p.enemy = humans[1-i]; p.index = i;
            perspectives.push_back(std::move(p));
        }
        if (perspectives.empty()) throw std::runtime_error("No Protoss perspective");
        std::ofstream output(argv[5]), audit(argv[6]), checkpoints(argv[7]);
        if (!output || !audit || !checkpoints) throw std::runtime_error("Cannot create extraction output");
        output << std::setprecision(9);
        const int end = player.replay_st.end_frame;
        if (end < 1 || end > 2000000) throw std::runtime_error("Invalid replay duration");
        for (;;) {
            auto& f = *player.opt_funcs; const int frame = player.st().current_frame;
            if (frame % 240 == 0 || frame == end) {
                auto* previous = std::cout.rdbuf(checkpoints.rdbuf());
                snapshot(player);
                std::cout.rdbuf(previous);
            }
            for (auto& p : perspectives) {
                const bool active = player.st().players[p.owner].controller == 2;
                if (frame % 24 == 0) {
                    if (frame > 0 && active) emit(p, game, output);
                    p.active = active && frame + 24 <= end;
                    p.action = 0; p.invalid = false; p.actionFrame = p.observationFrame = frame;
                }
                if (active) updateMemory(f, p);
                if (p.active && frame % 24 == 0) {
                    const auto observation = observe(f, p);
                    p.features = p.encoder.encode(observation); p.mask = learnedIntentMask(observation);
                }
            }
            if (frame == end) break;
            execute(player, perspectives, audit);
            player.opt_funcs->bwgame::state_functions::next_frame();
        }
        output.close(); audit.close(); checkpoints.close();
        if (!output || !audit || !checkpoints) throw std::runtime_error("Extraction output write failed");
        std::cout << "{\"schema\":\"" << macroSchemaVersion << "\",\"fingerprint\":\"" << std::hex << macroSchemaFingerprint()
                  << std::dec << "\",\"end_frame\":" << end << ",\"perspectives\":[";
        bool first = true;
        for (const auto& p : perspectives) {
            if (!first) std::cout << ','; first = false;
            std::cout << "{\"slot\":" << p.owner << ",\"perspective\":" << p.index << ",\"samples\":" << p.samples
                      << ",\"positive_samples\":" << p.positives << ",\"masked_windows\":" << p.masked
                      << ",\"accepted_commands\":" << p.accepted << ",\"rejected_commands\":" << p.rejected
                      << ",\"repeated_build_commands\":" << p.repeated << '}';
        }
        std::cout << "]}\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
