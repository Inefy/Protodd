#pragma once

#include "astra/GameState.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace astra {

enum class RuntimeLoad : std::uint8_t { normal, reduced, emergency };

struct RuntimeStats {
    std::uint64_t samples{};
    std::uint64_t over42ms{};
    std::uint64_t over55ms{};
    std::uint64_t overOneSecond{};
    std::uint64_t overTenSeconds{};
    double movingAverageMs{};
    double peakMs{};
};

class FrameBudget {
public:
    void reset() noexcept;
    void record(Frame frame, std::int64_t elapsedMicroseconds) noexcept;

    [[nodiscard]] RuntimeLoad load(Frame frame) const noexcept;
    [[nodiscard]] bool allowSimulation(Frame frame) const noexcept;
    [[nodiscard]] int expensiveCadenceMultiplier(Frame frame) const noexcept;
    [[nodiscard]] int navigationInterval(Frame frame) const noexcept;
    [[nodiscard]] std::size_t combatCommandLimit(Frame frame) const noexcept;
    [[nodiscard]] const RuntimeStats& stats() const noexcept;

private:
    RuntimeStats stats_{};
    Frame reducedUntil_{};
    Frame emergencyUntil_{};
};

[[nodiscard]] std::string_view runtimeLoadName(RuntimeLoad load) noexcept;

}  // namespace astra
