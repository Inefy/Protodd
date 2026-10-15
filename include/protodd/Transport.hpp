#pragma once

#include "protodd/CommandBus.hpp"
#include "protodd/GameState.hpp"
#include "protodd/InfluenceMap.hpp"
#include "protodd/Navigation.hpp"

#include <unordered_map>
#include <vector>

namespace protodd {

enum class TransportPhase : std::uint8_t { gathering, attacking, extracting, returning };

struct TransportHandoff {
    UnitId shuttle{-1};
    UnitId passenger{-1};
    std::uint64_t shuttleGeneration{};
    std::uint64_t passengerGeneration{};
    bool cargoRecovery{};
    [[nodiscard]] bool released() const noexcept { return shuttle >= 0; }
};

class TransportController {
public:
    [[nodiscard]] std::vector<Command> control(
        const GameState& state,
        Position objective,
        Position retreat,
        const InfluenceMap& influence,
        int reservedArmyReavers = 0,
        bool economicTargets = false,
        const NavigationGrid* navigation = nullptr);
    [[nodiscard]] TransportHandoff preemptActor(
        const GameState& state, const Command& winner, bool confirmed, Position retreat);
    [[nodiscard]] std::uint64_t leaseGeneration(UnitId actor) const noexcept;
    void reset();
    void forgetUnit(UnitId id);
    [[nodiscard]] bool ownsReaver(UnitId id) const;

private:
    struct Mission {
        UnitId reaver{-1};
        TransportPhase phase{TransportPhase::gathering};
        Frame transitionFrame{};
        Position target{-1, -1};
        Position waypoint{-1, -1};
        int observedAmmo{-1};
        int observedVolleys{};
        int pendingShotAmmo{-1};
        Frame pendingShotSince{-1};
        Frame loadRequestFrame{-1};
        int failedLoadAttempts{};
        Frame unloadRequestFrame{-1};
        Frame unloadAcknowledgmentDeadline{-1};
        int failedUnloadAttempts{};
        bool pendingUnload{};
        bool pendingUnloadForReturn{};
        bool waitingForScarabLaunch{};
        bool scarabWaitExpired{};
    };

    // The raid is closed immediately. This separate record watches in-flight
    // pickup acknowledgement and recovers observed cargo before releasing it.
    struct CargoHandoff {
        UnitId passenger{-1};
        Frame releasedFrame{};
        Position retreat{-1, -1};
        bool cargoObserved{};
        bool pendingUnload{};
        Frame unloadRequestFrame{-1};
        int failedUnloadAttempts{};
        bool cancelPickup{};
        Frame unloadAcknowledgmentDeadline{-1};
        Position emergencyStart{-1, -1};
        Position emergencyTarget{-1, -1};
        Frame emergencyTravelDeadline{-1};
        CommandOwner emergencyOwner{CommandOwner::unspecified};
        CommandType emergencyType{CommandType::move};
        UnitId emergencyUnit{-1};
        std::uint64_t emergencyGeneration{};
        Position emergencyIntentPosition{-1, -1};
        bool emergencyIntentValid{};
    };
    static void armEmergencyTravel(CargoHandoff& cargo, const GameState& state,
                                   const Command& winner, UnitId shuttle);
    std::unordered_map<UnitId, CargoHandoff> cargoHandoffs_;
    std::unordered_map<UnitId, std::uint64_t> leaseGenerations_;
    [[nodiscard]] std::uint64_t advanceLeaseGeneration(UnitId actor);
    std::unordered_map<UnitId, Mission> missions_;
    std::unordered_map<UnitId, Frame> nextLaunch_;
};

}  // namespace protodd
