#include "protodd/Strategy.hpp"
#include "protodd/MacroPlanner.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace protodd;

int failures{};
constexpr int mapEdge = 4095;

enum class MapTransform {
    identity,
    rotate90,
    rotate180,
    rotate270,
    reflectX,
    reflectY,
    reflectDiagonal,
    reflectAntiDiagonal,
};

void check(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

Position transformPosition(const Position point, const MapTransform transform) {
    if (!point.valid()) return point;
    switch (transform) {
        case MapTransform::identity: return point;
        case MapTransform::rotate90: return {mapEdge - point.y, point.x};
        case MapTransform::rotate180: return {mapEdge - point.x, mapEdge - point.y};
        case MapTransform::rotate270: return {point.y, mapEdge - point.x};
        case MapTransform::reflectX: return {mapEdge - point.x, point.y};
        case MapTransform::reflectY: return {point.x, mapEdge - point.y};
        case MapTransform::reflectDiagonal: return {point.y, point.x};
        case MapTransform::reflectAntiDiagonal:
            return {mapEdge - point.y, mapEdge - point.x};
    }
    return point;
}

GameState transformState(
    const GameState& source,
    const MapTransform transform,
    const std::string_view mapName) {
    auto state = source;
    state.mapName = mapName;
    for (auto* player : {&state.self, &state.enemy}) {
        for (auto& unit : player->units) {
            unit.position = transformPosition(unit.position, transform);
            unit.lastPosition = transformPosition(unit.lastPosition, transform);
        }
    }
    for (auto& base : state.bases) {
        base.center = transformPosition(base.center, transform);
        base.mineralLine = transformPosition(base.mineralLine, transform);
        base.defense.anchor = transformPosition(base.defense.anchor, transform);
        base.defense.entrance = transformPosition(base.defense.entrance, transform);
        base.defense.left = transformPosition(base.defense.left, transform);
        base.defense.right = transformPosition(base.defense.right, transform);
    }
    for (auto& storm : state.storms) storm = transformPosition(storm, transform);
    for (auto& sweep : state.scannerSweeps) sweep = transformPosition(sweep, transform);
    return state;
}

UnitSnapshot unit(
    const UnitId id,
    const UnitKind kind,
    const bool ours,
    const Position position,
    const UnitRole role) {
    UnitSnapshot result;
    result.id = id;
    result.kind = kind;
    result.ours = ours;
    result.position = position;
    result.lastPosition = position;
    result.role = role;
    result.visible = true;
    result.completed = true;
    result.hitPoints = 100;
    result.maxHitPoints = 100;
    return result;
}

GameState unseenMapScenario() {
    GameState state;
    state.frame = 12 * 60 * 24;
    state.mapWidthPixels = mapEdge + 1;
    state.mapHeightPixels = mapEdge + 1;
    state.mapName = "held-out-reference";
    state.self.id = 1;
    state.self.race = Race::protoss;
    state.self.supplyUsed = 64;
    state.self.supplyTotal = 100;
    state.self.minerals = 450;
    state.enemy.id = 2;
    state.enemy.race = Race::terran;
    state.bases = {
        {1, {512, 512}, {512, 560}, 8000, 5000, 1, 0, true, false, 8, 1},
        {2, {1800, 512}, {1800, 560}, 8000, 5000, -1, 0, false, false, 8, 1},
        {3, {3584, 3520}, {3584, 3568}, 0, 0, -1, 0, true, false, 8, 1},
    };
    state.self.units = {
        unit(1, UnitKind::nexus, true, {512, 512}, UnitRole::resourceDepot),
        unit(2, UnitKind::pylon, true, {448, 448}, UnitRole::supply),
        unit(3, UnitKind::gateway, true, {640, 448}, UnitRole::production),
        unit(4, UnitKind::cyberneticsCore, true, {704, 448}, UnitRole::production),
        unit(5, UnitKind::roboticsFacility, true, {640, 576}, UnitRole::production),
        unit(6, UnitKind::observatory, true, {704, 576}, UnitRole::production),
        unit(7, UnitKind::roboticsSupportBay, true, {768, 576}, UnitRole::production),
        unit(8, UnitKind::observer, true, {544, 624}, UnitRole::detector),
    };
    for (int i = 0; i < 20; ++i) {
        state.self.units.push_back(unit(
            100 + i, UnitKind::probe, true, {480 + (i % 5) * 16, 592 + (i / 5) * 16},
            UnitRole::worker));
    }
    for (int i = 0; i < 6; ++i) {
        state.self.units.push_back(unit(
            200 + i, UnitKind::dragoon, true, {800 + (i % 3) * 48, 720 + (i / 3) * 48},
            UnitRole::groundArmy));
    }
    state.enemy.units.push_back(
        unit(300, UnitKind::marine, false, {1200, 512}, UnitRole::groundArmy));
    state.enemy.units.back().groundWeapon = {6, 15, 0, 128, DamageType::normal,
                                              false, true, 1};
    return state;
}

GameState pvpGoldenState() {
    GameState state;
    state.frame = 4 * 60 * 24 + 20 * 24;
    state.mapWidthPixels = 4096;
    state.mapHeightPixels = 4096;
    state.mapName = "frozen-pvp-state";
    state.self.id = 1;
    state.self.race = Race::protoss;
    state.self.supplyUsed = 40;
    state.self.supplyTotal = 72;
    state.self.minerals = 520;
    state.self.gas = 180;
    state.enemy.id = 2;
    state.enemy.race = Race::protoss;
    state.bases = {
        {1, {256, 256}, {256, 304}, 8000, 5000, 1, 0, true, false, 8, 1},
        {2, {1500, 256}, {1500, 304}, 8000, 5000, -1, 0, false, false, 8, 1},
        {3, {3300, 3400}, {3300, 3448}, 8000, 5000, 2, 0, true, false, 8, 1},
    };
    state.self.units = {
        unit(1, UnitKind::nexus, true, {256, 256}, UnitRole::resourceDepot),
        unit(2, UnitKind::pylon, true, {160, 160}, UnitRole::supply),
        unit(3, UnitKind::gateway, true, {400, 256}, UnitRole::production),
        unit(4, UnitKind::assimilator, true, {480, 256}, UnitRole::production),
        unit(5, UnitKind::cyberneticsCore, true, {520, 256}, UnitRole::production),
    };
    for (int i = 0; i < 16; ++i) {
        state.self.units.push_back(unit(20 + i, UnitKind::probe, true,
            {480 + (i % 4) * 16, 480 + (i / 4) * 16}, UnitRole::worker));
    }
    for (int i = 0; i < 2; ++i) {
        state.self.units.push_back(unit(40 + i, UnitKind::zealot, true,
            {360 + i * 32, 320}, UnitRole::groundArmy));
        state.enemy.units.push_back(unit(100 + i, UnitKind::gateway, false,
            {3300 + i * 96, 3400}, UnitRole::production));
    }
    state.enemy.units.push_back(unit(102, UnitKind::zealot, false,
        {320, 256}, UnitRole::groundArmy));
    return state;
}

GameState pvzGoldenState() {
    GameState state;
    state.frame = 7 * 60 * 24;
    state.mapWidthPixels = 4096;
    state.mapHeightPixels = 4096;
    state.mapName = "frozen-pvz-state";
    state.self.id = 1;
    state.self.race = Race::protoss;
    state.self.supplyUsed = 76;
    state.self.supplyTotal = 120;
    state.self.minerals = 720;
    state.self.gas = 420;
    state.enemy.id = 2;
    state.enemy.race = Race::zerg;
    state.bases = {
        {1, {256, 256}, {256, 304}, 8000, 5000, 1, 0, true, false, 8, 1},
        {2, {1500, 256}, {1500, 304}, 8000, 5000, 1, 0, false, false, 8, 1},
        {3, {3300, 3400}, {3300, 3448}, 8000, 5000, 2, 0, true, false, 8, 1},
    };
    state.self.units = {
        unit(1, UnitKind::nexus, true, {256, 256}, UnitRole::resourceDepot),
        unit(2, UnitKind::nexus, true, {1500, 256}, UnitRole::resourceDepot),
        unit(3, UnitKind::pylon, true, {160, 160}, UnitRole::supply),
        unit(4, UnitKind::gateway, true, {400, 256}, UnitRole::production),
        unit(5, UnitKind::cyberneticsCore, true, {520, 256}, UnitRole::production),
        unit(6, UnitKind::forge, true, {360, 360}, UnitRole::production),
        unit(7, UnitKind::roboticsFacility, true, {640, 256}, UnitRole::production),
        unit(8, UnitKind::photonCannon, true, {420, 400}, UnitRole::staticDefense),
    };
    for (int i = 0; i < 20; ++i) {
        state.self.units.push_back(unit(30 + i, UnitKind::probe, true,
            {480 + (i % 5) * 16, 480 + (i / 5) * 16}, UnitRole::worker));
    }
    for (int i = 0; i < 6; ++i) {
        state.self.units.push_back(unit(60 + i, UnitKind::dragoon, true,
            {600 + (i % 3) * 48, 640 + (i / 3) * 48}, UnitRole::groundArmy));
    }
    for (int i = 0; i < 5; ++i) {
        state.enemy.units.push_back(unit(100 + i, i < 3 ? UnitKind::zergling : UnitKind::hydralisk,
            false, {300 + i * 32, 300}, UnitRole::groundArmy));
    }
    return state;
}

void appendPosition(std::ostringstream& out, const Position position) {
    out << position.x << ',' << position.y << ';';
}

void appendTiming(std::ostringstream& out, const CriticalGoalTiming& timing) {
    out << timing.requiredByFrame << ',' << timing.expectedReadyFrame << ','
        << timing.slackFrames << ',' << timing.feasible << ','
        << static_cast<int>(timing.reservationReason) << ';';
}

void appendSite(std::ostringstream& out, const ConstructionTaskSite& site) {
    out << site.id << ',' << site.baseId << ',';
    appendPosition(out, site.anchor);
}

std::string canonicalOutput(
    const StrategicPlan& plan,
    const std::vector<MacroAction>& actions) {
    std::ostringstream out;
    out << std::setprecision(17) << std::quoted(plan.name) << ';'
        << static_cast<int>(plan.posture) << ',' << plan.desiredBases << ','
        << plan.desiredWorkers << ',' << plan.desiredGasWorkers << ','
        << plan.attackThreshold << ',' << plan.minimumAttackSize << ',';
    appendPosition(out, plan.rallyPoint);
    appendPosition(out, plan.attackTarget);
    out << plan.maximumBases << ',' << plan.prioritizeReinforcements << ','
        << plan.requireMobileDetection << ',' << plan.sustainEconomy << ','
        << plan.breakContainment << ',';
    appendPosition(out, plan.expansionTarget);
    out << plan.deferExpansion << ',' << plan.expansionProtectionRequired << ','
        << plan.harassmentDrops << ',' << plan.recoveringLastNexus << ','
        << plan.estimatedMiningRunwayFrames << ';';
    for (const auto& goal : plan.goals) {
        out << static_cast<int>(goal.goal) << ',' << static_cast<int>(goal.target) << ','
            << goal.desiredCount << ',' << goal.priority << ',' << goal.blocking << ','
            << std::quoted(goal.reason) << ',' << static_cast<int>(goal.technology) << ','
            << goal.allowMineralFallback << ',' << goal.harassmentOnly << ',';
        appendSite(out, goal.constructionSite);
        appendTiming(out, goal.timing);
    }
    out << '|';
    for (const auto& target : plan.composition)
        out << static_cast<int>(target.kind) << ',' << target.weight << ';';
    out << '|';
    for (const auto& action : actions) {
        out << static_cast<int>(action.action) << ',' << static_cast<int>(action.target) << ','
            << action.priority << ',' << action.minerals << ',' << action.gas << ','
            << action.reserved << ',' << std::quoted(action.reason) << ','
            << static_cast<int>(action.technology) << ',' << action.blocksLowerPriority << ','
            << action.executable << ',';
        appendSite(out, action.constructionSite);
        appendTiming(out, action.timing);
    }
    return out.str();
}

std::string fingerprint(const std::string_view value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

void comparePlans(
    const StrategicPlan& expected,
    const StrategicPlan& actual,
    const MapTransform transform,
    const std::string_view label) {
    const auto prefix = std::string(label) + ": ";
    check(expected.name == actual.name, prefix + "strategy name is position invariant");
    check(expected.posture == actual.posture &&
              expected.desiredBases == actual.desiredBases &&
              expected.desiredWorkers == actual.desiredWorkers &&
              expected.desiredGasWorkers == actual.desiredGasWorkers &&
              expected.attackThreshold == actual.attackThreshold &&
              expected.minimumAttackSize == actual.minimumAttackSize &&
              expected.maximumBases == actual.maximumBases &&
              expected.prioritizeReinforcements == actual.prioritizeReinforcements &&
              expected.requireMobileDetection == actual.requireMobileDetection &&
              expected.sustainEconomy == actual.sustainEconomy &&
              expected.breakContainment == actual.breakContainment &&
              expected.deferExpansion == actual.deferExpansion &&
              expected.recoveringLastNexus == actual.recoveringLastNexus &&
              expected.harassmentDrops == actual.harassmentDrops &&
              expected.estimatedMiningRunwayFrames == actual.estimatedMiningRunwayFrames,
          prefix + "macro and tactical decisions match");
    check(transformPosition(expected.attackTarget, transform) == actual.attackTarget,
          prefix + "enemy-search target follows the spawn transformation");
    check(transformPosition(expected.rallyPoint, transform) == actual.rallyPoint,
          prefix + "rally point follows the spawn transformation");
    check(transformPosition(expected.expansionTarget, transform) == actual.expansionTarget,
          prefix + "expansion site follows the spawn transformation");
    check(expected.goals.size() == actual.goals.size(), prefix + "goal count matches");
    if (expected.goals.size() == actual.goals.size()) {
        for (std::size_t i = 0; i < expected.goals.size(); ++i) {
            const auto& left = expected.goals[i];
            const auto& right = actual.goals[i];
            check(left.goal == right.goal && left.target == right.target &&
                      left.desiredCount == right.desiredCount &&
                      left.priority == right.priority && left.blocking == right.blocking &&
                      left.reason == right.reason && left.technology == right.technology &&
                      left.allowMineralFallback == right.allowMineralFallback &&
                      left.harassmentOnly == right.harassmentOnly &&
                      left.constructionSite.id == right.constructionSite.id &&
                      left.constructionSite.baseId == right.constructionSite.baseId &&
                      transformPosition(left.constructionSite.anchor, transform) ==
                          right.constructionSite.anchor,
                  prefix + "production goal " + std::to_string(i) + " matches");
        }
    }
    check(expected.composition.size() == actual.composition.size(),
          prefix + "composition length matches");
    if (expected.composition.size() == actual.composition.size()) {
        for (std::size_t i = 0; i < expected.composition.size(); ++i) {
            check(expected.composition[i].kind == actual.composition[i].kind &&
                      std::abs(expected.composition[i].weight -
                               actual.composition[i].weight) < 0.0001,
                  prefix + "army composition " + std::to_string(i) + " matches");
        }
    }
}

}  // namespace

int main() {
    const auto referenceState = unseenMapScenario();
    const ThreatAssessment threat{};
    const auto reference = StrategyEngine{}.plan(referenceState, threat);
    check(reference.attackTarget == Position{3584, 3520},
          "an unscouted enemy spawn supplies a concrete map-independent search target");
    check(reference.expansionTarget == Position{1800, 512},
          "the legal gas natural supplies a concrete geometry-relative expansion target");
    check(reference.rallyPoint.valid(), "map-independent planning supplies a rally point");

    const std::array names{
        "Held-out-2P-Reference", "Held-out-2P-Rotated-90", "Held-out-2P-Rotated-180",
        "Held-out-2P-Rotated-270", "Held-out-2P-Reflected-X", "Held-out-2P-Reflected-Y",
        "Held-out-4P-Reflected-Diagonal", "Held-out-4P-Reflected-AntiDiagonal"};
    const std::array transforms{
        MapTransform::identity, MapTransform::rotate90, MapTransform::rotate180,
        MapTransform::rotate270, MapTransform::reflectX, MapTransform::reflectY,
        MapTransform::reflectDiagonal, MapTransform::reflectAntiDiagonal};
    for (std::size_t i = 0; i < transforms.size(); ++i) {
        const auto equivalent = transformState(referenceState, transforms[i], names[i]);
        const auto plan = StrategyEngine{}.plan(equivalent, threat);
        comparePlans(reference, plan, transforms[i], names[i]);
    }

    for (const auto name : {"Python", "Destination", "unknown-map-7f31"}) {
        auto alternateName = referenceState;
        alternateName.mapName = name;
        comparePlans(reference, StrategyEngine{}.plan(alternateName, threat),
                     MapTransform::identity, std::string("map-name independence: ") + name);
    }

    auto pvtEarly = unseenMapScenario();
    pvtEarly.mapName = "frozen-pvt-early";
    pvtEarly.frame = 4 * 60 * 24;
    std::erase_if(pvtEarly.self.units, [](const UnitSnapshot& item) {
        return item.kind == UnitKind::dragoon;
    });
    pvtEarly.enemy.units.front().position = {4000, 3500};
    auto pvtPressure = unseenMapScenario();
    pvtPressure.mapName = "frozen-pvt-pressure";
    pvtPressure.frame = 8 * 60 * 24;
    pvtPressure.enemy.units.front().position = {620, 512};
    pvtPressure.enemy.units.push_back(
        unit(301, UnitKind::siegeTank, false, {700, 600}, UnitRole::groundArmy));

    auto pvzEarly = pvzGoldenState();
    pvzEarly.mapName = "frozen-pvz-early";
    pvzEarly.frame = 3 * 60 * 24;
    std::erase_if(pvzEarly.self.units, [](const UnitSnapshot& item) {
        return item.kind == UnitKind::nexus && item.id == 2;
    });
    pvzEarly.enemy.units[0].position = {420, 280};
    pvzEarly.enemy.units[1].position = {452, 280};
    auto pvzLate = pvzGoldenState();
    pvzLate.mapName = "frozen-pvz-late";
    pvzLate.frame = 14 * 60 * 24;
    pvzLate.self.supplyUsed = 150;
    pvzLate.self.supplyTotal = 180;
    pvzLate.enemy.units.push_back(
        unit(120, UnitKind::mutalisk, false, {1000, 800}, UnitRole::airArmy));

    auto pvpOpening = pvpGoldenState();
    pvpOpening.mapName = "frozen-pvp-opening";
    pvpOpening.frame = 2 * 60 * 24;
    pvpOpening.enemy.units.back().position = {3300, 3400};
    auto pvpQuietMid = pvpGoldenState();
    pvpQuietMid.mapName = "frozen-pvp-quiet-mid";
    pvpQuietMid.frame = 7 * 60 * 24;
    pvpQuietMid.enemy.units = {
        unit(110, UnitKind::gateway, false, {3300, 3400}, UnitRole::production),
        unit(111, UnitKind::cyberneticsCore, false, {3400, 3400}, UnitRole::production),
        unit(112, UnitKind::reaver, false, {3200, 3300}, UnitRole::groundArmy),
    };
    auto pvpLate = pvpQuietMid;
    pvpLate.mapName = "frozen-pvp-late";
    pvpLate.frame = 12 * 60 * 24;
    pvpLate.self.supplyUsed = 150;
    pvpLate.self.supplyTotal = 180;
    pvpLate.self.units.push_back(
        unit(80, UnitKind::dragoon, true, {900, 700}, UnitRole::groundArmy));

    const std::array frozenStates{
        std::pair<std::string_view, GameState>{"pvt-early", std::move(pvtEarly)},
        std::pair<std::string_view, GameState>{"pvt-pressure", std::move(pvtPressure)},
        std::pair<std::string_view, GameState>{"pvt-mid", unseenMapScenario()},
        std::pair<std::string_view, GameState>{"pvz-early", std::move(pvzEarly)},
        std::pair<std::string_view, GameState>{"pvz-mid", pvzGoldenState()},
        std::pair<std::string_view, GameState>{"pvz-late", std::move(pvzLate)},
        std::pair<std::string_view, GameState>{"pvp-opening", std::move(pvpOpening)},
        std::pair<std::string_view, GameState>{"pvp-quiet-mid", std::move(pvpQuietMid)},
        std::pair<std::string_view, GameState>{"pvp-late", std::move(pvpLate)},
    };
    // These frozen hashes cover every plan field, ordered production goal and
    // composition row, plus every ordered MacroPlanner action and its timing.
    // Intentional strategy tuning must update this corpus in a separate change.
    const std::array<std::string_view, 9> expectedFingerprints{
        "eb8aa8b2aad6b40b", "e7d213f3c7cfbdb9", "c291d4d844bb2696",
        "d65f284e7de1106b", "deef03c9c5267193", "b8b97d619a01ffe1",
        "765d09ab50d6163e", "4cf1ec887e243bef", "b72635afaa2289db"};
    for (std::size_t i = 0; i < frozenStates.size(); ++i) {
        const auto& [name, state] = frozenStates[i];
        const auto plan = StrategyEngine{}.plan(state, ThreatAssessment{});
        ResourceLedger ledger{state.self.minerals, state.self.gas};
        const auto actions = MacroPlanner{}.reconcile(state, plan, ledger);
        const auto actual = fingerprint(canonicalOutput(plan, actions));
        check(actual == expectedFingerprints[i],
              std::string("plan and command snapshot matches frozen ") +
                  std::string(name) + " state; digest=" + actual);
    }

    if (failures == 0) {
        std::cout << "Strategy symmetry and unseen-map checks passed\n";
        return 0;
    }
    std::cerr << failures << " strategy symmetry check(s) failed\n";
    return 1;
}
