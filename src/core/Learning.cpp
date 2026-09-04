#include "astra/Learning.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <sstream>
#include <vector>

namespace astra {
namespace {

std::string clean(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        result.push_back(character == ',' || character == '\n' || character == '\r' ||
                                 character == '#'
                             ? '_'
                             : character);
    }
    return result;
}

OpeningStyle parseStyle(const std::string_view value) {
    for (auto raw = 0; raw < static_cast<int>(OpeningStyle::count); ++raw) {
        const auto style = static_cast<OpeningStyle>(raw);
        if (openingStyleName(style) == value) return style;
    }
    return OpeningStyle::standard;
}

bool parseInt(const std::string_view value, int& output) {
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), output);
    return error == std::errc{} && end == value.data() + value.size() && output >= 0;
}

}  // namespace

double OpeningRecord::winRate() const noexcept {
    return (static_cast<double>(wins) + 1.0) /
           (static_cast<double>(games()) + 2.0);
}

void OpponentHistory::parse(const std::string_view csv) {
    records_.clear();
    merge(csv);
}

void OpponentHistory::merge(const std::string_view csv) {
    std::istringstream input{std::string(csv)};
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.starts_with('#')) continue;
        std::array<std::string_view, 5> fields;
        auto remaining = std::string_view(line);
        auto valid = true;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            const auto comma = remaining.find(',');
            fields[i] = remaining.substr(0, comma);
            if (i + 1 < fields.size() && comma == std::string_view::npos) {
                valid = false;
                break;
            }
            remaining = comma == std::string_view::npos ? std::string_view{} : remaining.substr(comma + 1);
        }
        int wins = 0;
        int losses = 0;
        if (!valid || !parseInt(fields[3], wins) || !parseInt(fields[4], losses)) continue;
        const auto style = parseStyle(fields[2]);
        auto& record = records_[key(fields[0], fields[1], style)];
        // Each file is a cumulative snapshot. Component-wise maxima merge an
        // immutable tournament read baseline with a newer local write snapshot
        // without double-counting the common history.
        record.wins = std::max(record.wins, wins);
        record.losses = std::max(record.losses, losses);
    }
}

std::string OpponentHistory::serialize() const {
    std::vector<std::pair<std::string, OpeningRecord>> sorted(records_.begin(), records_.end());
    std::ranges::sort(sorted, {}, &std::pair<std::string, OpeningRecord>::first);
    std::ostringstream output;
    output << "# opponent,map,style,wins,losses\n";
    for (const auto& [keyValue, record] : sorted) {
        output << keyValue << ',' << record.wins << ',' << record.losses << '\n';
    }
    return output.str();
}

std::string OpponentHistory::filename(const std::string_view opponent) {
    // Encode the actual tournament alias losslessly. No path separators,
    // platform-dependent hash, or identification of the underlying bot.
    constexpr std::string_view hex = "0123456789abcdef";
    std::string result = "AstraBot-";
    for (const auto character : opponent) {
        const auto byte = static_cast<unsigned char>(character);
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result + ".csv";
}

OpeningStyle OpponentHistory::choose(
    const std::string_view opponent,
    const std::string_view map,
    const std::uint64_t deterministicSeed) const {
    auto totalGames = 0;
    for (auto raw = 0; raw < static_cast<int>(OpeningStyle::count); ++raw) {
        totalGames += lookup(opponent, map, static_cast<OpeningStyle>(raw)).games();
    }

    // Start unknown opponents from the balanced arm. Exploration still begins
    // after that evidence-bearing baseline game, but never spends the first
    // and least-informed tournament game on arbitrary greed or deception.
    if (totalGames == 0) return OpeningStyle::standard;

    // Try each remaining style once in a deterministic opponent/map-specific order.
    const auto offset = static_cast<int>(deterministicSeed %
                                         static_cast<std::uint64_t>(OpeningStyle::count));
    for (auto step = 0; step < static_cast<int>(OpeningStyle::count); ++step) {
        const auto raw = (offset + step) % static_cast<int>(OpeningStyle::count);
        const auto style = static_cast<OpeningStyle>(raw);
        if (lookup(opponent, map, style).games() == 0) return style;
    }

    auto best = OpeningStyle::standard;
    auto bestScore = -1.0;
    for (auto raw = 0; raw < static_cast<int>(OpeningStyle::count); ++raw) {
        const auto style = static_cast<OpeningStyle>(raw);
        const auto record = lookup(opponent, map, style);
        const auto exploration = std::sqrt(2.0 * std::log(static_cast<double>(totalGames)) /
                                           static_cast<double>(record.games()));
        const auto score = record.winRate() + exploration;
        if (score > bestScore) {
            bestScore = score;
            best = style;
        }
    }
    return best;
}

void OpponentHistory::record(
    const std::string_view opponent,
    const std::string_view map,
    const OpeningStyle style,
    const bool won) {
    auto& record = records_[key(opponent, map, style)];
    won ? ++record.wins : ++record.losses;
}

OpeningRecord OpponentHistory::lookup(
    const std::string_view opponent,
    const std::string_view map,
    const OpeningStyle style) const {
    const auto found = records_.find(key(opponent, map, style));
    return found == records_.end() ? OpeningRecord{} : found->second;
}

std::string OpponentHistory::key(
    const std::string_view opponent,
    const std::string_view map,
    const OpeningStyle style) {
    return clean(opponent) + ',' + clean(map) + ',' + std::string(openingStyleName(style));
}

}  // namespace astra
