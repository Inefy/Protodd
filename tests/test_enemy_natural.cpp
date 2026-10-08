#include "protodd/Information.hpp"

#include <cmath>
#include <iostream>
#include <string_view>

namespace {

using namespace protodd;

bool check(const bool condition, const std::string_view message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

BaseSnapshot base(const int id, const Position center, const int owner,
                  const bool start, const int minerals, const int patches,
                  const int geysers, const int route, const bool routeKnown = true) {
    BaseSnapshot result;
    result.id = id;
    result.center = center;
    result.mineralLine = center;
    result.ownerId = owner;
    result.startLocation = start;
    result.mineralsRemaining = minerals;
    result.mineralPatches = patches;
    result.geysers = geysers;
    result.enemyGroundDistanceFromMain = route;
    result.enemyGroundReachabilityKnown = routeKnown;
    return result;
}

UnitSnapshot hatchery(const UnitId id, const Position position, const Frame frame) {
    UnitSnapshot result;
    result.id = id;
    result.kind = UnitKind::hatchery;
    result.role = UnitRole::resourceDepot;
    result.position = position;
    result.lastPosition = position;
    result.lastSeen = frame;
    result.firstSeen = frame;
    result.visible = true;
    result.detected = true;
    result.completed = true;
    result.hitPoints = result.maxHitPoints = 1250;
    return result;
}

GameState naturalFixture() {
    GameState state;
    state.frame = 4 * 60 * 24;
    state.self.id = 1;
    state.enemy.id = 2;
    state.enemy.race = Race::zerg;
    state.bases = {
        base(1, {1024, 1024}, 2, true, 8000, 8, 1, 0),
        // The nearest-looking site lies across a cliff from the enemy main.
        base(2, {1120, 1024}, -1, false, 8000, 8, 1, -1),
        // A reachable mineral-only site is closer by route, but it is weaker
        // natural evidence than the reachable gas-bearing site.
        base(3, {1152, 1024}, -1, false, 8000, 8, 0, 250),
        base(4, {900, 1500}, -1, false, 8000, 8, 1, 600),
    };
    return state;
}

}  // namespace

int main() {
    using namespace protodd;
    bool passed = true;

    auto state = naturalFixture();
    auto estimate = enemyNaturalEstimate(state);
    passed &= check(estimate.enemyMainKnown && estimate.candidate != nullptr &&
                        estimate.candidate->id == 4 && estimate.candidateCount == 2,
                    "enemy natural ranks reachable resource sites rather than the nearest Euclidean marker");
    passed &= check(estimate.confidence < 0.75 && enemyNatural(state) == estimate.candidate,
                    "competing mineral-only and gas sites preserve natural-location uncertainty");
    state.bases[3].lastConfirmedEmpty = state.frame;
    OpponentModel uncertainEmpty;
    uncertainEmpty.update(state);
    passed &= check(!uncertainEmpty.assessment().enemyNaturalCheckedEmpty &&
                        uncertainEmpty.assessment().enemyNaturalConfidence < 0.75,
                    "an empty footprint at an ambiguous natural candidate is not treated as certainty");

    auto unknownMain = naturalFixture();
    unknownMain.bases[0].ownerId = -1;
    const auto unknownEstimate = enemyNaturalEstimate(unknownMain);
    passed &= check(!unknownEstimate.enemyMainKnown && unknownEstimate.candidate == nullptr &&
                        unknownEstimate.confidence == 0.0,
                    "an unobserved enemy start does not create a confident natural hypothesis");

    GameState bases;
    bases.frame = 5 * 60 * 24;
    bases.self.id = 1;
    bases.enemy.id = 2;
    bases.enemy.race = Race::zerg;
    bases.bases = {base(10, {512, 512}, 2, true, 8000, 8, 1, 0),
                   base(11, {1200, 512}, -1, false, 8000, 8, 1, 700)};
    bases.enemy.units = {hatchery(20, {512, 512}, bases.frame),
                         hatchery(21, {640, 512}, bases.frame)};
    OpponentModel oneSite;
    oneSite.update(bases);
    const auto oneSiteExpansion = oneSite.probability(EnemyPlan::fastExpand);
    bases.enemy.units.push_back(hatchery(22, {1200, 512}, bases.frame));
    ++bases.frame;
    bases.enemy.units.back().lastSeen = bases.frame;
    OpponentModel twoSites;
    twoSites.update(bases);
    passed &= check(twoSites.probability(EnemyPlan::fastExpand) > oneSiteExpansion,
                    "a second occupied resource site counts as expansion evidence");
    passed &= check(oneSiteExpansion < 0.2,
                    "multiple Hatcheries clustered at one main do not count as multiple bases");

    return passed ? 0 : 1;
}
