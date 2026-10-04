#pragma once

#include <cstddef>
#include <optional>

namespace protodd::bwapi {

template <typename UnitType>
[[nodiscard]] std::optional<UnitType> wholeGameTrainingType(
    const std::size_t kind, const std::optional<UnitType>& requestedType,
    const UnitType actorType, const UnitType reaverType, const UnitType scarabType,
    const UnitType carrierType, const UnitType interceptorType) {
    if (kind == 13) return requestedType;
    if (kind != 29) return std::nullopt;
    if (actorType == reaverType) return scarabType;
    if (actorType == carrierType) return interceptorType;
    return std::nullopt;
}

template <typename Actor, typename Command>
[[nodiscard]] bool wholeGameCommandIssueable(const Actor actor, const Command& command) {
    return actor != nullptr && actor->canIssueCommand(command);
}

}  // namespace protodd::bwapi