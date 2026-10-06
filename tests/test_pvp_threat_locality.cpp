#include "protodd/Information.hpp"
#include "protodd/Strategy.hpp"
#include "protodd/UnitCatalog.hpp"

#include <iostream>
#include <optional>
#include <string_view>

namespace {

int failures = 0;

void check(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

protodd::UnitSnapshot unit(
    const protodd::UnitId id,
    const protodd::UnitKind kind,
    const bool ours,
    const protodd::Position position) {
    protodd::UnitSnapshot result;
    result.id = id;
    result.kind = kind;
    result.ours = ours;
    result.position = position;
    result.lastPosition = position;
    result.lastSeen = 7 * 60 * 24;
    result.visible = true;
    result.completed = true;
    result.hitPoints = 100;
    result.maxHitPoints = 100;
    result.topSpeed = 4.0;
    return result;
}

protodd::GameState stableMirror() {
    using namespace protodd;
    GameState state;
    state.frame = 7 * 60 * 24;
    state.self.id = 1;
    state.self.race = Race::protoss;
    state.enemy.id = 2;
    state.enemy.race = Race::protoss;

    auto nexus = unit(1, UnitKind::nexus, true, {256, 256});
    nexus.role = UnitRole::resourceDepot;
    state.self.units.push_back(nexus);
    for (int i = 0; i < 2; ++i) {
        auto cannon = unit(10 + i, UnitKind::photonCannon, true,
                           {360 + i * 72, 360});
        cannon.role = UnitRole::staticDefense;
        state.self.units.push_back(cannon);
    }
    for (int i = 0; i < 5; ++i) {
        auto zealot = unit(20 + i, UnitKind::zealot, true,
                           {440 + i * 24, 440});
        zealot.role = UnitRole::groundArmy;
        state.self.units.push_back(zealot);
    }
    auto core = unit(30, UnitKind::cyberneticsCore, true, {400, 256});
    core.role = UnitRole::production;
    state.self.units.push_back(core);
    for (int i = 0; i < 18; ++i) {
        auto probe = unit(40 + i, UnitKind::probe, true,
                          {480 + (i % 4) * 16, 480 + (i / 4) * 16});
        probe.role = UnitRole::worker;
        state.self.units.push_back(probe);
    }

    BaseSnapshot main;
    main.id = 1;
    main.center = {256, 256};
    main.ownerId = state.self.id;
    main.startLocation = true;
    main.mineralsRemaining = 8000;

    BaseSnapshot natural;
    natural.id = 2;
    natural.center = {1500, 256};
    natural.ownerId = -1;
    natural.mineralsRemaining = 8000;
    natural.mineralPatches = 8;
    natural.geysers = 1;

    BaseSnapshot enemyMain;
    enemyMain.id = 3;
    enemyMain.center = {3300, 3400};
    enemyMain.ownerId = state.enemy.id;
    enemyMain.startLocation = true;
    enemyMain.mineralsRemaining = 8000;
    state.bases = {main, natural, enemyMain};

    for (int i = 0; i < 1; ++i) {
        auto gateway = unit(100 + i, UnitKind::gateway, false,
                           {3300 + i * 96, 3400});
        gateway.role = UnitRole::production;
        state.enemy.units.push_back(gateway);
    }
    return state;
}

protodd::StrategicPlan planAt(
    protodd::GameState state,
    const protodd::Position current,
    const protodd::Position previous,
    const std::optional<double> globalAggression = std::nullopt) {
    using namespace protodd;
    for (int i = 0; i < 2; ++i) {
        auto zealot = unit(200 + i, UnitKind::zealot, false, current);
        zealot.lastPosition = previous;
        state.enemy.units.push_back(zealot);
    }
    OpponentModel opponent;
    opponent.update(state);
    auto threat = opponent.assessment();
    if (globalAggression) threat.aggression = *globalAggression;
    return StrategyEngine{}.plan(state, threat);
}

void testRemoteArmyDoesNotCreateHomeEmergency() {
    using namespace protodd;
    const auto plan = planAt(stableMirror(), {3300, 3400}, {3300, 3400});
    check(plan.posture != Posture::defend &&
              plan.name.find("two-gate emergency defense") == std::string::npos,
          "enemy production-area Zealots do not trigger home emergency defense");
    check(plan.desiredBases >= 2,
          "enemy production-area Zealots do not block a safe natural expansion");
}

void testHomeContactRequestsHomeDefense() {
    using namespace protodd;
    const auto plan = planAt(stableMirror(), {300, 256}, {3300, 3400});
    check(plan.posture == Posture::defend &&
              plan.name.find("two-gate emergency defense") != std::string::npos,
          "the same Zealot group requests home defense when it reaches the main");
    check(plan.desiredBases == 1,
          "home contact suppresses the next-base transition");
}

void testGlobalAggressionDoesNotCreateHomeBreach() {
    using namespace protodd;
    const auto plan = planAt(stableMirror(), {3300, 3400}, {3300, 3400}, 0.9);
    check(plan.posture != Posture::defend &&
              plan.name.find("two-gate emergency defense") == std::string::npos,
          "global opponent aggression without local contact does not trigger home defense");
}

void testNaturalApproachIsLocalToTheExpansion() {
    using namespace protodd;
    const auto plan = planAt(stableMirror(), {1600, 256}, {1880, 256});
    check(plan.posture != Posture::defend &&
              plan.name.find("two-gate emergency defense") == std::string::npos,
          "Zealots approaching the natural do not masquerade as a home breach");
    check(plan.desiredBases == 1,
          "Zealots approaching the natural postpone that exposed expansion");
}

}  // namespace

int main() {
    testRemoteArmyDoesNotCreateHomeEmergency();
    testHomeContactRequestsHomeDefense();
    testGlobalAggressionDoesNotCreateHomeBreach();
    testNaturalApproachIsLocalToTheExpansion();
    if (failures != 0) return 1;
    std::cout << "PvP threat locality checks passed\n";
    return 0;
}
