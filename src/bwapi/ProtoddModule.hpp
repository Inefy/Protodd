#pragma once

#include "BwapiBridge.hpp"

#include "protodd/Combat.hpp"
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

private:
    BwapiBridge bridge_;
    OpponentModel opponent_;
    InfluenceMap influence_;
    NavigationGrid navigation_;
    StrategyEngine strategy_;
    StrategicDirector strategicDirector_;
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
    StrategicPlan plan_;
    CombatEstimate fight_;
    GameState state_;
    DebugOverlay debug_;
    OpponentHistory history_;
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
    std::ofstream log_;

    void runFrame();
    void updateStrategy();
    void updateMacro();
    void updateWorkers();
    void updateScouting();
    void updateCombat(bool runSimulation, int navigationInterval,
                      std::size_t commandLimit);
    [[nodiscard]] std::vector<UnitSnapshot> combatUnits(bool ours) const;
    [[nodiscard]] Position retreatPoint() const;
    void sampleTelemetry();
    void logDecision();
};

}  // namespace protodd::bwapi
