#include "protodd/Learning.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <sstream>
#include <utility>
#include <vector>

namespace protodd {
namespace {

std::string encodeField(const std::string_view value) {
    constexpr std::string_view hex = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size() * 3);
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '%' || character == ',' || character == '#' ||
            byte < 0x20 || byte == 0x7f) {
            result.push_back('%');
            result.push_back(hex[byte >> 4]);
            result.push_back(hex[byte & 15]);
        } else {
            result.push_back(character);
        }
    }
    return result;
}

int hexDigit(const char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    return -1;
}

bool decodeField(const std::string_view encoded, std::string& value) {
    if (encoded.size() > OpponentHistory::maximumEncodedFieldBytes) return false;
    value.clear();
    value.reserve(encoded.size());
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        auto character = encoded[index];
        if (character == '%') {
            if (index + 2 >= encoded.size()) return false;
            const auto high = hexDigit(encoded[index + 1]);
            const auto low = hexDigit(encoded[index + 2]);
            if (high < 0 || low < 0) return false;
            character = static_cast<char>((high << 4) | low);
            index += 2;
        } else if (character == ',' || character == '#' ||
                   static_cast<unsigned char>(character) < 0x20 ||
                   static_cast<unsigned char>(character) == 0x7f) {
            return false;
        }
        value.push_back(character);
        if (value.size() > OpponentHistory::maximumFieldBytes) return false;
    }
    // Only accept the canonical spelling emitted by encodeField. Otherwise
    // an unnecessary escape such as `%41` aliases the distinct plain value `A`.
    return encodeField(value) == encoded;
}

OpeningStyle parseStyle(const std::string_view value) {
    for (auto raw = 0; raw < static_cast<int>(OpeningStyle::count); ++raw) {
        const auto style = static_cast<OpeningStyle>(raw);
        if (openingStyleName(style) == value) return style;
    }
    return OpeningStyle::count;
}

bool parseInt(const std::string_view value, int& output) {
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), output);
    return error == std::errc{} && end == value.data() + value.size() && output >= 0;
}

bool validOutcomeId(const std::string_view value) {
    return !value.empty() && value.size() <= OpponentHistory::maximumOutcomeIdBytes &&
        std::ranges::all_of(value, [](const char character) {
            return (character >= '0' && character <= '9') ||
                   (character >= 'a' && character <= 'f');
        });
}

}  // namespace

double OpeningRecord::winRate() const noexcept {
    return (static_cast<double>(wins) + 1.0) /
           (static_cast<double>(games()) + 2.0);
}

void OpponentHistory::parse(const std::string_view csv) {
    records_.clear();
    legacyRecords_.clear();
    outcomes_.clear();
    merge(csv);
}

