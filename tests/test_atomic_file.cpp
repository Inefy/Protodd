#include "AtomicFile.hpp"

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
    std::filesystem::remove_all(directory);
    return 0;
}
