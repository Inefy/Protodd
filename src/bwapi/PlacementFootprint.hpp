#pragma once

#include <BWAPI/Position.h>
#include <BWAPI/Race.h>
#include <BWAPI/UnitType.h>

namespace protodd::bwapi {

struct PlacementBuilderBounds {
    int left{};
    int top{};
    int right{-1};
    int bottom{-1};
};

// For a Protoss structure, the builder overload adds a path check and
// excludes that worker from occupancy. If the map check fails and this
// worker cannot overlap the structure's collision rectangle, excluding it
// cannot make the placement legal. Keep the builder check for that exception.
// Unknown bounds and other races retain the original checks conservatively.
template <typename MapCheck, typename BuilderCheck>
[[nodiscard]] bool placementFootprintBuildable(
    const BWAPI::TilePosition location, const BWAPI::UnitType type,
    const PlacementBuilderBounds builder, MapCheck&& mapCheck, BuilderCheck&& builderCheck) {
    if (type.getRace() != BWAPI::Races::Protoss || type.isAddon())
        return builderCheck() || mapCheck();
    if (mapCheck()) return true;
    if (builder.right < builder.left || builder.bottom < builder.top)
        return builderCheck();
    const auto center = BWAPI::Position(location) + BWAPI::Position(type.tileSize()) / 2;
    if (builder.right < center.x - type.dimensionLeft() ||
        builder.left > center.x + type.dimensionRight() ||
        builder.bottom < center.y - type.dimensionUp() ||
        builder.top > center.y + type.dimensionDown()) return false;
    return builderCheck();
}

}  // namespace protodd::bwapi
