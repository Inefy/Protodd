#include "BoundedFile.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() /
        ("protodd-bounded-file-" + std::to_string(unique));
    std::filesystem::create_directories(directory);
    const auto exactPath = directory / "exact.txt";
    const auto oversizedPath = directory / "oversized.txt";
    const auto modePath = directory / "mode.txt";
    {
        std::ofstream output(exactPath, std::ios::binary);
        output << "1234";
    }
    {
        std::ofstream output(oversizedPath, std::ios::binary);
        output << "12345";
    }
    {
        std::ofstream output(modePath, std::ios::binary);
        output << "shadow\r\nignored\n";
    }

    const auto exact = protodd::bwapi::readBoundedFile(exactPath, 4);
    const auto oversized = protodd::bwapi::readBoundedFile(oversizedPath, 4);
    const auto missing = protodd::bwapi::readBoundedFile(directory / "missing.txt", 4);
    const auto mode = protodd::bwapi::readBoundedFirstLine(modePath, 32);
    std::filesystem::remove_all(directory);

    if (!exact.withinLimit || exact.contents != "1234" || oversized.withinLimit ||
        !oversized.contents.empty() || !missing.withinLimit || !missing.contents.empty() ||
        mode != "shadow") {
        std::cerr << "bounded file reader failed its boundary checks\n";
        return 1;
    }
    return 0;
}
