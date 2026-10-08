#include "protodd/Scouting.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <string_view>

namespace {

using namespace protodd;

bool check(const bool condition, const std::string_view message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

UnitSnapshot unit(const UnitId id, const UnitKind kind, const Position position,
                  const bool ours, const Frame frame) {
    UnitSnapshot result;
    result.id = id;
    result.kind = kind;
    result.role = kind == UnitKind::nexus || kind == UnitKind::hatchery
        ? UnitRole::resourceDepot : UnitRole::worker;
    result.position = position;
    result.lastPosition = position;
    result.lastSeen = result.firstSeen = frame;
    result.ours = ours;
    result.visible = result.detected = result.completed = true;
    result.hitPoints = result.maxHitPoints = kind == UnitKind::nexus ? 1000 : 40;
    return result;
}

GameState state() {
    GameState result;
    result.frame = 6 * 60 * 24;
    result.self.id = 1;
    result.enemy.id = 2;
    result.enemy.race = Race::terran;
    result.bases.push_back({1, {512, 512}, {512, 560}, 8000, 5000,
                            1, result.frame, true, false, 8, 1});
    result.bases.push_back({2, {1800, 512}, {1800, 560}, 8000, 5000,
                            2, result.frame - 4 * 60 * 24, true, false, 8, 1});
    auto nexus = unit(1, UnitKind::nexus, {512, 512}, true, result.frame);
    result.self.units.push_back(nexus);
    for (int i = 0; i < 12; ++i)
        result.self.units.push_back(unit(10 + i, UnitKind::probe,
            {560 + (i % 4) * 12, 560 + (i / 4) * 12}, true, result.frame));
    result.enemy.units.push_back(unit(50, UnitKind::commandCenter,
        {1800, 512}, false, result.frame - 4 * 60 * 24));
    result.enemy.units.back().role = UnitRole::resourceDepot;
    result.enemy.units.back().lastSeen = result.frame - 4 * 60 * 24;
    return result;
}

}  // namespace

int main() {
    using namespace protodd;
    bool passed = true;
    auto snapshot = state();
    ThreatAssessment threat;
    threat.uncertainty = 0.8;
    InfluenceMap influence;
    influence.update(snapshot);

    ScoutManager available;
    const auto worker = available.selectWorkerScout(snapshot, threat);
    passed &= check(worker >= 0, "safe follow-up worker scouting remains affordable after finding the enemy main");
    const UnitId scout[]{worker};
    const auto orders = available.assign(snapshot, scout, influence, threat);
    passed &= check(orders.size() == 1 && orders.front().purpose == ScoutPurpose::checkTech &&
                        !available.informationGap().unresolved,
                    "an available safe scout receives a later technology update mission");

    ScoutManager unavailable;
    static_cast<void>(unavailable.assign(snapshot, {}, influence, threat));
    const auto& gap = unavailable.informationGap();
    passed &= check(gap.unresolved && gap.purpose == ScoutPurpose::checkTech &&
                        gap.target == Position{1800, 512} &&
                        gap.deadlineFrame == snapshot.frame && gap.risk > 0.5 &&
                        gap.reason == ScoutGapReason::noAvailableScout,
                    "a blocked follow-up records its target, expired deadline, and information risk");

    GameState many;
    many.frame = 30000;
    many.self.id = 1;
    many.enemy.id = 2;
    many.mapWidthPixels = many.mapHeightPixels = 4096;
    many.self.units = {unit(100, UnitKind::probe, {256, 512}, true, many.frame)};
    for (int index = 0; index < 32; ++index)
        many.bases.push_back({10 + index, {3008, 128 + index * 96},
            {3008, 160 + index * 96}, 8000, 5000, -1, 0, false, false, 8, 1});
    many.bases.push_back({2, {640, 512}, {640, 560}, 8000, 5000, 2, 0, true, false, 8, 1});
    std::vector<std::uint8_t> cells(128 * 128, 1);
    for (int y = 0; y < 128; ++y) cells[static_cast<std::size_t>(y * 128 + 64)] = 0;
    NavigationGrid separated(128, 128, 32, std::move(cells));
    InfluenceMap empty;
    empty.update(many);
    const auto before = NavigationGrid::diagnosticsForCurrentThread().searches;
    ScoutManager ranked;
    const auto rankedOrders = ranked.assign(many, std::array<UnitId, 1>{100}, empty, threat, &separated);
    const auto searched = NavigationGrid::diagnosticsForCurrentThread().searches - before;
    passed &= check(rankedOrders.size() == 1 && rankedOrders.front().informationTarget == Position{640, 512} &&
                    rankedOrders.front().purpose == ScoutPurpose::checkTech &&
                    !ranked.informationGap().unresolved && searched == 0,
                    "a proven best nearby mission prunes all dominated distant route searches without losing scouting");
    std::ranges::reverse(many.bases);
    const auto beforeReverse = NavigationGrid::diagnosticsForCurrentThread().searches;
    ScoutManager reversed;
    const auto reversedOrders = reversed.assign(many, std::array<UnitId, 1>{100}, empty, threat, &separated);
    passed &= check(reversedOrders.size() == 1 && !rankedOrders.empty() &&
                    reversedOrders.front().informationTarget == rankedOrders.front().informationTarget &&
                    reversedOrders.front().score == rankedOrders.front().score &&
                    NavigationGrid::diagnosticsForCurrentThread().searches == beforeReverse,
                    "dominated-route pruning preserves the chosen mission under candidate reversal");
    std::cout << "dominated scout route searches=" << searched << '\n';

    many.bases = {{3, {3008, 1024}, {3008, 1056}, 8000, 5000, -1, 0, false, false, 8, 1}};
    const auto beforeUnsafe = NavigationGrid::diagnosticsForCurrentThread().searches;
    ScoutManager disconnected;
    const auto noRoute = disconnected.assign(many, std::array<UnitId, 1>{100}, empty, threat, &separated);
    const auto unsafeSearches = NavigationGrid::diagnosticsForCurrentThread().searches - beforeUnsafe;
    passed &= check(noRoute.empty() && disconnected.informationGap().unresolved &&
                    disconnected.informationGap().target == Position{3008, 1024} &&
                    disconnected.informationGap().reason == ScoutGapReason::unsafeRoute &&
                    unsafeSearches > 0 && unsafeSearches <= 5,
                    "information-gap reporting reuses the failed route proof without hiding the unresolved mission");
    std::cout << "unsafe mission route searches=" << unsafeSearches << '\n';

    // Equal mission values must still be resolved by proved travel distance,
    // and equal final scores must retain the original candidate-order tie.
    many.bases = {
        {4, {1200, 512}, {1200, 560}, 8000, 5000, -1, 0, false, false, 8, 1},
        {5, {640, 512}, {640, 560}, 8000, 5000, -1, 0, false, false, 8, 1},
    };
    ScoutManager closest;
    const auto closestOrder = closest.assign(many, std::array<UnitId, 1>{100}, empty, threat, &separated);
    passed &= check(closestOrder.size() == 1 && closestOrder.front().informationTarget == Position{640, 512},
                    "candidate rank does not substitute for exact proved mission travel score");
    many.bases = {
        {6, {256, 896}, {256, 928}, 8000, 5000, -1, 0, false, false, 8, 1},
        {7, {640, 512}, {640, 560}, 8000, 5000, -1, 0, false, false, 8, 1},
    };
    ScoutManager tied;
    const auto tiedOrder = tied.assign(many, std::array<UnitId, 1>{100}, empty, threat, &separated);
    passed &= check(tiedOrder.size() == 1 && tiedOrder.front().informationTarget == Position{256, 896},
                    "exact equal-score missions retain the preexisting candidate-order tie break");
    many.bases = {
        {8, {3008, 1024}, {3008, 1056}, 8000, 5000, 2, 0, true, false, 8, 1},
        {9, {640, 512}, {640, 560}, 8000, 5000, -1, 0, false, false, 8, 1},
    };
    const auto beforeFallback = NavigationGrid::diagnosticsForCurrentThread().searches;
    ScoutManager fallback;
    const auto fallbackOrder = fallback.assign(many, std::array<UnitId, 1>{100}, empty, threat, &separated);
    passed &= check(fallbackOrder.size() == 1 &&
                    fallbackOrder.front().informationTarget == Position{640, 512} &&
                    fallback.informationGap().target == Position{3008, 1024} &&
                    fallback.informationGap().reason == ScoutGapReason::unsafeRoute &&
                    NavigationGrid::diagnosticsForCurrentThread().searches - beforeFallback <= 5,
                    "an unproved high-value route never prevents a lower-value safe mission from progressing");
    return passed ? 0 : 1;
}
