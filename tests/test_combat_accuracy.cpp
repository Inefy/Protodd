#include "protodd/Combat.hpp"
#include "protodd/Navigation.hpp"
#include "protodd/Squads.hpp"
#include "protodd/UnitCatalog.hpp"
#include "protodd/UnitMemory.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string_view>
#include <vector>

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
    check(engagementSimulationWithinBudget(0, 0) &&
          engagementSimulationWithinBudget(16, 12) &&
          engagementSimulationWithinBudget(24, 8),
          "bounded simulation preserves small and asymmetric engagements");
    check(!engagementSimulationWithinBudget(80, 116) &&
          !engagementSimulationWithinBudget(16, 16) &&
          !engagementSimulationWithinBudget(32, 1),
          "bounded simulation defers interactions beyond its callback work budget");
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
    check(result.simulatedFrames == 336 && !result.simulatedOutcomeReached,
          "unresolved engagements report the bounded horizon explicitly");

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
void boundedApproach() {
    auto runner = unit(1, UnitKind::marine, true, {272, 272});
    runner.groundWeapon = {5, 30, 0, 32, DamageType::normal, false, true};
    auto fleeing = unit(2, UnitKind::dragoon, false, {400, 272});
    fleeing.hitPoints = fleeing.maxHitPoints = 1000;
    fleeing.lastPosition = {380, 272};
    const auto sameSpeed = CombatEvaluator{}.evaluate(
        std::array{runner}, std::array{fleeing}, 1, 0);
    check(sameSpeed.simulatedEnemyRemaining == unitStats(fleeing.kind).combatValue,
          "a ranged target observed fleeing at equal speed is not pulled into range");
    runner.topSpeed = 8.0;
    const auto fasterChaser = CombatEvaluator{}.evaluate(
        std::array{runner}, std::array{fleeing}, 1, 0);
    check(fasterChaser.simulatedEnemyRemaining < unitStats(fleeing.kind).combatValue,
          "a faster pursuer can still close on a fleeing ranged target");

    constexpr int width = 24;
    constexpr int height = 16;
    constexpr int cell = 32;
    auto target = unit(10, UnitKind::dragoon, false, {400, 272});
    target.hitPoints = target.maxHitPoints = 5;
    target.groundWeapon = {};
    runner.topSpeed = 4.0;
    runner.hitPoints = runner.maxHitPoints = 1000;
    runner.groundWeapon = {5, 30, 0, 32, DamageType::normal, false, true};
    const auto openCells = std::vector<std::uint8_t>(width * height, 1);
    const NavigationGrid open{width, height, cell, openCells};
    const auto straight = CombatEvaluator{}.evaluate(
        std::array{runner}, std::array{target}, 1, 0, true, &open);

    auto detourCells = openCells;
    for (int y = 0; y < height; ++y)
        if (y != 1) detourCells[static_cast<std::size_t>(y * width + 10)] = 0;
    const NavigationGrid detour{width, height, cell, detourCells};
    const auto routed = CombatEvaluator{}.evaluate(
        std::array{runner}, std::array{target}, 1, 0, true, &detour);
    check(routed.simulatedOutcomeReached && routed.simulatedFrames > straight.simulatedFrames,
          "ground attackers reach a target through a bounded terrain route");

    auto blockedCells = openCells;
    for (int y = 0; y < height; ++y)
        blockedCells[static_cast<std::size_t>(y * width + 10)] = 0;
    const NavigationGrid blocked{width, height, cell, blockedCells};
    const auto unreachable = CombatEvaluator{}.evaluate(
        std::array{runner}, std::array{target}, 1, 0, true, &blocked);
    check(!unreachable.simulatedOutcomeReached &&
          unreachable.simulatedEnemyLoss == 0.0 &&
          (!unreachable.simulationRoutingDeferred || unreachable.confidence < 0.8),
          "a blocked or unproved ground approach grants no simulated damage or confident terrain outcome");
    auto disconnectedTarget = target;
    disconnectedTarget.position = {720, 272};
    const auto provenDisconnected = CombatEvaluator{}.evaluate(
        std::array{runner}, std::array{disconnectedTarget}, 1, 0, true, &blocked);
    check(!provenDisconnected.simulationRoutingDeferred &&
          !provenDisconnected.simulatedOutcomeReached &&
          provenDisconnected.simulatedEnemyRemaining == unitStats(target.kind).combatValue,
          "fully exhausted disconnected terrain retains a proven no-contact simulation");

    auto packed = std::array{runner, runner, runner};
    packed[0].id = 1;
    packed[1].id = 2;
    packed[2].id = 3;
    target.hitPoints = target.maxHitPoints = 15;
    const auto openChoke = CombatEvaluator{}.evaluate(
        packed, std::array{target}, 1, 0, true, &open);
    auto chokeCells = openCells;
    for (int y = 0; y < height; ++y)
        if (y != 8) chokeCells[static_cast<std::size_t>(y * width + 10)] = 0;
    const NavigationGrid choke{width, height, cell, chokeCells};
    const auto throughChoke = CombatEvaluator{}.evaluate(
        packed, std::array{target}, 1, 0, true, &choke);
    check(throughChoke.simulatedOutcomeReached &&
          throughChoke.simulatedFrames > openChoke.simulatedFrames,
          "a one-cell choke staggers ground attackers instead of granting immediate group DPS");
    std::cout << "bounded approach frames: straight=" << straight.simulatedFrames
              << " detour=" << routed.simulatedFrames
              << " open-choke=" << openChoke.simulatedFrames
              << " one-cell-choke=" << throughChoke.simulatedFrames << '\n';

    auto ranged = runner;
    ranged.position = {272, 272};
    ranged.groundWeapon.maxRange = 160;
    target.hitPoints = target.maxHitPoints = 5;
    const auto beforeRanged = NavigationGrid::diagnosticsForCurrentThread().searches;
    const auto rangedAcrossWall = CombatEvaluator{}.evaluate(
        std::array{ranged}, std::array{target}, 1, 0, true, &blocked);
    check(rangedAcrossWall.simulatedOutcomeReached && rangedAcrossWall.simulatedFrames == 0 &&
          !rangedAcrossWall.simulationRoutingDeferred &&
          NavigationGrid::diagnosticsForCurrentThread().searches == beforeRanged,
          "an observed legal in-range ranged shot crosses impassable ground without routing");

    NavigationGrid largeOpen(512, 448, 8, std::vector<std::uint8_t>(512 * 448, 1));
    std::vector<UnitSnapshot> manyAttackers(12, runner);
    std::vector<UnitSnapshot> manyDefenders(12, target);
    for (int index = 0; index < 12; ++index) {
        manyAttackers[index].id = 100 + index;
        manyAttackers[index].position = {272, 272 + index * 8};
        manyDefenders[index].id = 200 + index;
        manyDefenders[index].position = {1008, 272 + index * 8};
        manyDefenders[index].hitPoints = manyDefenders[index].maxHitPoints = 1000;
    }
    const auto boundedGeometry = CombatEvaluator{}.evaluate(
        manyAttackers, manyDefenders, 1, 0, true, &largeOpen);
    const auto staticGeometry = CombatEvaluator{}.evaluate(
        manyAttackers, manyDefenders, 1, 0, false, &largeOpen);
    check(boundedGeometry.simulationRoutingDeferred &&
          boundedGeometry.simulatedFrames == 0 && !boundedGeometry.simulatedOutcomeReached &&
          boundedGeometry.simulatedFriendlyDeaths == 0 && boundedGeometry.simulatedEnemyDeaths == 0 &&
          boundedGeometry.ratio == staticGeometry.ratio &&
          boundedGeometry.confidence == staticGeometry.confidence &&
          boundedGeometry.decision == staticGeometry.decision,
          "exhausted aggregate clearance work discards partial damage and uses the low-confidence static fallback");

    std::vector<std::uint8_t> largeBlockedCells(512 * 448, 1);
    for (int y = 0; y < 448; ++y) largeBlockedCells[y * 512 + 256] = 0;
    NavigationGrid largeBlocked(512, 448, 8, std::move(largeBlockedCells));
    manyAttackers.front().position = {1008, 272};
    manyDefenders.front().position = {3008, 272};
    manyDefenders.front().groundWeapon = {20, 30, 0, 128, DamageType::normal, false, true};
    const auto beforeBounded = NavigationGrid::diagnosticsForCurrentThread().searches;
    const auto incompleteRoute = CombatEvaluator{}.evaluate(
        std::span{manyAttackers}.first(1), std::span{manyDefenders}.first(1),
        1, 0, true, &largeBlocked);
    check(incompleteRoute.simulationRoutingDeferred && incompleteRoute.simulatedFrames == 0 &&
          incompleteRoute.confidence < 0.8 &&
          NavigationGrid::diagnosticsForCurrentThread().searches - beforeBounded <=
              maximumEngagementRouteSearches,
          "an incomplete bounded terrain search reduces authority instead of proving the opponent harmless");
}
void combatStatusAdmissibility() {
    CombatEvaluator evaluator;
    auto dragoon = unit(1, UnitKind::dragoon, true);
    dragoon.groundWeapon = {20, 30, 0, 128, DamageType::normal, false, true};
    auto darkTemplar = unit(2, UnitKind::darkTemplar, false, {560, 512});
    darkTemplar.cloaked = true;
    darkTemplar.detected = false;
    darkTemplar.groundWeapon = {40, 30, 0, 128, DamageType::normal, false, true};
    const auto hiddenStatic = evaluator.evaluate(
        std::array{dragoon}, std::array{darkTemplar}, 1, 0, false);
    const auto hiddenFight = evaluator.evaluate(
        std::array{dragoon}, std::array{darkTemplar}, 1, 0);
    check(hiddenStatic.friendlyPower == 0.0,
          "static power does not credit fire against an undetected cloaked target");
    check(hiddenFight.simulatedEnemyRemaining == unitStats(darkTemplar.kind).combatValue,
          "simulation leaves an undetected cloaked enemy untargeted after detector loss");
    darkTemplar.detected = true;
    const auto detectedFight = evaluator.evaluate(
        std::array{dragoon}, std::array{darkTemplar}, 1, 0);
    check(detectedFight.friendlyPower > 0.0 &&
          detectedFight.simulatedEnemyRemaining < unitStats(darkTemplar.kind).combatValue,
          "restored detection returns the cloaked target to legal fire and damage");

    auto ordinary = darkTemplar;
    ordinary.kind = UnitKind::dragoon;
    ordinary.cloaked = false;
    const auto visibleOrdinary = evaluator.evaluate(
        std::array{dragoon}, std::array{ordinary}, 1, 0);
    const auto fogged = reconcileEnemyMemory(ordinary, std::nullopt, 24, false, false).snapshot;
    const auto rememberedFight = evaluator.evaluate(
        std::array{dragoon}, std::array{fogged}, 1, 0);
    const auto rememberedStatic = evaluator.evaluate(
        std::array{dragoon}, std::array{fogged}, 1, 0, false);
    check(!fogged.visible && !fogged.detected && !fogged.requiresDetection() &&
          rememberedStatic.friendlyPower > 0.0 && rememberedStatic.enemyPower > 0.0 &&
          rememberedFight.friendlyPower == visibleOrdinary.friendlyPower &&
          rememberedFight.enemyPower == visibleOrdinary.enemyPower &&
          rememberedFight.simulatedEnemyRemaining == visibleOrdinary.simulatedEnemyRemaining,
          "ordinary fog memory preserves prospective combat on both sides instead of requiring detection");
    const auto hiddenCommands = TacticalController{}.control(
        std::array{dragoon}, std::array{fogged}, rememberedFight,
        {900, 512}, {400, 512}, InfluenceMap{});
    check(std::ranges::none_of(hiddenCommands, [&fogged](const Command& command) {
              return command.type == CommandType::attackUnit && command.targetUnit == fogged.id;
          }), "prospective fog combat never authorizes a hidden attack target");
    for (const auto kind : {UnitKind::darkTemplar, UnitKind::lurker, UnitKind::spiderMine}) {
        auto covert = ordinary;
        covert.kind = kind;
        covert.detected = false;
        covert.groundWeapon = {}; // Isolate our fire from a mine's own suicide impact.
        const auto memory = reconcileEnemyMemory(covert, std::nullopt, 24, false, false).snapshot;
        const auto estimate = evaluator.evaluate(std::array{dragoon}, std::array{memory}, 1, 0);
        check(memory.requiresDetection() && estimate.friendlyPower == 0.0 &&
              estimate.simulatedEnemyRemaining == unitStats(kind).combatValue,
              "remembered inherently covert targets still block predicted fire without detection");
    }

    auto marine = unit(10, UnitKind::marine, true);
    marine.hitPoints = marine.maxHitPoints = 40;
    marine.groundWeapon = {6, 15, 0, 128, DamageType::normal, false, true};
    auto opponent = marine;
    opponent.id = 11;
    opponent.ours = false;
    const auto checkDisabledEnemy = [&](const UnitKind kind,
                                        const std::string_view effect) {
        auto disabled = opponent;
        disabled.kind = kind;
        disabled.disabled = true; // The BWAPI adapter normalizes all three effects here.
        const auto estimate = evaluator.evaluate(std::array{marine}, std::array{disabled}, 1, 0);
        check(estimate.enemyPower == 0.0 && estimate.simulatedFriendlyRemaining > 0.0,
              effect);
    };
    checkDisabledEnemy(UnitKind::marine,
        "a stasised opponent adds no defensive damage");
    checkDisabledEnemy(UnitKind::siegeTank,
        "a locked-down mechanical opponent adds no defensive damage");
    checkDisabledEnemy(UnitKind::dragoon,
        "a maelstrommed opponent adds no defensive damage");
    auto disabledMarine = marine;
    disabledMarine.disabled = true;
    const auto disabledFriendly = evaluator.evaluate(
        std::array{disabledMarine}, std::array{opponent}, 1, 0);
    check(disabledFriendly.friendlyPower == 0.0 &&
          disabledFriendly.simulatedEnemyRemaining == unitStats(opponent.kind).combatValue,
          "a disabled friendly unit contributes no fictitious offensive coverage");

    auto cannon = unit(20, UnitKind::photonCannon, true);
    cannon.groundWeapon = {20, 30, 0, 128, DamageType::normal, false, true};
    auto target = unit(21, UnitKind::marine, false, {560, 512});
    target.hitPoints = target.maxHitPoints = 5;
    target.groundWeapon = {};
    const auto powered = evaluator.evaluate(std::array{cannon}, std::array{target}, 1, 0);
    check(powered.friendlyPower > 0.0 && powered.simulatedEnemyRemaining == 0.0,
          "a powered Cannon contributes defensive coverage and fires in the simulation");
    cannon.powered = false;
    const auto unpowered = evaluator.evaluate(std::array{cannon}, std::array{target}, 1, 0);
    check(unpowered.friendlyPower == 0.0 && unpowered.simulatedEnemyRemaining ==
              unitStats(target.kind).combatValue,
          "power loss removes Cannon threat from both static and simulated combat");
}
void splashAndSpellCalibration() {
    auto reaver = unit(1, UnitKind::reaver, true, {512, 512});
    reaver.ammo = 1;
    reaver.groundWeapon = {100, 60, 0, 256, DamageType::normal, false, true};
    reaver.groundWeapon.splashInner = 12;
    reaver.groundWeapon.splashMiddle = 28;
    reaver.groundWeapon.splashOuter = 48;
    std::vector<UnitSnapshot> packed;
    for (int index = 0; index < 3; ++index) {
        auto target = unit(10 + index, UnitKind::zergling, false,
                           {620 + index * 16, 512});
        target.hitPoints = target.maxHitPoints = 35;
        target.topSpeed = 0.0;
        target.groundWeapon = {};
        packed.push_back(target);
    }
    auto spread = packed;
    spread[1].position.x += 160;
    spread[2].position.x += 320;
    const auto clusteredImpact = CombatEvaluator{}.evaluate(
        std::array{reaver}, packed, 1, 0);
    const auto spreadImpact = CombatEvaluator{}.evaluate(
        std::array{reaver}, spread, 1, 0);
    check(clusteredImpact.simulatedEnemyLoss > spreadImpact.simulatedEnemyLoss,
          "engine-calibrated splash bands improve damage only when enemies are clustered");

    auto tank = unit(20, UnitKind::siegeTank, true, {512, 512});
    tank.groundWeapon = {40, 30, 0, 256, DamageType::normal, false, true};
    tank.groundWeapon.splashInner = 10;
    tank.groundWeapon.splashMiddle = 25;
    tank.groundWeapon.splashOuter = 40;
    tank.groundWeapon.splashFriendlyFire = true;
    auto stationaryAlly = unit(21, UnitKind::photonCannon, true, {620, 512});
    stationaryAlly.topSpeed = 0.0;
    stationaryAlly.groundWeapon = {};
    auto enemy = unit(22, UnitKind::marine, false, {600, 512});
    enemy.groundWeapon = {};
    const auto radial = CombatEvaluator{}.evaluate(
        std::array{tank, stationaryAlly}, std::array{enemy}, 1, 0);
    tank.groundWeapon.splashFriendlyFire = false;
    const auto enemyOnly = CombatEvaluator{}.evaluate(
        std::array{tank, stationaryAlly}, std::array{enemy}, 1, 0);
    check(radial.simulatedFriendlyLoss > 0.0 && enemyOnly.simulatedFriendlyLoss == 0.0,
          "Radial_Splash accounts for friendly exposure while Enemy_Splash does not");
    stationaryAlly.position.x = 700;
    tank.groundWeapon.splashFriendlyFire = true;
    const auto separatedAlly = CombatEvaluator{}.evaluate(
        std::array{tank, stationaryAlly}, std::array{enemy}, 1, 0);
    check(separatedAlly.simulatedFriendlyLoss == 0.0,
          "spread allies outside the engine outer radius avoid splash damage");

    auto templar = unit(30, UnitKind::highTemplar, true);
    templar.role = UnitRole::spellcaster;
    templar.energy = 200;
    auto marines = std::array{unit(31, UnitKind::marine, false),
                              unit(32, UnitKind::marine, false)};
    for (auto& marine : marines) {
        marine.groundWeapon = {6, 15, 0, 128, DamageType::normal, false, true};
    }
    const auto theoreticalStorm = CombatEvaluator{}.evaluate(
        std::array{templar}, marines, 1.2, 0, false);
    check(theoreticalStorm.friendlyPower == 0.0 &&
          theoreticalStorm.decision == FightDecision::retreat,
          "unmodeled spell value alone cannot turn a losing force into an engagement");
}
void outcomeMetrics() {
    auto attacker = unit(1, UnitKind::marine, true);
    attacker.hitPoints = attacker.maxHitPoints = 40;
    attacker.weaponCooldown = 2;
    attacker.groundWeapon = {10, 5, 0, 128, DamageType::normal, false, true};
    auto target = unit(2, UnitKind::dragoon, false);
    target.hitPoints = target.maxHitPoints = 30;
    target.groundWeapon = {};

    const auto result = CombatEvaluator{}.evaluate(std::array{attacker}, std::array{target}, 1, 0);
    check(result.simulatedFrames == 12 && result.simulatedOutcomeReached,
          "prediction records the frame when the final target is killed");
    check(result.simulatedEnemyDeaths == 1 && result.simulatedFriendlyDeaths == 0,
          "prediction separates unit casualties from health loss");
    check(std::abs(result.simulatedEnemyLoss - unitStats(target.kind).combatValue) < 1e-9 &&
          result.simulatedFriendlyLoss == 0,
          "prediction reports casualty value and zero attacker losses");

    const auto staticOnly = CombatEvaluator{}.evaluate(
        std::array{attacker}, std::array{target}, 1, 0, false);
    check(staticOnly.simulatedFrames == 0 && !staticOnly.simulatedOutcomeReached &&
          staticOnly.simulatedEnemyDeaths == 0 && staticOnly.simulatedEnemyLoss == 0,
          "static-only estimates do not fabricate simulation outcomes");
}
void truncationCoverage() {
    auto fighters = std::vector<UnitSnapshot>{};
    fighters.reserve(97);
    for (int index = 0; index < 96; ++index) {
        auto dragoon = unit(index + 1, UnitKind::dragoon, true,
                            {400 + (index % 12) * 20, 400 + (index / 12) * 20});
        dragoon.role = UnitRole::groundArmy;
        dragoon.groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
        // This formation has no represented anti-air despite the Dragoon label.
        dragoon.airWeapon = {};
        fighters.push_back(dragoon);
    }
    auto scout = unit(200, UnitKind::scout, false, {640, 512});
    scout.flying = true;
    scout.groundWeapon = {};
    scout.airWeapon = {8, 30, 0, 192, DamageType::normal, true, false};
    const auto uncapped = CombatEvaluator{}.evaluate(fighters, std::array{scout}, 1, 0);

    auto corsair = unit(97, UnitKind::corsair, true, {400, 400});
    corsair.role = UnitRole::airArmy;
    corsair.groundWeapon = {};
    corsair.airWeapon = {5, 10, 0, 192, DamageType::normal, true, false};
    fighters.push_back(corsair);
    const auto capped = CombatEvaluator{}.evaluate(fighters, std::array{scout}, 1, 0);
    const auto airArmyRole = std::uint32_t{1} << static_cast<std::uint32_t>(UnitRole::airArmy);
    check(capped.omittedFriendly.unitCount == 1 &&
          capped.omittedFriendly.combatValue > 0.0,
          "the 97th eligible fighter is explicitly counted with omitted combat value");
    check((capped.omittedFriendly.roleMask & airArmyRole) != 0U &&
          capped.omittedFriendly.airWeaponValue > 0.0,
          "truncation reports the omitted role and anti-air weapon domain");
    check(capped.confidence < uncapped.confidence * 0.6,
          "an omitted sole anti-air counter sharply lowers decision confidence");

    auto spellForce = std::vector<UnitSnapshot>(fighters.begin(), fighters.begin() + 96);
    auto groundEnemy = unit(201, UnitKind::marine, false, {640, 512});
    groundEnemy.groundWeapon = {6, 15, 0, 128, DamageType::normal, false, true};
    const auto noOmittedSupport = CombatEvaluator{}.evaluate(
        spellForce, std::array{groundEnemy}, 1, 0);
    auto templar = unit(97, UnitKind::highTemplar, true, {400, 400});
    templar.role = UnitRole::spellcaster;
    templar.energy = 150;
    templar.groundWeapon = {};
    templar.airWeapon = {};
    spellForce.push_back(templar);
    const auto omittedSupport = CombatEvaluator{}.evaluate(
        spellForce, std::array{groundEnemy}, 1, 0);
    const auto spellcasterRole = std::uint32_t{1} <<
                                 static_cast<std::uint32_t>(UnitRole::spellcaster);
    check((omittedSupport.omittedFriendly.roleMask & spellcasterRole) != 0U &&
          omittedSupport.confidence < noOmittedSupport.confidence,
          "an omitted energized spellcaster support role further reduces authority");
}
void confidencePolicy() {
    auto marine = unit(1, UnitKind::marine, true);
    marine.groundWeapon = {6, 15, 0, 128, DamageType::normal, false, true};
    auto equalMarine = marine;
    equalMarine.id = 2;
    equalMarine.ours = false;
    equalMarine.position = {600, 512};
    const auto staged = CombatEvaluator{}.evaluate(
        std::array{marine}, std::array{equalMarine}, 0.8, 0.6, false);
    check(staged.confidence < 0.8 && staged.ratio >= 0.8 &&
          staged.decision == FightDecision::kite && staged.confidenceStaged,
          "a low-confidence near-equal static estimate stages as kite for reassessment");

    std::array<UnitSnapshot, 4> advantaged{};
    for (int index = 0; index < static_cast<int>(advantaged.size()); ++index) {
        auto zealot = unit(10 + index, UnitKind::zealot, true,
                           {512 + index * 16, 512});
        zealot.groundWeapon = {8, 22, 0, 15, DamageType::normal, false, true};
        advantaged[static_cast<std::size_t>(index)] = zealot;
    }
    const auto decisive = CombatEvaluator{}.evaluate(
        advantaged, std::array{equalMarine}, 1.0, 0.9, false);
    check(decisive.confidence < 0.8 && decisive.ratio >= 1.25 &&
          decisive.decision == FightDecision::engage && !decisive.confidenceStaged,
          "decisive legal weapon advantage can still commit under low confidence");

    auto emptyReavers = std::vector<UnitSnapshot>{};
    for (int index = 0; index < 34; ++index) {
        auto reaver = unit(100 + index, UnitKind::reaver, true,
                           {512 + index * 8, 512});
        reaver.hitPoints = 1;
        reaver.ammo = 0;
        reaver.groundWeapon = {100, 60, 0, 256, DamageType::normal, false, true};
        emptyReavers.push_back(reaver);
    }
    auto lethalContact = unit(300, UnitKind::zergling, false, {560, 512});
    lethalContact.groundWeapon = {200, 13, 0, 128, DamageType::normal, false, true};
    const auto uncertainLoss = CombatEvaluator{}.evaluate(
        emptyReavers, std::array{lethalContact}, 1.0, 0.5, true);
    check(uncertainLoss.confidence < 0.8 && uncertainLoss.ratio < 0.72 &&
          uncertainLoss.decision == FightDecision::kite &&
          uncertainLoss.confidenceStaged,
          "a fragile fight estimate probes instead of hard-retreating against decisive static power");

    const auto noDecisiveEvidence = CombatEvaluator{}.evaluate(
        std::array{marine}, std::array{equalMarine}, 1.2, 0.6, false);
    check(noDecisiveEvidence.decision == FightDecision::retreat &&
          !noDecisiveEvidence.confidenceStaged,
          "low confidence alone does not invent a probe without decisive legal power");
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
    check(nav.findPath(goon.position, {496, 432}).reached() &&
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
    const auto mixedFriendly = evaluator.evaluate(std::array{b, a}, std::array{x, y}, 1, 0);
    const auto mixedEnemy = evaluator.evaluate(std::array{a, b}, std::array{y, x}, 1, 0);
    const auto samePrediction = [](const CombatEstimate& left, const CombatEstimate& right) {
        const auto sameOmissions = [](const SimulationOmissions& a, const SimulationOmissions& b) {
            return a.combatValue == b.combatValue && a.unitCount == b.unitCount &&
                a.roleMask == b.roleMask && a.groundWeaponValue == b.groundWeaponValue &&
                a.airWeaponValue == b.airWeaponValue;
        };
        return left.friendlyPower == right.friendlyPower && left.enemyPower == right.enemyPower &&
            left.ratio == right.ratio && left.confidence == right.confidence &&
            left.simulatedEnemyRemaining == right.simulatedEnemyRemaining &&
            left.simulatedFriendlyRemaining == right.simulatedFriendlyRemaining &&
            left.simulatedEnemyLoss == right.simulatedEnemyLoss &&
            left.simulatedFriendlyLoss == right.simulatedFriendlyLoss &&
            left.simulatedFrames == right.simulatedFrames &&
            left.simulatedFriendlyDeaths == right.simulatedFriendlyDeaths &&
            left.simulatedEnemyDeaths == right.simulatedEnemyDeaths &&
            left.simulatedOutcomeReached == right.simulatedOutcomeReached &&
            left.confidenceStaged == right.confidenceStaged &&
            sameOmissions(left.omittedFriendly, right.omittedFriendly) &&
            sameOmissions(left.omittedEnemy, right.omittedEnemy) &&
            left.decision == right.decision && left.advanceBlocked == right.advanceBlocked &&
            left.holdScreen == right.holdScreen;
    };
    check(samePrediction(original, reordered) && samePrediction(original, mixedFriendly) &&
          samePrediction(original, mixedEnemy),
          "all friendly/enemy input permutations preserve every public engine-case prediction");
    a.groundWeapon = {}; x.groundWeapon = {};
    const auto passive = evaluator.evaluate(std::array{a}, std::array{x}, 1, 0);
    check(passive.simulatedFriendlyRemaining == unitStats(a.kind).combatValue &&
          passive.simulatedEnemyRemaining == unitStats(x.kind).combatValue,
          "an encounter with no eligible weapon events preserves both armies");

    auto tieAttacker = unit(20, UnitKind::dragoon, true);
    tieAttacker.groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
    auto lowerId = unit(31, UnitKind::marine, false, {600, 512});
    auto higherId = lowerId;
    higherId.id = 32;
    const auto lowerFirstOrder = std::array{lowerId, higherId};
    const auto higherFirstOrder = std::array{higherId, lowerId};
    const auto lowerFirst = evaluator.selectTarget(tieAttacker, lowerFirstOrder);
    const auto higherFirst = evaluator.selectTarget(tieAttacker, higherFirstOrder);
    check(lowerFirst != nullptr && higherFirst != nullptr &&
          lowerFirst->id == 31 && higherFirst->id == lowerFirst->id,
          "equal-score target comparator resolves to the same lower unit ID in either order");
}

void dragoonFiringCycle() {
    auto dragoon = unit(1, UnitKind::dragoon, true, {500, 500});
    dragoon.hitPoints = dragoon.maxHitPoints = 100;
    dragoon.maxShields = dragoon.shields = 80;
    dragoon.groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
    auto marine = unit(2, UnitKind::marine, false, {850, 500});
    marine.hitPoints = marine.maxHitPoints = 40;
    marine.groundWeapon = {6, 15, 0, 128, DamageType::normal, false, true};
    CombatEstimate kite;
    kite.decision = FightDecision::kite;
    CombatEstimate retreat;
    retreat.decision = FightDecision::retreat;
    TacticalController controller;
    InfluenceMap influence;

    for (const auto latency : {0, 3, 6, 12, 24}) {
        auto orders = controller.control(std::array{dragoon}, std::array{marine}, kite,
            marine.position, {400, 500}, influence, {}, latency);
        check(orders.size() == 1 && orders.front().type == CommandType::attackUnit &&
              orders.front().source == "focus-fire",
              "a Dragoon starts an attack approach to a moving target outside firing range");

        marine.position = {650 + latency, 512};
        dragoon.weaponCooldown = 0;
        dragoon.attackWindup = true;
        orders = controller.control(std::array{dragoon}, std::array{marine}, kite,
            marine.position, {400, 500}, influence, {}, latency);
        check(orders.empty(), "windup is allowed to finish before kiting or retargeting");
        dragoon.attackWindup = false;
        dragoon.attackFrame = true;
        orders = controller.control(std::array{dragoon}, std::array{marine}, kite,
            marine.position, {400, 500}, influence, {}, latency);
        check(orders.empty(), "the projectile release attack frame cannot be interrupted by movement");

        dragoon.attackFrame = false;
        dragoon.weaponCooldown = latency + 3;
        marine.position.x += 8;
        orders = controller.control(std::array{dragoon}, std::array{marine}, kite,
            marine.position, {400, 500}, influence, {}, latency);
        check(orders.size() == 1 && orders.front().type == CommandType::move &&
              orders.front().source == "combat-kite",
              "post-release movement uses reload time against a moving Marine target");

        dragoon.weaponCooldown = latency + 2;
        marine.position.x += 8;
        orders = controller.control(std::array{dragoon}, std::array{marine}, kite,
            marine.position, {400, 500}, influence, {}, latency);
        check(orders.size() == 1 && orders.front().type == CommandType::attackUnit &&
              orders.front().targetUnit == marine.id && orders.front().source == "focus-fire",
              "the Dragoon resumes its volley at the latency-aware ready boundary");

        dragoon.weaponCooldown = 0;
        orders = controller.control(std::array{dragoon}, std::array{marine}, retreat,
            marine.position, {400, 500}, influence, {}, latency);
        check(orders.size() == 1 && orders.front().type == CommandType::attackUnit &&
              orders.front().source == "retreat-volley",
              "retreat preserves an immediately available Dragoon shot at every latency");
        dragoon.weaponCooldown = latency + 3;
        orders = controller.control(std::array{dragoon}, std::array{marine}, retreat,
            marine.position, {400, 500}, influence, {}, latency);
        check(orders.size() == 1 && orders.front().type == CommandType::move &&
              orders.front().source == "combat-retreat",
              "retreat movement resumes between Dragoon volleys");
    }

    // Model a path-stuck Dragoon whose Attack_Unit order never completes.
    // The bridge reports that order inactive, while CommandBus bounds retry
    // cadence so the actor can recover without resending every frame.
    marine.position = {650, 500};
    dragoon.weaponCooldown = 0;
    CommandBus bus;
    std::vector<Frame> attackFrames;
    for (Frame frame = 0; frame <= 60; ++frame) {
        marine.position.x = 650 + static_cast<int>(frame % 3) * 2;
        const auto proposed = controller.control(std::array{dragoon}, std::array{marine}, kite,
            marine.position, {400, 500}, influence, {}, 6);
        bus.beginFrame(frame, 6);
        for (const auto& command : proposed) bus.submit(command);
        const auto issued = bus.finalize();
        for (const auto& command : issued) {
            if (command.type == CommandType::attackUnit) attackFrames.push_back(frame);
            bus.markIssued(command);
        }
    }
    check(attackFrames == std::vector<Frame>{0, 19, 38, 57},
          "a stuck Dragoon retries after bounded suppression instead of flooding attack orders");
}

void zealotContactCoordination() {
    constexpr int width = 40;
    constexpr int height = 30;
    constexpr int cell = 32;
    const auto openCells = std::vector<std::uint8_t>(width * height, 1);
    const NavigationGrid open{width, height, cell, openCells};
    CombatEstimate engage;
    engage.decision = FightDecision::engage;
    InfluenceMap influence;
    TacticalController controller;

    auto marine = unit(50, UnitKind::marine, false, {800, 480});
    marine.dimensionLeft = marine.dimensionRight = 16;
    marine.dimensionUp = marine.dimensionDown = 16;
    std::vector<UnitSnapshot> zealots;
    for (auto index = 0; index < 4; ++index) {
        auto zealot = unit(static_cast<UnitId>(10 + index), UnitKind::zealot, true,
                           {600 + index * 20, 480});
        zealot.dimensionLeft = 11;
        zealot.dimensionRight = 11;
        zealot.dimensionUp = 5;
        zealot.dimensionDown = 13;
        zealot.groundWeapon = {16, 22, 0, 32, DamageType::normal, false, true};
        zealots.push_back(zealot);
    }

    auto orders = controller.control(zealots, std::array{marine}, engage, marine.position,
        {320, 480}, influence, {400, 480}, 0, false, {}, TacticalIntent::battle,
        {}, &open);
    std::vector<Position> contactSlots;
    auto allContactMoves = orders.size() == zealots.size();
    for (const auto& order : orders) {
        allContactMoves = allContactMoves && order.type == CommandType::move &&
                          order.source == "melee-contact" &&
                          order.targetPosition.valid() && open.walkable(order.targetPosition);
        if (order.source == "melee-contact") contactSlots.push_back(order.targetPosition);
    }
    std::ranges::sort(contactSlots, [](const Position left, const Position right) {
        return left.y != right.y ? left.y < right.y : left.x < right.x;
    });
    auto spacedSlots = contactSlots.size() == zealots.size();
    for (std::size_t first = 0; first < contactSlots.size(); ++first)
        for (std::size_t second = first + 1; second < contactSlots.size(); ++second)
            spacedSlots = spacedSlots &&
                distanceSquared(contactSlots[first], contactSlots[second]) >= 40 * 40;
    check(allContactMoves && spacedSlots,
          "four Zealots receive separate, walkable firing positions in open terrain");

    for (std::size_t index = 0; index < zealots.size() && index < contactSlots.size(); ++index)
        zealots[index].position = contactSlots[index];
    orders = controller.control(zealots, std::array{marine}, engage, marine.position,
        {320, 480}, influence, {400, 480}, 0, false, {}, TacticalIntent::battle,
        {}, &open);
    check(orders.size() == zealots.size() &&
          std::ranges::all_of(orders, [](const Command& order) {
              return order.type == CommandType::attackUnit && order.targetUnit == 50;
          }),
          "all four Zealots issue attacks after reaching their assigned open-terrain slots");

    auto chokeCells = openCells;
    for (auto y = 0; y < height; ++y)
        if (y != 15) chokeCells[static_cast<std::size_t>(y * width + 20)] = 0;
    const NavigationGrid choke{width, height, cell, chokeCells};
    marine.position = {800, 496};
    for (auto& zealot : zealots) zealot.position = {592, 496};
    orders = controller.control(zealots, std::array{marine}, engage, marine.position,
        {224, 496}, influence, {256, 496}, 0, false, {}, TacticalIntent::battle,
        {}, &choke);
    const auto chokeRoutes = orders.size() == zealots.size() &&
        std::ranges::all_of(orders, [&choke](const Command& order) {
            return order.source == "melee-contact" && order.targetPosition.valid() &&
                   choke.findPath({592, 480}, order.targetPosition, 4096).reached();
        });
    check(chokeRoutes,
          "Zealot contact slots behind a one-tile choke remain connected through its opening");
    std::vector<Position> chokeSlots;
    for (const auto& order : orders)
        if (order.source == "melee-contact") chokeSlots.push_back(order.targetPosition);
    for (std::size_t index = 0; index < zealots.size() && index < chokeSlots.size(); ++index)
        zealots[index].position = chokeSlots[index];
    orders = controller.control(zealots, std::array{marine}, engage, marine.position,
        {224, 480}, influence, {256, 480}, 0, false, {}, TacticalIntent::battle,
        {}, &choke);
    check(orders.size() == zealots.size() &&
          std::ranges::all_of(orders, [](const Command& order) {
              return order.type == CommandType::attackUnit && order.targetUnit == 50;
          }),
          "all four Zealots attack after navigating through the one-tile choke");

    auto blockedCells = openCells;
    for (auto y = 0; y < height; ++y)
        blockedCells[static_cast<std::size_t>(y * width + 20)] = 0;
    const NavigationGrid blocked{width, height, cell, blockedCells};
    for (auto& zealot : zealots) zealot.position = {592, 480};
    orders = controller.control(zealots, std::array{marine}, engage, marine.position,
        {224, 480}, influence, {256, 480}, 0, false, {}, TacticalIntent::battle,
        {}, &blocked);
    check(orders.size() == zealots.size() &&
          std::ranges::all_of(orders, [](const Command& order) {
              return order.type == CommandType::hold &&
                     order.source == "melee-contact-wait";
          }),
          "Zealots wait instead of attacking across an impassable wall");

    std::vector<UnitSnapshot> distantArmy;
    for (auto index = 0; index < 64; ++index) {
        auto zealot = zealots.front();
        zealot.id = static_cast<UnitId>(100 + index);
        zealot.position = {256 + index % 4 * 16, 480 + index / 4 * 8};
        distantArmy.push_back(zealot);
    }
    const auto searchesBefore = NavigationGrid::diagnosticsForCurrentThread().searches;
    orders = controller.control(distantArmy, std::array{marine}, engage, marine.position,
        {224, 480}, influence, {256, 480}, 0, false, {}, TacticalIntent::battle,
        {}, &blocked);
    check(orders.size() == distantArmy.size() &&
          std::ranges::all_of(orders, [target = marine.id](const Command& order) {
              return order.type == CommandType::attackUnit && order.targetUnit == target;
          }) && NavigationGrid::diagnosticsForCurrentThread().searches == searchesBefore,
          "a distant army approaches before running local contact searches for every member");

    auto dragoon = unit(70, UnitKind::dragoon, true, {600, 480});
    dragoon.dimensionLeft = dragoon.dimensionRight = 16;
    dragoon.dimensionUp = dragoon.dimensionDown = 16;
    dragoon.groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
    marine.position = {1024, 480};
    marine.groundWeapon = {};
    auto advancingZealot = zealots.front();
    advancingZealot.position = {850, 480};
    const std::array support{dragoon, advancingZealot};
    orders = controller.control(std::array{dragoon}, std::array{marine}, engage,
        marine.position, {480, 480}, influence, dragoon.position, 0, false, {},
        TacticalIntent::battle, support, &open);
    check(orders.size() == 1 && orders.front().type == CommandType::hold &&
          orders.front().source == "melee-support-hold",
          "an out-of-range Dragoon holds position while its reachable Zealot is still approaching");

    advancingZealot.position = {992, 480};
    const std::array arrivedSupport{dragoon, advancingZealot};
    orders = controller.control(std::array{dragoon}, std::array{marine}, engage,
        marine.position, {480, 480}, influence, dragoon.position, 0, false, {},
        TacticalIntent::battle, arrivedSupport, &open);
    check(orders.size() == 1 && orders.front().type == CommandType::attackUnit &&
          orders.front().targetUnit == marine.id,
          "ranged support resumes its attack order once the Zealot reaches contact");
}

void stormReservationCoordination() {
    auto templar = unit(80, UnitKind::highTemplar, true, {500, 500});
    templar.energy = 150;
    std::vector<UnitSnapshot> enemies;
    for (auto index = 0; index < 4; ++index) {
        auto marine = unit(static_cast<UnitId>(90 + index), UnitKind::marine, false,
                           {650 + index * 12, 500 + (index % 2) * 12});
        marine.hitPoints = marine.maxHitPoints = 40;
        enemies.push_back(marine);
    }
    CombatEstimate engage;
    engage.decision = FightDecision::engage;
    InfluenceMap influence;
    TacticalController controller;
    const auto proposed = controller.control(std::array{templar}, enemies, engage,
        {900, 900}, {100, 100}, influence, templar.position, 6, true);
    const auto firstCast = std::ranges::find_if(proposed, [](const Command& command) {
        return command.type == CommandType::useTech &&
               command.technology == TechnologyKind::psionicStorm;
    });
    check(firstCast != proposed.end(),
          "a safe, valuable Storm candidate is proposed before bridge acceptance");
    if (firstCast == proposed.end()) return;

    auto movedDragoon = unit(81, UnitKind::dragoon, true, firstCast->targetPosition);
    const std::array safeAllies{templar};
    const std::array movedAllies{templar, movedDragoon};
    check(psionicStormSafe(firstCast->targetPosition, enemies, safeAllies) &&
          !psionicStormSafe(firstCast->targetPosition, enemies, movedAllies),
          "live friendly movement into the impact area invalidates the stale cast safety score");

    auto secondTemplar = templar;
    secondTemplar.id = 82;
    const std::array acceptedZone{firstCast->targetPosition};
    const auto rejectedFirstCastRetry = controller.control(
        std::array{secondTemplar}, enemies, engage, {900, 900}, {100, 100}, influence,
        secondTemplar.position, 6, true, {}, TacticalIntent::battle, {}, nullptr, {},
        nullptr, false);
    check(std::ranges::any_of(rejectedFirstCastRetry, [](const Command& command) {
              return command.type == CommandType::useTech &&
                     command.technology == TechnologyKind::psionicStorm;
          }),
          "a rejected proposal leaves no cross-frame reservation and permits reassessment");

    const auto acceptedFirstCast = controller.control(
        std::array{secondTemplar}, enemies, engage, {900, 900}, {100, 100}, influence,
        secondTemplar.position, 6, true, {}, TacticalIntent::battle, {}, nullptr, {},
        nullptr, false, acceptedZone);
    check(std::ranges::none_of(acceptedFirstCast, [](const Command& command) {
              return command.type == CommandType::useTech &&
                     command.technology == TechnologyKind::psionicStorm;
          }),
          "a bridge-accepted Storm reservation suppresses overlapping Templar proposals");
}

void differentialEngineCaseSweeps() {
    const auto decisionRank = [](const FightDecision decision) {
        switch (decision) {
        case FightDecision::retreat: return 0;
        case FightDecision::kite: return 1;
        case FightDecision::engage: return 2;
        }
        return -1;
    };
    const auto favorableMargin = [](const CombatEstimate& estimate) {
        return estimate.simulatedFriendlyRemaining - estimate.simulatedEnemyRemaining;
    };
    CombatEvaluator evaluator;

    // Portable differential replay of the T061 ranged-cooldown lane. These
    // snapshots sweep model inputs around the native matchup archetype; they
    // are not another engine run. BWAPI folds upgrades into weapon damage.
    auto dragoon = unit(1, UnitKind::dragoon, true, {496, 496});
    dragoon.hitPoints = dragoon.maxHitPoints = 100;
    dragoon.maxShields = dragoon.shields = 80;
    dragoon.groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
    std::array<decltype(dragoon), 3> zerglings{};
    for (std::size_t index = 0; index < zerglings.size(); ++index) {
        auto& ling = zerglings[index];
        ling = unit(static_cast<UnitId>(10 + index), UnitKind::zergling, false,
                    {640 + static_cast<int>(index) * 12, 496});
        ling.hitPoints = ling.maxHitPoints = 35;
        ling.groundWeapon = {5, 8, 0, 32, DamageType::normal, false, true};
    }

    auto baseline = evaluator.evaluate(std::array{dragoon}, zerglings, 1.2, 0.0);
    auto upgradedDragoon = dragoon;
    for (const auto damage : {22, 24, 26, 28, 30}) {
        upgradedDragoon.groundWeapon.damage = damage;
        const auto upgraded = evaluator.evaluate(std::array{upgradedDragoon}, zerglings, 1.2, 0.0);
        check(upgraded.friendlyPower + 1e-9 >= baseline.friendlyPower &&
              upgraded.ratio + 1e-9 >= baseline.ratio &&
              upgraded.simulatedEnemyRemaining <= baseline.simulatedEnemyRemaining + 1e-9 &&
              upgraded.simulatedFriendlyRemaining + 1e-9 >= baseline.simulatedFriendlyRemaining &&
              favorableMargin(upgraded) + 1e-9 >= favorableMargin(baseline) &&
              decisionRank(upgraded.decision) >= decisionRank(baseline.decision),
              "each relevant Dragoon weapon-upgrade step is non-worsening in the T061 ranged case");
        baseline = upgraded;
    }

    // Add effective allied Dragoons from the same engine lane. Each ally can
    // legally damage the ground attackers and shares the captured position and
    // post-upgrade weapon profile, so these are not inert support units.
    std::vector<UnitSnapshot> friendly{dragoon};
    baseline = evaluator.evaluate(friendly, zerglings, 1.2, 0.0);
    for (auto id = UnitId{2}; id <= 5; ++id) {
        auto ally = dragoon;
        ally.id = id;
        ally.position.x += static_cast<int>(id - 1) * 8;
        friendly.push_back(ally);
        const auto reinforced = evaluator.evaluate(friendly, zerglings, 1.2, 0.0);
        check(reinforced.friendlyPower + 1e-9 >= baseline.friendlyPower &&
              reinforced.ratio + 1e-9 >= baseline.ratio &&
              reinforced.simulatedEnemyRemaining <= baseline.simulatedEnemyRemaining + 1e-9 &&
              reinforced.simulatedFriendlyRemaining + 1e-9 >= baseline.simulatedFriendlyRemaining &&
              favorableMargin(reinforced) + 1e-9 >= favorableMargin(baseline) &&
              decisionRank(reinforced.decision) >= decisionRank(baseline.decision),
              "adding an effective ally cannot worsen the captured Dragoon-versus-Zergling prediction");
        baseline = reinforced;
    }

    // Removing opponents from the same ranged-cooldown fixture is swept down
    // to one active enemy. The zero-enemy floor intentionally reports the
    // required ratio, so it is outside this ratio-monotonic comparison.
    std::vector<UnitSnapshot> remainingEnemies(zerglings.begin(), zerglings.end());
    baseline = evaluator.evaluate(std::array{dragoon}, remainingEnemies, 1.2, 0.0);
    while (remainingEnemies.size() > 1) {
        remainingEnemies.pop_back();
        const auto fewerEnemies = evaluator.evaluate(std::array{dragoon}, remainingEnemies, 1.2, 0.0);
        check(fewerEnemies.simulatedFriendlyLoss <= baseline.simulatedFriendlyLoss + 1e-9 &&
              fewerEnemies.ratio + 1e-9 >= baseline.ratio &&
              fewerEnemies.simulatedFriendlyRemaining + 1e-9 >= baseline.simulatedFriendlyRemaining &&
              favorableMargin(fewerEnemies) + 1e-9 >= favorableMargin(baseline) &&
              decisionRank(fewerEnemies.decision) >= decisionRank(baseline.decision),
              "removing a relevant enemy cannot worsen the captured ranged-case prediction");
        baseline = fewerEnemies;
    }

    // The T061 Zealot/Marine shield lane is also swept over the Marine's
    // observed weapon cooldown. A slower cooldown must not increase modeled
    // friendly losses or make the engagement estimate less favorable.
    auto zealot = unit(50, UnitKind::zealot, true, {496, 496});
    zealot.hitPoints = zealot.maxHitPoints = 100;
    zealot.maxShields = zealot.shields = 60;
    zealot.groundWeapon = {8, 22, 0, 32, DamageType::normal, false, true, 2};
    auto marine = unit(51, UnitKind::marine, false, {560, 496});
    marine.hitPoints = marine.maxHitPoints = 40;
    marine.groundWeapon = {6, 15, 0, 128, DamageType::normal, false, true};
    baseline = evaluator.evaluate(std::array{zealot}, std::array{marine}, 1.2, 0.0);
    for (const auto cooldown : {18, 21, 24, 27, 30}) {
        marine.groundWeapon.cooldown = cooldown;
        const auto slowerEnemy = evaluator.evaluate(std::array{zealot}, std::array{marine}, 1.2, 0.0);
        check(slowerEnemy.simulatedFriendlyLoss <= baseline.simulatedFriendlyLoss + 1e-9 &&
              slowerEnemy.ratio + 1e-9 >= baseline.ratio &&
              favorableMargin(slowerEnemy) + 1e-9 >= favorableMargin(baseline) &&
              decisionRank(slowerEnemy.decision) >= decisionRank(baseline.decision),
              "enemy cooldown sweep does not worsen the T061 shield-and-melee prediction");
        baseline = slowerEnemy;
    }
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
    cadence(); boundedApproach(); combatStatusAdmissibility(); splashAndSpellCalibration(); outcomeMetrics(); truncationCoverage(); confidencePolicy(); consumableAttackers(); workerDefense(); unavailableThreats(); terrainSpacing();
    deterministicScheduling(); dragoonFiringCycle(); zealotContactCoordination();
    stormReservationCoordination();
    differentialEngineCaseSweeps();
    if (failures) std::cerr << failures << " combat accuracy checks failed\n";
    else std::cout << checks << " combat accuracy checks passed\n";
    return failures == 0 ? 0 : 1;
}
