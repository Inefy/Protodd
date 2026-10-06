#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

namespace protodd::bwapi {

struct BoundedFileRead {
    std::string contents;
    bool withinLimit{true};
};

// Read one byte past the limit so an oversized file is rejected without ever
// retaining more than the configured bound. A missing file remains an empty,
// valid result, matching the runtime's optional-input behavior.
inline BoundedFileRead readBoundedFile(const std::filesystem::path& path,
                                       const std::size_t maximumBytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    if (maximumBytes == std::numeric_limits<std::size_t>::max()) return {{}, false};

    BoundedFileRead result;
    std::array<char, 4096> buffer{};
    while (result.contents.size() <= maximumBytes) {
        const auto remaining = maximumBytes + 1 - result.contents.size();
        const auto request = std::min(remaining, buffer.size());
        input.read(buffer.data(), static_cast<std::streamsize>(request));
        const auto bytesRead = input.gcount();
        if (bytesRead > 0)
            result.contents.append(buffer.data(), static_cast<std::size_t>(bytesRead));
        if (result.contents.size() > maximumBytes) {
            result.contents.clear();
            result.withinLimit = false;
            return result;
        }
        if (input.bad()) {
            result.contents.clear();
            result.withinLimit = false;
            return result;
        }
        if (input.eof()) return result;
        if (!input) {
            result.contents.clear();
            result.withinLimit = false;
            return result;
        }
    }
    result.contents.clear();
    result.withinLimit = false;
    return result;
}

inline std::string readBoundedFirstLine(const std::filesystem::path& path,
                                        const std::size_t maximumBytes = 4096) {
    auto result = readBoundedFile(path, maximumBytes);
    if (!result.withinLimit) return {};
    if (const auto newline = result.contents.find('\n'); newline != std::string::npos)
        result.contents.resize(newline);
    if (!result.contents.empty() && result.contents.back() == '\r')
        result.contents.pop_back();
    return result.contents;
}

}  // namespace protodd::bwapi
