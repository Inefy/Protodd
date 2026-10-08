#include "protodd/WholeGameObservation.hpp"

#include <iostream>
#include <map>
#include <set>
#include <string_view>
#include <vector>

namespace {
int failures{};

void expect(const bool condition, const std::string_view message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

protodd::whole_observation::Entity entity(
    const int relation, const bool visible, const int lastSeen) {
    protodd::whole_observation::Entity result;
    result.relation = relation;
    result.visible = visible;
    result.lastSeen = lastSeen;
    return result;
}
}  // namespace

int main() {
    using namespace protodd::whole_observation;
    static_assert(maximumRememberedEntities == 4096);

    std::map<int, Entity> entities{
        {1, entity(0, true, 1)},
        {2, entity(1, true, 20)},
        {3, entity(1, false, 2)},
        {4, entity(2, false, 5)},
        {5, entity(1, false, 8)},
    };
    const auto pruned = pruneStaleEntities(entities, 3);
    expect(pruned.withinLimit && entities.size() == 3 &&
               pruned.removedIds == std::vector<int>{3, 4} &&
               entities.contains(1) && entities.contains(2) && entities.contains(5),
           "oldest stale non-own entities are evicted while own and visible entities stay");

    std::map<int, int> engineIds{{101, 1}, {202, 3}, {303, 5}};
    std::set<int> published{1, 3, 5};
    forgetUnretainedTokens(engineIds, published, entities);
    expect(engineIds == std::map<int, int>{{101, 1}, {303, 5}} &&
               published == std::set<int>{1, 5},
           "evicted or forgotten entity tokens leave engine-ID and published registries");

    std::map<int, Entity> allCurrent{
        {1, entity(0, true, 1)},
        {2, entity(0, true, 2)},
        {3, entity(1, true, 3)},
    };
    const auto overflow = pruneStaleEntities(allCurrent, 2);
    expect(!overflow.withinLimit && overflow.removedIds.empty() &&
               allCurrent.size() == 3,
           "a live observation above the cap is reported for safe optional-runtime shutdown");

    return failures == 0 ? 0 : 1;
}
