#pragma once

#include "protodd/GameState.hpp"
#include "protodd/Strategy.hpp"
#include "protodd/Navigation.hpp"

#include <span>
#include <string>
#include <vector>

namespace protodd {

struct HarassmentOpportunity {
    Position target{-1, -1};
    double score{};
    int economicTargets{};
    Position waypoint{-1, -1};
    bool probing{};
};

// Scores observed targets and scouting probes of remembered enemy economies.
// Flying transport routes still need a safe ground drop.
[[nodiscard]] HarassmentOpportunity harassmentOpportunity(
    const GameState& state, const UnitSnapshot& raider, bool flyingRoute = false,
    const NavigationGrid* navigation = nullptr);
[[nodiscard]] bool harassmentRouteSafe(const GameState& state,
    const UnitSnapshot& raider, Position target, bool flyingRoute = false);

struct RaidMission {
    std::vector<UnitId> members;
    Position target{-1, -1};
    bool withdrawing{};
    std::string reason;
    Position waypoint{-1, -1};
    bool probing{};
};

class HarassmentPlanner {
public:
    [[nodiscard]] RaidMission update(const GameState& state,
        std::span<const UnitSnapshot> available, const StrategicPlan& plan,
        Position home, bool baseThreat, const NavigationGrid* navigation = nullptr);
    void reset();
private:
    RaidMission mission_;
    Frame started_{};
    Frame nextAttempt_{};
    Frame lastFrame_{-1};
    Position lastTarget_{-1, -1};
    Frame revisitAfter_{};
};

} // namespace protodd
