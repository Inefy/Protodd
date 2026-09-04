#pragma once

#include "astra/Strategy.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace astra {

struct OpeningRecord {
    int wins{};
    int losses{};

    [[nodiscard]] int games() const noexcept { return wins + losses; }
    [[nodiscard]] double winRate() const noexcept;
};

class OpponentHistory {
public:
    void parse(std::string_view csv);
    void merge(std::string_view csv);
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static std::string filename(std::string_view opponent);

    [[nodiscard]] OpeningStyle choose(
        std::string_view opponent,
        std::string_view map,
        std::uint64_t deterministicSeed) const;
    void record(
        std::string_view opponent,
        std::string_view map,
        OpeningStyle style,
        bool won);
    [[nodiscard]] OpeningRecord lookup(
        std::string_view opponent,
        std::string_view map,
        OpeningStyle style) const;

private:
    std::unordered_map<std::string, OpeningRecord> records_;

    [[nodiscard]] static std::string key(
        std::string_view opponent,
        std::string_view map,
        OpeningStyle style);
};

}  // namespace astra