void OpponentHistory::merge(const std::string_view csv) {
    if (csv.size() > maximumSerializedBytes) return;
    constexpr std::size_t maximumLineBytes = 1024;
    auto formatV3 = false;
    auto remainingCsv = csv;
    while (!remainingCsv.empty()) {
        const auto newline = remainingCsv.find('\n');
        auto line = remainingCsv.substr(0, newline);
        remainingCsv = newline == std::string_view::npos
            ? std::string_view{}
            : remainingCsv.substr(newline + 1);
        if (line.size() > maximumLineBytes) continue;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line == "# protodd-history-v3") {
            formatV3 = true;
            continue;
        }
        if (line.empty() || line.starts_with('#')) continue;
        const auto isOutcome = line.starts_with("outcome,");
        std::array<std::string_view, 6> fields{};
        const auto fieldCount = isOutcome ? 6U : 5U;
        auto remaining = line;
        auto valid = true;
        for (std::size_t i = 0; i < fieldCount; ++i) {
            const auto comma = remaining.find(',');
            fields[i] = remaining.substr(0, comma);
            if ((i + 1 < fieldCount && comma == std::string_view::npos) ||
                (i + 1 == fieldCount && comma != std::string_view::npos)) {
                valid = false;
                break;
            }
            remaining = comma == std::string_view::npos ? std::string_view{} : remaining.substr(comma + 1);
        }
        if (!valid || !remaining.empty()) continue;
        if (isOutcome) {
            const auto id = fields[1];
            auto opponent = std::string(fields[2]);
            auto map = std::string(fields[3]);
            const auto style = parseStyle(fields[4]);
            const auto result = fields[5];
            if (formatV3 && (!decodeField(fields[2], opponent) ||
                             !decodeField(fields[3], map))) continue;
            if (fields[0] != "outcome" || !validOutcomeId(id) ||
                (!formatV3 && (opponent.size() > maximumFieldBytes ||
                               map.size() > maximumFieldBytes)) ||
                style == OpeningStyle::count || (result != "win" && result != "loss")) continue;
            if (outcomes_.contains(std::string(id)) || outcomes_.size() >= maximumOutcomes) continue;
            const auto recordKey = key(opponent, map, style);
            if (!records_.contains(recordKey) && records_.size() >= maximumRecords) continue;
            outcomes_.emplace(std::string(id), Outcome{std::move(opponent), std::move(map),
                                                       style, result == "win"});
            auto& total = records_[recordKey];
            auto& count = result == "win" ? total.wins : total.losses;
            if (count < 1000000) ++count;
            continue;
        }
        auto opponent = std::string(fields[0]);
        auto map = std::string(fields[1]);
        if (formatV3) {
            if (!decodeField(fields[0], opponent) || !decodeField(fields[1], map)) continue;
        } else if (opponent.size() > maximumFieldBytes || map.size() > maximumFieldBytes) {
            continue;
        }
        int wins = 0;
        int losses = 0;
        if (!valid || !parseInt(fields[3], wins) || !parseInt(fields[4], losses)) continue;
        const auto style = parseStyle(fields[2]);
        if (style == OpeningStyle::count || wins > 1000000 || losses > 1000000) continue;
        const auto recordKey = key(opponent, map, style);
        const auto found = legacyRecords_.find(recordKey);
        if (found == legacyRecords_.end() && records_.size() >= maximumRecords) continue;
        auto& legacy = found == legacyRecords_.end()
            ? legacyRecords_[recordKey] : found->second;
        auto& total = records_[recordKey];
        const auto mergedWins = std::max(legacy.wins, wins);
        const auto mergedLosses = std::max(legacy.losses, losses);
        total.wins += mergedWins - legacy.wins;
        total.losses += mergedLosses - legacy.losses;
        legacy.wins = mergedWins;
        legacy.losses = mergedLosses;
    }
}

std::string OpponentHistory::serialize() const {
    std::vector<std::pair<std::string, OpeningRecord>> sortedLegacy(
        legacyRecords_.begin(), legacyRecords_.end());
    std::ranges::sort(sortedLegacy, {}, &std::pair<std::string, OpeningRecord>::first);
    std::vector<std::pair<std::string, Outcome>> sortedOutcomes(
        outcomes_.begin(), outcomes_.end());
    std::ranges::sort(sortedOutcomes, {}, &std::pair<std::string, Outcome>::first);
    std::ostringstream output;
    output << "# protodd-history-v3\n";
    output << "# opponent,map,style,wins,losses\n";
    for (const auto& [keyValue, record] : sortedLegacy) {
        output << keyValue << ',' << record.wins << ',' << record.losses << '\n';
    }
    output << "# outcome,id,opponent,map,style,result\n";
    for (const auto& [id, outcome] : sortedOutcomes) {
        output << "outcome," << id << ',' << encodeField(outcome.opponent) << ','
               << encodeField(outcome.map) << ',' << openingStyleName(outcome.style) << ','
               << (outcome.won ? "win" : "loss") << '\n';
    }
    return output.str();
}

