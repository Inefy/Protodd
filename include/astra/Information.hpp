#pragma once

#include "astra/GameState.hpp"

#include <array>
#include <cstddef>
#include <string_view>

namespace astra {

enum class EnemyPlan : std::uint8_t {
    unknown,
    workerRush,
    proxyRush,
    staticContain,
    fastRush,
    heavyPressure,
    fastExpand,
    fastTech,
    airTech,
    cloakedTech,
    count,
};

struct ThreatAssessment {
    double immediateGround{};
    double workerRush{};
    double proxy{};
    double staticContain{};
    double air{};
    double cloak{};
    double aggression{};
    double expansion{};
    double uncertainty{1.0};
    double estimatedArmyValue{};
    int enemiesNearMain{};
    EnemyPlan mostLikely{EnemyPlan::unknown};
};

class OpponentModel {
public:
    OpponentModel();

    void reset(Race enemyRace);
    void update(const GameState& state);

    [[nodiscard]] double probability(EnemyPlan plan) const noexcept;
    [[nodiscard]] const ThreatAssessment& assessment() const noexcept;
    [[nodiscard]] EnemyPlan mostLikelyPlan() const noexcept;

private:
    using Beliefs = std::array<double, static_cast<std::size_t>(EnemyPlan::count)>;

    Race enemyRace_{Race::unknown};
    Beliefs beliefs_{};
    ThreatAssessment assessment_{};
    Frame lastUpdate_{-1};

    void normalize() noexcept;
};

[[nodiscard]] std::string_view enemyPlanName(EnemyPlan plan) noexcept;

}  // namespace astra
