#pragma once

#include "protodd/ObservationEncoder.hpp"

#include <array>
#include <istream>
#include <optional>

namespace protodd {

struct LearnedPrediction {
    std::size_t action{};
    float probability{};
    std::array<float, 64> logits{};
};

// Bounded two-hidden-layer ReLU MLP, portable to the tournament Win32 DLL.
// The loader checks the full schema and all dimensions/weights before mutation.
class LearnedPolicy {
public:
    static constexpr std::size_t maximumWidth = 2048;
    static constexpr std::size_t maximumParameters = 8'000'000;
    static constexpr std::size_t maximumFileBytes = maximumParameters * 4 + 128;
    bool load(std::istream& input, std::string& error);
    void clear() noexcept;
    [[nodiscard]] bool loaded() const noexcept { return !weights_.empty(); }
    [[nodiscard]] std::size_t parameterCount() const noexcept { return weights_.size(); }
    // No allocation, I/O, or training during inference. Invalid input => nullopt.
    [[nodiscard]] std::optional<LearnedPrediction> predict(
        std::span<const float> features, LearnedActionMask allowed) const noexcept;
private:
    std::size_t inputs_{}, hidden1_{}, hidden2_{}, outputs_{};
    std::vector<float> weights_;
};

}  // namespace protodd
