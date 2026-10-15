#pragma once

#include "protodd/Combat.hpp"
#include "protodd/BuildCancellation.hpp"
#include "protodd/BuildTaskProgress.hpp"
#include "protodd/ConstructionTraffic.hpp"
#include "protodd/BaseTerrainAnalysis.hpp"
#include "protodd/CommandBudget.hpp"
#include "protodd/CommandBus.hpp"
#include "protodd/GameState.hpp"
#include "protodd/InfluenceMap.hpp"
#include "protodd/MacroPlanner.hpp"
#include "protodd/Navigation.hpp"
#include "protodd/Operations.hpp"
#include "protodd/PlacementSafety.hpp"
#include "protodd/ResourceIncome.hpp"
#include "protodd/Scouting.hpp"
#include "protodd/Strategy.hpp"
#include "protodd/Workers.hpp"

#include <BWAPI.h>

#include <cstddef>
#include <array>
#include <functional>
#include <optional>
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
    std::int64_t elapsedUs{};
    std::uint64_t navigationSearches{};
    std::uint64_t taskId{};
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
    Frame travelDeadline{};
    Frame hardTravelDeadline{};
    int bestDistanceToTarget{};
    std::string reason;
    std::string order;
    bool commandedBuild{};
    bool buildTypeMatches{};
    bool builderCanBuildHere{};
    bool mapCanBuildHere{};
    bool hasPath{};
    std::uint64_t taskId{};
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

struct BuildRouteDiagnostic {
    Frame frame{};
    UnitId builder{-1};
    Position from{-1, -1};
    Position anchor{-1, -1};
    Position destination{-1, -1};
    std::string stage;
    bool reachable{};
    double peakThreat{};
    double anchorThreat{};
    bool fromWalkable{};
    bool destinationWalkable{};
    bool nativeHasPath{};
    std::uint64_t searches{};
    bool fromTerrainWalkable{};
    MovementFootprint footprint{};
    UnitId mineralTarget{-1};
    NavigationObstacleBounds mineralBounds{};
    std::string nativeOrder;
    NavigationStatus navigationStatus{NavigationStatus::invalidInput};
    bool unresolvedAlternatives{};
    bool searchBudgetExhausted{};
};

class BwapiBridge {
public:
    BwapiBridge() = default;

    std::function<void(const ActionDiagnostic&)> actionDiagnostic;
    std::function<void(const BuildLeaseDiagnostic&)> buildLeaseDiagnostic;
    std::function<void(const BuildSelectionDiagnostic&)> buildSelectionDiagnostic;
    std::function<void(const BuildRouteDiagnostic&)> buildRouteDiagnostic;
    std::function<void(std::string_view)> placementDiagnostic;
    std::function<bool(const BWAPI::UnitCommand&, bool, bool)> productionDiagnostic;
    std::function<bool(const BWAPI::UnitCommand&, std::string_view)> productionPermission;
    [[nodiscard]] std::uint64_t diagnosticErrors() const noexcept { return diagnosticErrors_; }
    [[nodiscard]] BWAPI::Error lastIssueError() const noexcept { return lastIssueError_; }

    void onStart();
    void setSpendingLedger(ResourceLedger* ledger) noexcept { spendingLedger_ = ledger; }
    [[nodiscard]] GameState observe();
    [[nodiscard]] NavigationGrid navigationGrid() const;
    void initializeNavigationObstacles(NavigationGrid& navigation);
    void updateNavigationObstacle(NavigationGrid& navigation, BWAPI::Unit unit);
    void removeNavigationObstacle(NavigationGrid& navigation, BWAPI::Unit unit) const;
    void remember(BWAPI::Unit unit);
    void forget(BWAPI::Unit unit);
    [[nodiscard]] std::vector<UnitId> reservedBuilders() const;
    [[nodiscard]] bool pendingBuildAlreadyPaid(const MacroAction& action) const;
    [[nodiscard]] std::vector<ConstructionTaskSite> leasedExpansionSites() const;
    [[nodiscard]] std::vector<BuildBlockerFeedback> buildBlockerFeedback() const;
    [[nodiscard]] std::string_view lastMacroStatus() const noexcept {
        return lastMacroStatus_;
    }

