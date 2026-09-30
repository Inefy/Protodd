#pragma once

#include "protodd/GameState.hpp"

namespace protodd {

[[nodiscard]] constexpr bool usesHomeConstructionAnchor(UnitKind kind) noexcept {
    switch (kind) {
    case UnitKind::pylon:
    case UnitKind::gateway:
    case UnitKind::forge:
    case UnitKind::cyberneticsCore:
    case UnitKind::roboticsFacility:
    case UnitKind::observatory:
    case UnitKind::roboticsSupportBay:
    case UnitKind::stargate:
    case UnitKind::citadelOfAdun:
    case UnitKind::templarArchives:
    case UnitKind::fleetBeacon:
    case UnitKind::arbiterTribunal:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] constexpr Position constructionBuilderAnchor(
    UnitKind kind, Position home, Position rally, Position expansion) noexcept {
    if (kind == UnitKind::nexus && expansion.valid()) return expansion;
    if (usesHomeConstructionAnchor(kind) && home.valid()) return home;
    return rally.valid() ? rally : home;
}

}  // namespace protodd
