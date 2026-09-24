#pragma once

#include "WholeGameIntent.hpp"

#include <BWAPI.h>
#include <map>
#include <vector>

namespace protodd::bwapi {

struct LegalWholeGameCommand {
    int actorToken{};
    BWAPI::UnitCommand command;
};

// Translate a decoded model intent against current BWAPI state. This performs
// legality checks but never issues a command or claims unit ownership.
[[nodiscard]] std::vector<LegalWholeGameCommand> legalWholeGameCommands(
    const cpu::Intent& intent, const whole_observation::Snapshot& observation,
    const std::map<int, BWAPI::Unit>& unitByToken, BWAPI::Game* game,
    std::size_t maximumCommands = 8);

}  // namespace protodd::bwapi
