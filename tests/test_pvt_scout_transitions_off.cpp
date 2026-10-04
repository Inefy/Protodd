#include "protodd/Information.hpp"

#include <cmath>
#include <iostream>

int main() {
    using namespace protodd;
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { ++failures; std::cerr << message << '\n'; }
    };
    GameState state;
    state.frame = 8 * 60 * 24;
    state.enemy.race = Race::terran;
    UnitSnapshot tank;
    tank.id = 5;
    tank.kind = UnitKind::siegeTank;
    tank.visible = true;
    tank.completed = true;
    tank.lastSeen = state.frame;
    tank.position = {2400, 2400};
    state.enemy.units.push_back(tank);

    OpponentModel model;
    model.update(state);
    constexpr auto baselineTotal = 11.5;
    const auto baselineUnknown = 2.5 / baselineTotal;
    const auto baselineOther = 1.0 / baselineTotal;
    const auto baselineEntropy = -baselineUnknown * std::log(baselineUnknown) -
        9.0 * baselineOther * std::log(baselineOther);
    const auto baselineUncertainty = baselineEntropy / std::log(10.0);
    check(model.mostLikelyPlan() == EnemyPlan::unknown,
          "default-off model preserves the baseline unknown plan for one Tank");
    check(std::abs(model.probability(EnemyPlan::unknown) - baselineUnknown) < 1e-12 &&
              std::abs(model.probability(EnemyPlan::fastExpand) - baselineOther) < 1e-12,
          "default-off probabilities preserve the baseline ten-plan normalization");
    check(std::abs(model.assessment().uncertainty - baselineUncertainty) < 1e-12,
          "default-off uncertainty matches the baseline ten-plan distribution");
    return failures == 0 ? 0 : 1;
}
