#pragma once

#include "protodd/Combat.hpp"
#include "protodd/BuildCancellation.hpp"
#include "protodd/BuildTaskProgress.hpp"
#include "protodd/CommandBus.hpp"
#include "protodd/GameState.hpp"
#include "protodd/InfluenceMap.hpp"
#include "protodd/MacroPlanner.hpp"
#include "protodd/Navigation.hpp"
#include "protodd/Operations.hpp"
#include "protodd/Scouting.hpp"
#include "protodd/Strategy.hpp"
#include "protodd/Workers.hpp"

#include <BWAPI.h>

#include <functional>
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
    int level{1};
    std::vector<MacroAction> macro;
    std::vector<DebugSquad> squads;
    std::unordered_map<UnitId, std::string> orders;
    std::string operation;
    std::string health;
    std::string scout;
    int idleGateways{};
    int usableGateways{};
    int idleWorkers{};
    int unpoweredBuildings{};
};

struct MacroExecution {
    MacroAction action;
    std::string outcome;
    bool accepted{};
};

struct ActionDiagnostic {
    UnitId actor{-1};
    UnitId target{-1};
    Position position{-1, -1};
    std::string type;
    std::string source;
    std::string outcome;
    int extra{};
    bool attempted{};
    bool accepted{};
};

struct BuildLeaseDiagnostic {
    UnitKind kind{UnitKind::unknown};
    UnitId builder{-1};
    Position target{-1, -1};
    Position builderPosition{-1, -1};
    Frame issued{};
    Frame frame{};
    Frame lastProgress{};
    std::string reason;
    std::string order;
    bool commandedBuild{};
    bool buildTypeMatches{};
    bool builderCanBuildHere{};
    bool mapCanBuildHere{};
    bool hasPath{};
};

struct BuildSelectionDiagnostic {
    UnitKind kind{UnitKind::unknown};
    Frame frame{};
    UnitId selected{-1};
    UnitId siteCandidate{-1};
    Position target{-1, -1};
    Position anchor{-1, -1};
    int selectedDistance{};
    int siteCandidateDistance{};
    bool candidateCanBuildHere{};
    bool candidateHasPath{};
};

class BwapiBridge {
public:
    BwapiBridge() = default;

    std::function<void(const ActionDiagnostic&)> actionDiagnostic;
    std::function<void(const BuildLeaseDiagnostic&)> buildLeaseDiagnostic;
    std::function<void(const BuildSelectionDiagnostic&)> buildSelectionDiagnostic;
    std::function<bool(const BWAPI::UnitCommand&, bool, bool)> productionDiagnostic;
    std::function<bool(const BWAPI::UnitCommand&, std::string_view)> productionPermission;
    [[nodiscard]] std::uint64_t diagnosticErrors() const noexcept { return diagnosticErrors_; }

    void onStart();
    void setSpendingLedger(ResourceLedger* ledger) noexcept { spendingLedger_ = ledger; }
    [[nodiscard]] GameState observe();
    [[nodiscard]] NavigationGrid navigationGrid() const;
    void remember(BWAPI::Unit unit);
    void forget(BWAPI::Unit unit);
    [[nodiscard]] std::vector<UnitId> reservedBuilders() const;
    [[nodiscard]] bool pendingBuildAlreadyPaid(const MacroAction& action) const;
    [[nodiscard]] std::vector<BuildBlockerFeedback> buildBlockerFeedback() const;
    [[nodiscard]] std::string_view lastMacroStatus() const noexcept {
        return lastMacroStatus_;
    }

