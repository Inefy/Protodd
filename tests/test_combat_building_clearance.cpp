#include "protodd/Combat.hpp"
#include "protodd/Navigation.hpp"

#include <array>
#include <iostream>
#include <string_view>

namespace {
using namespace protodd;
int failures{};
int checks{};
void check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
UnitSnapshot unit(UnitId id, UnitKind kind, Position position) {
    UnitSnapshot result;
    result.id = id; result.kind = kind; result.position = position;
    result.visible = result.detected = result.completed = result.powered = true;
    result.hitPoints = 20; result.maxHitPoints = 100;
    result.topSpeed = 1.78;
    return result;
}
Command retreatAround(UnitSnapshot mover, std::span<const UnitSnapshot> buildings,
                      const NavigationGrid* grid = nullptr,
                      Position fallback = {200, 512}, Position enemyPosition = {400, 512}) {
    auto enemy = unit(99, UnitKind::dragoon, enemyPosition);
    enemy.hitPoints = 100;
    enemy.groundWeapon = {20, 30, 0, 128, DamageType::normal, false, true};
    enemy.airWeapon = {20, 30, 0, 128, DamageType::normal, true, false};
    CombatEstimate estimate;
    estimate.decision = FightDecision::retreat;
    const auto commands = TacticalController{}.control(std::array{mover}, std::array{enemy},
        estimate, {600, 512}, fallback, InfluenceMap{}, {-1, -1}, 0, false,
        {}, TacticalIntent::battle, {}, grid, buildings);
    check(commands.size() == 1, "contact produces one command");
    return commands.empty() ? Command{} : commands.front();
}
Command retreat(UnitSnapshot mover, UnitSnapshot building, const NavigationGrid* grid = nullptr) {
    return retreatAround(mover, std::array{building}, grid);
}
}
int main() {
    auto reaver = unit(274, UnitKind::reaver, {320, 512});
    reaver.dimensionLeft = reaver.dimensionRight = 15;
    reaver.dimensionUp = reaver.dimensionDown = 15;
    reaver.groundWeapon = {100, 60, 0, 256, DamageType::normal, false, true};
    auto robotics = unit(177, UnitKind::roboticsFacility, {272, 512});
    robotics.dimensionLeft = robotics.dimensionRight = 32;
    robotics.dimensionUp = robotics.dimensionDown = 32;
    auto order = retreat(reaver, robotics);
    check(order.type == CommandType::move &&
          (order.targetPosition.x > 318 || order.targetPosition.x < 226 ||
           order.targetPosition.y < 466 || order.targetPosition.y > 558),
          "retreat cannot place a Reaver footprint inside a live Robotics Facility");
    NavigationGrid terrain(32, 32, 32, std::vector<std::uint8_t>(32 * 32, 1));
    order = retreat(reaver, robotics, &terrain);
    check(order.targetPosition.x > 318 || order.targetPosition.x < 226 ||
          order.targetPosition.y < 466 || order.targetPosition.y > 558,
          "an entirely walkable terrain grid does not legalize a building collision");

    auto dragoon = reaver; dragoon.kind = UnitKind::dragoon;
    auto thinWall = robotics; thinWall.position = {288, 512};
    thinWall.dimensionLeft = thinWall.dimensionRight = 4;
    thinWall.dimensionUp = thinWall.dimensionDown = 128;
    dragoon.dimensionLeft = dragoon.dimensionRight = 0;
    dragoon.dimensionUp = dragoon.dimensionDown = 0;
    order = retreat(dragoon, thinWall);
    check(order.targetPosition.x >= 293,
          "a clear destination across a building wall is not a reachable local step");
    auto clearanceWall = robotics; clearanceWall.position = {224, 512};
    clearanceWall.dimensionLeft = clearanceWall.dimensionRight = 8;
    clearanceWall.dimensionUp = clearanceWall.dimensionDown = 128;
    dragoon.dimensionLeft = dragoon.dimensionRight = 32;
    order = retreat(dragoon, clearanceWall);
    check(order.targetPosition.x > 263,
          "Dragoon dimensions matter when its center alone would clear the building");

    auto edgeOrigin = reaver; edgeOrigin.position.x = 310;
    order = retreat(edgeOrigin, robotics);
    check(order.targetPosition.x < 226 || order.targetPosition.x > 318 ||
          order.targetPosition.y < 466 || order.targetPosition.y > 558,
          "an overlapping observed origin may take an outward escape");
    auto incompleteBuilding = robotics; incompleteBuilding.completed = false;
    order = retreat(reaver, incompleteBuilding);
    check(order.targetPosition.x > 318 || order.targetPosition.x < 226 ||
          order.targetPosition.y < 466 || order.targetPosition.y > 558,
          "unfinished buildings still obstruct a retreat");

    auto centralOrigin = reaver; centralOrigin.position = robotics.position;
    order = retreat(centralOrigin, robotics);
    check(order.targetPosition != centralOrigin.position &&
          (order.targetPosition.x < 226 || order.targetPosition.x > 318 ||
           order.targetPosition.y < 466 || order.targetPosition.y > 558),
          "a central overlapping origin takes an exterior escape instead of freezing");
    auto adjacentOrigin = reaver; adjacentOrigin.position.x = 319;
    order = retreatAround(adjacentOrigin, std::array{robotics}, nullptr,
                          {600, 512}, {219, 512});
    check(order.targetPosition == Position{383, 512},
          "an exactly adjacent unit can retreat away along the building edge");

    auto upperWall = robotics; upperWall.position = {288, 465};
    auto lowerWall = robotics; lowerWall.id = 178; lowerWall.position = {288, 559};
    order = retreatAround(reaver, std::array{upperWall, lowerWall}, &terrain);
    check(order.targetPosition == Position{256, 512},
          "a legal passage exactly as wide as the moving body remains traversable");
    order = retreat(reaver, upperWall, &terrain);
    check(order.targetPosition == Position{256, 512},
          "touching a building edge without overlap remains a legal movement segment");
    lowerWall.position.y -= 1;
    order = retreatAround(reaver, std::array{upperWall, lowerWall}, &terrain);
    check(order.targetPosition != Position{256, 512},
          "a passage narrower than the moving body cannot be used");

    robotics.flying = true;
    order = retreat(reaver, robotics);
    check(order.targetPosition == Position{256, 512}, "lifted buildings do not block ground movement");
    robotics.flying = false;
    reaver.flying = true;
    order = retreat(reaver, robotics);
    check(order.targetPosition == Position{256, 512}, "flying units may move over ground buildings");
    reaver.flying = false;
    reaver.hitPoints = 100; reaver.ammo = 1;
    order = retreat(reaver, robotics);
    check(order.type == CommandType::attackUnit && order.targetUnit == 99,
          "a loaded ready Reaver keeps its legal retreat volley");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
