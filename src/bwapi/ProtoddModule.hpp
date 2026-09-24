#pragma once

#include "BwapiBridge.hpp"
#include "PolicyRuntime.hpp"
#include "ModelRuntime.hpp"
#include "WholeGameRuntime.hpp"

#include "protodd/Combat.hpp"
#include "protodd/Diagnostics.hpp"
#include "protodd/Operations.hpp"
#include "protodd/CommandBus.hpp"
#include "protodd/InfluenceMap.hpp"
#include "protodd/Information.hpp"
#include "protodd/Learning.hpp"
#include "protodd/MacroPlanner.hpp"
#include "protodd/Navigation.hpp"
#include "protodd/Scouting.hpp"
#include "protodd/Runtime.hpp"
#include "protodd/Squads.hpp"
#include "protodd/Strategy.hpp"
#include "protodd/Transport.hpp"
#include "protodd/Workers.hpp"

#include <BWAPI.h>

#include <cstdint>
#include <fstream>
#include <map>
#include <vector>

namespace protodd::bwapi {

class ProtoddModule final : public BWAPI::AIModule {
public:
    void onStart() override;
    void onEnd(bool winner) override;
    void onFrame() override;
    void onSendText(std::string text) override;
    void onUnitDiscover(BWAPI::Unit unit) override;
    void onUnitShow(BWAPI::Unit unit) override;
    void onUnitDestroy(BWAPI::Unit unit) override;
    void onUnitMorph(BWAPI::Unit unit) override;
    void onUnitRenegade(BWAPI::Unit unit) override;
    void onUnitCreate(BWAPI::Unit unit) override;
    void onUnitComplete(BWAPI::Unit unit) override;

private:
    BwapiBridge bridge_;
    OpponentModel opponent_;
    InfluenceMap influence_;
    NavigationGrid navigation_;
    StrategyEngine strategy_;
    StrategicDirector strategicDirector_;
    ExpansionCoordinator expansion_;
    MacroPlanner macro_;
    WorkerManager workers_;
    ScoutManager scouts_;
    CombatEvaluator combat_;
    EngagementTracker engagements_;
    TacticalController tactics_;
    SquadPlanner squads_;
    TransportController transports_;
    FrameBudget frameBudget_;
    CommandBus commands_;
    CommandBus scoutCommands_;
    StrategicPlan plan_;
    CombatEstimate fight_;
    GameState state_;
    DebugOverlay debug_;
    OpponentHistory history_;
    PolicyRuntime policy_;
    ModelRuntime model_;
    WholeGameRuntime wholeGame_;
    bool validatedLearning_{false};
    OpeningStyle openingStyle_{OpeningStyle::standard};
    std::string opponentName_;
    std::string mapName_;
    std::vector<UnitId> detectorEscorts_;
    std::vector<UnitId> leasedScouts_;
    std::vector<Position> advanceWaypoints_;
    std::vector<std::uint64_t> navigationSignatures_;
    Frame navigationRefresh_{-1};
    Frame firstCounterattackFrame_{-1};
    Frame firstEnemyContactFrame_{-1};
    Frame firstBaseBreachFrame_{-1};
    Frame firstCoreFrame_{-1};
    Frame firstDragoonFrame_{-1};
    Frame firstRangeFrame_{-1};
    Frame firstExpansionFrame_{-1};
    Frame firstArmyZeroFrame_{-1};
    Frame firstNexusLossFrame_{-1};
    Frame firstAttackFrame_{-1};
    Frame lastTelemetryFrame_{-1};
    Frame lastEventFrame_{-1};
    int lastArmyCount_{-1};
    int lastProbeCount_{-1};
    int lastNexusCount_{-1};
    int lastCompletedNexusCount_{-1};
    int lastEnemyVisibleArmy_{-1};
    int maxArmyCount_{};
    int maxProbeCount_{};
    int maxNexusCount_{};
    int maxEnemyVisibleArmy_{};
    int peakMinerals_{};
    int peakGas_{};
    int telemetrySamples_{};
    int supplyBlockSamples_{};
    int highBankSamples_{};
    int planChanges_{};
    int postureChanges_{};
    std::string lastPlanName_;
    Posture lastPosture_{Posture::hold};
    int maintenanceMineralReserve_{};
    int maintenanceGasReserve_{};
    Frame lastErrorFrame_{-1000};
    Frame slowWindowStart_{-1};
    Frame slowWindowPeakFrame_{-1};
    std::int64_t slowWindowPeakUs_{};
    RuntimeLoad slowWindowLoad_{RuntimeLoad::normal};
    std::ofstream log_;
    ResourceLedger lastLedger_;
    Frame lastMacroFrame_{-1};
    Frame lastSquadLogFrame_{-1};
    FrameIntegral supplyBlockedFrames_;
    FrameIntegral idleGatewayFrames_;
    FrameIntegral idleWorkerFrames_;
    std::map<std::string, PhaseTiming> phases_;
    struct TraceEntry { std::string value; Frame frame{-1}; };
    std::map<std::string, TraceEntry> traceMemory_;
    std::uint64_t commandsAttempted_{};
    std::uint64_t commandsAccepted_{};
    std::uint64_t macroAttempted_{};
    std::uint64_t macroAccepted_{};
    std::uint64_t commandsProposed_{};
    std::uint64_t commandsSuperseded_{};
    std::uint64_t commandsRedundant_{};
    std::uint64_t commandsDeferred_{};
    std::map<std::string, std::uint64_t> actionTotals_;
    struct LastAction { Frame frame{-1}; std::string source; std::string type; UnitId target{-1}; };
    std::map<UnitId, LastAction> lastActions_;
    std::map<UnitId, UnitSnapshot> damageSamples_;
    std::map<std::string, DiagnosticEpisode> incidents_;
    struct MotionSample { Position anchor{-1, -1}; Frame since{}; };
    std::map<UnitId, MotionSample> motionSamples_;
    const char* activePhase_{"startup"};
    std::uint64_t caughtErrors_{};
    std::uint64_t loggingErrors_{};

    void logAction(const ActionDiagnostic& action) noexcept;
    void logLifecycle(BWAPI::Unit unit, std::string_view event);
    void logDamage();
    void incident(std::string_view kind, UnitId unit, bool active, Frame threshold,
                  std::string_view evidence);

    void runFrame();
    void updateStrategy();
    void updateMacro();
    void updateWorkers();
    void updateScouting();
    void updateScoutMicro();
    void updateCombat(bool runSimulation, int navigationInterval,
                      std::size_t commandLimit);
    [[nodiscard]] std::vector<UnitSnapshot> combatUnits(bool ours) const;
    [[nodiscard]] Position retreatPoint() const;
    void sampleTelemetry();
    void logDecision();
    void logDiagnostics();
    void recordPerformance(Frame frame, std::int64_t elapsedUs);
    void flushPerformanceRecord();
    void trace(std::string key, std::string value, Frame heartbeat = 120,
               std::string comparison = {}, Frame frame = -1);
};

}  // namespace protodd::bwapi
