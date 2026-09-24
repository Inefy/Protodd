#include "WholeGameCpu.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    try {
        const auto module = LoadLibraryExW(std::filesystem::path(argv[1]).c_str(),
                                            nullptr, LOAD_LIBRARY_AS_DATAFILE);
        if (!module) throw std::runtime_error("cannot open DLL as resource data");
        const auto resource = FindResourceW(module, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10));
        const auto loaded = resource ? LoadResource(module, resource) : nullptr;
        const auto data = loaded ? LockResource(loaded) : nullptr;
        const auto size = resource ? SizeofResource(module, resource) : 0;
        if (!data || size == 0) throw std::runtime_error("embedded weight resource missing");
        std::ifstream source(argv[2], std::ios::binary);
        if (!source) throw std::runtime_error("cannot open source weights");
        const std::vector<char> expected(std::istreambuf_iterator<char>{source}, {});
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        if (expected.size() != size ||
            !std::equal(expected.begin(), expected.end(), bytes,
                        [](char a, std::uint8_t b) { return static_cast<std::uint8_t>(a) == b; }))
            throw std::runtime_error("embedded weights differ from source package");

        const protodd::cpu::WholeGameCpu embedded(std::span<const std::uint8_t>(bytes, size));
        const protodd::cpu::WholeGameCpu fromFile{std::filesystem::path(argv[2])};
        protodd::cpu::Observation row;
        row.height = row.width = 32;
        row.type = {0}; row.relation = {0}; row.order = {0};
        row.entityNumeric.assign(16, 0.0f);
        row.spatial.assign(3 * 32 * 32, 0.0f);
        row.global.assign(216, 0.0f);
        float maximum = 0;
        const auto compare = [&maximum](const std::vector<float>& left,
                                        const std::vector<float>& right) {
            if (left.size() != right.size())
                throw std::runtime_error("embedded output shape differs from file output");
            for (std::size_t i = 0; i < left.size(); ++i)
                maximum = std::max(maximum, std::abs(left[i] - right[i]));
        };
        std::size_t slots = 0;
        if (embedded.multiSlot()) {
            const auto left = embedded.inferSlots(row);
            const auto right = fromFile.inferSlots(row);
            if (left.slots.size() != right.slots.size())
                throw std::runtime_error("embedded slot count differs from file output");
            slots = left.slots.size();
            compare(left.memory, right.memory);
            for (std::size_t slot = 0; slot < slots; ++slot) {
                if (left.slots[slot].heads.size() != right.slots[slot].heads.size())
                    throw std::runtime_error("embedded slot heads differ from file output");
                for (const auto& [name, values] : left.slots[slot].heads)
                    compare(values, right.slots[slot].heads.at(name));
            }
        } else {
            const auto left = embedded.infer(row);
            const auto right = fromFile.infer(row);
            compare(left.memory, right.memory);
            if (left.heads.size() != right.heads.size())
                throw std::runtime_error("embedded heads differ from file output");
            for (const auto& [name, values] : left.heads)
                compare(values, right.heads.at(name));
        }
        FreeLibrary(module);
        if (maximum != 0) throw std::runtime_error("embedded inference differs from file inference");
        std::cout << "embedded_bytes=" << size << " parameters=" << embedded.parameterCount()
                  << " active_slots=" << slots
                  << " max_abs_difference=" << maximum << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
