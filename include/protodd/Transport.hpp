#pragma once

#include "protodd/CommandBus.hpp"
#include "protodd/GameState.hpp"
#include "protodd/InfluenceMap.hpp"

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
        bool economicTargets = false);
    void reset();

private:
    struct Mission {
        UnitId reaver{-1};
        TransportPhase phase{TransportPhase::gathering};
        Frame transitionFrame{};
        Position target{-1, -1};
    };

    std::unordered_map<UnitId, Mission> missions_;
};

}  // namespace protodd
