#include "protodd/CallbackBoundary.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main() {
    std::vector<std::pair<std::string, std::string>> failures;
    const auto report = [&failures](const std::string_view name,
                                    const std::string_view message) {
        failures.emplace_back(name, message);
    };

    bool callbackRan = false;
    protodd::invokeCallbackBoundary("success", [&] { callbackRan = true; }, report);
    require(callbackRan && failures.empty(), "successful callback changed failure state");

    // Every overridden BWAPI callback is routed through the same boundary in
    // ProtoddModule; RaceBot's lifecycle callbacks use it as well.
    for (const auto* name : {
             "onStart", "onEnd", "onFrame", "onSendText", "onUnitDiscover",
             "onUnitShow", "onUnitDestroy", "onUnitMorph", "onUnitRenegade",
             "onUnitCreate", "onUnitComplete", "RaceBot.onStart", "RaceBot.onEnd",
             "RaceBot.onFrame", "onFrame.runFrame", "onFrame.frameRead",
             "onFrame.budget", "onFrame.performance", "onFrame.audit"}) {
        const auto failuresBefore = failures.size();
        protodd::invokeCallbackBoundary(name, [] {
            throw std::runtime_error("injected public-callback failure");
        }, report);
        require(failures.size() == failuresBefore + 1,
                "callback exception did not reach the failure reporter");
        require(failures.back().first == name &&
                    failures.back().second == "injected public-callback failure",
                "callback injection lost its identity or message");
    }

    // These are the failure-prone callback stages that must never unwind into
    // the tournament host: startup, observation, diagnostics, history output,
    // and reporting from game shutdown.
    for (const auto* name : {"startup", "observation", "diagnostics",
                             "history-output", "reporting"}) {
        protodd::invokeCallbackBoundary(name, [] {
            throw std::runtime_error("injected callback failure");
        }, report);
        require(failures.back().first == name, "callback failure lost its callback name");
        require(failures.back().second == "injected callback failure",
                "callback failure lost its exception message");
    }

    protodd::invokeCallbackBoundary("unknown", [] { throw 42; }, report);
    require(failures.back().first == "unknown" &&
                failures.back().second == "unknown exception",
            "unknown exception was not classified");

    // Even a broken failure reporter must not allow an exception across the
    // host boundary.
    protodd::invokeCallbackBoundary("reporter", [] {
        throw std::runtime_error("body failed");
    }, [](std::string_view, std::string_view) {
        throw std::runtime_error("reporter failed");
    });

    return 0;
}
