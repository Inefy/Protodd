#pragma once

#include "protodd/Strategy.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace protodd {

inline constexpr std::string_view pvtStrategyPortfolioVersion =
    "r3-siege-v3-pvt-portfolio-v1";

// Version is part of the key so that evidence from another strategy build is
// never silently treated as evidence for this portfolio.
struct PvTStrategyContext {
    std::string opponent;
    std::string opponentRace;
    std::string map;
    std::string strategyVersion{pvtStrategyPortfolioVersion};
};

struct PvTStrategyRecord {
    std::uint32_t wins{};
    std::uint32_t losses{};
    [[nodiscard]] std::uint32_t games() const noexcept { return wins + losses; }
};

struct PvTStrategyRuntimeGate {
    bool adaptive{};
    bool mayWriteHistory{};
};

[[nodiscard]] PvTStrategyRuntimeGate pvtStrategyRuntimeGate(
    bool buildEnabled, bool overridePresent, bool adaptiveMode, bool frozen,
    bool terranOpponent, bool uniqueMatchIdAvailable) noexcept;

inline constexpr std::size_t pvtPortfolioMaximumBytes = 4 * 1024 * 1024;
inline constexpr std::size_t pvtPortfolioMaximumLineBytes = 2048;
inline constexpr std::size_t pvtPortfolioMaximumRows = 50000;

class PvTStrategyPortfolio {
public:
    [[nodiscard]] bool parse(std::string_view csv);
    [[nodiscard]] bool merge(std::string_view csv);
    [[nodiscard]] std::string serialize() const;

    [[nodiscard]] PvTStrategyId choose(
        const PvTStrategyContext& context,
        std::uint64_t deterministicSeed,
        std::uint8_t legalMask = allLegalArms,
        bool explore = true) const noexcept;

    [[nodiscard]] PvTStrategyRecord lookup(
        const PvTStrategyContext& context,
        PvTStrategyId strategy) const noexcept;

    static constexpr std::uint8_t allLegalArms =
        (1u << (static_cast<unsigned>(PvTStrategyId::economicOneGatewayObserver) + 1u)) - 1u;

private:
    struct Row {
        PvTStrategyContext context;
        PvTStrategyId strategy{PvTStrategyId::standard};
        PvTStrategyRecord record;
    };
    std::vector<Row> rows_;

    [[nodiscard]] static bool valid(const PvTStrategyContext& context) noexcept;
    [[nodiscard]] static bool sameKey(const Row& row, const PvTStrategyContext& context,
                                      PvTStrategyId strategy) noexcept;
};

}  // namespace protodd
