#pragma once

#include "astra/Combat.hpp"
#include "astra/CommandBus.hpp"
#include "astra/GameState.hpp"
#include "astra/MacroPlanner.hpp"
#include "astra/Navigation.hpp"
#include "astra/Scouting.hpp"
#include "astra/Strategy.hpp"
#include "astra/Workers.hpp"

#include <BWAPI.h>

#include <unordered_map>
#include <vector>

namespace astra::bwapi {

class BwapiBridge {
public:
    BwapiBridge() = default;

    void onStart();
    [[nodiscard]] GameState observe();
    [[nodiscard]] NavigationGrid navigationGrid() const;
    void remember(BWAPI::Unit unit);
    void forget(BWAPI::Unit unit);
    [[nodiscard]] std::vector<UnitId> reservedBuilders() const;

    [[nodiscard]] bool execute(const Command& command);
    int executeMacro(
        std::span<const MacroAction> actions,
        const StrategicPlan& plan,
        int maximumCommands = 2);
    void executeWorkers(std::span<const WorkerAssignment> assignments);
    void executeScouts(std::span<const ScoutOrder> orders);
    void runMaintenance(int mineralReserve = 0, int gasReserve = 0);
    void drawDebug(
        const StrategicPlan& plan,
        const ThreatAssessment& threat,
        const CombatEstimate& combat) const;

    [[nodiscard]] static UnitKind toKind(BWAPI::UnitType type) noexcept;
    [[nodiscard]] static BWAPI::UnitType toBwapi(UnitKind kind) noexcept;
    [[nodiscard]] static BWAPI::TechType toBwapiTech(TechnologyKind kind) noexcept;
    [[nodiscard]] static BWAPI::UpgradeType toBwapiUpgrade(TechnologyKind kind) noexcept;

private:
    struct SpellZone {
        Position center{-1, -1};
        Frame expires{};
    };

    struct PendingBuild {
        UnitId builder{-1};
        Frame issued{};
        Position target{-1, -1};
    };

    std::unordered_map<UnitId, UnitSnapshot> enemyMemory_;
    std::unordered_map<int, Frame> baseLastScouted_;
    std::unordered_map<UnitKind, PendingBuild> pendingBuilds_;
    std::vector<Position> resourceClusters_;
    std::vector<SpellZone> recentAreaSpells_;

    [[nodiscard]] static Race toRace(BWAPI::Race race) noexcept;
    [[nodiscard]] static DamageType toDamageType(BWAPI::DamageType type) noexcept;
    [[nodiscard]] static UnitRole roleOf(BWAPI::UnitType type, UnitKind kind) noexcept;
    [[nodiscard]] static WeaponSnapshot weapon(BWAPI::WeaponType type) noexcept;
    [[nodiscard]] static UnitSnapshot snapshotUnit(BWAPI::Unit unit, bool ours);
    [[nodiscard]] static PlayerSnapshot snapshotPlayer(BWAPI::Player player, bool ours);
    [[nodiscard]] std::vector<BaseSnapshot> snapshotBases(const GameState& state);
    void discoverResourceClusters();

    [[nodiscard]] BWAPI::Unit findBuilder(BWAPI::UnitType type, BWAPI::Position near) const;
    [[nodiscard]] BWAPI::TilePosition buildLocation(
        UnitKind kind,
        BWAPI::UnitType type,
        BWAPI::Unit builder,
        const StrategicPlan& plan) const;
    [[nodiscard]] bool build(const MacroAction& action, const StrategicPlan& plan);
    [[nodiscard]] bool train(const MacroAction& action);
    [[nodiscard]] bool executeTechnology(const MacroAction& action);
    [[nodiscard]] static BWAPI::Position toBwapiPosition(Position position) noexcept;
};

}  // namespace astra::bwapi
