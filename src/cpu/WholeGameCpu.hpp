#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace protodd::cpu {

struct Tensor {
    std::vector<std::uint32_t> shape;
    std::vector<float> values;
};

struct Observation {
    std::uint32_t height{}, width{};
    std::vector<std::int32_t> type, relation, order;
    std::vector<float> entityNumeric;  // N x 16
    std::vector<float> spatial;        // 3 x H x W
    std::vector<float> global;         // 216
};

struct Output {
    std::vector<float> memory;
    std::unordered_map<std::string, std::vector<float>> heads;
};

struct SlotOutput {
    std::vector<float> memory;
    std::vector<Output> slots;
};

class WholeGameCpu {
public:
    explicit WholeGameCpu(const std::filesystem::path& weights);
    explicit WholeGameCpu(std::span<const std::uint8_t> weights);
    [[nodiscard]] Output infer(const Observation& observation,
                               const std::vector<float>& previousMemory = {}) const;
    [[nodiscard]] SlotOutput inferSlots(const Observation& observation,
                                        const std::vector<float>& previousMemory = {}) const;
    [[nodiscard]] bool multiSlot() const noexcept { return version_ == 2; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t mixtureComponents() const noexcept { return components_; }
    [[nodiscard]] std::size_t parameterCount() const noexcept { return parameterCount_; }
private:
    void load(std::istream& input);
    const Tensor& parameter(const std::string& name) const;
    std::vector<std::vector<float>> encodeEntities(const Observation& observation) const;
    Output inferImpl(const Observation& observation,
                     const std::vector<float>& previousMemory, bool includeHeads,
                     std::vector<std::vector<float>>* encodedEntities = nullptr) const;
    std::unordered_map<std::string, Tensor> parameters_;
    std::uint32_t width_{}, components_{};
    std::uint32_t version_{};
    std::size_t parameterCount_{};
};

}  // namespace protodd::cpu
