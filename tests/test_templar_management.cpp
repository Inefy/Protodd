#include "protodd/TemplarManagement.hpp"

#include <array>
#include <iostream>
#include <string_view>

namespace {
int failures{};
int checks{};
void check(const bool condition, const std::string_view message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
}

int main() {
    using namespace protodd;

    const std::array fightCasters{180, 21, 28, 42};
    const auto urgentPair = selectTemplarMergePair(
        fightCasters, templarMergePolicy(true, true));
    check(urgentPair == std::array<std::size_t, 2>{1, 2},
          "an imminent Storm fight preserves its ready caster and merges the two emptiest extras");

    const std::array onlyTwoDepleted{20, 31};
    check(!selectTemplarMergePair(
               onlyTwoDepleted, templarMergePolicy(true, true)),
          "an imminent Storm fight keeps a depleted pair available for the next energy cycle");
    const auto idlePair = selectTemplarMergePair(
        onlyTwoDepleted, templarMergePolicy(true, false));
    check(idlePair == std::array<std::size_t, 2>{0, 1},
          "outside an imminent fight, two exhausted Storm casters can form an Archon");

    const std::array noStormMission{104, 156};
    const auto irrelevantPair = selectTemplarMergePair(
        noStormMission, templarMergePolicy(false, false));
    check(irrelevantPair == std::array<std::size_t, 2>{0, 1},
          "when the current technology package has no Storm, unused energy does not strand a mergeable pair");

    const std::array readyAndLow{75, 74, 10, 74};
    const auto boundaryPair = selectTemplarMergePair(
        readyAndLow, templarMergePolicy(true, false));
    check(boundaryPair == std::array<std::size_t, 2>{1, 2},
          "exactly Storm-ready energy is excluded while low-energy extras are merged first");

    const std::array distantLowest{4, 12, 19};
    const auto reachableFallback = selectTemplarMergePair(
        distantLowest, templarMergePolicy(false, false),
        [](const std::size_t first, const std::size_t second) {
            return first != 0 || second != 1;
        });
    check(reachableFallback == std::array<std::size_t, 2>{0, 2},
          "an unreachable lowest-energy pair falls back to the next legal merge pair");

    std::cout << checks - failures << "/" << checks
              << " templar management checks passed\n";
    return failures == 0 ? 0 : 1;
}
