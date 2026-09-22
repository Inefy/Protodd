#pragma once

#include "protodd/MacroEnemyMemory.hpp"
#include "protodd/ObservationEncoder.hpp"
#include "protodd/Runtime.hpp"

namespace protodd {

// Legal memory follows every frame. Feature history keeps the extractor's
// 24-frame cadence even when shadow inference is deferred or shed under load.
class MacroObservationStream {
public:
    static constexpr Frame sampleInterval = 24;
    static constexpr Frame inferenceOffset = 7;

    void reset() {
        memory_.reset(); encoder_.reset(); features_.clear(); mask_ = 0;
        lastFrame_ = sampleFrame_ = consumedFrame_ = -1;
    }

    template<class VisibleTile>
    void observe(const GameState& state, std::span<const UnitId> removals,
                 VisibleTile visibleTile) {
        if (state.frame < lastFrame_) reset();
        lastFrame_ = state.frame;
        memory_.update(state.frame, state.enemy.units, removals, visibleTile);
        if (state.frame < 0 || state.frame % sampleInterval != 0 || state.frame == sampleFrame_) return;
        auto legalState = state;
        legalState.enemy.units = memory_.snapshot();
        features_ = encoder_.encode(legalState);
        mask_ = learnedIntentMask(legalState);
        sampleFrame_ = state.frame;
    }

    [[nodiscard]] bool inferenceDue(Frame frame) const noexcept {
        return sampleFrame_ >= 0 && frame == sampleFrame_ + inferenceOffset &&
               consumedFrame_ != sampleFrame_;
    }

    // A skipped sample is consumed rather than replayed later with stale inputs.
    [[nodiscard]] bool beginInference(Frame frame, const FrameBudget& budget) noexcept {
        if (!inferenceDue(frame)) return false;
        consumedFrame_ = sampleFrame_;
        return budget.load(frame) == RuntimeLoad::normal;
    }

    [[nodiscard]] Frame sampleFrame() const noexcept { return sampleFrame_; }
    [[nodiscard]] std::span<const float> features() const noexcept { return features_; }
    [[nodiscard]] LearnedActionMask mask() const noexcept { return mask_; }
    [[nodiscard]] std::vector<UnitSnapshot> rememberedEnemies() const { return memory_.snapshot(); }

private:
    MacroEnemyMemory memory_;
    ObservationEncoder encoder_;
    std::vector<float> features_;
    LearnedActionMask mask_{};
    Frame lastFrame_{-1}, sampleFrame_{-1}, consumedFrame_{-1};
};

}  // namespace protodd