    [[nodiscard]] bool execute(const Command& command);
    [[nodiscard]] bool executeProduction(const BWAPI::UnitCommand& command) { return issue(command, "production-demand"); }
    [[nodiscard]] bool executeWholeGame(const BWAPI::UnitCommand& command,
                                        Frame leaseFrames = 24);
    [[nodiscard]] bool commandActive(const Command& command) const;
    [[nodiscard]] ExpansionFeedback expansionFeedback() const;
    [[nodiscard]] bool cancelExpansion();
    [[nodiscard]] const std::vector<MacroExecution>& macroExecutions() const noexcept {
        return macroExecutions_;
    }
    int executeMacro(
        std::span<const MacroAction> actions,
        const StrategicPlan& plan,
        const InfluenceMap& influence,
        std::span<const UnitId> unavailableBuilders = {},
        int maximumCommands = 8);
    void executeWorkers(std::span<const WorkerAssignment> assignments);
    void executeScouts(std::span<const ScoutOrder> orders);
    void runMaintenance();
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
        UnitKind kind{UnitKind::unknown};
        ConstructionTaskSite constructionSite{};
        bool resourcesPaid{};
        BuildTaskPhase phase{BuildTaskPhase::commandPending};
        UnitId builder{-1};
        Frame issued{};
        Frame commandIssued{-1};
        Frame travelDeadline{-1};
        Frame commandAcknowledged{-1};
        // Pixel coordinates of the exact top-left build tile for every type.
        Position target{-1, -1};
        bool prepositioned{};
        Position lastPosition{-1, -1};
        Frame lastRouteProgress{-1};
        bool footprintAccessible{};
        bool plannedRemotePower{};
        BuildCancellation cancellation{};
    };

    struct FailedBuildSite {
        UnitKind kind{UnitKind::unknown};
        Position target{-1, -1};
        Frame expires{};
    };

    struct PlacementSearchState {
        int offset{};
        BWAPI::TilePosition fireFallback{BWAPI::TilePositions::None};
        BWAPI::TilePosition laneFallback{BWAPI::TilePositions::None};
    };

    struct ResourceSite {
        Position resourceCenter{-1, -1};
        Position depotCenter{-1, -1};
        Position mineralLine{-1, -1};
        BWAPI::TilePosition depotTile{BWAPI::TilePositions::None};
        std::vector<DefensivePosition> defenses{};
        int groundDistanceFromStart{-1};
    };

    std::unordered_map<UnitId, UnitSnapshot> enemyMemory_;
    MineralAllocator mineralAllocator_;
    std::unordered_map<int, Frame> baseLastScouted_;
    std::unordered_map<int, Frame> baseLastConfirmedEmpty_;
    // Stable task IDs allow same-kind structures at separate bases to retain
    // independent builders, placements, retries, and completion feedback.
    std::unordered_map<std::uint64_t, PendingBuild> pendingBuilds_;
    std::unordered_map<std::uint64_t, BuildBlockerFeedback> buildBlockers_;
    std::vector<FailedBuildSite> failedBuildSites_;
    std::unordered_map<std::uint64_t, PlacementSearchState> placementSearches_;
    std::unordered_map<UnitId, Frame> unitCommandLocks_;
    std::unordered_map<UnitId, Frame> learnedCommandLeases_;
    std::vector<ResourceSite> resourceSites_;
    bool defensesInitialized_{};
    std::vector<SpellZone> recentAreaSpells_;
    std::string lastMacroStatus_{"idle"};
    std::vector<MacroExecution> macroExecutions_;
    ResourceLedger* spendingLedger_{};
    BWAPI::Error lastIssueError_{BWAPI::Errors::None};
    std::uint64_t diagnosticErrors_{};

    enum class ResourceUse : std::uint8_t { available, committed };
    bool issue(const BWAPI::UnitCommand& command, std::string_view source,
               ResourceUse resourceUse = ResourceUse::available,
               bool* resourcesPaid = nullptr);
    bool requestBuildCancellation(PendingBuild& pending, BWAPI::Unit builder,
                                 std::string_view source);
    bool reject(const Command& command, std::string_view reason);

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
        const InfluenceMap& influence,
        std::span<const UnitId> unavailableBuilders,
        bool requireCanBuild = true) const;
    void recordBuildBlocker(const MacroAction& action, BuildBlockerReason reason,
                            Frame retryFrames);
    void inspectUnfundedBuild(const MacroAction& action, const StrategicPlan& plan,
                              const InfluenceMap& influence,
                              std::span<const UnitId> unavailableBuilders);
    [[nodiscard]] BWAPI::TilePosition buildLocation(
        UnitKind kind,
        BWAPI::UnitType type,
        BWAPI::Unit builder,
        const StrategicPlan& plan,
        const ConstructionTaskSite& constructionSite,
        std::uint64_t taskKey,
        std::string_view reason);
    [[nodiscard]] bool blocksMiningLane(
        BWAPI::TilePosition tile,
        BWAPI::UnitType type) const;
    [[nodiscard]] bool build(
        const MacroAction& action,
        const StrategicPlan& plan,
        const InfluenceMap& influence,
        std::span<const UnitId> unavailableBuilders);
    [[nodiscard]] bool train(const MacroAction& action);
    [[nodiscard]] bool executeTechnology(const MacroAction& action);
    [[nodiscard]] static BWAPI::Position toBwapiPosition(Position position) noexcept;
};

}  // namespace protodd::bwapi
