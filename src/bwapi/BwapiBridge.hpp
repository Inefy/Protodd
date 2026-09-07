#pragma once

#include "protodd/Combat.hpp"
#include "protodd/CommandBus.hpp"
#include "protodd/GameState.hpp"
#include "protodd/MacroPlanner.hpp"
#include "protodd/Navigation.hpp"
#include "protodd/Scouting.hpp"
#include "protodd/Strategy.hpp"
#include "protodd/Workers.hpp"

#include <BWAPI.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace protodd::bwapi {

struct DebugSquad {
    std::string role;
    std::string reason;
    Position center{-1, -1};
    Position objective{-1, -1};
    Position retreat{-1, -1};
    double ratio{};
    double required{};
    int units{};
    int enemies{};
    FightDecision decision{FightDecision::retreat};
};

struct DebugOverlay {
    int level{2};
    std::vector<MacroAction> macro;
    std::vector<DebugSquad> squads;
    std::unordered_map<UnitId, std::string> orders;
};

class BwapiBridge {
public:
    BwapiBridge() = default;

    void onStart();
    [[nodiscard]] GameState observe();
    [[nodiscard]] NavigationGrid navigationGrid() const;
    void remember(BWAPI::Unit unit);
    void forget(BWAPI::Unit unit);
    [[nodiscard]] std::vector<UnitId> reservedBuilders() const;
    [[nodiscard]] std::string_view lastMacroStatus() const noexcept {
        return lastMacroStatus_;
    }

    [[nodiscard]] bool execute(const Command& command);
    int executeMacro(
        std::span<const MacroAction> actions,
        const StrategicPlan& plan,
        std::span<const UnitId> unavailableBuilders = {},
        int maximumCommands = 8);
    void executeWorkers(std::span<const WorkerAssignment> assignments);
    void executeScouts(std::span<const ScoutOrder> orders);
    void runMaintenance(int mineralReserve = 0, int gasReserve = 0);
    void drawDebug(
        const GameState& state,
        const StrategicPlan& plan,
        const ThreatAssessment& threat,
        const DebugOverlay& debug) const;

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
        // Pixel coordinates of the exact top-left build tile for every type.
        Position target{-1, -1};
        bool prepositioned{};
        Position lastPosition{-1, -1};
        Frame lastProgress{-1};
    };

    struct FailedBuildSite {
        UnitKind kind{UnitKind::unknown};
        Position target{-1, -1};
        Frame expires{};
    };

    struct ResourceSite {
        Position resourceCenter{-1, -1};
        Position depotCenter{-1, -1};
        Position mineralLine{-1, -1};
        BWAPI::TilePosition depotTile{BWAPI::TilePositions::None};
        std::vector<DefensivePosition> defenses;
    };

    std::unordered_map<UnitId, UnitSnapshot> enemyMemory_;
    MineralAllocator mineralAllocator_;
    std::unordered_map<int, Frame> baseLastScouted_;
    std::unordered_map<int, Frame> baseLastConfirmedEmpty_;
    std::unordered_map<UnitKind, PendingBuild> pendingBuilds_;
    std::vector<FailedBuildSite> failedBuildSites_;
    std::unordered_map<UnitId, Frame> unitCommandLocks_;
    std::vector<ResourceSite> resourceSites_;
    std::vector<SpellZone> recentAreaSpells_;
    std::string lastMacroStatus_{"idle"};

    [[nodiscard]] static Race toRace(BWAPI::Race race) noexcept;
    [[nodiscard]] static DamageType toDamageType(BWAPI::DamageType type) noexcept;
    [[nodiscard]] static UnitSize toUnitSize(BWAPI::UnitSizeType type) noexcept;
    [[nodiscard]] static UnitRole roleOf(BWAPI::UnitType type, UnitKind kind) noexcept;
    [[nodiscard]] static WeaponSnapshot weapon(BWAPI::WeaponType type) noexcept;
    [[nodiscard]] static UnitSnapshot snapshotUnit(BWAPI::Unit unit, bool ours);
    [[nodiscard]] static PlayerSnapshot snapshotPlayer(BWAPI::Player player, bool ours);
    [[nodiscard]] std::vector<BaseSnapshot> snapshotBases(const GameState& state);
    void discoverResourceClusters();

    [[nodiscard]] BWAPI::Unit findBuilder(
        BWAPI::UnitType type,
        BWAPI::Position near,
        std::span<const UnitId> unavailableBuilders) const;
    [[nodiscard]] BWAPI::TilePosition buildLocation(
        UnitKind kind,
        BWAPI::UnitType type,
        BWAPI::Unit builder,
        const StrategicPlan& plan);
    [[nodiscard]] bool blocksMiningLane(
        BWAPI::TilePosition tile,
        BWAPI::UnitType type) const;
    [[nodiscard]] bool build(
        const MacroAction& action,
        const StrategicPlan& plan,
        std::span<const UnitId> unavailableBuilders);
    [[nodiscard]] bool train(const MacroAction& action);
    [[nodiscard]] bool executeTechnology(const MacroAction& action);
    [[nodiscard]] static BWAPI::Position toBwapiPosition(Position position) noexcept;
};

}  // namespace protodd::bwapi
