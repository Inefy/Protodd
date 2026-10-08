#pragma once

namespace protodd {

// A producer's waiting queue is not guaranteed to expose its active item.
// Accept either a newly visible queue entry or a transition into the matching
// active type, while excluding a same-type job that was already in progress.
[[nodiscard]] constexpr bool trainingEffectObserved(
    const int queueBefore,
    const int queueNow,
    const bool trainingBefore,
    const bool trainingNow,
    const bool activeTypeMatches) noexcept {
    return queueNow > queueBefore ||
        (!trainingBefore && trainingNow && activeTypeMatches);
}

}  // namespace protodd
