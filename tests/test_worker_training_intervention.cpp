#include "protodd/WorkerTrainingIntervention.hpp"
#include <stdexcept>

using namespace protodd;

void require(bool condition) {
    if (!condition) throw std::runtime_error("worker training intervention contract failed");
}

int main() {
    GameState state;
    state.frame = 4800;
    state.self.race = Race::protoss;
    UnitSnapshot nexus; nexus.kind = UnitKind::nexus;
    state.self.units.push_back(nexus);
    UnitSnapshot probe; probe.kind = UnitKind::probe;
    for (int i = 0; i < 19; ++i) state.self.units.push_back(probe);
    state.self.queuedUnits.push_back(UnitKind::probe);
    StrategicPlan baseline; baseline.desiredWorkers = 22;
    baseline.goals.push_back({GoalKind::train, UnitKind::probe, 22, 93, false, "normal"});
    const auto unchanged = baseline;
    require(!applyWorkerTrainingIntervention(baseline, state, WorkerTrainingProfile::baseline).applied);
    require(baseline.desiredWorkers == unchanged.desiredWorkers);
    require(baseline.goals[0].priority == unchanged.goals[0].priority);
    auto one = unchanged;
    auto oneDecision = applyWorkerTrainingIntervention(one, state, WorkerTrainingProfile::plusOne);
    require(oneDecision.applied && oneDecision.committed == 20);
    require(one.desiredWorkers == 23 && one.goals[0].priority == 100);
    auto two = unchanged;
    auto twoDecision = applyWorkerTrainingIntervention(two, state, WorkerTrainingProfile::plusTwo);
    require(twoDecision.applied && two.desiredWorkers == 24 && two.goals[0].priority == 108);
    require(two.goals[0].desiredCount == 24);
    two.prioritizeReinforcements = true;
    require(!applyWorkerTrainingIntervention(two, state, WorkerTrainingProfile::plusTwo).applied);
    state.frame = 7200;
    auto out = unchanged;
    require(!applyWorkerTrainingIntervention(out, state, WorkerTrainingProfile::plusTwo).applied);
}
