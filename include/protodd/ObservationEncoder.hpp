#pragma once

#include "protodd/GameState.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace protodd {

inline constexpr std::string_view macroSchemaVersion = "protodd-macro-v2";

struct ModelFeature {
    std::string name;
    float scale{1.0F};
};

enum class LearnedIntentKind { wait, build, train, expand, research, upgrade };
struct LearnedIntent {
    std::string name;
    LearnedIntentKind kind{LearnedIntentKind::wait};
    UnitKind unit{UnitKind::unknown};
    TechnologyKind technology{TechnologyKind::none};
};

using LearnedActionMask = std::uint64_t;
[[nodiscard]] const std::vector<ModelFeature>& modelFeatures();
[[nodiscard]] const std::vector<LearnedIntent>& learnedIntents();
[[nodiscard]] std::uint64_t macroSchemaFingerprint();
// Candidate INTENTS, not permission to issue a command: resources, occupancy,
// placement and final BWAPI legality must still be checked by the executor.
[[nodiscard]] LearnedActionMask learnedIntentMask(const GameState& state);

// Input contract: GameState from the legal adapter, including only observed
// enemy memory. Never pass an omniscient replay snapshot. No enemy private
// fields, coordinates, global IDs or future state enter this encoding.
// Call on 24-frame samples (and explicitly aligned replay decision events).
class ObservationEncoder {
public:
    void reset() noexcept;
    // Duplicate-frame reads are idempotent; a backwards frame resets history.
    // A non-Protoss or invalid frame returns an empty vector.
    [[nodiscard]] std::vector<float> encode(const GameState& state);
private:
    Frame previousFrame_{-1};
    int previousMinerals_{};
    int previousGas_{};
    std::vector<float> cached_;
};

}  // namespace protodd
