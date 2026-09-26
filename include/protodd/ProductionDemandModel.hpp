#pragma once
#include "protodd/ObservationEncoder.hpp"
#include <array>
#include <istream>
#include <optional>

namespace protodd {

inline constexpr std::array<std::string_view, 8> productionDemandActions{
    "train_probe", "build_pylon", "build_gateway", "build_assimilator",
    "build_cybernetics_core", "train_zealot", "train_dragoon", "expand_nexus"};

struct ProductionDemandPrediction {
    std::array<float, 32> ordinalLogits{};
    std::array<int, 8> quantities{};
};

// Offline/native inference component only. It has no BWAPI execution interface.
// Inputs: macro-v2 features, eight prior-240-frame counts / 4, eight ages / 2400.
// History includes accepted OWN events strictly before the observation frame.
class ProductionDemandModel {
public:
    bool load(std::istream& input, std::string& error);
    [[nodiscard]] std::optional<ProductionDemandPrediction> predict(std::span<const float> features) const noexcept;
    [[nodiscard]] std::size_t inputCount() const noexcept { return inputs_; }
private:
    std::size_t inputs_{}, width_{};
    std::vector<float> parameters_;
};
} // namespace protodd
