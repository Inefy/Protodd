#include "protodd/Workers.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <ranges>
#include <unordered_map>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::unordered_map<protodd::UnitId, protodd::UnitId> equalLoadBaseline(
    const std::vector<protodd::MineralWorker>& workers,
    const std::vector<protodd::MineralPatchCandidate>& patches) {
    using namespace protodd;
    std::vector<const MineralWorker*> ordered;
    ordered.reserve(workers.size());
    for (const auto& worker : workers) ordered.push_back(&worker);
    std::ranges::sort(ordered, {}, [](const MineralWorker* worker) { return worker->id; });

    std::unordered_map<UnitId, int> load;
    std::unordered_map<UnitId, UnitId> result;
    for (const auto* worker : ordered) {
        auto selected = UnitId{-1};
        auto best = std::numeric_limits<long long>::max();
        for (const auto& patch : patches) {
            const auto score = static_cast<long long>(load[patch.id]) * 1'000'000'000LL +
                static_cast<long long>(distanceSquared(patch.position, worker->mineralLine)) * 4LL +
                distanceSquared(patch.position, worker->position);
            if (score < best || (score == best && (selected < 0 || patch.id < selected))) {
                selected = patch.id;
                best = score;
            }
        }
        if (selected >= 0) {
            result[worker->id] = selected;
            ++load[selected];
        }
    }
    return result;
}

std::uint64_t simulateDeliveredMinerals(
    const std::vector<protodd::MineralWorker>& workers,
    const std::vector<protodd::MineralPatchCandidate>& patches,
    const std::unordered_map<protodd::UnitId, protodd::UnitId>& assignments,
    const protodd::MineralServiceModel model,
    const int horizonFrames) {
    using namespace protodd;
    std::unordered_map<UnitId, int> load;
    for (const auto& [worker, patch] : assignments) {
        static_cast<void>(worker);
        ++load[patch];
    }

    auto delivered = std::uint64_t{};
    for (const auto& worker : workers) {
        const auto assignment = assignments.find(worker.id);
        if (assignment == assignments.end()) continue;
        const auto patch = std::ranges::find(patches, assignment->second,
                                             &MineralPatchCandidate::id);
        if (patch == patches.end()) continue;
        const auto speed = std::isfinite(worker.topSpeed) && worker.topSpeed > 0.1
            ? worker.topSpeed : 4.0;
        const auto travel = [speed](const Position from, const Position to) {
            return static_cast<int>(std::ceil(distance(from, to) / speed));
        };
        const auto congestion = std::max(0, load[patch->id] - 1) *
                                std::max(0, model.additionalWorkerFrames);
        const auto cargoReturn = worker.carryingResources
            ? travel(worker.position, worker.mineralLine) : 0;
        const auto firstOrigin = worker.carryingResources
            ? worker.mineralLine : worker.position;
        const auto outbound = travel(firstOrigin, patch->position);
        const auto returnToDepot = travel(patch->position, worker.mineralLine);
        const auto switchCost = worker.currentTarget >= 0 &&
                worker.currentTarget != patch->id
            ? model.switchingFrames : 0;
        const auto firstDelivery = cargoReturn + outbound + returnToDepot +
            model.miningFrames + congestion + switchCost;
        if (firstDelivery <= 0 || firstDelivery > horizonFrames) continue;
        const auto cycleFrames = travel(worker.mineralLine, patch->position) + returnToDepot +
                                 model.miningFrames + congestion;
        if (cycleFrames <= 0) continue;
        const auto trips = 1 + (horizonFrames - firstDelivery) / cycleFrames;
        delivered += static_cast<std::uint64_t>(trips) * 8U;
    }
    return delivered;
}

void benchmarkScenario(const std::vector<protodd::MineralWorker>& workers,
                       const std::vector<protodd::MineralPatchCandidate>& patches,
                       const std::uint64_t expectedMinimum) {
    using namespace protodd;
    const MineralServiceModel model{96, 48, 48};
    const auto baseline = equalLoadBaseline(workers, patches);
    MineralAllocator allocator{model};
    const auto estimated = allocator.assign(workers, patches);
    const auto baselineMinerals = simulateDeliveredMinerals(
        workers, patches, baseline, model, 24 * 60 * 5);
    const auto estimatedMinerals = simulateDeliveredMinerals(
        workers, patches, estimated, model, 24 * 60 * 5);
    check(baselineMinerals > 0 && estimatedMinerals >= baselineMinerals &&
              estimatedMinerals >= expectedMinimum,
          "service-time assignment preserves or improves delivered minerals over equal-load allocation");
    std::cout << "mineral-delivery benchmark: equal-load=" << baselineMinerals
              << " estimated-service=" << estimatedMinerals << " minerals/5min\n";
}

void testRoundTripCargoAndSwitchingCosts() {
    using namespace protodd;
    const MineralPatchCandidate patch{7, {120, 0}, 0};
    MineralWorker worker{1, {80, 0}, {0, 0}, 7, 4.0, false};
    const auto empty = estimateMineralService(worker, patch, 0, 7);
    check(empty.travelToPatchFrames == 10 && empty.travelToDepotFrames == 30 &&
              empty.cargoReturnFrames == 0 && empty.switchingFrames == 0,
          "empty worker estimates outbound and return travel without switching cost");

    worker.carryingResources = true;
    const auto carrying = estimateMineralService(worker, patch, 0, 7);
    check(carrying.cargoReturnFrames == 20 && carrying.travelToPatchFrames == 30 &&
              carrying.travelToDepotFrames == 30 &&
              carrying.totalFrames == empty.totalFrames + 40,
          "cargo state prices a depot return before starting the next mineral trip");

    const auto retargeted = estimateMineralService(worker, patch, 0, 8);
    check(retargeted.switchingFrames == 48 &&
              retargeted.totalFrames == carrying.totalFrames + 48,
          "changing patches pays a bounded switching cost");
    const auto crowded = estimateMineralService(worker, patch, 3, 7);
    check(crowded.congestionFrames == 96,
          "service estimate accounts for other miners sharing a patch");
}

void testDeliveryBenchmarkAgainstEqualLoadBaseline() {
    using namespace protodd;
    constexpr Position depot{0, 0};
    const std::vector<MineralPatchCandidate> asymmetricPatches{
        {10, {32, 0}, 0}, {11, {432, 0}, 0},
    };
    std::vector<MineralWorker> workers;
    workers.push_back({1, depot, depot, -1, 4.0, false});
    workers.push_back({2, depot, depot, -1, 4.0, false});
    benchmarkScenario(workers, asymmetricPatches, 720);

    const std::vector<MineralPatchCandidate> balancedPatches{
        {20, {64, 0}, 0}, {21, {180, 100}, 0}, {22, {220, 64}, 0},
    };
    for (int id = 3; id < 15; ++id) {
        const Position start{(id % 4) * 24, (id % 3) * 20};
        workers.push_back({id, start, depot, -1,
                           id % 4 == 0 ? 3.8 : 4.0, id % 5 == 0});
    }
    benchmarkScenario(workers, balancedPatches, 2232);

    MineralAllocator allocator;
    const auto estimated = allocator.assign(workers, balancedPatches);
    check(estimated.size() == workers.size(),
          "all eligible workers retain a valid mineral assignment");
    const auto repeat = allocator.assign(workers, balancedPatches);
    check(repeat == estimated,
          "repeated assignment with unchanged workers does not reshuffle mineral targets");
}

}  // namespace

int main() {
    testRoundTripCargoAndSwitchingCosts();
    testDeliveryBenchmarkAgainstEqualLoadBaseline();
    if (failures != 0) return 1;
    return 0;
}
