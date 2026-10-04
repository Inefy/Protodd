#include "protodd/PvTStrategyPortfolio.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <sstream>
#include <tuple>

namespace protodd {
namespace {

constexpr std::string_view header = "# PROTODD_PVT_PORTFOLIO 1";

std::string field(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 0x20 || byte == 0x7f) continue;
        result.push_back(ch == ',' ? ';' : ch);
    }
    return result;
}

bool parseCount(std::string_view text, std::uint32_t& value) {
    unsigned parsed{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || end != text.data() + text.size() || parsed > 1000000u)
        return false;
    value = parsed;
    return true;
}

std::string_view armName(const PvTStrategyId strategy) {
    switch (strategy) {
        case PvTStrategyId::standard: return "standard";
        case PvTStrategyId::safeTwoGatewayRangeObserver: return "safe-2gateway-range-observer";
        case PvTStrategyId::economicOneGatewayObserver: return "economic-1gateway-observer";
    }
    return "invalid";
}

PvTStrategyId parseStrategy(const std::string_view value) {
    for (auto raw = 0u; raw <= static_cast<unsigned>(PvTStrategyId::economicOneGatewayObserver); ++raw) {
        const auto strategy = static_cast<PvTStrategyId>(raw);
        if (armName(strategy) == value) return strategy;
    }
    return static_cast<PvTStrategyId>(255);
}

std::array<std::string_view, 7> split(std::string_view line, bool& valid) {
    std::array<std::string_view, 7> result{};
    valid = true;
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto comma = line.find(',');
        if (i + 1 < result.size() && comma == std::string_view::npos) {
            valid = false;
            return result;
        }
        result[i] = line.substr(0, comma);
        if (i + 1 < result.size()) line.remove_prefix(comma + 1);
        else if (comma != std::string_view::npos) valid = false;
    }
    return result;
}

}  // namespace

PvTStrategyRuntimeGate pvtStrategyRuntimeGate(
    const bool buildEnabled, const bool overridePresent, const bool adaptiveMode,
    const bool frozen, const bool terranOpponent, const bool uniqueMatchIdAvailable) noexcept {
    const auto active = buildEnabled && !overridePresent && adaptiveMode && !frozen &&
                        terranOpponent && uniqueMatchIdAvailable;
    return {active, active};
}

bool PvTStrategyPortfolio::valid(const PvTStrategyContext& context) noexcept {
    const auto safe = [](const std::string_view value) {
        return !value.empty() && value.size() <= 256 &&
               std::ranges::none_of(value, [](const char ch) {
                   const auto byte = static_cast<unsigned char>(ch);
                   return byte < 0x20 || byte == 0x7f || ch == ',';
               });
    };
    return safe(context.opponent) && context.opponentRace == "terran" &&
           safe(context.map) && context.strategyVersion == pvtStrategyPortfolioVersion;
}

bool PvTStrategyPortfolio::sameKey(
    const Row& row, const PvTStrategyContext& context, const PvTStrategyId strategy) noexcept {
    return row.context.opponent == context.opponent &&
           row.context.opponentRace == context.opponentRace &&
           row.context.map == context.map && row.context.strategyVersion == context.strategyVersion &&
           row.strategy == strategy;
}

bool PvTStrategyPortfolio::parse(const std::string_view csv) {
    PvTStrategyPortfolio parsed;
    if (csv.size() > pvtPortfolioMaximumBytes) return false;
    if (csv.empty()) {
        rows_.clear();
        return true;
    }
    std::istringstream input{std::string(csv)};
    std::string line;
    bool sawHeader = false;
    std::size_t parsedRows = 0;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.size() > pvtPortfolioMaximumLineBytes) return false;
        if (!sawHeader) {
            if (line != header) return false;
            sawHeader = true;
            continue;
        }
        if (line.front() == '#') continue;
        bool fieldsValid = false;
        const auto columns = split(line, fieldsValid);
        if (!fieldsValid || columns[0] != pvtStrategyPortfolioVersion) return false;
        Row row;
        row.context.strategyVersion = columns[0];
        row.context.opponentRace = columns[1];
        row.context.opponent = columns[2];
        row.context.map = columns[3];
        row.strategy = parseStrategy(columns[4]);
        if (row.strategy == static_cast<PvTStrategyId>(255) || !valid(row.context) ||
            !parseCount(columns[5], row.record.wins) ||
            !parseCount(columns[6], row.record.losses)) return false;
        if (++parsedRows > pvtPortfolioMaximumRows) return false;
        const auto existing = std::ranges::find_if(parsed.rows_, [&](const Row& item) {
            return sameKey(item, row.context, row.strategy);
        });
        if (existing == parsed.rows_.end()) parsed.rows_.push_back(std::move(row));
        else {
            existing->record.wins = std::max(existing->record.wins, row.record.wins);
            existing->record.losses = std::max(existing->record.losses, row.record.losses);
        }
    }
    if (!sawHeader) return false;
    rows_ = std::move(parsed.rows_);
    return true;
}

