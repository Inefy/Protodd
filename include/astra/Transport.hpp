#pragma once

#include "astra/CommandBus.hpp"
#include "astra/GameState.hpp"
#include "astra/InfluenceMap.hpp"

#include <unordered_map>
#include <vector>

namespace astra {

enum class TransportPhase : std::uint8_t { gathering, attacking, extracting, returning };

class TransportController {
public:
    [[nodiscard]] std::vector<Command> control(
        const GameState& state,
        Position objective,
        Position retreat,
        const InfluenceMap& influence);
    void reset();

private:
    struct Mission {
        UnitId reaver{-1};
        TransportPhase phase{TransportPhase::gathering};
        Frame transitionFrame{};
    };

    std::unordered_map<UnitId, Mission> missions_;
};

}  // namespace astra
