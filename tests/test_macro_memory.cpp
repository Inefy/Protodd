#include "protodd/MacroEnemyMemory.hpp"
#include "protodd/MacroObservationStream.hpp"
#include "protodd/ObservationEncoder.hpp"
#include <algorithm>
#include <iostream>

int main() {
    using namespace protodd;
    int errors = 0;
    const auto check = [&](bool value, const char* message) { if (!value) { ++errors; std::cerr << message << '\n'; } };
    MacroEnemyMemory memory;
    UnitSnapshot enemy;
    enemy.id = 9; enemy.kind = UnitKind::marine; enemy.race = Race::terran;
    enemy.position = {320, 320}; enemy.visible = false; enemy.detected = true;
    auto update = [&](int frame, std::vector<UnitSnapshot> units, std::vector<UnitId> removed, bool tile) {
        memory.update(frame, units, removed, [=](Position p) { check(p == Position{320,320}, "memory used unobserved position"); return tile; });
    };
    update(0, {enemy}, {}, false);
    check(memory.snapshot().empty(), "never-seen enemy leaked");
    enemy.visible = true; enemy.detected = false;
    update(1, {enemy}, {}, false);
    check(memory.snapshot().empty(), "undetected cloak leaked");
    enemy.detected = true;
    update(2, {enemy}, {}, false);
    check(memory.snapshot().size() == 1 && memory.snapshot()[0].firstSeen == 2, "legal observation not recorded");
    enemy.visible = false; enemy.position = {1800, 1900}; enemy.kind = UnitKind::battlecruiser;
    update(3, {enemy}, {}, false);
    check(memory.snapshot()[0].kind == UnitKind::marine && memory.snapshot()[0].position == Position{320,320}, "hidden state changed memory");
    update(4, {}, {}, true);
    check(memory.snapshot().size() == 1, "mobile unit forgotten from visible old tile");
    update(5, {}, {9}, true);
    check(memory.snapshot().empty(), "observed death retained");
    enemy.visible = true; enemy.kind = UnitKind::barracks; enemy.position = {320,320};
    update(6, {enemy}, {}, false);
    update(7, {}, {}, false);
    check(memory.snapshot().size() == 1, "unseen building disappearance leaked");
    update(8, {}, {}, true);
    check(memory.snapshot().empty(), "visible empty building tile retained");
    update(9, {enemy}, {}, false);
    update(0, {}, {}, false);
    check(memory.snapshot().empty(), "replay boundary retained enemy memory");
    GameState a; a.self.race = Race::protoss; a.enemy.race = Race::terran;
    auto b = a; b.enemy.race = Race::zerg; b.latencyFrames = 99;
    ObservationEncoder ea, eb;
    check(ea.encode(a) == eb.encode(b), "header race or unavailable latency leaked into v2");

    // Replay extraction observes every frame but encodes only on 24-frame
    // boundaries. A live shadow model must retain a scout's brief sighting even
    // when inference is skipped, and must not change the income-history window.
    MacroObservationStream live;
    MacroEnemyMemory replay;
    ObservationEncoder replayEncoder;
    FrameBudget budget;
    for (Frame frame = 0; frame <= 48; ++frame) {
        GameState state;
        state.frame = frame; state.self.race = Race::protoss;
        state.self.gatheredMinerals = frame / 4;
        state.self.gatheredGas = frame / 8;
        UnitSnapshot flash;
        flash.id = 21; flash.kind = UnitKind::marine; flash.race = Race::terran;
        flash.position = {320, 320}; flash.detected = true;
        flash.visible = frame == 5 || frame == 6;
        if (frame >= 5 && frame < 40) state.enemy.units.push_back(flash);
        UnitSnapshot cloak = flash;
        cloak.id = 22; cloak.kind = UnitKind::darkTemplar; cloak.race = Race::protoss;
        cloak.visible = true; cloak.detected = false;
        state.enemy.units.push_back(cloak);
        UnitSnapshot building = flash;
        building.id = 23; building.kind = UnitKind::barracks; building.visible = frame == 8;
        if (frame >= 8 && frame <= 10) state.enemy.units.push_back(building);
        std::vector<UnitId> removed;
        if (frame == 40) removed.push_back(21);
        const auto visibleTile = [frame](Position) { return frame == 10; };
        live.observe(state, removed, visibleTile);
        replay.update(frame, state.enemy.units, removed, visibleTile);
        if (frame % 24 == 0) {
            auto reference = state;
            reference.enemy.units = replay.snapshot();
            const auto expected = replayEncoder.encode(reference);
            check(std::ranges::equal(live.features(), expected), "live/replay feature history differs");
            check(live.mask() == learnedIntentMask(reference), "live/replay intent masks differ");
            check(live.sampleFrame() == frame, "feature sample shifted to inference cadence");
        }
        if (frame == 7) {
            check(live.inferenceDue(frame), "shadow inference was not staggered");
            check(live.beginInference(frame, budget), "normal load skipped inference");
            check(!live.beginInference(frame, budget), "same observation inferred twice");
        }
        if (frame == 24) {
            const auto remembered = live.rememberedEnemies();
            check(remembered.size() == 1, "flash sighting lost or cloak/empty building leaked");
            if (!remembered.empty()) {
                check(remembered[0].id == 21 && remembered[0].lastSeen == 6 &&
                      remembered[0].firstSeen == 5 && !remembered[0].visible,
                      "flash sighting timestamps changed between samples");
            }
            check(!live.inferenceDue(frame), "inference shares strategy/sample frame");
        }
        if (frame == 30) budget.record(frame, 29000);
        if (frame == 31) {
            check(live.inferenceDue(frame), "scheduled shadow tick missing");
            check(!live.beginInference(frame, budget), "reduced load ran shadow inference");
            check(!live.inferenceDue(frame), "shed observation was not consumed");
        }
        if (frame == 32) check(!live.beginInference(frame, budget), "stale inference ran after a skipped tick");
    }
    check(live.rememberedEnemies().empty(), "observed death retained in live stream");
    FrameBudget emergency;
    emergency.record(50, 40000);
    check(!live.beginInference(55, emergency), "emergency load ran shadow inference");
    GameState restart; restart.self.race = Race::protoss;
    live.observe(restart, {}, [](Position) { return false; });
    ObservationEncoder fresh;
    check(std::ranges::equal(live.features(), fresh.encode(restart)), "game restart retained feature history");
    check(live.sampleFrame() == 0 && live.rememberedEnemies().empty(), "game restart retained observation state");
    budget.reset();
    check(live.beginInference(7, budget), "game restart retained consumed inference tick");
    return errors ? 1 : 0;
}
