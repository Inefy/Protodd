#pragma once

#include <exception>
#include <string_view>
#include <utility>

namespace protodd {

// Keep exceptions raised by bot code inside the host's callback boundary.
// The failure reporter is also contained because logging must never leak an
// exception back through BWAPI.
template <class Callback, class FailureHandler>
void invokeCallbackBoundary(const std::string_view callbackName,
                            Callback&& callback,
                            FailureHandler&& onFailure) noexcept {
    try {
        std::forward<Callback>(callback)();
    } catch (const std::exception& error) {
        try {
            std::forward<FailureHandler>(onFailure)(callbackName, error.what());
        } catch (...) {
        }
    } catch (...) {
        try {
            std::forward<FailureHandler>(onFailure)(callbackName, "unknown exception");
        } catch (...) {
        }
    }
}

}  // namespace protodd