std::string OpponentHistory::filename(const std::string_view opponent) {
    // Accepted aliases encode losslessly with no separators or user lookup.
    // Reject longer aliases before hex expansion could exceed a native basename.
    constexpr std::string_view hex = "0123456789abcdef";
    if (opponent.size() > maximumOpponentFilenameBytes) return {};
    std::string result = "Protodd-";
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
    const std::uint64_t deterministicSeed,
    const bool explore) const {
    if (opponent.size() > maximumFieldBytes || map.size() > maximumFieldBytes)
        return OpeningStyle::standard;
    // Transfer only same-opponent evidence from OTHER maps. Cap its influence
    // at four virtual games per arm, so local outcomes can override it.
    std::array<double, static_cast<std::size_t>(OpeningStyle::count)> priorWins{};
    std::array<double, static_cast<std::size_t>(OpeningStyle::count)> priorGames{};
    const auto prefix = encodeField(opponent) + ',';
    for (auto raw = 0; raw < static_cast<int>(OpeningStyle::count); ++raw) {
        const auto style = static_cast<OpeningStyle>(raw);
        const auto suffix = ',' + std::string(openingStyleName(style));
        double wins = 0.0;
        double games = 0.0;
        for (const auto& [recordKey, record] : records_) {
            if (recordKey.starts_with(prefix) && recordKey.ends_with(suffix) &&
                recordKey != key(opponent, map, style)) {
                wins += record.wins;
                games += record.games();
            }
        }
        const auto index = static_cast<std::size_t>(raw);
        priorGames[index] = std::min(4.0, games);
        priorWins[index] = games > 0.0 ? priorGames[index] * wins / games : 0.0;
    }
    auto totalGames = 0;
    for (auto raw = 0; raw < static_cast<int>(OpeningStyle::count); ++raw) {
        totalGames += lookup(opponent, map, static_cast<OpeningStyle>(raw)).games();
    }

    // Start unknown opponents from the balanced arm. Exploration still begins
    // after that evidence-bearing baseline game, but never spends the first
    // and least-informed tournament game on arbitrary greed or deception.
    const auto posterior = [&](const OpeningStyle style) {
        const auto raw = static_cast<std::size_t>(style);
        const auto local = lookup(opponent, map, style);
        return (local.wins + priorWins[raw] + 1.0) /
               (local.games() + priorGames[raw] + 2.0);
    };
    if (!explore || totalGames == 0) {
        auto best = OpeningStyle::standard;
        for (auto raw = 1; raw < static_cast<int>(OpeningStyle::count); ++raw) {
            const auto style = static_cast<OpeningStyle>(raw);
            // A training cold start needs at least four cross-map observations
            // before departing from the robust standard opening.
            if ((!explore || priorGames[static_cast<std::size_t>(raw)] >= 4.0) && posterior(style) > posterior(best))
                best = style;
        }
        return best;
    }

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
        const auto score = posterior(style) + exploration;
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
    const bool won,
    const std::string_view outcomeId) {
    if (opponent.size() > maximumFieldBytes || map.size() > maximumFieldBytes ||
        static_cast<std::size_t>(style) >= static_cast<std::size_t>(OpeningStyle::count)) return;
    const auto recordKey = key(opponent, map, style);
    const auto found = records_.find(recordKey);
    if (found == records_.end() && records_.size() >= maximumRecords) return;
    if (!outcomeId.empty()) {
        if (!validOutcomeId(outcomeId) || outcomes_.contains(std::string(outcomeId)) ||
            outcomes_.size() >= maximumOutcomes) return;
        outcomes_.emplace(std::string(outcomeId), Outcome{std::string(opponent),
                                                           std::string(map), style, won});
    } else {
        auto& legacy = legacyRecords_[recordKey];
        auto& legacyCount = won ? legacy.wins : legacy.losses;
        if (legacyCount < 1000000) ++legacyCount;
    }
    auto& record = found == records_.end() ? records_[recordKey] : found->second;
    auto& count = won ? record.wins : record.losses;
    if (count < 1000000) ++count;
}

OpeningRecord OpponentHistory::lookup(
    const std::string_view opponent,
    const std::string_view map,
    const OpeningStyle style) const {
    if (opponent.size() > maximumFieldBytes || map.size() > maximumFieldBytes ||
        static_cast<std::size_t>(style) >= static_cast<std::size_t>(OpeningStyle::count)) return {};
    const auto found = records_.find(key(opponent, map, style));
    return found == records_.end() ? OpeningRecord{} : found->second;
}

std::string OpponentHistory::key(
    const std::string_view opponent,
    const std::string_view map,
    const OpeningStyle style) {
    return encodeField(opponent) + ',' + encodeField(map) + ',' +
           std::string(openingStyleName(style));
}

}  // namespace protodd
