#pragma once

#include "BwapiBridge.hpp"

#include "astra/Combat.hpp"
#include "astra/CommandBus.hpp"
#include "astra/InfluenceMap.hpp"
#include "astra/Information.hpp"
#include "astra/Learning.hpp"
#include "astra/MacroPlanner.hpp"
#include "astra/Navigation.hpp"
#include "astra/Scouting.hpp"
#include "astra/Runtime.hpp"
#include "astra/Squads.hpp"
#include "astra/Strategy.hpp"
#include "astra/Transport.hpp"
#include "astra/Workers.hpp"

#include <BWAPI.h>

#include <cstdint>
#include <fstream>
#include <vector>

namespace astra::bwapi {

class AstraModule final : public BWAPI::AIModule {
public:
    void onStart() override;
    void onEnd(bool winner) override;
    void onFrame() override;
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
    void logDecision();
};

}  // namespace astra::bwapi
