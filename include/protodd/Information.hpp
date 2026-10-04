#pragma once

#include "protodd/GameState.hpp"

#include <array>
#include <cstddef>
#include <string_view>

namespace protodd {

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
#ifdef PROTODD_PVT_SCOUT_TRANSITIONS
    mechanized,
#endif
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
    double approachingArmyValue{};
    double enemyProductionCapacity{};
    int enemiesNearMain{};
    int combatEnemiesNearMain{};
    int approachingCombatEnemies{};
    EnemyPlan mostLikely{EnemyPlan::unknown};
    bool enemyNaturalCheckedEmpty{};
#ifdef PROTODD_PVT_SCOUT_TRANSITIONS
    // Legal, age-bounded mech observations. A stale clue is unknown, not proof
    // that the opponent has no mech production or units.
    double mechanizedConfidence{};
    Frame mechanizedEvidenceAge{-1};
    bool mechanizedEvidenceFresh{};
    bool mechanizedPlanActive{};
#endif
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
#ifdef PROTODD_PVT_SCOUT_TRANSITIONS
    Frame lastMechanizedEvidenceFrame_{-1};
    bool mechanizedPlanActive_{};
#endif

    void normalize() noexcept;
};

[[nodiscard]] std::string_view enemyPlanName(EnemyPlan plan) noexcept;
[[nodiscard]] const BaseSnapshot* enemyNatural(const GameState& state) noexcept;

}  // namespace protodd
