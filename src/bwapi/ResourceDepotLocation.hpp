#pragma once

#include <BWAPI.h>
#include <algorithm>
#include <array>
#include <vector>

namespace protodd::bwapi {
inline BWAPI::TilePosition resourceDepotLocation(const BWAPI::TilePosition origin) {
    using namespace BWAPI;
    // BWAPI 4.4.0 getBuildLocation(Nexus, origin, 12) considers this 12x12
    // square, prefers the origin's elevation, then ranks by approximate tile
    // distance and y/x. Probe candidates in that order so native legality
    // checks stop at the same selected tile instead of visiting all 144 cells.
    std::array<TilePosition, 144> candidates;
    std::size_t index = 0;
    for (int y = origin.y - 6; y < origin.y + 6; ++y)
        for (int x = origin.x - 6; x < origin.x + 6; ++x)
            candidates[index++] = {x, y};
    std::ranges::sort(candidates, [origin](const TilePosition a, const TilePosition b) {
        const auto first = origin.getApproxDistance(a);
        const auto second = origin.getApproxDistance(b);
        if (first != second) return first < second;
        return a.y != b.y ? a.y < b.y : a.x < b.x;
    });
    std::vector<TilePosition> minerals, geysers;
    for (const auto mineral : Broodwar->getStaticMinerals()) {
        const auto tile = mineral->getInitialTilePosition();
        if ((Broodwar->isVisible(tile) || Broodwar->isVisible(tile.x + 1, tile.y)) &&
            !mineral->exists()) continue;
        minerals.push_back(tile);
    }
    for (const auto geyser : Broodwar->getStaticGeysers())
        geysers.push_back(geyser->getInitialTilePosition());
    const auto reservedForResources = [&minerals, &geysers](const TilePosition candidate) {
        // These are BWAPI 4.4.0's exact Nexus resource exclusions. Reject
        // them before its more expensive unit-rectangle scan; all surviving
        // candidates still need the complete native legality/path checks.
        return std::ranges::any_of(minerals, [candidate](const TilePosition tile) {
            return tile.x > candidate.x - 5 && tile.y > candidate.y - 4 &&
                   tile.x < candidate.x + 7 && tile.y < candidate.y + 6;
        }) || std::ranges::any_of(geysers, [candidate](const TilePosition tile) {
            return tile.x > candidate.x - 7 && tile.y > candidate.y - 5 &&
                   tile.x < candidate.x + 7 && tile.y < candidate.y + 6;
        });
    };
    const auto elevation = Broodwar->getGroundHeight(origin);
    for (const auto sameElevation : {true, false}) {
        for (const auto candidate : candidates) {
            if (!candidate.isValid() ||
                reservedForResources(candidate) ||
                (Broodwar->getGroundHeight(candidate) == elevation) != sameElevation ||
                !Broodwar->canBuildHere(candidate, UnitTypes::Protoss_Nexus) ||
                !Broodwar->hasPath(BWAPI::Position(origin), BWAPI::Position(candidate))) continue;
            return candidate;
        }
    }
    return TilePositions::None;
}

} // namespace protodd::bwapi