    [[nodiscard]] bool execute(const Command& command, bool* resourcesPaid = nullptr);
    void beginCommandBudget(Frame frame, std::size_t maximumCommands) noexcept;
    [[nodiscard]] std::size_t commandBudgetRemaining() const noexcept {
        return commandBudget_.remaining();
    }
    [[nodiscard]] std::size_t directCommandBudgetRemaining() const noexcept {
        return commandBudget_.directRemaining();
    }
    void beginFrameCommands(CommandBus& commands, Frame frame);
    void advanceConstructionRoutes(Frame frame, const NavigationGrid& navigation);
    void refreshBuildRouteContext(const InfluenceMap& influence, NavigationGrid& navigation);
    [[nodiscard]] NavigationWorkDiagnostics constructionRoutingDiagnostics() const noexcept {
        return constructionRouteWork_.diagnostics();
    }
    void prepareConstructionTraffic(const GameState& state, std::vector<Command>& combat,
                                    const InfluenceMap& influence, NavigationGrid& navigation);
    [[nodiscard]] std::vector<ConstructionTrafficContext> constructionTrafficContexts() const;
    void endFrameCommands() noexcept;
    [[nodiscard]] bool executeFrameCommand(const Command& command,
                                          bool* resourcesPaid = nullptr);
    [[nodiscard]] bool executeProduction(const BWAPI::UnitCommand& command) { return issue(command, "production-demand"); }
    [[nodiscard]] bool executeWholeGame(const BWAPI::UnitCommand& command,
                                        Frame leaseFrames = 24, int urgency = 70);
    [[nodiscard]] bool commandActive(const Command& command) const;
    [[nodiscard]] bool claimConfirmedFrameAuthority(const Command& command, bool apiAccepted);
    [[nodiscard]] std::vector<Position> reservedStormZones(Frame currentFrame) const;
    [[nodiscard]] ExpansionFeedback expansionFeedback(Position plannedSite) const;
    [[nodiscard]] bool cancelExpansion();
    [[nodiscard]] const std::vector<MacroExecution>& macroExecutions() const noexcept {
        return macroExecutions_;
    }
    int executeMacro(
        std::span<const MacroAction> actions,
        const StrategicPlan& plan,
        const InfluenceMap& influence,
        std::span<const UnitId> unavailableBuilders = {},
        int maximumCommands = 8,
        NavigationGrid* navigation = nullptr,
        std::int64_t planningBudgetUs = 32'000);
    [[nodiscard]] std::size_t submitWorkerCommands(
        std::span<const WorkerAssignment> assignments, CommandBus& commands);
    [[nodiscard]] std::uint64_t releaseWorkerCommandLease(UnitId actor) noexcept;
    [[nodiscard]] std::uint64_t releaseLearnedCommandLease(const Command& winner, bool apiAccepted);
    [[nodiscard]] std::uint64_t releaseConstructionCommandLease(
        const Command& winner, bool apiAccepted);
    void invalidateCommandLease(UnitId actor, CommandOwner owner,
                                std::uint64_t generation);
    [[nodiscard]] std::vector<ScoutCommandFeedback> submitScouts(
        std::span<const ScoutOrder> orders, CommandBus& commands);
    void runMaintenance(const StrategicPlan& plan, CommandBus& commands);
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
        bool psionicStorm{};
    };

    struct PendingBuild {
        std::uint64_t taskId{};
        UnitKind kind{UnitKind::unknown};
        ConstructionTaskSite constructionSite{};
        bool resourcesPaid{};
        BuildTaskPhase phase{BuildTaskPhase::commandPending};
        UnitId builder{-1};
        Frame issued{};
        Frame commandIssued{-1};
        Frame travelDeadline{-1};
        Frame hardTravelDeadline{-1};
        int bestDistanceToTarget{-1};
        Frame commandAcknowledged{-1};
        // Pixel coordinates of the exact top-left build tile for every type.
        Position target{-1, -1};
        Position routeWaypoint{-1, -1};
        Position travelWaypoint{-1, -1};
        BuildWaypointProgress waypointProgress{};
        bool prepositioned{};
        Position lastPosition{-1, -1};
        Frame lastRouteProgress{-1};
        bool unsafeRoute{};
        bool routePending{};
        bool footprintAccessible{};
        bool plannedRemotePower{};
        BuildCancellation cancellation{};
        bool cancellationInvalidatesSite{true};
    };

    struct ConstructionCommandProposal {
        Command command;
        std::function<void(bool accepted, bool resourcesPaid)> feedback;
    };

    struct FailedBuildSite {
        UnitKind kind{UnitKind::unknown};
        Position target{-1, -1};
        Frame expires{};
    };

    struct PlacementSearchState {
        struct FailedExit {
            BWAPI::TilePosition tile{BWAPI::TilePositions::None};
            std::uint64_t terrain{}, collision{};
            std::vector<PlacementAccessRoute> routes;
        };
        int offset{};
        Position anchor{-1, -1};
        BWAPI::TilePosition fireFallback{BWAPI::TilePositions::None};
        BWAPI::TilePosition laneFallback{BWAPI::TilePositions::None};
        BWAPI::TilePosition pendingAccessTile{BWAPI::TilePositions::None};
        std::vector<FailedExit> failedExits;
    };

