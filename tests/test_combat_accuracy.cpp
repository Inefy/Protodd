#include "protodd/Combat.hpp"
#include "protodd/Navigation.hpp"
#include "protodd/Squads.hpp"
#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string_view>

namespace {
using namespace protodd;
int failures{};
int checks{};
void check(const bool condition, const std::string_view message) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
UnitSnapshot unit(const UnitId id, const UnitKind kind, const bool ours,
                  const Position position = {512, 512}) {
    UnitSnapshot result;
    result.id = id; result.kind = kind; result.ours = ours; result.position = position;
    result.visible = result.completed = result.powered = true;
    result.hitPoints = result.maxHitPoints = 1000;
    result.topSpeed = 4.0;
    return result;
}
double remainingValue(const UnitSnapshot& target, const int damage) {
    return unitStats(target.kind).combatValue *
        static_cast<double>(target.durability() - damage) /
        static_cast<double>(target.maxHitPoints + target.maxShields);
}
void cadence() {
    auto attacker = unit(1, UnitKind::marine, true);
    auto target = unit(2, UnitKind::dragoon, false, {600, 512});
    attacker.groundWeapon = {5, 8, 0, 128, DamageType::normal, false, true};
    for (const auto cooldown : {7, 8, 15, 22, 30}) {
        for (const auto delay : {0, 5, 17}) {
            attacker.groundWeapon.cooldown = cooldown;
            attacker.weaponCooldown = delay;
            const auto result = CombatEvaluator{}.evaluate(std::array{attacker}, std::array{target}, 1, 0);
            const auto hits = 1 + (336 - delay) / cooldown;
            check(std::abs(result.simulatedEnemyRemaining - remainingValue(target, hits * 5)) < 1e-9,
                  "observed first-shot delay and weapon cadence retain individual frames");
        }
    }
    target.topSpeed = 0;
    target.position = {660, 512}; // 20 pixels outside range: first contact at frame five.
    attacker.groundWeapon.cooldown = 8;
    attacker.weaponCooldown = 2;
    auto result = CombatEvaluator{}.evaluate(std::array{attacker}, std::array{target}, 1, 0);
    check(std::abs(result.simulatedEnemyRemaining - remainingValue(target, (1 + (336 - 5) / 8) * 5)) < 1e-9,
          "approach delays the first shot without accumulating free volleys");
    attacker.weaponCooldown = 17;
    result = CombatEvaluator{}.evaluate(std::array{attacker}, std::array{target}, 1, 0);
    check(std::abs(result.simulatedEnemyRemaining - remainingValue(target, (1 + (336 - 17) / 8) * 5)) < 1e-9,
          "weapon reload still governs when contact occurs first");
    target.position = {4000, 512};
    result = CombatEvaluator{}.evaluate(std::array{attacker}, std::array{target}, 1, 0);
    check(result.simulatedEnemyRemaining == unitStats(target.kind).combatValue,
          "contact beyond the simulation horizon cannot deal damage");

    auto first = unit(3, UnitKind::marine, true);
    first.hitPoints = first.maxHitPoints = 40;
    first.groundWeapon = {40, 15, 0, 128, DamageType::normal, false, true};
    auto second = first; second.id = 4; second.ours = false;
    result = CombatEvaluator{}.evaluate(std::array{first}, std::array{second}, 1, 0);
    check(result.simulatedFriendlyRemaining == 0 && result.simulatedEnemyRemaining == 0,
          "lethal volleys on the same frame remain simultaneous");
    second.weaponCooldown = 1;
    result = CombatEvaluator{}.evaluate(std::array{first}, std::array{second}, 1, 0);
    check(result.simulatedFriendlyRemaining > 0 && result.simulatedEnemyRemaining == 0,
          "a killed unit cannot return a volley on the following frame");
}
void consumableAttackers() {
    for (const auto kind : {UnitKind::scourge, UnitKind::spiderMine, UnitKind::infestedTerran}) {
        auto attacker = unit(1, kind, true);
        attacker.flying = kind == UnitKind::scourge;
        attacker.groundWeapon = {110, 1, 0, 32, DamageType::normal, false, true};
        attacker.airWeapon = {110, 1, 0, 32, DamageType::normal, true, false};
        auto target = unit(2, UnitKind::dragoon, false);
        target.flying = attacker.flying;
        const auto result = CombatEvaluator{}.evaluate(std::array{attacker}, std::array{target}, 1, 0);
        check(result.simulatedFriendlyRemaining == 0, "suicide attack consumes its attacker");
        check(std::abs(result.simulatedEnemyRemaining - remainingValue(target, 110)) < 1e-9,
              "suicide attacker contributes exactly one impact");
        const auto fast = CombatEvaluator{}.evaluate(std::array{attacker}, std::array{target}, 1, 0, false);
        check(fast.friendlyPower < unitStats(kind).combatValue * 2,
              "static estimate does not interpret suicide cooldown as sustained damage");
        target.invincible = true;
        const auto noShot = CombatEvaluator{}.evaluate(std::array{attacker}, std::array{target}, 1, 0);
        check(noShot.simulatedFriendlyRemaining == unitStats(kind).combatValue,
              "suicide unit survives when no legal impact occurs");
    }
}
void workerDefense() {
    auto raider = unit(1, UnitKind::zealot, true);
    auto worker = unit(2, UnitKind::scv, false);
    worker.groundWeapon = {5, 15, 0, 32, DamageType::normal, false, true};
    const auto result = CombatEvaluator{}.evaluate(std::array{raider}, std::array{worker}, 1, 0);
    check(result.enemyPower > 0, "local worker weapons contribute defensive power");
    check(result.simulatedFriendlyRemaining < unitStats(raider.kind).combatValue,
          "a worker surround is not simulated as a harmless mineral line");
    raider.flying = true;
    check(CombatEvaluator{}.evaluate(std::array{raider}, std::array{worker}, 1, 0).enemyPower == 0,
          "workers do not create an anti-air threat");
    raider.flying = false;
    worker.loaded = true;
    check(CombatEvaluator{}.evaluate(std::array{raider}, std::array{worker}, 1, 0).enemyPower == 0,
          "loaded workers cannot contribute defense");
    worker.loaded = false;
    GameState state;
    state.frame = 5000;
    state.self.units = {raider};
    state.enemy.units = {worker};
    StrategicPlan plan; plan.attackTarget = {900, 512};
    const auto defenders = [&]() {
        // The live adapter supplies an army-only enemy span separately from
        // the complete legal observations, including workers, in GameState.
        const auto squads = SquadPlanner{}.form(state, state.self.units, {}, plan, {100, 512});
        return squads.empty() ? std::vector<UnitSnapshot>{} : squads.front().enemies;
    };
    check(defenders().size() == 1, "live squad formation includes an immediate worker surround");
    state.enemy.units[0].position.x += 140;
    check(defenders().empty(), "distant mining workers do not inflate the local enemy army");
    state.enemy.units[0].orderTargetId = raider.id;
    check(defenders().size() == 1, "visible workers approaching a squad member to attack count as defenders");
    state.enemy.units[0].visible = false;
    check(defenders().empty(), "fogged workers cannot create a speculative surround");
    state.enemy.units[0] = worker;
    state.self.units[0].flying = true;
    check(defenders().empty(), "worker defense cannot pin an air squad");
}
void unavailableThreats() {
    auto goon = unit(1, UnitKind::dragoon, true);
    goon.groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
    goon.weaponCooldown = 20;
    auto neighbor = goon; neighbor.id = 2; neighbor.position.y += 32;
    auto target = unit(10, UnitKind::probe, false, {680, 512});
    auto threat = unit(11, UnitKind::reaver, false, {650, 512});
    threat.groundWeapon = {100, 60, 0, 256, DamageType::normal, false, true};
    threat.ammo = 5;
    CombatEstimate engage; engage.decision = FightDecision::engage; engage.ratio = 2;
    const auto commands = [&](const UnitSnapshot& candidate) {
        return TacticalController{}.control(std::array{goon, neighbor}, std::array{target, candidate},
            engage, {800, 512}, {200, 512}, InfluenceMap{}, goon.position, 3);
    };
    const auto hasSpacing = [](const std::vector<Command>& orders) {
        return std::ranges::any_of(orders, [](const Command& c) { return c.source == "splash-spacing"; });
    };
    check(hasSpacing(commands(threat)), "armed Reaver still triggers splash spacing");
    for (int variant = 0; variant < 4; ++variant) {
        auto unavailable = threat;
        if (variant == 0) unavailable.loaded = true;
        if (variant == 1) unavailable.hallucination = true;
        if (variant == 2) unavailable.ammo = 0;
        if (variant == 3) unavailable.invincible = true;
        check(!hasSpacing(commands(unavailable)), "unavailable splash cannot scatter the frontline");
    }
    engage.decision = FightDecision::kite;
    threat.kind = UnitKind::marine;
    threat.groundWeapon = {6, 15, 0, 128, DamageType::normal, false, true};
    const auto hasKite = [](const std::vector<Command>& orders) {
        return std::ranges::any_of(orders, [](const Command& c) { return c.source == "combat-kite"; });
    };
    check(hasKite(commands(threat)), "real pursuer still triggers reload kiting");
    threat.loaded = true;
    check(!hasKite(commands(threat)), "loaded passenger cannot cause reload kiting");
    threat.loaded = false; threat.hallucination = true;
    check(!hasKite(commands(threat)), "hallucinated pursuer cannot cause reload kiting");
    threat.hallucination = false; threat.kind = UnitKind::photonCannon;
    threat.powered = false;
    check(!hasKite(commands(threat)), "unpowered Cannon cannot cause reload kiting");
}
void terrainSpacing() {
    auto goon = unit(1, UnitKind::dragoon, true, {496, 496});
    goon.groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
    goon.weaponCooldown = 20;
    auto neighbor = goon; neighbor.id = 2; neighbor.position.y += 32;
    auto reaver = unit(10, UnitKind::reaver, false, {680, 496});
    reaver.ammo = 5;
    reaver.groundWeapon = {100, 60, 0, 256, DamageType::normal, false, true};
    std::vector<std::uint8_t> cells(32 * 32, 0);
    cells[15 * 32 + 15] = 1; // Only the observed origin is traversable.
    NavigationGrid nav{32, 32, 32, cells};
    CombatEstimate engage; engage.decision = FightDecision::engage;
    const auto orders = TacticalController{}.control(std::array{goon, neighbor}, std::array{reaver},
        engage, {800, 496}, {200, 496}, InfluenceMap{}, goon.position, 3, false, {},
        TacticalIntent::battle, {}, &nav);
    check(std::ranges::none_of(orders, [&nav, &goon](const Command& command) {
        return command.actor == goon.id && command.source == "splash-spacing" &&
            !nav.lineWalkable(goon.position, command.targetPosition);
    }), "splash spacing cannot choose a blocked terrain segment");

    // The far end of this wall has an opening. A global hasPath check accepts
    // the old preferred destination, but a short reload move cannot cross it.
    cells.assign(32 * 32, 1);
    for (int x = 0; x < 31; ++x) cells[14 * 32 + static_cast<std::size_t>(x)] = 0;
    nav = NavigationGrid{32, 32, 32, cells};
    check(!nav.findPath(goon.position, {496, 432}).empty() &&
          !nav.lineWalkable(goon.position, {496, 432}), "cliff fixture has a distant legal detour");
    const auto rerouted = TacticalController{}.control(std::array{goon, neighbor}, std::array{reaver},
        engage, {800, 496}, {200, 496}, InfluenceMap{}, goon.position, 3, false, {},
        TacticalIntent::battle, {}, &nav);
    check(std::ranges::any_of(rerouted, [&nav, &goon](const Command& command) {
        return command.actor == goon.id && command.source == "splash-spacing" &&
            nav.lineWalkable(goon.position, command.targetPosition);
    }), "splash spacing chooses a reachable alternative beside the cliff");
}
void deterministicScheduling() {
    auto a = unit(1, UnitKind::dragoon, true);
    a.groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
    auto b = a; b.id = 2; b.weaponCooldown = 5;
    auto x = unit(10, UnitKind::marine, false, {600, 512});
    x.groundWeapon = {6, 15, 0, 128, DamageType::normal, false, true};
    auto y = x; y.id = 11; y.weaponCooldown = 7;
    CombatEvaluator evaluator;
    const auto original = evaluator.evaluate(std::array{a, b}, std::array{x, y}, 1, 0);
    const auto reordered = evaluator.evaluate(std::array{b, a}, std::array{y, x}, 1, 0);
    check(original.simulatedEnemyRemaining == reordered.simulatedEnemyRemaining &&
          original.simulatedFriendlyRemaining == reordered.simulatedFriendlyRemaining,
          "event scheduling is invariant to observation container order");
    a.groundWeapon = {}; x.groundWeapon = {};
    const auto passive = evaluator.evaluate(std::array{a}, std::array{x}, 1, 0);
    check(passive.simulatedFriendlyRemaining == unitStats(a.kind).combatValue &&
          passive.simulatedEnemyRemaining == unitStats(x.kind).combatValue,
          "an encounter with no eligible weapon events preserves both armies");
}
void benchmark() {
    std::vector<UnitSnapshot> friendly, enemy;
    for (int i = 0; i < 96; ++i) {
        auto a = unit(i, UnitKind::dragoon, true, {400 + i % 12 * 20, 400 + i / 12 * 20});
        a.hitPoints = a.maxHitPoints = 100;
        a.groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
        a.weaponCooldown = i % 30;
        friendly.push_back(a);
        auto b = a; b.id += 100; b.ours = false; b.kind = UnitKind::zergling;
        b.position.x += 240; b.hitPoints = b.maxHitPoints = 35;
        b.groundWeapon = {5, 8, 0, 32, DamageType::normal, false, true};
        b.weaponCooldown = i % 8;
        enemy.push_back(b);
    }
    std::vector<double> timings;
    double checksum{};
    for (int i = 0; i < 55; ++i) {
        const auto start = std::chrono::steady_clock::now();
        const auto result = CombatEvaluator{}.evaluate(friendly, enemy, 1.1, 0.1);
        const auto end = std::chrono::steady_clock::now();
        checksum += result.ratio;
        if (i >= 5) timings.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }
    std::ranges::sort(timings);
    std::cout << "96v96 simulation: median_ms=" << timings[25]
              << " max_ms=" << timings.back() << " checksum=" << checksum << '\n';
}
} // namespace

int main(const int argc, char**) {
    if (argc > 1) { benchmark(); return 0; }
    cadence(); consumableAttackers(); workerDefense(); unavailableThreats(); terrainSpacing();
    deterministicScheduling();
    if (failures) std::cerr << failures << " combat accuracy checks failed\n";
    else std::cout << checks << " combat accuracy checks passed\n";
    return failures == 0 ? 0 : 1;
}
