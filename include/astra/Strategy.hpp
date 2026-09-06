#pragma once

#include "astra/GameState.hpp"
#include "astra/Information.hpp"

#include <string>
#include <vector>

namespace astra {

enum class Posture : std::uint8_t { hold, defend, pressure, attack, harass, recover };

enum class OpeningStyle : std::uint8_t { standard, aggressive, economic, deceptive, count };

enum class GoalKind : std::uint8_t { build, train, expand, detect, research, upgrade };

struct ProductionGoal {
    GoalKind goal{GoalKind::train};
    UnitKind target{UnitKind::unknown};
    int desiredCount{};
    int priority{};
    bool blocking{};
    std::string reason;
    TechnologyKind technology{TechnologyKind::none};
};

struct CompositionTarget {
    UnitKind kind{UnitKind::unknown};
    double weight{};
};

struct StrategicPlan {
    std::string name;
    Posture posture{Posture::hold};
    int desiredBases{1};
    int desiredWorkers{8};
    int desiredGasWorkers{};
    double attackThreshold{1.25};
    int minimumAttackSize{8};
    Position rallyPoint{-1, -1};
    Position attackTarget{-1, -1};
    std::vector<ProductionGoal> goals;
    std::vector<CompositionTarget> composition;
    // Explicit matchup safety constraints survive style and recovery modifiers.
    int maximumBases{8};
};

class StrategyEngine {
public:
    [[nodiscard]] StrategicPlan plan(
        const GameState& state,
        const ThreatAssessment& threat,
        OpeningStyle style = OpeningStyle::standard) const;

private:
    [[nodiscard]] StrategicPlan planPvT(
        const GameState& state,
        const ThreatAssessment& threat) const;
    [[nodiscard]] StrategicPlan planPvZ(
        const GameState& state,
        const ThreatAssessment& threat) const;
    [[nodiscard]] StrategicPlan planPvP(
        const GameState& state,
        const ThreatAssessment& threat) const;
    static void addInfrastructure(
        StrategicPlan& plan,
        const GameState& state,
        const ThreatAssessment& threat);
    static void addAdaptiveCounters(StrategicPlan& plan, const GameState& state);
    static void addSafetyReactions(StrategicPlan& plan, const ThreatAssessment& threat);
    static void addEconomicRecovery(StrategicPlan& plan, const GameState& state);
    static void applyOpeningStyle(
        StrategicPlan& plan,
        const GameState& state,
        OpeningStyle style);
};

// Strategy rules may legitimately change from one observation to the next,
// but an army should not reverse its map-level intent on a single clear frame.
// Emergency states take effect immediately; leaving them requires sustained
// safety so reinforcements can assemble before the next push.
class StrategicDirector {
public:
    [[nodiscard]] StrategicPlan stabilize(
        StrategicPlan candidate,
        const GameState& state,
        const ThreatAssessment& threat);
    void reset() noexcept;

private:
    Posture posture_{Posture::hold};
    Frame lastEmergencyFrame_{-1};
    bool initialized_{};
};

[[nodiscard]] std::string_view postureName(Posture posture) noexcept;
[[nodiscard]] std::string_view openingStyleName(OpeningStyle style) noexcept;

}  // namespace astra
