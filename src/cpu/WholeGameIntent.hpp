#pragma once

#include "WholeGameCpu.hpp"
#include "WholeGameEncoder.hpp"
#include "protodd/WholeGameObservation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace protodd::cpu {

// Index order is part of the exported model's action schema. Keep this in
// exact sync with training.whole_game_model and test it before using weights.
inline constexpr auto kindNames = std::to_array<std::string_view>({
    "right_click", "move", "attack", "attack_move", "patrol", "follow", "repair",
    "gather", "rally", "load", "unload_position", "cast", "build", "train", "morph",
    "research", "upgrade", "cancel_queue", "cancel_build", "cancel_morph",
    "cancel_research", "cancel_upgrade", "cancel_addon", "stop", "return_cargo",
    "cloak", "decloak", "siege", "unsiege", "train_fighter", "unload_all",
    "unload_unit", "merge_archon", "merge_dark_archon", "hold", "burrow",
    "unburrow", "cancel_nuke", "liftoff", "stim", "order"
});
inline constexpr auto domainNames = std::to_array<std::string_view>({
    "unit_control", "production", "economy", "ability", "transport"
});
inline constexpr auto targetModeNames = std::to_array<std::string_view>({
    "none", "entity", "position"
});
// Bit 0: none, bit 1: visible entity, bit 2: map position. Zero means the
// Protoss BWAPI adapter cannot execute that replay-only kind.
inline constexpr auto kindTargetModeMask = std::to_array<std::uint8_t>({
    6, 4, 6, 4, 4, 2, 0, 2, 6, 2, 4, 7, 4, 1, 0, 1, 1, 1, 1, 0, 1,
    1, 0, 1, 1, 0, 0, 0, 0, 1, 5, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0
});
static_assert(kindNames.size() == 41 && domainNames.size() == 5 && targetModeNames.size() == 3);
static_assert(kindTargetModeMask.size() == kindNames.size());

struct Intent {
    std::size_t kind{}, domain{}, targetMode{};
    float eventProbability{}, kindProbability{};
    std::vector<int> actorIds;
    std::optional<int> targetEntityId;
    std::optional<std::pair<int, int>> targetPixel;
    int unitType{}, technology{}, upgrade{}, queueSlot{};
    bool queued{};
};

struct IntentThresholds {
    float eventProbability{0.5f};
    float actorLogit{0.0f};
    std::size_t maxActors{12};
};

[[nodiscard]] std::optional<Intent> decodeIntent(
    const Output& prediction, const EncodedObservation& encoded,
    const whole_observation::Snapshot& source,
    const IntentThresholds& thresholds = {});

}  // namespace protodd::cpu