    struct ExpansionBuilderPreference {
        UnitId builder{-1};
        Position anchor{-1, -1};
        Frame observedAt{-1};
        Frame firstSeen{-1};
    };

    struct WorkerCommandLease {
        WorkerJob job{WorkerJob::idle};
        int baseId{-1};
        UnitId targetUnit{-1};
        std::uint64_t generation{};
    };

    struct ResourceSite {
        Position resourceCenter{-1, -1};
        Position depotCenter{-1, -1};
        Position mineralLine{-1, -1};
        BWAPI::TilePosition depotTile{BWAPI::TilePositions::None};
        std::vector<DefensivePosition> defenses{};
        int groundDistanceFromStart{-1};
        bool groundReachabilityKnown{};
        int enemyGroundDistanceFromMain{-1};
        bool enemyGroundReachabilityKnown{};
    };

    struct UnitLifetime {
        UnitKind kind{UnitKind::unknown};
        Frame firstSeen{};
        int lastHitPoints{};
        int lastShields{};
        Frame lastDamageFrame{-1};
    };

    struct TrackedProjectile {
        IncomingProjectileFamily family{IncomingProjectileFamily::unknown};
        UnitId source{-1};
        UnitId target{-1};
        Frame sourceFirstSeen{-1};
        Frame targetFirstSeen{-1};
        WeaponSnapshot groundWeapon{};
        WeaponSnapshot airWeapon{};
    };

    std::unordered_map<UnitId, UnitLifetime> selfUnitLifetimes_;
    std::unordered_map<UnitId, UnitSnapshot> enemyMemory_;
    std::unordered_map<int, TrackedProjectile> incomingProjectileMemory_;
    ResourceIncomeTracker resourceIncomeTracker_;
    MineralAllocator mineralAllocator_;
    std::unordered_map<int, Frame> baseLastScouted_;
    std::unordered_map<int, Frame> baseLastConfirmedEmpty_;
    // Stable task IDs allow same-kind structures at separate bases to retain
    // independent builders, placements, retries, and completion feedback.
    std::unordered_map<std::uint64_t, PendingBuild> pendingBuilds_;
    std::unordered_map<std::uint64_t, ConstructionTrafficAssistance> constructionTraffic_;
    std::uint64_t trafficGeneration_{};
    void observeConstructionTraffic(const GameState& state);
    [[nodiscard]] bool trafficActorSafe(UnitId actor, Frame firstSeen) const;
    [[nodiscard]] bool trafficWaypointHold(const PendingBuild& pending,
                                         const InfluenceMap& influence, NavigationGrid* navigation);
    [[nodiscard]] bool trafficCommandCurrent(const Command& command) const;
    void acknowledgeTrafficCommand(const Command& command, bool accepted);
    std::unordered_map<std::uint64_t, BuildBlockerFeedback> buildBlockers_;
    std::unordered_map<std::uint64_t, ExpansionBuilderPreference> expansionBuilderPreferences_;
    std::vector<FailedBuildSite> failedBuildSites_;
    std::unordered_map<std::uint64_t, PlacementSearchState> placementSearches_;
    std::vector<PlacementAccessRoute> placementAccessBaselines_;
    std::vector<std::array<int, 5>> placementAccessSourceKeys_;
    std::vector<PlacementAccessRequirement> placementAccessTemplates_;
    std::uint64_t placementAccessTerrain_{};
    std::uint64_t placementAccessCollision_{};
    std::unordered_map<UnitId, Frame> unitCommandLocks_;
    struct LearnedCommandLease { Frame expires{}; int urgency{70}; };
    std::unordered_map<UnitId, LearnedCommandLease> learnedCommandLeases_;
    const Command* activeTypedCommand_{};
    [[nodiscard]] bool canPreemptLearnedCommandLease(const Command& winner) const noexcept;
    std::unordered_map<UnitId, WorkerCommandLease> workerCommandLeases_;
    std::unordered_map<UnitId, std::unordered_map<CommandOwner, std::uint64_t>>
        issuedCommandLeaseGenerations_;
    FrameCommandClaims frameCommandClaims_;
    CommandBus* frameCommandBus_{};
    FrameCommandBudget commandBudget_;
    bool dispatchingFrameCommand_{};
    Frame constructionProposalFrame_{-1};
    std::vector<ConstructionCommandProposal> constructionCommandProposals_;
    bool constructionCommandQueuedThisAction_{};
    std::vector<ResourceSite> resourceSites_;
    Position enemyMainRouteSource_{-1, -1};
    std::optional<NavigationGrid> baseTerrain_;
    BaseTerrainAnalysis baseTerrainAnalysis_;
    std::vector<SpellZone> recentAreaSpells_;
    std::string lastMacroStatus_{"idle"};
    std::vector<MacroExecution> macroExecutions_;
    ResourceLedger* spendingLedger_{};
    NavigationGrid* activeNavigation_{};
    bool buildRouteContextKnown_{};
    std::uint64_t buildRouteTerrainIdentity_{};
    std::uint64_t buildRouteCollisionIdentity_{};
    std::uint64_t buildRouteThreatIdentity_{};
    NavigationWorkQueue constructionRouteWork_;
    NavigationWorkQueue placementExitWork_;
    BWAPI::Error lastIssueError_{BWAPI::Errors::None};
    std::uint64_t diagnosticErrors_{};

