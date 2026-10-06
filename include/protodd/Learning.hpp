#pragma once

#include "protodd/Strategy.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace protodd {

struct OpeningRecord {
    int wins{};
    int losses{};

    [[nodiscard]] int games() const noexcept { return wins + losses; }
    [[nodiscard]] double winRate() const noexcept;
};

class OpponentHistory {
public:
    static constexpr std::size_t maximumSerializedBytes = 4 * 1024 * 1024;
    static constexpr std::size_t maximumRecords = 4096;
    static constexpr std::size_t maximumOutcomes = 1024;
    static constexpr std::size_t maximumFieldBytes = 128;
    static constexpr std::size_t maximumEncodedFieldBytes = maximumFieldBytes * 3;
    static constexpr std::size_t maximumOpponentFilenameBytes = 120;
    static constexpr std::size_t maximumOutcomeIdBytes = 32;

    void parse(std::string_view csv);
    void merge(std::string_view csv);
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static std::string filename(std::string_view opponent);

    [[nodiscard]] OpeningStyle choose(
        std::string_view opponent,
        std::string_view map,
        std::uint64_t deterministicSeed,
        bool explore = true) const;
    void record(
        std::string_view opponent,
        std::string_view map,
        OpeningStyle style,
        bool won,
        std::string_view outcomeId = {});
    [[nodiscard]] OpeningRecord lookup(
        std::string_view opponent,
        std::string_view map,
        OpeningStyle style) const;

private:
    struct Outcome {
        std::string opponent;
        std::string map;
        OpeningStyle style{OpeningStyle::standard};
        bool won{};
    };

    std::unordered_map<std::string, OpeningRecord> records_;
    std::unordered_map<std::string, OpeningRecord> legacyRecords_;
    std::unordered_map<std::string, Outcome> outcomes_;

    [[nodiscard]] static std::string key(
        std::string_view opponent,
        std::string_view map,
        OpeningStyle style);
};

}  // namespace protodd
