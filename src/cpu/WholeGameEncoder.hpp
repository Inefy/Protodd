#pragma once

#include "WholeGameCpu.hpp"
#include "protodd/WholeGameObservation.hpp"

#include <cstddef>
#include <vector>

namespace protodd::cpu {

struct Terrain {
    std::uint32_t height{}, width{};  // build tiles
    std::vector<float> walkableFraction;
};

struct EncodedObservation {
    Observation input;
    std::vector<int> entityIds;
    std::size_t overflow{};
};

[[nodiscard]] EncodedObservation encode(const whole_observation::Snapshot& source,
                                        const Terrain& terrain, std::size_t limit = 512);

}  // namespace protodd::cpu
