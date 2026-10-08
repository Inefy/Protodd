#pragma once

#include "WholeGameIntent.hpp"

#include <BWAPI.h>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace protodd::bwapi {

struct WholeGameActionIdentity {
    std::uint64_t attemptId{};
    int proposalFrame{};
    int dueFrame{};
    std::size_t slot{};
    std::size_t actorOrdinal{};
    std::size_t actorCount{};
    std::size_t intentKind{};
    std::size_t targetMode{};
    int targetEntity{-1};
    int targetX{-1};
    int targetY{-1};
};

struct LegalWholeGameCommand {
    int actorToken{};
    BWAPI::UnitCommand command;
    WholeGameActionIdentity identity;
};

struct WholeGameActorAssessment {
    int actorToken{};
    std::optional<BWAPI::UnitCommand> command;
    std::string reason;

    [[nodiscard]] bool legal() const noexcept { return command.has_value(); }
};

[[nodiscard]] std::vector<WholeGameActorAssessment> assessWholeGameCommands(
    const cpu::Intent& intent, const whole_observation::Snapshot& observation,
    const std::map<int, BWAPI::Unit>& unitByToken, BWAPI::Game* game);

// Translate a decoded model intent against current BWAPI state. This performs
// legality checks but never issues a command or claims unit ownership.
[[nodiscard]] std::vector<LegalWholeGameCommand> legalWholeGameCommands(
    const cpu::Intent& intent, const whole_observation::Snapshot& observation,
    const std::map<int, BWAPI::Unit>& unitByToken, BWAPI::Game* game,
    std::size_t maximumCommands = 8);

}  // namespace protodd::bwapi
