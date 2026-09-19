#pragma once

#include "PolicyRuntime.hpp"

#include <BWAPI.h>

#include <functional>
#include <cstdint>
#include <fstream>
#include <map>
#include <vector>

namespace protodd::bwapi {

// Standalone, legal-observation baseline; shares only the policy runtime/core.
class RaceBotModule final : public BWAPI::AIModule {
public:
    // Opening: 0 balanced bio/ling, 1 early pressure, 2 medic/hydra tech.
    // Posture: 0 automatic, 1 defend, 2 attack (still answers base threats).
    struct ActionChoice { int opening{0}; int posture{0}; };
    struct Observation {
        int frame{}, race{}, minerals{}, gas{}, supplyUsed{}, supplyTotal{};
        int workers{}, army{}, bases{}, visibleEnemyArmy{}, visibleBaseThreats{};
    };
    using ActionChoiceHook = std::function<ActionChoice(const Observation&)>;

    explicit RaceBotModule(int brandedRace = 1);
    void configure(int opening, int posture) noexcept;
    // Optional read-only inference when PolicyRuntime is disabled. Called every
    // 240 frames. Shared PolicyRuntime takes precedence when enabled.
    void setActionChoiceHook(ActionChoiceHook hook);
    [[nodiscard]] ActionChoice actionChoice() const noexcept { return choice_; }
    [[nodiscard]] Observation observation() const;
    void onStart() override;
    void onEnd(bool winner) override;
    void onFrame() override;

private:
    struct Construction {
        BWAPI::Unit worker{nullptr};
        BWAPI::UnitType type{BWAPI::UnitTypes::None};
        BWAPI::TilePosition tile{BWAPI::TilePositions::None};
        int issued{-1};
    };
    int brandedRace_{1};
    bool terran_{true};
    bool supported_{false};
    bool attacking_{false};
    bool economyMode_{false};
    int lastMacro_{-1000}, lastCombat_{-1000}, lastPolicy_{-1000};
    int lastSearch_{-1000};
    std::size_t searchIndex_{0};
    BWAPI::Position searchTarget_{BWAPI::Positions::None};
    ActionChoice choice_;
    ActionChoiceHook hook_;
    PolicyRuntime policy_;
    std::ofstream log_;
    int lastLogFrame_{-240}, lastAcceptedFrame_{-1};
    std::uint64_t acceptedProduction_{}, acceptedBuilds_{}, acceptedResumes_{};
    std::uint64_t observedBuildStarts_{}, abandonedBuildOrders_{};
    std::map<int, std::uint64_t> acceptedByType_;
    std::map<std::string, std::uint64_t> buildDiagnostics_;
    Construction construction_;
    std::map<int, BWAPI::TilePosition> enemyBuildings_;
    std::vector<BWAPI::Position> searchPoints_;

    [[nodiscard]] int count(BWAPI::UnitType type, bool completedOnly = false) const;
    [[nodiscard]] int incomingSupply() const;
    [[nodiscard]] bool affordable(BWAPI::UnitType type) const;
    [[nodiscard]] bool reserved(BWAPI::Unit worker) const;
    [[nodiscard]] BWAPI::Position home() const;
    [[nodiscard]] std::vector<BWAPI::Unit> bases() const;
    [[nodiscard]] BWAPI::Unit workerNear(BWAPI::Position position) const;
    [[nodiscard]] BWAPI::Unit threatNear(BWAPI::Position position, int radius) const;
    [[nodiscard]] bool wantsGas() const;
    bool build(BWAPI::UnitType type);
    bool produce(BWAPI::UnitType type);
    bool maintainConstruction();
    void macro();
    void workers();
    void combat();
    void rememberEnemies();
    void logSnapshot() noexcept;
    BWAPI::Position attackDestination(BWAPI::Unit leader);
};

} // namespace protodd::bwapi
