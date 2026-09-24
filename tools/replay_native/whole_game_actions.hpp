#pragma once
#include "whole_game.hpp"
#include <string>

namespace whole_game {
inline constexpr auto commandSchema = "protodd-command-v1";

struct Intent {
    std::string kind{"interface"}, domain{"interface"};
    int order{-1}, unitType{-1}, technology{-1}, upgrade{-1}, queueSlot{-1};
    int target{-1}, x{-1}, y{-1};
    bool targetRequested{}, targetAvailable{}, position{}, buildTile{}, queued{}, decoded{};
    size_t consumed{};
    const bwgame::unit_t* nativeTarget{}; // adapter-only; never serialized
};

inline void resolveTarget(Intent& intent, bwgame::state_functions& f, const View& view, unsigned raw) {
    intent.targetRequested = raw != 0;
    if (!intent.targetRequested) return;
    const auto* unit = f.get_unit(bwgame::unit_id(raw));
    const auto found = view.entities.find(view.known(unit));
    // Do not allocate an ID or expose current state simply because a replay
    // packet names it. Unit-target labels require a current legal observation.
    if (found != view.entities.end() && found->second.visible && view.observable(f, unit)) {
        intent.target = found->first;
        intent.targetAvailable = true;
        intent.nativeTarget = unit;
    }
}

inline Intent decodeIntent(bwgame::state_functions& f, const View& view,
                           const uint8_t* start, const uint8_t* end) {
    bwgame::data_loading::data_reader_le r(start, end);
    const int code = r.get<uint8_t>();
    Intent intent;
    const auto kind = [&](const char* name, const char* domain) { intent.kind = name; intent.domain = domain; };
    const auto queued = [&] { intent.queued = r.get<uint8_t>() != 0; };
    switch (code) {
    case 20: case 21: case 96: case 97: {
        intent.position = true;
        intent.x = r.get<int16_t>(); intent.y = r.get<int16_t>();
        resolveTarget(intent, f, view, r.get<uint16_t>());
        if (code == 96 || code == 97) r.get<uint16_t>(); // Remastered padding, not an identity.
        r.get<uint16_t>(); // Fog-building type hint is diagnostic only; do not leak it into inputs.
        if (code == 21 || code == 97) {
            intent.order = r.get<uint8_t>();
            kind("order", "unit_control");
            const auto* order = f.get_order_type(static_cast<bwgame::Orders>(intent.order));
            if (order->tech_type != bwgame::TechTypes::None) {
                kind("cast", "ability"); intent.technology = int(order->tech_type);
            } else {
                using enum bwgame::Orders;
                switch (order->id) {
                case Move: kind("move", "unit_control"); break;
                case AttackDefault: case AttackUnit: kind("attack", "unit_control"); break;
                case AttackMove: kind("attack_move", "unit_control"); break;
                case Patrol: kind("patrol", "unit_control"); break;
                case Follow: kind("follow", "unit_control"); break;
                case Repair: kind("repair", "economy"); break;
                case Harvest1: kind("gather", "economy"); break;
                case RallyPointUnit: case RallyPointTile: kind("rally", "production"); break;
                case EnterTransport: case PickupTransport: case PickupBunker: kind("load", "transport"); break;
                case Unload: case MoveUnload: kind("unload_position", "transport"); break;
                default: break;
                }
            }
        } else kind("right_click", "unit_control");
        queued(); break;
    }
    case 12:
        kind("build", "production"); intent.order = r.get<uint8_t>();
        intent.position = intent.buildTile = true;
        intent.x = r.get<uint16_t>(); intent.y = r.get<uint16_t>();
        intent.unitType = r.get<uint16_t>(); break;
    case 31: case 35: case 53:
        kind(code == 31 ? "train" : "morph", "production"); intent.unitType = r.get<uint16_t>(); break;
    case 48: kind("research", "production"); intent.technology = r.get<uint8_t>(); break;
    case 50: kind("upgrade", "production"); intent.upgrade = r.get<uint8_t>(); break;
    case 32: kind("cancel_queue", "production"); intent.queueSlot = r.get<uint16_t>(); break;
    case 24: kind("cancel_build", "production"); break;
    case 25: kind("cancel_morph", "production"); break;
    case 49: kind("cancel_research", "production"); break;
    case 51: kind("cancel_upgrade", "production"); break;
    case 52: kind("cancel_addon", "production"); break;
    case 26: kind("stop", "unit_control"); queued(); break;
    case 27: case 28: kind("stop", "unit_control"); break;
    case 30: kind("return_cargo", "economy"); queued(); break;
    case 33: kind("cloak", "ability"); r.get<uint8_t>(); break;
    case 34: kind("decloak", "ability"); r.get<uint8_t>(); break;
    case 37: kind("unsiege", "ability"); queued(); break;
    case 38: kind("siege", "ability"); queued(); break;
    case 39: kind("train_fighter", "production"); break;
    case 40: kind("unload_all", "transport"); queued(); break;
    case 41: case 98:
        kind("unload_unit", "transport"); resolveTarget(intent, f, view, r.get<uint16_t>());
        if (code == 98) r.get<uint16_t>(); break;
    case 42: kind("merge_archon", "ability"); break;
    case 90: kind("merge_dark_archon", "ability"); break;
    case 43: kind("hold", "unit_control"); queued(); break;
    case 44: kind("burrow", "ability"); queued(); break;
    case 45: kind("unburrow", "ability"); r.get<uint8_t>(); break;
    case 46: kind("cancel_nuke", "ability"); break;
    case 47:
        kind("liftoff", "production"); intent.position = true;
        intent.x = r.get<int16_t>(); intent.y = r.get<int16_t>(); break;
    case 54: kind("stim", "ability"); break;
    default:
        // Selection, control groups, chat, leave, alliance and cheats are
        // preserved in diagnostics but are not unit-control imitation labels.
        return intent;
    }
    intent.decoded = true; intent.consumed = r.ptr - start;
    return intent;
}

struct CommandState {
    int order{-1};
    std::vector<std::int64_t> signature;
    bool operator==(const CommandState&) const = default;
};

inline CommandState commandState(bwgame::state_functions& f, const bwgame::unit_t* u) {
    CommandState state;
    if (!u || f.unit_dead(u)) return state;
    const auto key = [](const bwgame::unit_t* target) { return target ? std::int64_t(View::key(target)) : -1; };
    state.order = u->order_type ? int(u->order_type->id) : -1;
    // Own command state only, used for evidence, never copied into a pre-action
    // policy input. Include queued orders: issuing a shift-order need not change
    // the currently executing order.
    state.signature = {int(u->unit_type->id), state.order,
        u->secondary_order_type ? int(u->secondary_order_type->id) : -1,
        u->order_target.pos.x, u->order_target.pos.y, key(u->order_target.unit),
        u->energy.raw_value, u->hp.raw_value,
        int(f.u_loaded(u)), int(f.u_burrowed(u)), int(f.u_cloaked(u))};
    state.signature.push_back(u->build_queue.size());
    for (const auto* type : u->build_queue) state.signature.push_back(int(type->id));
    state.signature.push_back(u->order_queue_count);
    for (const auto& order : u->order_queue) {
        state.signature.insert(state.signature.end(), {int(order.order_type->id), order.target.position.x,
            order.target.position.y, key(order.target.unit), order.target.unit_type ? int(order.target.unit_type->id) : -1});
    }
    if (f.ut_building(u)) {
        state.signature.push_back(u->building.researching_type ? int(u->building.researching_type->id) : -1);
        state.signature.push_back(u->building.upgrading_type ? int(u->building.upgrading_type->id) : -1);
    }
    if (f.unit_is_factory(u)) {
        state.signature.insert(state.signature.end(), {u->building.rally.pos.x, u->building.rally.pos.y, key(u->building.rally.unit)});
    }
    for (const auto* cargo : f.loaded_units(u)) state.signature.push_back(key(cargo));
    return state;
}

struct ActorBefore { int id{}; size_t index{}; std::uint64_t key{}; CommandState state; };
struct ActorEffect { int id{}, beforeOrder{}, afterOrder{}; bool changed{}, removed{}; };

inline std::vector<ActorBefore> captureActors(bwgame::replay_player& player, const View& view, const Intent& intent) {
    std::vector<ActorBefore> actors;
    auto& f = *player.opt_funcs;
    const auto add = [&](const bwgame::unit_t* u) {
        const int id = view.known(u);
        if (u && u->owner == view.owner && id >= 0 && view.entities.contains(id))
            actors.push_back({id, u->index, View::key(u), commandState(f, u)});
    };
    if (intent.kind == "unload_unit") {
        // Clicking one cargo icon is addressed to its transport independently
        // of the player's current selection.
        if (intent.nativeTarget && intent.nativeTarget->owner == view.owner && f.u_loaded(intent.nativeTarget))
            add(intent.nativeTarget->connected_unit);
    } else {
        for (const auto* u : player.action_st.selection[view.owner]) add(u);
    }
    return actors;
}

inline std::vector<ActorEffect> effects(bwgame::state_functions& f, const std::vector<ActorBefore>& before) {
    std::vector<ActorEffect> result;
    for (const auto& actor : before) {
        const auto* u = f.st.units_container.try_get(actor.index);
        const bool removed = !u || View::key(u) != actor.key || f.unit_dead(u);
        const auto after = removed ? CommandState{} : commandState(f, u);
        result.push_back({actor.id, actor.state.order, after.order, removed || after != actor.state, removed});
    }
    return result;
}

inline const char* acceptance(const Intent& intent, bool returned, const std::vector<ActorEffect>& actors) {
    if (!intent.decoded) return "not_applicable";
    const bool changed = std::any_of(actors.begin(), actors.end(), [](const auto& a) { return a.changed; });
    // OpenBW's unload-all wrapper returns false even after issuing orders.
    // The per-actor state transition is stronger evidence than that aggregate
    // boolean; retain both for audits instead of discarding transport examples.
    if (changed) return "confirmed_transition";
    return returned ? "unconfirmed_no_change" : "engine_rejected";
}

inline void writeIntent(std::ostream& out, const Intent& intent, const std::vector<ActorEffect>& actors) {
    out << "{\"kind\":\"" << intent.kind << "\",\"domain\":\"" << intent.domain
        << "\",\"decoded\":" << (intent.decoded ? "true" : "false")
        << ",\"order\":" << intent.order << ",\"unit_type\":" << intent.unitType
        << ",\"technology\":" << intent.technology << ",\"upgrade\":" << intent.upgrade
        << ",\"queue_slot\":" << intent.queueSlot << ",\"queued\":" << (intent.queued ? "true" : "false")
        << ",\"target\":" << intent.target << ",\"target_requested\":" << (intent.targetRequested ? "true" : "false")
        << ",\"target_available\":" << (intent.targetAvailable ? "true" : "false")
        << ",\"position\":";
    if (intent.position) out << '[' << intent.x << ',' << intent.y << ']'; else out << "null";
    out << ",\"coordinate_space\":\"" << (intent.buildTile ? "build_tile" : "pixel") << "\",\"actor_effects\":[";
    for (size_t i = 0; i < actors.size(); ++i) {
        if (i) out << ',';
        const auto& actor = actors[i];
        out << "{\"id\":" << actor.id << ",\"before_order\":" << actor.beforeOrder
            << ",\"after_order\":" << actor.afterOrder << ",\"changed\":" << (actor.changed ? "true" : "false")
            << ",\"removed\":" << (actor.removed ? "true" : "false") << '}';
    }
    out << "]}";
}
} // namespace whole_game
