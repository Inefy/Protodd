#include "protodd/Information.hpp"

#include <cmath>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using namespace protodd;

UnitSnapshot unit(const UnitId id, const UnitKind kind, const Position position,
                  const Frame seen, const bool visible = true) {
    UnitSnapshot result;
    result.id = id;
    result.kind = kind;
    result.position = position;
    result.lastPosition = position;
    result.lastSeen = seen;
    result.firstSeen = seen;
    result.visible = visible;
    result.detected = visible;
    result.completed = true;
    result.hitPoints = result.maxHitPoints = 100;
    return result;
}

GameState stateAt(const Frame frame) {
    GameState state;
    state.frame = frame;
    state.self.id = 1;
    state.enemy.id = 2;
    state.enemy.race = Race::zerg;
    auto nexus = unit(1, UnitKind::nexus, {512, 512}, frame);
    nexus.ours = true;
    nexus.role = UnitRole::resourceDepot;
    state.self.units.push_back(nexus);
    return state;
}

bool check(const bool condition, const std::string_view message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

struct HeldOutCase {
    const char* name{};
    GameState state;
    EnemyPlan target{EnemyPlan::fastRush};
    bool rushByDeadline{};
};

}  // namespace

int main() {
    using namespace protodd;
    bool passed = true;

    auto blind = stateAt(8 * 60 * 24);
    OpponentModel blindModel;
    blindModel.update(blind);
    passed &= check(blindModel.mostLikelyPlan() == EnemyPlan::unknown &&
                        blindModel.probability(EnemyPlan::unknown) >
                            blindModel.probability(EnemyPlan::fastRush) &&
                        blindModel.assessment().uncertainty > 0.95 &&
                        blindModel.assessment().latestEvidenceFrame == -1,
                    "no scouting preserves an uncertain Unknown assessment");

    auto approach = stateAt(10'800);
    auto marine = unit(20, UnitKind::marine, {1500, 512}, approach.frame);
    marine.lastPosition = {1600, 512};
    marine.topSpeed = 3.0;
    approach.enemy.units.push_back(marine);
    OpponentModel approachModel;
    approachModel.update(approach);
    const auto timing = approachModel.assessment();
    passed &= check(timing.earliestApproachArrivalFrame >= approach.frame &&
                        timing.latestApproachArrivalFrame >=
                            timing.earliestApproachArrivalFrame &&
                        timing.latestEvidenceFrame == approach.frame &&
                        timing.evidence[static_cast<std::size_t>(
                            ThreatEvidenceFamily::rushCombat)].observedSources == 1,
                    "visible movement toward home reports a travel interval and evidence stamp");

    auto production = stateAt(3'000);
    auto barracks = unit(30, UnitKind::barracks, {1800, 512}, production.frame);
    barracks.completed = false;
    barracks.buildProgress = 50;
    production.enemy.units.push_back(barracks);
    OpponentModel productionModel;
    productionModel.update(production);
    passed &= check(productionModel.assessment().earliestProductionReadyFrame == 3'960,
                    "visible production progress yields an optimistic legal ready-time lower bound");

    auto zergProduction = stateAt(3'000);
    zergProduction.enemy.units.push_back(
        unit(31, UnitKind::hatchery, {1900, 512}, zergProduction.frame));
    auto pool = unit(32, UnitKind::spawningPool, {1800, 700}, zergProduction.frame);
    pool.completed = false;
    pool.buildProgress = 50;
    zergProduction.enemy.units.push_back(pool);
    OpponentModel zergProductionModel;
    zergProductionModel.update(zergProduction);
    passed &= check(zergProductionModel.assessment().earliestProductionReadyFrame == 4'020,
                    "observed Pool progress bounds earliest Zergling readiness");

    auto repeated = stateAt(2'000);
    auto zealotA = unit(40, UnitKind::zealot, {900, 900}, 1'800, false);
    auto zealotB = unit(41, UnitKind::zealot, {920, 900}, 1'850, false);
    repeated.enemy.units = {zealotA, zealotB};
    OpponentModel repeatedModel;
    repeatedModel.update(repeated);
    const auto firstProbability = repeatedModel.probability(EnemyPlan::fastRush);
    const auto firstStamp = repeatedModel.assessment().evidence[static_cast<std::size_t>(
        ThreatEvidenceFamily::rushCombat)];
    ++repeated.frame;
    repeatedModel.update(repeated);
    const auto secondStamp = repeatedModel.assessment().evidence[static_cast<std::size_t>(
        ThreatEvidenceFamily::rushCombat)];
    passed &= check(firstStamp.observedSources == 2 && secondStamp.observedSources == 2 &&
                        firstStamp.latestFrame == 1'850 && secondStamp.latestFrame == 1'850 &&
                        std::abs(firstProbability -
                                 repeatedModel.probability(EnemyPlan::fastRush)) < 1e-12,
                    "repeated callbacks do not turn the same sightings into new confirmations");

    std::vector<HeldOutCase> heldOut;
    heldOut.push_back({"unscouted", stateAt(8 * 60 * 24), EnemyPlan::fastRush, false});
    auto workerScout = stateAt(4 * 60 * 24);
    workerScout.enemy.units.push_back(unit(50, UnitKind::drone, {1800, 1700},
                                                workerScout.frame));
    heldOut.push_back({"single-remote-worker", workerScout, EnemyPlan::workerRush, false});
    auto remoteLing = stateAt(5 * 60 * 24);
    remoteLing.enemy.units.push_back(unit(51, UnitKind::zergling, {2300, 1900},
                                               remoteLing.frame));
    heldOut.push_back({"single-remote-zergling", remoteLing, EnemyPlan::fastRush, false});
    auto stalePool = stateAt(3'200);
    auto latePool = unit(52, UnitKind::spawningPool, {1900, 1900}, 3'100);
    latePool.firstSeen = 3'100;
    latePool.constructionStartUpperBound = 1'900;
    stalePool.enemy.units.push_back(latePool);
    heldOut.push_back({"late-pool", stalePool, EnemyPlan::fastRush, false});
    auto clusteredWorkers = stateAt(2'500);
    for (int i = 0; i < 3; ++i) {
        clusteredWorkers.enemy.units.push_back(unit(60 + i, UnitKind::drone,
            {540 + i * 20, 540}, clusteredWorkers.frame));
    }
    heldOut.push_back({"clustered-workers", clusteredWorkers,
                       EnemyPlan::workerRush, true});
    auto earlyPool = stateAt(1'600);
    auto rushPool = unit(70, UnitKind::spawningPool, {1900, 1900}, earlyPool.frame);
    rushPool.firstSeen = 1'200;
    rushPool.constructionStartUpperBound = 1'000;
    earlyPool.enemy.units.push_back(rushPool);
    heldOut.push_back({"early-pool", earlyPool, EnemyPlan::fastRush, true});

    constexpr double classificationThreshold = 0.40;
    int falseAlarms = 0;
    int falseNegatives = 0;
    double brier = 0.0;
    std::cout << "scenario,target,predicted,truth,classified_rush\n";
    for (const auto& scenario : heldOut) {
        OpponentModel model;
        model.update(scenario.state);
        const auto prediction = model.probability(scenario.target);
        const auto classified = prediction >= classificationThreshold;
        falseAlarms += !scenario.rushByDeadline && classified ? 1 : 0;
        falseNegatives += scenario.rushByDeadline && !classified ? 1 : 0;
        const auto error = prediction - (scenario.rushByDeadline ? 1.0 : 0.0);
        brier += error * error;
        std::cout << scenario.name << ',' << enemyPlanName(scenario.target) << ','
                  << std::fixed << std::setprecision(4) << prediction << ','
                  << (scenario.rushByDeadline ? 1 : 0) << ',' << (classified ? 1 : 0)
                  << '\n';
    }
    brier /= static_cast<double>(heldOut.size());
    std::cout << "HELDOUT_CALIBRATION,scenarios=" << heldOut.size()
              << ",falseAlarms=" << falseAlarms
              << ",falseNegatives=" << falseNegatives
              << ",threshold=" << classificationThreshold
              << ",brier=" << std::fixed << std::setprecision(4) << brier << '\n';
    passed &= check(falseAlarms == 0 && falseNegatives == 0 && brier < 0.25,
                    "held-out opening fixtures meet the declared false-alarm and score gates");
    return passed ? 0 : 1;
}
