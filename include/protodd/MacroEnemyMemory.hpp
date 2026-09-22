#pragma once

#include "protodd/GameState.hpp"
#include "protodd/UnitCatalog.hpp"
#include <map>

namespace protodd {
// Both adapters pass only currently visible AND detected unit snapshots, and
// observed removals. An unseen death is not an observation. Visibility queries
// use the LAST observed building position, never its private current position.
class MacroEnemyMemory {
public:
    void reset() { units_.clear(); frame_ = -1; }
    template<class VisibleTile>
    void update(Frame frame, std::span<const UnitSnapshot> observations,
                std::span<const UnitId> removals, VisibleTile visibleTile) {
        if (frame < frame_) reset();
        frame_ = frame;
        for (auto& [id, unit] : units_) { (void)id; unit.visible = false; }
        for (const auto id : removals) units_.erase(id);
        for (auto unit : observations) {
            if (!unit.visible || !unit.detected || unit.ours || unit.kind == UnitKind::unknown) continue;
            const auto old = units_.find(unit.id);
            unit.firstSeen = old == units_.end() ? frame : old->second.firstSeen;
            unit.lastSeen = frame;
            units_[unit.id] = unit;
        }
        std::erase_if(units_, [&](const auto& entry) {
            const auto& unit = entry.second;
            return !unit.visible && isBuilding(unit.kind) && visibleTile(unit.position);
        });
    }
    [[nodiscard]] std::vector<UnitSnapshot> snapshot() const {
        std::vector<UnitSnapshot> result;
        for (const auto& [id, unit] : units_) { (void)id; result.push_back(unit); }
        return result;
    }
private:
    Frame frame_{-1};
    std::map<UnitId, UnitSnapshot> units_;
};
} // namespace protodd
