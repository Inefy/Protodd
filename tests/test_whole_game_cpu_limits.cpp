#include "WholeGameCpu.hpp"

#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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

std::vector<std::uint8_t> nonfiniteModelParameter() {
    auto bytes = validModel();
    constexpr std::string_view name = "memory.weight_hh";
    const auto valueOffset = 8 + 4 * 4 + 2 + 1 + name.size() + 2 * 4;
    bytes[valueOffset] = 0;
    bytes[valueOffset + 1] = 0;
    bytes[valueOffset + 2] = 0xc0;
    bytes[valueOffset + 3] = 0x7f; // quiet NaN
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

    const auto directory = std::filesystem::temp_directory_path() /
        ("protodd-whole-game-model-limit-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const auto validPath = directory / "valid.bin";
    const auto oversizedPath = directory / "oversized.bin";
    const auto trailingPath = directory / "trailing.bin";
    const auto writeBytes = [](const std::filesystem::path& path,
                               const std::vector<std::uint8_t>& bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(output);
    };
    if (!writeBytes(validPath, valid) || !writeBytes(oversizedPath, oversizedModelHeader()) ||
        !writeBytes(trailingPath, valid)) {
        std::cerr << "could not write whole-game model path fixtures\n";
        std::filesystem::remove_all(directory);
        return 1;
    }
    bool fileModelLoaded = false;
    try {
        const protodd::cpu::WholeGameCpu fileModel{validPath};
        fileModelLoaded = fileModel.parameterCount() == model.parameterCount();
    } catch (const std::exception&) {
    }
    {
        std::ofstream output(trailingPath, std::ios::binary | std::ios::app);
        output.put('\0');
    }
    bool pathOversizeRejected = false;
    try {
        const protodd::cpu::WholeGameCpu invalid{oversizedPath};
    } catch (const std::runtime_error& error) {
        pathOversizeRejected = std::string_view(error.what()).find("parameter limit") !=
                               std::string_view::npos;
    }
    bool trailingDataRejected = false;
    try {
        const protodd::cpu::WholeGameCpu invalid{trailingPath};
    } catch (const std::runtime_error& error) {
        trailingDataRejected = std::string_view(error.what()).find("trailing") !=
                               std::string_view::npos;
    }
    std::filesystem::remove_all(directory);
    if (!fileModelLoaded || !pathOversizeRejected || !trailingDataRejected) {
        std::cerr << "file-backed whole-game model bounds failed\n";
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

    rejected = false;
    try {
        const auto invalid = nonfiniteModelParameter();
        protodd::cpu::WholeGameCpu modelWithNan{std::span<const std::uint8_t>(invalid)};
    } catch (const std::runtime_error& error) {
        rejected = std::string_view(error.what()).find("nonfinite") != std::string_view::npos;
    }
    if (!rejected) {
        std::cerr << "non-finite model parameter was accepted\n";
        return 1;
    }

    protodd::cpu::Output safeOutput;
    safeOutput.memory = {0.0F, -1.0F};
    safeOutput.heads["kind"] = {2.0F, -3.0F};
    if (!protodd::cpu::safeWholeGameOutput(safeOutput)) {
        std::cerr << "finite model output was rejected\n";
        return 1;
    }
    safeOutput.heads["kind"][0] = std::numeric_limits<float>::infinity();
    if (protodd::cpu::safeWholeGameOutput(safeOutput)) {
        std::cerr << "non-finite model output was accepted\n";
        return 1;
    }
    protodd::cpu::SlotOutput safeSlots;
    safeSlots.memory = {0.0F};
    safeSlots.slots = {protodd::cpu::Output{.heads = {{"stop", {0.0F, 1.0F}}}}};
    if (!protodd::cpu::safeWholeGameOutput(safeSlots)) {
        std::cerr << "finite slot output was rejected\n";
        return 1;
    }
    safeSlots.slots.front().heads["stop"][0] = std::numeric_limits<float>::quiet_NaN();
    if (protodd::cpu::safeWholeGameOutput(safeSlots)) {
        std::cerr << "non-finite slot output was accepted\n";
        return 1;
    }
    return 0;
}
