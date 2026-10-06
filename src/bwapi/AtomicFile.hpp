#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#else
#include <unistd.h>
#endif

namespace protodd::bwapi {

// A process- and invocation-unique lowercase hexadecimal token suitable for
// stable CSV outcome identifiers and same-directory temporary file names.
inline std::string uniqueToken() {
    static std::atomic<std::uint32_t> sequence{};
    const auto ticks = static_cast<std::uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());
#if defined(_WIN32)
    const auto processId = static_cast<std::uint32_t>(::GetCurrentProcessId());
#else
    const auto processId = static_cast<std::uint32_t>(::getpid());
#endif
    const auto call = sequence.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(8) << processId
           << std::setw(16) << ticks << std::setw(8) << call;
    return output.str();
}

// Write a complete sibling file first, then atomically replace the destination.
// A failed write or rename leaves the previous destination intact.
inline bool writeAtomicFile(const std::filesystem::path& destination,
                            const std::string_view contents) {
    auto temporary = destination;
    temporary += ".tmp-" + uniqueToken();
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.flush();
    if (!output) {
        output.close();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    output.close();
    if (output.fail()) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }

#if defined(_WIN32)
    const auto replaced = ::MoveFileExW(
        temporary.c_str(), destination.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code renameError;
    std::filesystem::rename(temporary, destination, renameError);
    const auto replaced = !renameError;
#endif
    if (!replaced) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
    }
    return replaced;
}

}  // namespace protodd::bwapi
