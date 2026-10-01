#pragma once

#include "protodd/Strategy.hpp"

namespace protodd {

enum class AllInBuild : std::uint8_t { standard, twoGateZealot, threeGateDragoon,
                                       fourGateDragoon, darkTemplar };
enum class AllInPhase : std::uint8_t { assemble, pressure, transition };

[[nodiscard]] AllInBuild allInBuild(std::string_view name) noexcept;
[[nodiscard]] std::string_view allInBuildName(AllInBuild build) noexcept;
[[nodiscard]] std::string_view allInPhaseName(AllInPhase phase) noexcept;

// Counts and time windows come from training-group ladder replays. Local
// evaluation keeps this repertoire separate from tournament promotion.
class AllInOpeningPlanner {
public:
    void reset(AllInBuild build) noexcept;
    void apply(StrategicPlan& plan, const GameState& state, const ThreatAssessment& threat);
    [[nodiscard]] bool active() const noexcept { return build_ != AllInBuild::standard; }
    [[nodiscard]] AllInBuild build() const noexcept { return build_; }
    [[nodiscard]] AllInPhase phase() const noexcept { return phase_; }
    [[nodiscard]] Frame launchFrame() const noexcept { return launch_; }
    [[nodiscard]] std::string_view transitionReason() const noexcept { return reason_; }
private:
    AllInBuild build_{AllInBuild::standard};
    AllInPhase phase_{AllInPhase::assemble};
    Frame launch_{-1};
    int peakArmy_{};
    std::string_view reason_{"none"};
};

}  // namespace protodd
