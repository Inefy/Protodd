#include "protodd/GameState.hpp"
#include "protodd/InfluenceMap.hpp"
#include "protodd/UnitMemory.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

namespace {
int failures{};

void expect(const bool condition, const std::string_view message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

protodd::UnitSnapshot threat(protodd::UnitKind kind, protodd::Position position) {
    using namespace protodd;
    UnitSnapshot unit;
    unit.id = 17;
    unit.kind = kind;
    unit.position = position;
    unit.lastPosition = position;
    unit.lastSeen = 0;
    unit.completed = true;
    unit.visible = false;
    unit.detected = false;
    unit.hitPoints = 20;
    unit.maxHitPoints = 100;
    unit.groundWeapon = {12, 24, 0, 96, DamageType::normal, false, true, 1};
    return unit;
}
}

int main() {
    using namespace protodd;

    const std::array<std::uint8_t, 12> full{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    auto partial = full;
    partial[11] = 0;
    expect(fullyVisibleFootprint(4, 3, full),
           "all tiles in a 4x3 structure footprint are visible");
    expect(!fullyVisibleFootprint(4, 3, partial),
           "one fogged edge tile prevents an empty-footprint conclusion");
    expect(!fullyVisibleFootprint(4, 3, std::span<const std::uint8_t>(full).first(11)),
           "an incomplete footprint sample is not evidence of vacancy");

    UnitSnapshot rememberedStructure = threat(UnitKind::photonCannon, {768, 768});
    rememberedStructure.visible = true;
    rememberedStructure.detected = true;
    rememberedStructure.lastSeen = 100;
    auto partialFootprint = reconcileEnemyMemory(
        rememberedStructure, std::nullopt, 200, false, false);
    expect(!partialFootprint.forget && partialFootprint.snapshot.position.valid(),
           "partial footprint visibility retains a remembered static structure");
    auto emptyStaticSite = reconcileEnemyMemory(
        rememberedStructure, std::nullopt, 200, true, false);
    expect(emptyStaticSite.forget,
           "a fully visible empty footprint removes a non-liftable building memory");
    auto liftableStructure = rememberedStructure;
    liftableStructure.kind = UnitKind::barracks;
    auto emptyLiftableSite = reconcileEnemyMemory(
        liftableStructure, std::nullopt, 200, true, true);
    expect(!emptyLiftableSite.forget && !emptyLiftableSite.snapshot.position.valid() &&
               emptyLiftableSite.snapshot.lastPosition == liftableStructure.position &&
               emptyLiftableSite.snapshot.existenceConfidence > 0.0 &&
               emptyLiftableSite.snapshot.locationConfidence == 0.0,
           "an empty liftable-building site clears location but retains remembered identity");
    auto detectionLost = reconcileEnemyMemory(
        threat(UnitKind::darkTemplar, {512, 512}), std::nullopt, 200, false, false);
    expect(!detectionLost.snapshot.visible && !detectionLost.snapshot.detected &&
               !detectionLost.snapshot.underAttack &&
               detectionLost.snapshot.position == Position{512, 512},
           "detection loss retains only the last legal sample and clears current-state sentinels");

    auto mobile = threat(UnitKind::marine, {512, 512});
    mobile.healthConfidence = 1.0;
    mobile.updateMemoryConfidence(24 * 12);
    expect(mobile.existenceConfidence > mobile.locationConfidence &&
               std::abs(mobile.locationConfidence - std::exp(-1.0)) < 1e-9,
           "mobile existence and location confidence decay on distinct horizons");
    expect(mobile.estimatedHealthFraction() > mobile.healthFraction(),
           "uncertain old health estimates move conservatively toward full health");
    expect(mobile.hitPoints == 20,
           "confidence updates preserve the last observed health sample");

    GameState state;
    state.frame = 24 * 12 * 60 * 10;
    state.mapWidthPixels = state.mapHeightPixels = 2048;
    mobile.updateMemoryConfidence(state.frame);
    state.enemy.units = {mobile};
    InfluenceMap influence;
    influence.update(state);
    expect(influence.at({512, 512}).groundThreat < 0.001F,
           "an old mobile sighting no longer creates a permanent local threat alarm");

    auto structure = threat(UnitKind::bunker, {1024, 1024});
    structure.updateMemoryConfidence(state.frame);
    expect(structure.existenceConfidence == 1.0 && structure.locationConfidence == 1.0,
           "a known structure retains existence and location while its footprint is unverified");
    state.enemy.units = {structure};
    influence.update(state);
    expect(influence.at({1024, 1024}).groundThreat > 0.0F,
           "an incompletely observed building footprint remains a threat candidate");

    auto lifted = structure;
    lifted.lastPosition = lifted.position;
    lifted.position = {-1, -1};
    lifted.updateMemoryConfidence(state.frame);
    expect(lifted.existenceConfidence > 0.0 && lifted.locationConfidence == 0.0,
           "an empty former landing site can clear location without deleting known existence");

    auto hatchery = threat(UnitKind::hatchery, {768, 768});
    hatchery.firstSeen = 100;
    hatchery.lastSeen = 500;
    hatchery.constructionStartUpperBound = 200;
    auto lair = hatchery;
    lair.kind = UnitKind::lair;
    lair.position = {800, 768};
    lair.firstSeen = 700;
    lair.lastSeen = 700;
    lair.visible = true;
    lair.detected = true;
    lair.constructionStartUpperBound = 650;
    const auto morph = reconcileEnemyMemory(hatchery, lair, 700, false, false);
    expect(!morph.forget && morph.snapshot.position == Position{800, 768} &&
               morph.snapshot.lastPosition == Position{768, 768} &&
               morph.snapshot.firstSeen == 700 &&
               morph.snapshot.constructionStartUpperBound == 650,
           "a Zerg morph keeps its identity and last location but starts a new type-age estimate");

    return failures == 0 ? 0 : 1;
}
