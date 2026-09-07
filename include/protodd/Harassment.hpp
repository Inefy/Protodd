#pragma once

#include "protodd/GameState.hpp"
#include "protodd/Strategy.hpp"

#include <span>
#include <string>
#include <vector>

namespace protodd {

struct HarassmentOpportunity {
    Position target{-1, -1};
    double score{};
    int economicTargets{};
};

// Scores only observed economic targets, with movement-domain-specific route
// and defender checks. Flying transport routes still need a safe ground drop.
[[nodiscard]] HarassmentOpportunity harassmentOpportunity(
    const GameState& state, const UnitSnapshot& raider, bool flyingRoute = false);

struct RaidMission {
    std::vector<UnitId> members;
    Position target{-1, -1};
    bool withdrawing{};
    std::string reason;
};

class HarassmentPlanner {
public:
    [[nodiscard]] RaidMission update(const GameState& state,
        std::span<const UnitSnapshot> available, const StrategicPlan& plan,
        Position home, bool baseThreat);
    void reset();
private:
    RaidMission mission_;
    Frame started_{};
    Frame nextAttempt_{};
    Frame lastFrame_{-1};
};

} // namespace protodd
