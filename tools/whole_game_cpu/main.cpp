#include "WholeGameCpu.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
std::uint32_t number(std::ifstream& stream) {
    std::uint32_t value{};
    stream.read(reinterpret_cast<char*>(&value), 4);
    if (!stream) throw std::runtime_error("truncated probe observation");
    return value;
}
template<class T> std::vector<T> values(std::ifstream& stream, std::size_t count) {
    std::vector<T> result(count);
    stream.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(count * sizeof(T)));
    if (!stream) throw std::runtime_error("truncated probe observation values");
    return result;
}
void write32(std::ofstream& stream, std::uint32_t value) {
    stream.write(reinterpret_cast<const char*>(&value), 4);
}
}

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    try {
        protodd::cpu::WholeGameCpu model(argv[1]);
        std::ifstream source(argv[2], std::ios::binary);
        if (!source) throw std::runtime_error("cannot open probe observation");
        const auto magic = values<char>(source, 8);
        if (magic != std::vector<char>{'P', 'W', 'G', 'I', '1', 0, 0, 0})
            throw std::runtime_error("probe observation magic mismatch");
        protodd::cpu::Observation observation;
        observation.height = number(source); observation.width = number(source);
        const auto count = number(source);
        if (count == 0 || count > 4096 || observation.height == 0 || observation.width == 0
            || observation.height > 512 || observation.width > 512)
            throw std::runtime_error("invalid probe observation shape");
        observation.type = values<std::int32_t>(source, count);
        observation.relation = values<std::int32_t>(source, count);
        observation.order = values<std::int32_t>(source, count);
        observation.entityNumeric = values<float>(source, count * 16);
        observation.spatial = values<float>(source, 3ull * observation.height * observation.width);
        observation.global = values<float>(source, 216);
        const auto hasMemory = number(source);
        if (hasMemory > 1) throw std::runtime_error("invalid probe memory flag");
        const auto memory = hasMemory ? values<float>(source, model.width()) : std::vector<float>{};
        if (source.peek() != std::char_traits<char>::eof())
            throw std::runtime_error("trailing probe observation data");
        const auto began = std::chrono::steady_clock::now();
        const auto output = model.multiSlot()
            ? protodd::cpu::Output{} : model.infer(observation, memory);
        const auto slots = model.multiSlot()
            ? model.inferSlots(observation, memory) : protodd::cpu::SlotOutput{};
        const auto duration = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - began).count();
        std::ofstream destination(argv[3], std::ios::binary | std::ios::trunc);
        if (!destination) throw std::runtime_error("cannot create probe result");
        destination.write("PWGO1\0\0\0", 8);
        auto ordered = output.heads;
        ordered.emplace("memory", model.multiSlot() ? slots.memory : output.memory);
        if (model.multiSlot()) {
            ordered.emplace("slot_count", std::vector<float>{static_cast<float>(slots.slots.size())});
            for (std::size_t index = 0; index < slots.slots.size(); ++index)
                for (const auto& [name, values] : slots.slots[index].heads)
                    ordered.emplace("slot" + std::to_string(index) + "." + name, values);
        }
        write32(destination, static_cast<std::uint32_t>(ordered.size()));
        std::vector<std::string> names;
        for (const auto& [name, value] : ordered) names.push_back(name);
        std::sort(names.begin(), names.end());
        for (const auto& name : names) {
            const auto& value = ordered.at(name);
            write32(destination, static_cast<std::uint32_t>(name.size()));
            destination.write(name.data(), static_cast<std::streamsize>(name.size()));
            write32(destination, static_cast<std::uint32_t>(value.size()));
            destination.write(reinterpret_cast<const char*>(value.data()),
                              static_cast<std::streamsize>(value.size() * sizeof(float)));
        }
        std::cout << "parameters=" << model.parameterCount() << " entities=" << count
                  << " forward_ms=" << duration << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
