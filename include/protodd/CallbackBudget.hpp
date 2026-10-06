#pragma once

#include <chrono>
#include <cstdint>

namespace protodd {

// The tournament callback guidance is 42 ms. Optional work must leave a
// bounded reserve for economy and immediate safety decisions.
class CallbackBudget {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::int64_t callbackLimitUs = 42'000;
    static constexpr std::int64_t emergencyReserveUs = 10'000;
    static constexpr std::int64_t optionalWorkLimitUs =
        callbackLimitUs - emergencyReserveUs;

    [[nodiscard]] constexpr bool allowsOptionalWork(
        const std::int64_t elapsedUs,
        const std::int64_t estimatedCostUs) const noexcept {
        if (elapsedUs < 0 || estimatedCostUs < 0 || elapsedUs > optionalWorkLimitUs)
            return false;
        return estimatedCostUs <= optionalWorkLimitUs - elapsedUs;
    }
};

}  // namespace protodd
