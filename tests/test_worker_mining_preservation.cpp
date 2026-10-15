#include "protodd/Workers.hpp"

#include <algorithm>
#include <iostream>

using namespace protodd;

int main() {
    int failures = 0;
    const auto check = [&](const bool condition, const char* message) {
        if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };
    GameState state;
    state.frame = 11000; state.self.id = 1; state.enemy.id = 2;
    state.mapWidthPixels = state.mapHeightPixels = 1024;
    BaseSnapshot main;
    main.id = 1; main.ownerId = 1; main.center = {448, 512};
    main.mineralLine = {640, 512}; main.mineralsRemaining = 7000;
    main.mineralPatches = 8;
    auto natural = main;
    natural.id = 2; natural.center = {832, 512};
    natural.mineralLine = {880, 512}; natural.mineralsRemaining = 12000;
    state.bases = {main, natural};
    UnitSnapshot nexus;
    nexus.id = 1; nexus.kind = UnitKind::nexus;
    nexus.role = UnitRole::resourceDepot; nexus.completed = true;
    nexus.position = main.center;
    auto second = nexus; second.id = 2; second.position = natural.center;
    state.self.units = {nexus, second};
    UnitSnapshot worker;
    worker.id = 10; worker.kind = UnitKind::probe; worker.role = UnitRole::worker;
    worker.position = worker.lastPosition = {360, 512};
    worker.completed = worker.visible = worker.ours = true;
    worker.hitPoints = worker.maxHitPoints = worker.shields = worker.maxShields = 20;
    worker.topSpeed = 4; worker.gatheringMinerals = true;
    state.self.units.push_back(worker);
    std::vector<std::uint8_t> cells(128U * 128U, 1U);
    for (int y = 0; y < 128; ++y) cells[static_cast<std::size_t>(y * 128 + 64)] = 0;
    cells[8U * 128U + 64U] = 1;
    NavigationGrid map{128, 128, 8, std::move(cells)};
    auto exhausted = WorkerRoutingLimits{};
    exhausted.totalQueries = 0; exhausted.totalExpansions = 0;
    WorkerManager manager;
    for (int tick = 0; tick < 12; ++tick) {
        state.frame += 12;
        const auto jobs = manager.assign(state, {}, InfluenceMap{}, {}, false, false, &map, exhausted);
        check(manager.routingStats().economicDeferredChecks > 0,
              "the regression actually exhausts the economic route budget");
        check(jobs.size() == 1 && jobs.front().job == WorkerJob::minerals &&
                  jobs.front().baseId == main.id && jobs.front().preserveNativeOrder,
              "a rich safe local miner retains its native cycle instead of evacuating to the natural");
        check(manager.constructionExclusions(state).empty(),
              "a deferred economic proof does not label a healthy miner an emergency");
    }
    auto depleted = state;
    depleted.bases.front().mineralsRemaining = 0;
    auto jobs = WorkerManager{}.assign(depleted, {}, InfluenceMap{}, {}, false, false, &map, exhausted);
    check(!jobs.front().preserveNativeOrder,
          "a depleted local mineral line cannot retain a stale mining order");
    auto destroyed = state;
    destroyed.self.units.erase(destroyed.self.units.begin());
    jobs = WorkerManager{}.assign(destroyed, {}, InfluenceMap{}, {}, false, false, &map, exhausted);
    check(!jobs.front().preserveNativeOrder,
          "a lost local Nexus cannot certify an existing mining cycle");
    auto attacked = state;
    attacked.self.units.back().underAttack = true;
    jobs = WorkerManager{}.assign(attacked, {}, InfluenceMap{}, {}, false, false, &map, exhausted);
    check(jobs.front().job == WorkerJob::evacuate && !jobs.front().preserveNativeOrder,
          "actual danger takes precedence over retaining mineral income");
    const UnitId leased[]{worker.id};
    jobs = WorkerManager{}.assign(state, {}, InfluenceMap{}, leased, false, false, &map, exhausted);
    check(jobs.front().job == WorkerJob::build && !jobs.front().preserveNativeOrder,
          "a construction lease takes precedence over retaining a mining cycle");
    MineralAllocator allocator;
    const MineralPatchCandidate patches[]{{100, {440, 512}, 0}, {101, {360, 512}, 0}};
    const MineralWorker miners[]{{10, {360, 512}, {448, 512}, 100, 4, false, true},
                                 {11, {360, 512}, {448, 512}, -1, 4, false, false}};
    const auto& targets = allocator.assign(miners, patches);
    check(targets.at(10) == 100 && targets.at(11) == 101,
          "the allocator keeps the actual retained patch and accounts for its worker load");
    return failures == 0 ? 0 : 1;
}
