#include "WholeGameCpu.hpp"

#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void append16(std::vector<std::uint8_t>& bytes, const std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

void append32(std::vector<std::uint8_t>& bytes, const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void appendTensor(std::vector<std::uint8_t>& bytes, const std::string& name,
                  const std::vector<std::uint32_t>& shape, const bool includeValues) {
    append16(bytes, static_cast<std::uint16_t>(name.size()));
    bytes.push_back(static_cast<std::uint8_t>(shape.size()));
    bytes.insert(bytes.end(), name.begin(), name.end());
    std::size_t count = 1;
    for (const auto dimension : shape) {
        append32(bytes, dimension);
        count *= dimension;
    }
    if (includeValues)
        for (std::size_t index = 0; index < count; ++index) append32(bytes, 0);
}

std::vector<std::uint8_t> validModel() {
    std::vector<std::uint8_t> bytes{'P','W','G','M','1',0,0,0};
    append32(bytes, 1);  // version
    append32(bytes, 64); // width
    append32(bytes, 1);  // components
    append32(bytes, 40); // tensor count
    appendTensor(bytes, "memory.weight_hh", {192, 64}, true);
    appendTensor(bytes, "position.weight", {5, 64}, true);
    for (int index = 0; index < 38; ++index)
        appendTensor(bytes, "unused." + std::to_string(index), {1}, true);
    return bytes;
}

std::vector<std::uint8_t> oversizedModelHeader() {
    std::vector<std::uint8_t> bytes{'P','W','G','M','1',0,0,0};
    append32(bytes, 1);
    append32(bytes, 64);
    append32(bytes, 1);
    append32(bytes, 40);
    appendTensor(bytes, "oversized", {4096, 4096}, false);
    return bytes;
}

}  // namespace

int main() {
    const auto valid = validModel();
    protodd::cpu::WholeGameCpu model{std::span<const std::uint8_t>(valid)};
    if (model.parameterCount() == 0 ||
        model.parameterCount() > protodd::cpu::WholeGameCpu::maximumParameterCount) {
        std::cerr << "valid model fixture did not load within the configured limit\n";
        return 1;
    }

    bool rejected = false;
    try {
        const auto oversized = oversizedModelHeader();
        protodd::cpu::WholeGameCpu invalid{std::span<const std::uint8_t>(oversized)};
    } catch (const std::runtime_error& error) {
        rejected = std::string_view(error.what()).find("parameter limit") != std::string_view::npos;
    }
    if (!rejected) {
        std::cerr << "oversized tensor header was accepted\n";
        return 1;
    }
    return 0;
}
