#include "WholeGameEncoder.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
template<class T> T read(std::ifstream& stream) {
    T value{};
    stream.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!stream) throw std::runtime_error("truncated encoder input");
    return value;
}
template<class T> std::vector<T> readArray(std::ifstream& stream, std::size_t count) {
    std::vector<T> values(count);
    stream.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(sizeof(T) * count));
    if (!stream) throw std::runtime_error("truncated encoder array");
    return values;
}
template<class T> void writeArray(std::ofstream& stream, const std::vector<T>& values) {
    stream.write(reinterpret_cast<const char*>(values.data()),
                 static_cast<std::streamsize>(sizeof(T) * values.size()));
}
void write32(std::ofstream& stream, std::uint32_t value) {
    stream.write(reinterpret_cast<const char*>(&value), 4);
}
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    try {
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::runtime_error("cannot open encoder fixture");
        const auto magic = readArray<char>(input, 8);
        if (magic != std::vector<char>{'P', 'W', 'G', 'S', '1', 0, 0, 0})
            throw std::runtime_error("encoder fixture magic mismatch");
        protodd::cpu::Terrain terrain;
        terrain.width = read<std::uint32_t>(input);
        terrain.height = read<std::uint32_t>(input);
        const auto pixels = static_cast<std::size_t>(terrain.width) * terrain.height;
        if (!pixels || pixels > 512 * 512) throw std::runtime_error("invalid terrain shape");
        protodd::whole_observation::Snapshot source;
        source.frame = read<std::int32_t>(input);
        source.minerals = read<std::int32_t>(input); source.gas = read<std::int32_t>(input);
        source.supplyUsed = read<std::int32_t>(input); source.supplyTotal = read<std::int32_t>(input);
        source.technologyCompleted = readArray<std::int32_t>(input, 44);
        source.technologyInProgress = readArray<std::int32_t>(input, 44);
        source.upgradeLevels = readArray<std::int32_t>(input, 61);
        source.upgradeInProgress = readArray<std::int32_t>(input, 61);
        const auto vision = readArray<char>(input, pixels);
        source.vision.assign(vision.begin(), vision.end());
        terrain.walkableFraction = readArray<float>(input, pixels);
        const auto count = read<std::uint32_t>(input);
        if (count > 10000) throw std::runtime_error("too many encoder entities");
        for (std::uint32_t index = 0; index < count; ++index) {
            auto entity = protodd::whole_observation::Entity{};
            entity.id = read<std::int32_t>(input); entity.type = read<std::int32_t>(input);
            entity.relation = read<std::int32_t>(input); entity.x = read<std::int32_t>(input);
            entity.y = read<std::int32_t>(input); entity.hp = read<std::int32_t>(input);
            entity.shields = read<std::int32_t>(input); entity.firstSeen = read<std::int32_t>(input);
            entity.lastSeen = read<std::int32_t>(input); entity.visible = read<std::int32_t>(input) != 0;
            entity.completed = read<std::int32_t>(input) != 0;
            entity.energy = read<std::int32_t>(input);
            entity.groundCooldown = read<std::int32_t>(input);
            entity.airCooldown = read<std::int32_t>(input);
            entity.order = read<std::int32_t>(input); entity.orderX = read<std::int32_t>(input);
            entity.orderY = read<std::int32_t>(input); entity.loaded = read<std::int32_t>(input) != 0;
            const auto queueCount = read<std::uint32_t>(input);
            const auto cargoCount = read<std::uint32_t>(input);
            if (queueCount > 32 || cargoCount > 32) throw std::runtime_error("invalid queue/cargo length");
            entity.queue = readArray<std::int32_t>(input, queueCount);
            entity.cargo = readArray<std::int32_t>(input, cargoCount);
            if (!source.entities.emplace(entity.id, std::move(entity)).second)
                throw std::runtime_error("duplicate encoder entity id");
        }
        if (input.peek() != std::char_traits<char>::eof()) throw std::runtime_error("trailing encoder input");
        const auto encoded = protodd::cpu::encode(source, terrain);
        const auto& row = encoded.input;
        std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("cannot create encoder output");
        output.write("PWGI1\0\0\0", 8);
        write32(output, row.height); write32(output, row.width);
        write32(output, static_cast<std::uint32_t>(row.type.size()));
        writeArray(output, row.type); writeArray(output, row.relation); writeArray(output, row.order);
        writeArray(output, row.entityNumeric); writeArray(output, row.spatial); writeArray(output, row.global);
        write32(output, 0);  // no recurrent memory in an encoder fixture
        write32(output, static_cast<std::uint32_t>(encoded.overflow));
        writeArray(output, encoded.entityIds);
        std::cout << "entities=" << row.type.size() << " overflow=" << encoded.overflow << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
