#include "protodd/Scouting.hpp"

#include <iostream>
#include <string_view>

namespace {

using namespace protodd;

UnitSnapshot unit(const UnitId id, const UnitKind kind, const Position position,
                  const bool ours) {
    UnitSnapshot result;
    result.id = id;
    result.kind = kind;
    result.position = result.lastPosition = position;
    result.ours = ours;
    result.visible = result.detected = result.completed = true;
    result.cloaked = kind == UnitKind::observer;
    result.hitPoints = result.maxHitPoints = 100;
    result.shields = result.maxShields = 100;
    result.lastSeen = result.firstSeen = 2400;
    return result;
}

}  // namespace

int main() {
    using namespace protodd;
    bool passed = true;
    const auto check = [&passed](const bool condition, const std::string_view message) {
        if (!condition) {
            passed = false;
            std::cerr << "FAIL: " << message << '\n';
        }
    };

    GameState state;
    state.frame = 2400;
    state.mapWidthPixels = state.mapHeightPixels = 2048;
    state.self.id = 1;
    state.enemy.id = 2;
    state.enemy.race = Race::terran;
    auto observer = unit(1, UnitKind::observer, {512, 512}, true);
    state.self.units.push_back(observer);
    InfluenceMap influence;
    influence.update(state);

    auto vessel = unit(10, UnitKind::scienceVessel, {512, 512}, false);
    vessel.role = UnitRole::detector;
    vessel.sightRange = 320;
    state.enemy.units = {vessel};
    influence.update(state);
    check(ScoutManager::observerExposure(state, observer, influence, observer.position) == 0.0 &&
              ScoutManager::observerRouteSafe(state, observer, influence, {900, 512}) &&
              !ScoutManager::observerInDanger(state, observer, influence),
          "detection without anti-air damage does not expose an Observer");

    auto wraith = unit(11, UnitKind::wraith, {700, 512}, false);
    wraith.airWeapon = {.damage = 20, .cooldown = 22, .maxRange = 160,
                        .targetsAir = true};
    state.enemy.units = {wraith};
    influence.update(state);
    check(ScoutManager::observerExposure(state, observer, influence, observer.position) == 0.0 &&
              ScoutManager::observerRouteSafe(state, observer, influence, {900, 512}),
          "anti-air without detection cannot hit the cloaked Observer");

    state.enemy.units = {vessel, wraith};
    influence.update(state);
    check(ScoutManager::observerExposure(state, observer, influence, observer.position) > 0.08 &&
              !ScoutManager::observerRouteSafe(state, observer, influence, {900, 512}) &&
              ScoutManager::observerInDanger(state, observer, influence),
          "detector-supported anti-air blocks scouting and triggers withdrawal");

    state.enemy.units.clear();
    state.scannerSweeps = {{512, 512}};
    influence.update(state);
    check(ScoutManager::observerExposure(state, observer, influence, observer.position) == 0.0,
          "an active scanner sweep without nearby anti-air causes no damage exposure");
    state.enemy.units = {wraith};
    influence.update(state);
    check(ScoutManager::observerExposure(state, observer, influence, observer.position) > 0.08,
          "an active scanner sweep makes nearby anti-air effective");

    state.scannerSweeps.clear();
    auto comsat = unit(12, UnitKind::comsatStation, {1600, 1600}, false);
    comsat.energy = 50;
    comsat.lastSeen = state.frame - 24;
    wraith.position = {700, 512};
    state.enemy.units = {comsat, wraith};
    influence.update(state);
    check(ScoutManager::observerExposure(state, observer, influence, observer.position) > 0.08,
          "a recently observed Comsat with scan energy makes nearby anti-air exposure plausible");
    state.enemy.units.front().energy = 0;
    check(ScoutManager::observerExposure(state, observer, influence, observer.position) == 0.0,
          "a Comsat without known scan energy does not invent detection coverage");

    state.enemy.race = Race::zerg;
    auto overlord = unit(13, UnitKind::overlord, {512, 512}, false);
    overlord.role = UnitRole::detector;
    overlord.sightRange = 320;
    auto hydralisk = unit(14, UnitKind::hydralisk, {700, 512}, false);
    hydralisk.airWeapon = {.damage = 10, .cooldown = 30, .maxRange = 128,
                           .targetsAir = true};
    state.enemy.units = {overlord, hydralisk};
    influence.update(state);
    check(ScoutManager::observerExposure(state, observer, influence, observer.position) > 0.08,
          "race-neutral detector and anti-air pairing catches Zerg coverage");

    state.enemy.units.clear();
    state.storms = {observer.position};
    influence.update(state);
    check(ScoutManager::observerExposure(state, observer, influence, observer.position) > 0.08 &&
              !ScoutManager::observerRouteSafe(state, observer, influence, {900, 512}),
          "Psionic Storm remains an area hazard even without detection or anti-air");

    return passed ? 0 : 1;
}
