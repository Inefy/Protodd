#include "protodd/Learning.hpp"

#include "AtomicFile.hpp"
#include "BoundedFile.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string readAll(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool hasTemporarySnapshot(const std::filesystem::path& directory) {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().filename().string().find(".tmp-") != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

int main() {
    const auto firstId = protodd::bwapi::uniqueToken();
    const auto secondId = protodd::bwapi::uniqueToken();
    const auto validToken = [](const std::string& value) {
        return value.size() == 32 && std::ranges::all_of(value, [](const char character) {
            return (character >= '0' && character <= '9') ||
                   (character >= 'a' && character <= 'f');
        });
    };
    if (!validToken(firstId) || !validToken(secondId) || firstId == secondId) {
        std::cerr << "outcome identifier format or uniqueness failed\n";
        return 1;
    }

    const auto directory = std::filesystem::temp_directory_path() /
        ("protodd-atomic-file-" + firstId);
    std::filesystem::create_directories(directory);
    const auto destination = directory / "history.csv";
    if (!protodd::bwapi::writeAtomicFile(destination, "first snapshot\n") ||
        readAll(destination) != "first snapshot\n" ||
        !protodd::bwapi::writeAtomicFile(destination, "replacement snapshot\n") ||
        readAll(destination) != "replacement snapshot\n") {
        std::cerr << "atomic history replacement failed\n";
        std::filesystem::remove_all(directory);
        return 1;
    }

    const auto original = readAll(destination);
    if (protodd::bwapi::writeAtomicFile(directory / "missing" / "history.csv", "broken") ||
        readAll(destination) != original) {
        std::cerr << "failed atomic write changed an existing snapshot\n";
        std::filesystem::remove_all(directory);
        return 1;
    }

#if defined(_WIN32)
    const auto originalAttributes = ::GetFileAttributesW(destination.c_str());
    if (originalAttributes == INVALID_FILE_ATTRIBUTES ||
        !::SetFileAttributesW(destination.c_str(), originalAttributes | FILE_ATTRIBUTE_READONLY)) {
        std::cerr << "could not create isolated read-only history fixture\n";
        std::filesystem::remove_all(directory);
        return 1;
    }
    const auto readOnlyRejected = !protodd::bwapi::writeAtomicFile(
        destination, "must not replace read-only history\n");
    const auto readOnlyPreserved = readAll(destination) == original;
    const auto readOnlyTempCleaned = !hasTemporarySnapshot(directory);
    const auto attributesRestored = ::SetFileAttributesW(destination.c_str(), originalAttributes);
    if (!attributesRestored || !readOnlyRejected || !readOnlyPreserved || !readOnlyTempCleaned) {
        std::cerr << "read-only history replacement was not safely rejected\n";
        std::filesystem::remove_all(directory);
        return 1;
    }
#endif

    const auto commitCollision = directory / "directory-in-place-of-history.csv";
    std::filesystem::create_directory(commitCollision);
    if (protodd::bwapi::writeAtomicFile(commitCollision, "uncommittable snapshot\n") ||
        !std::filesystem::is_directory(commitCollision) || hasTemporarySnapshot(directory)) {
        std::cerr << "failed atomic commit changed its destination or leaked a temporary file\n";
        std::filesystem::remove_all(directory);
        return 1;
    }

    std::vector<std::thread> writers;
    std::vector<std::string> payloads;
    for (int index = 0; index < 8; ++index) {
        payloads.emplace_back(8192, static_cast<char>('A' + index));
    }
    std::vector<int> succeeded(payloads.size(), 0);
    for (std::size_t index = 0; index < payloads.size(); ++index) {
        writers.emplace_back([&, index] {
        succeeded[index] = protodd::bwapi::writeAtomicFile(destination, payloads[index]) ? 1 : 0;
        });
    }
    for (auto& writer : writers) writer.join();
    const auto finalContents = readAll(destination);
    const auto wholePayload = std::ranges::any_of(payloads, [&finalContents](const auto& payload) {
        return payload == finalContents;
    });
    const auto anyCommit = std::ranges::any_of(succeeded, [](const int result) {
        return result == 1;
    });
    if (!anyCommit || (!wholePayload && finalContents != original)) {
        std::cerr << "concurrent replacement exposed a partial history snapshot\n";
        std::filesystem::remove_all(directory);
        return 1;
    }

    // Exercise the same per-alias read/write layout used by ProtoddModule.
    // The tournament read snapshot stays fixed during a round; each new game
    // must merge the preceding local write before appending its unique result.
    const auto readDirectory = directory / "round-read";
    const auto writeDirectory = directory / "round-write";
    std::filesystem::create_directories(readDirectory);
    std::filesystem::create_directories(writeDirectory);
    constexpr std::string_view alias = "Alias,A";
    constexpr std::string_view otherAlias = "Alias_A";
    constexpr std::string_view map = "Python#map-hash";
    const auto aliasFile = protodd::OpponentHistory::filename(alias);
    const auto otherAliasFile = protodd::OpponentHistory::filename(otherAlias);
    const auto applyLocalGame = [&](const std::filesystem::path& readRoot,
                                    const std::filesystem::path& writeRoot,
                                    const std::string_view opponent,
                                    const bool won) {
        const auto filename = protodd::OpponentHistory::filename(opponent);
        if (filename.empty()) return false;
        const auto read = protodd::bwapi::readBoundedFile(
            readRoot / filename, protodd::OpponentHistory::maximumSerializedBytes);
        if (!read.withinLimit) return false;
        protodd::OpponentHistory history;
        history.parse(read.contents);  // A missing first-round file is blank input.
        const auto local = protodd::bwapi::readBoundedFile(
            writeRoot / filename, protodd::OpponentHistory::maximumSerializedBytes);
        if (!local.withinLimit) return false;
        history.merge(local.contents);
        history.record(opponent, map, protodd::OpeningStyle::standard, won,
                       protodd::bwapi::uniqueToken());
        return protodd::bwapi::writeAtomicFile(writeRoot / filename,
                                                history.serialize());
    };
    const auto loadHistory = [](const std::filesystem::path& path) {
        const auto stored = protodd::bwapi::readBoundedFile(
            path, protodd::OpponentHistory::maximumSerializedBytes);
        protodd::OpponentHistory history;
        if (stored.withinLimit) history.parse(stored.contents);
        return history;
    };
    const auto aliasWrite = writeDirectory / aliasFile;
    const auto aliasRead = readDirectory / aliasFile;
    if (!applyLocalGame(readDirectory, writeDirectory, alias, true) ||
        !applyLocalGame(readDirectory, writeDirectory, alias, false)) {
        std::cerr << "same-round history read/write transfer failed\n";
        std::filesystem::remove_all(directory);
        return 1;
    }
    auto firstRound = loadHistory(aliasWrite);
    const auto firstRoundCounts = firstRound.lookup(
        alias, map, protodd::OpeningStyle::standard);
    if (firstRoundCounts.wins != 1 || firstRoundCounts.losses != 1 ||
        aliasFile == otherAliasFile || std::filesystem::exists(aliasRead)) {
        std::cerr << "blank read snapshot, cumulative write merge, or alias file separation failed\n";
        std::filesystem::remove_all(directory);
        return 1;
    }

    // Round turnover copies the cumulative write snapshot into read and starts
    // a fresh write directory. The next game must inherit both prior outcomes.
    std::filesystem::copy_file(aliasWrite, aliasRead,
        std::filesystem::copy_options::overwrite_existing);
    std::filesystem::remove(aliasWrite);
    if (!applyLocalGame(readDirectory, writeDirectory, alias, true)) {
        std::cerr << "round-two snapshot import failed\n";
        std::filesystem::remove_all(directory);
        return 1;
    }
    auto roundTwo = loadHistory(aliasWrite);
    const auto roundTwoCounts = roundTwo.lookup(
        alias, map, protodd::OpeningStyle::standard);
    if (roundTwoCounts.wins != 2 || roundTwoCounts.losses != 1 ||
        !applyLocalGame(readDirectory, writeDirectory, otherAlias, false) ||
        !std::filesystem::exists(writeDirectory / otherAliasFile)) {
        std::cerr << "round rollover or opponent-specific history output failed\n";
        std::filesystem::remove_all(directory);
        return 1;
    }

    // Two independent local jobs start from the same round baseline. Their
    // unique outcomes merge without losing either game or double-counting a
    // repeated worker snapshot.
    const auto parallelRoot = directory / "parallel";
    const auto parallelRead = parallelRoot / "read";
    const auto parallelWinWrite = parallelRoot / "win";
    const auto parallelLossWrite = parallelRoot / "loss";
    std::filesystem::create_directories(parallelRead);
    std::filesystem::create_directories(parallelWinWrite);
    std::filesystem::create_directories(parallelLossWrite);
    std::filesystem::copy_file(aliasWrite, parallelRead / aliasFile,
        std::filesystem::copy_options::overwrite_existing);
    if (!applyLocalGame(parallelRead, parallelWinWrite, alias, true) ||
        !applyLocalGame(parallelRead, parallelLossWrite, alias, false)) {
        std::cerr << "parallel history snapshots failed\n";
        std::filesystem::remove_all(directory);
        return 1;
    }
    auto mergedRuns = loadHistory(parallelRead / aliasFile);
    mergedRuns.merge(protodd::bwapi::readBoundedFile(
        parallelWinWrite / aliasFile,
        protodd::OpponentHistory::maximumSerializedBytes).contents);
    const auto parallelLoss = protodd::bwapi::readBoundedFile(
        parallelLossWrite / aliasFile,
        protodd::OpponentHistory::maximumSerializedBytes);
    mergedRuns.merge(parallelLoss.contents);
    mergedRuns.merge(protodd::bwapi::readBoundedFile(
        parallelWinWrite / aliasFile,
        protodd::OpponentHistory::maximumSerializedBytes).contents);
    const auto parallelCounts = mergedRuns.lookup(
        alias, map, protodd::OpeningStyle::standard);
    if (parallelCounts.wins != 3 || parallelCounts.losses != 2) {
        std::cerr << "parallel snapshot merge lost or double-counted outcomes\n";
        std::filesystem::remove_all(directory);
        return 1;
    }
    std::filesystem::remove_all(directory);
    return 0;
}
