#pragma once

#include "protodd/CommandBus.hpp"
#include "protodd/GameState.hpp"
#include "protodd/InfluenceMap.hpp"
#include "protodd/Navigation.hpp"

#include <unordered_map>
#include <vector>

namespace protodd {

enum class TransportPhase : std::uint8_t { gathering, attacking, extracting, returning };

class TransportController {
public:
    [[nodiscard]] std::vector<Command> control(
        const GameState& state,
        Position objective,
        Position retreat,
        const InfluenceMap& influence,
        int reservedArmyReavers = 0,
        bool economicTargets = false,
        const NavigationGrid* navigation = nullptr);
    void reset();
    [[nodiscard]] bool ownsReaver(UnitId id) const;

private:
    struct Mission {
        UnitId reaver{-1};
        TransportPhase phase{TransportPhase::gathering};
        Frame transitionFrame{};
        Position target{-1, -1};
        Position waypoint{-1, -1};
    };

    std::unordered_map<UnitId, Mission> missions_;
    std::unordered_map<UnitId, Frame> nextLaunch_;
};

}  // namespace protodd