    enum class ResourceUse : std::uint8_t { available, committed };
    bool issue(const BWAPI::UnitCommand& command, std::string_view source,
               ResourceUse resourceUse = ResourceUse::available,
               bool* resourcesPaid = nullptr);
    bool executeOwnedCommand(UnitId actor, CommandOwner owner, CommandType type,
                             UnitId targetUnit, Position targetPosition,
                             UnitKind targetKind, TechnologyKind technology,
                             int urgency, std::string_view source,
                             bool* resourcesPaid = nullptr);
    bool submitOwnedCommand(CommandBus& commands, UnitId actor, CommandOwner owner,
                            CommandType type, UnitId targetUnit, Position targetPosition,
                            UnitKind targetKind, TechnologyKind technology,
                            int urgency, std::string_view source);
    bool executeConstructionCommand(UnitId actor, CommandType type, UnitKind targetKind,
                                    Position targetPosition, int urgency,
                                    std::string_view source, bool* resourcesPaid = nullptr,
                                    std::function<void(bool accepted, bool resourcesPaid)> feedback = {});
    bool requestBuildCancellation(PendingBuild& pending, BWAPI::Unit builder,
                                 std::string_view source);
    bool reject(const Command& command, std::string_view reason);
    [[nodiscard]] bool psionicStormUsefulNow(Position center) const;
    [[nodiscard]] bool executeUnchecked(const Command& command, bool* resourcesPaid);

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
        bool requireCanBuild = true,
        bool* rejectedForUnsafeRoute = nullptr,
        bool* routePending = nullptr,
        UnitId preferredBuilder = -1,
        BWAPI::Unit* pendingCandidate = nullptr);
    [[nodiscard]] UnitId preferredExpansionBuilder(const MacroAction& action) const;
    void rememberExpansionBuilderPreference(const MacroAction& action, BWAPI::Unit builder);
    void reportBuildRoute(BWAPI::Unit builder, Position anchor, Position destination,
                          std::string_view stage, bool reachable, double peakThreat,
                          const InfluenceMap& influence, std::uint64_t searches,
                          NavigationStatus navigationStatus = NavigationStatus::invalidInput,
                          bool unresolvedAlternatives = false,
                          bool searchBudgetExhausted = false) noexcept;
    void recordBuildBlocker(const MacroAction& action, BuildBlockerReason reason,
                            Frame retryFrames);
    void inspectUnfundedBuild(const MacroAction& action, const StrategicPlan& plan,
                              const InfluenceMap& influence,
                              std::span<const UnitId> unavailableBuilders);
    struct StaticDefenseConstructionContext {
        BWAPI::Unit base{nullptr};
        BWAPI::Position anchor{BWAPI::Positions::None};
        bool forwardLayout{};
        bool underPressure{};
    };
    [[nodiscard]] StaticDefenseConstructionContext staticDefenseConstructionContext(
        BWAPI::UnitType type, const StrategicPlan& plan) const;
    [[nodiscard]] BWAPI::Position supplyConstructionHome(BWAPI::Position home) const;
    [[nodiscard]] BWAPI::Position refineryConstructionHome(
        BWAPI::Position home, const InfluenceMap& influence) const;
    [[nodiscard]] BWAPI::TilePosition buildLocation(
        UnitKind kind,
        BWAPI::UnitType type,
        BWAPI::Unit builder,
        const StrategicPlan& plan,
        const ConstructionTaskSite& constructionSite,
        std::uint64_t taskKey,
        std::string_view reason,
        Position constructionAnchor);
    [[nodiscard]] bool blocksMiningLane(
        BWAPI::TilePosition tile,
        BWAPI::UnitType type) const;
    [[nodiscard]] bool build(
        const MacroAction& action,
        const StrategicPlan& plan,
        const InfluenceMap& influence,
        std::span<const UnitId> unavailableBuilders,
        NavigationGrid* navigation,
        bool prepositionOnly = false);
    [[nodiscard]] bool train(const MacroAction& action);
    [[nodiscard]] bool executeTechnology(const MacroAction& action);
    [[nodiscard]] static BWAPI::Position toBwapiPosition(Position position) noexcept;
};

}  // namespace protodd::bwapi
