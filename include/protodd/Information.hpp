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
    count,
};

enum class ThreatEvidenceFamily : std::uint8_t {
    localWorkers,
    forwardStructures,
    rushCombat,
    technology,
    air,
    cloak,
    expansion,
    count,
};

struct ThreatEvidenceStamp {
    Frame latestFrame{-1};
    int observedSources{};
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
    double enemyNaturalConfidence{};
    int enemyNaturalCandidateCount{};
    // Current legal evidence grouped by coarse family. A source is counted
    // once per snapshot, regardless of callback repetition; family membership
    // is diagnostic and does not multiply the posterior.
    std::array<ThreatEvidenceStamp,
               static_cast<std::size_t>(ThreatEvidenceFamily::count)> evidence{};
    Frame latestEvidenceFrame{-1};
    Frame earliestProductionReadyFrame{-1};
    // A travel-only envelope for a visibly approaching combat unit. The
    // upper end assumes continued movement and a broad route/speed margin.
    Frame earliestApproachArrivalFrame{-1};
    Frame latestApproachArrivalFrame{-1};
    int evidenceFamiliesPresent{};
};

struct EnemyNaturalEstimate {
    const BaseSnapshot* candidate{};
    double confidence{};
    int candidateCount{};
    bool enemyMainKnown{};
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
[[nodiscard]] EnemyNaturalEstimate enemyNaturalEstimate(const GameState& state) noexcept;
[[nodiscard]] const BaseSnapshot* enemyNatural(const GameState& state) noexcept;

}  // namespace protodd