bool PvTStrategyPortfolio::merge(const std::string_view csv) {
    if (csv.empty()) return true;
    PvTStrategyPortfolio incoming;
    if (!incoming.parse(csv)) return false;
    auto merged = *this;
    for (const auto& row : incoming.rows_) {
        const auto existing = std::ranges::find_if(merged.rows_, [&](const Row& item) {
            return sameKey(item, row.context, row.strategy);
        });
        if (existing == merged.rows_.end()) merged.rows_.push_back(row);
        else {
            existing->record.wins = std::max(existing->record.wins, row.record.wins);
            existing->record.losses = std::max(existing->record.losses, row.record.losses);
        }
        if (merged.rows_.size() > pvtPortfolioMaximumRows) return false;
    }
    if (merged.serialize().size() > pvtPortfolioMaximumBytes) return false;
    rows_ = std::move(merged.rows_);
    return true;
}

std::string PvTStrategyPortfolio::serialize() const {
    auto sorted = rows_;
    std::ranges::sort(sorted, [](const Row& left, const Row& right) {
        return std::tie(left.context.strategyVersion, left.context.opponentRace,
                        left.context.opponent, left.context.map, left.strategy) <
               std::tie(right.context.strategyVersion, right.context.opponentRace,
                        right.context.opponent, right.context.map, right.strategy);
    });
    std::ostringstream output;
    output << header << "\n# version,race,opponent,map,strategy,wins,losses\n";
    for (const auto& row : sorted) {
        output << field(row.context.strategyVersion) << ',' << field(row.context.opponentRace)
               << ',' << field(row.context.opponent) << ',' << field(row.context.map) << ','
               << armName(row.strategy) << ',' << row.record.wins << ','
               << row.record.losses << '\n';
    }
    return output.str();
}

PvTStrategyId PvTStrategyPortfolio::choose(
    const PvTStrategyContext& context, const std::uint64_t seed,
    const std::uint8_t legalMask, const bool explore) const noexcept {
    const auto mask = static_cast<std::uint8_t>(legalMask & allLegalArms);
    if (mask == 0) return PvTStrategyId::standard;
    const auto legal = [mask](const int raw) { return (mask & (1u << raw)) != 0; };
    const auto firstLegal = [&]() {
        for (int raw = 0; raw <= static_cast<int>(PvTStrategyId::economicOneGatewayObserver); ++raw)
            if (legal(raw)) return static_cast<PvTStrategyId>(raw);
        return PvTStrategyId::standard;
    };
    if (!valid(context)) return legal(0) ? PvTStrategyId::standard : firstLegal();

    const auto armRecord = [&](const PvTStrategyId strategy) {
        return lookup(context, strategy);
    };
    const auto posterior = [&](const PvTStrategyId strategy) {
        const auto local = armRecord(strategy);
        std::uint64_t priorWins = 0;
        std::uint64_t priorGames = 0;
        for (const auto& row : rows_) {
            if (row.strategy != strategy || row.context.opponent != context.opponent ||
                row.context.opponentRace != context.opponentRace ||
                row.context.strategyVersion != context.strategyVersion || row.context.map == context.map)
                continue;
            priorWins += row.record.wins;
            priorGames += row.record.games();
        }
        const auto usedPrior = std::min<std::uint64_t>(4u, priorGames);
        const auto scaledWins = priorGames == 0 ? 0.0 :
            static_cast<double>(usedPrior) * static_cast<double>(priorWins) / priorGames;
        return (local.wins + scaledWins + 1.0) / (local.games() + usedPrior + 2.0);
    };

    if (explore) {
        const auto count = static_cast<int>(PvTStrategyId::economicOneGatewayObserver) + 1;
        const auto offset = static_cast<int>(seed % static_cast<std::uint64_t>(count));
        for (int step = 0; step < count; ++step) {
            const auto raw = (offset + step) % count;
            const auto strategy = static_cast<PvTStrategyId>(raw);
            if (legal(raw) && armRecord(strategy).games() == 0) return strategy;
        }
    }

    int total = 0;
    for (int raw = 0; raw <= static_cast<int>(PvTStrategyId::economicOneGatewayObserver); ++raw)
        if (legal(raw)) total += armRecord(static_cast<PvTStrategyId>(raw)).games();
    auto best = firstLegal();
    auto bestScore = -std::numeric_limits<double>::infinity();
    const auto count = static_cast<int>(PvTStrategyId::economicOneGatewayObserver) + 1;
    const auto offset = static_cast<int>(seed % static_cast<std::uint64_t>(count));
    for (int step = 0; step < count; ++step) {
        const auto raw = (offset + step) % count;
        if (!legal(raw)) continue;
        const auto strategy = static_cast<PvTStrategyId>(raw);
        const auto local = armRecord(strategy);
        const auto bonus = explore && local.games() > 0
            ? std::sqrt(2.0 * std::log(static_cast<double>(std::max(total, 2))) / local.games())
            : 0.0;
        const auto score = posterior(strategy) + bonus;
        if (score > bestScore) { bestScore = score; best = strategy; }
    }
    return best;
}

PvTStrategyRecord PvTStrategyPortfolio::lookup(
    const PvTStrategyContext& context, const PvTStrategyId strategy) const noexcept {
    const auto found = std::ranges::find_if(rows_, [&](const Row& row) {
        return sameKey(row, context, strategy);
    });
    return found == rows_.end() ? PvTStrategyRecord{} : found->record;
}

}  // namespace protodd
