#pragma once

#include "protodd/GameState.hpp"
#include "protodd/Information.hpp"

#include <string>
#include <vector>

namespace protodd {

enum class Posture : std::uint8_t { hold, defend, pressure, attack, harass, recover };

enum class OpeningStyle : std::uint8_t { standard, aggressive, economic, deceptive, count };

enum class GoalKind : std::uint8_t { build, train, expand, detect, research, upgrade };

// Optional stable identity for a structure demand tied to one map location.
// IDs are caller-owned, globally unique within a game, and stable across
// planning frames. The upper bit is reserved by the BWAPI adapter for legacy
// type-only leases.
struct ConstructionTaskSite {
    std::uint64_t id{};
    int baseId{-1};
    Position anchor{-1, -1};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return id != 0 && (id >> 63U) == 0 && anchor.valid();
    }
};

enum class CriticalReservationReason : std::uint8_t {
    none,
    supply,
    detection,
    range,
};

struct CriticalGoalTiming {
    Frame requiredByFrame{-1};
    Frame expectedReadyFrame{-1};
    Frame slackFrames{-1};
    bool feasible{};
    CriticalReservationReason reservationReason{CriticalReservationReason::none};

    [[nodiscard]] constexpr bool active() const noexcept {
        return reservationReason != CriticalReservationReason::none &&
               requiredByFrame >= 0;
    }
};

[[nodiscard]] int criticalDeadlinePriorityAdjustment(
    const CriticalGoalTiming& timing, Frame currentFrame) noexcept;

struct ProductionGoal {
    GoalKind goal{GoalKind::train};
    UnitKind target{UnitKind::unknown};
    int desiredCount{};
    int priority{};
    bool blocking{};
    std::string reason;
    TechnologyKind technology{TechnologyKind::none};
    bool allowMineralFallback{};
    bool harassmentOnly{};
    ConstructionTaskSite constructionSite{};
    CriticalGoalTiming timing{};
};

[[nodiscard]] CriticalGoalTiming assessCriticalGoalTiming(
    const GameState& state, const ProductionGoal& goal, Frame requiredByFrame,
    CriticalReservationReason reason);

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
    // Current threat, independent of whether the army is holding or attacking.
    bool prioritizeReinforcements{};
    bool requireMobileDetection{};
    // A stabilized army can cover economic growth despite perimeter contact.
    bool sustainEconomy{};
    bool breakContainment{};
    Position expansionTarget{-1, -1};
    // Temporarily release expansion savings while a failed builder recovers.
    bool deferExpansion{};
    // Keep mobile cover assigned while securing an expansion route or when a
    // started Nexus is locally threatened, even during an offensive posture.
    bool expansionProtectionRequired{};
    int harassmentDrops{};
    // Stop supply/production fallback spending while a lost last Nexus has no
    // legal mineral drop-off and recovery is awaiting a viable rebuild.
    bool recoveringLastNexus{};
    // Estimated frames of mining remaining across all currently owned mineral
    // sites, based on the live worker count. -1 means the estimate is unknown.
    Frame estimatedMiningRunwayFrames{-1};
};

class StrategyEngine {
public:
    [[nodiscard]] static bool coveredPressureRelease(
        const GameState& state, const StrategicPlan& plan) noexcept;
    [[nodiscard]] static Position pvTContainBreakTarget(
        const GameState& state, const StrategicPlan& plan) noexcept;
    explicit StrategyEngine(bool pvzGatewayOpening = false,
                            bool pvzEarlySplash = false,
                            bool pvpFogDetection = false,
                            bool pvzReplayOpening = false,
                            bool pvzArchivesFirst = false,
                            bool lateEconomyRecovery = false,
                            bool pvzPoweredCannonScreen = false,
                            bool pvzProactiveReaver = false,
                            bool pvzArmyFloor = false,
                            bool pvpScoutedTwoGateAnchor = false,
                            bool pvpCoveredRangedNatural = false) noexcept
        : pvzGatewayOpening_(pvzGatewayOpening), pvzEarlySplash_(pvzEarlySplash),
          pvpFogDetection_(pvpFogDetection), pvzReplayOpening_(pvzReplayOpening),
          pvzArchivesFirst_(pvzArchivesFirst),
          lateEconomyRecovery_(lateEconomyRecovery),
          pvzPoweredCannonScreen_(pvzPoweredCannonScreen),
          pvzProactiveReaver_(pvzProactiveReaver),
          pvzArmyFloor_(pvzArmyFloor),
          pvpScoutedTwoGateAnchor_(pvpScoutedTwoGateAnchor),
          pvpCoveredRangedNatural_(pvpCoveredRangedNatural) {}

    [[nodiscard]] StrategicPlan plan(
        const GameState& state,
        const ThreatAssessment& threat,
        OpeningStyle style = OpeningStyle::standard) const;
    void reset() noexcept;

private:
    bool pvzGatewayOpening_{};
    bool pvzEarlySplash_{};
    bool pvpFogDetection_{};
    bool pvzReplayOpening_{};
    bool pvzArchivesFirst_{};
    bool lateEconomyRecovery_{};
    bool pvzPoweredCannonScreen_{};
    bool pvzProactiveReaver_{};
    bool pvzArmyFloor_{};
    bool pvpScoutedTwoGateAnchor_{};
    bool pvpCoveredRangedNatural_{};
    mutable bool pvpCoveredRangedNaturalActive_{};
    mutable Frame pvpCoveredRangedNaturalLastFrame_{-1};
    // Midfield pressure is based on mobile enemy sightings. Keep its rally
    // anchor until the observed front moves materially, rather than tracking
    // every small unit-position change at strategy-tick cadence.
    mutable Position pvpMidfieldRally_{-1, -1};
    mutable Frame pvpMidfieldRallyLastFrame_{-1};
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
    static void addEconomicRecovery(StrategicPlan& plan, const GameState& state,
                                    bool lateEconomyRecovery);
    static void addPostPressureTransition(StrategicPlan& plan, const GameState& state,
                                          const ThreatAssessment& threat);
    static void addMapControlEconomy(StrategicPlan& plan, const GameState& state,
                                     const ThreatAssessment& threat);
    static void addHarassmentProduction(StrategicPlan& plan, const GameState& state,
                                        bool pvzArchivesFirst);
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
    Position expansionCommitment_{-1, -1};
    Frame lastExpansionRequestFrame_{-1};
    int committedExpansionBases_{};
};

[[nodiscard]] std::string_view postureName(Posture posture) noexcept;
[[nodiscard]] std::string_view openingStyleName(OpeningStyle style) noexcept;

}  // namespace protodd
