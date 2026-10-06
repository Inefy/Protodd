#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace protodd {

class PhaseFailurePolicy {
public:
    struct Update {
        std::uint32_t consecutiveFailures{};
        bool disabled{};
        bool newlyDisabled{};
    };

    explicit PhaseFailurePolicy(const std::uint32_t failureLimit = 3)
        : failureLimit_(failureLimit == 0 ? 1 : failureLimit) {}

    void reset() noexcept { states_.clear(); }

    [[nodiscard]] bool disabled(const std::string_view phase) const noexcept {
        const auto found = states_.find(phase);
        return found != states_.end() && found->second.disabled;
    }

    [[nodiscard]] Update recordFailure(const std::string_view phase) {
        auto [found, inserted] = states_.try_emplace(std::string(phase));
        (void)inserted;
        auto& state = found->second;
        if (state.disabled) return {state.consecutiveFailures, true, false};
        ++state.consecutiveFailures;
        if (state.consecutiveFailures >= failureLimit_) {
            state.disabled = true;
            return {state.consecutiveFailures, true, true};
        }
        return {state.consecutiveFailures, false, false};
    }

    void recordSuccess(const std::string_view phase) noexcept {
        const auto found = states_.find(phase);
        if (found != states_.end() && !found->second.disabled)
            found->second.consecutiveFailures = 0;
    }

    [[nodiscard]] std::uint32_t consecutiveFailures(
        const std::string_view phase) const noexcept {
        const auto found = states_.find(phase);
        return found == states_.end() ? 0 : found->second.consecutiveFailures;
    }

private:
    struct State {
        std::uint32_t consecutiveFailures{};
        bool disabled{};
    };
    std::uint32_t failureLimit_;
    std::map<std::string, State, std::less<>> states_;
};

}  // namespace protodd
