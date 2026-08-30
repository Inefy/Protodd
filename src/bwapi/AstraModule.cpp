#include "AstraModule.hpp"

#include "astra/UnitCatalog.hpp"

#include <algorithm>
#include <filesystem>

namespace astra::bwapi {

void AstraModule::onStart() {
    BWAPI::Broodwar->setCommandOptimizationLevel(2);
    BWAPI::Broodwar->setLatCom(true);
    bridge_.onStart();
    opponent_.reset(bridge_.observe().enemy.race);
    influence_ = InfluenceMap(64);
    commands_.clear();

    std::error_code error;
    std::filesystem::create_directories("bwapi-data/write", error);
    log_.open("bwapi-data/write/AstraBot.log", std::ios::app);
    if (log_) {
        log_ << "START," << BWAPI::Broodwar->mapName() << ','
             << (BWAPI::Broodwar->enemy() ? BWAPI::Broodwar->enemy()->getName() : "unknown")
             << '\n';
    }
}

void AstraModule::onEnd(const bool winner) {
    if (log_) {
        log_ << "END," << (winner ? "win" : "loss") << ',' << state_.frame << '\n';
        log_.flush();
    }
}

void AstraModule::onFrame() {
    if (BWAPI::Broodwar->isReplay() || BWAPI::Broodwar->isPaused() ||
        BWAPI::Broodwar->self() == nullptr || BWAPI::Broodwar->enemy() == nullptr) {
        return;
    }
    state_ = bridge_.observe();
    if (state_.self.race != Race::protoss) {
        BWAPI::Broodwar->drawTextScreen(8, 8, "AstraBot requires Protoss");
        return;
    }

    // Work is staggered to keep frame time predictable under tournament load.
    if (state_.frame % 8 == 0) influence_.update(state_);
    if (state_.frame % 12 == 0) opponent_.update(state_);
    if (state_.frame % 24 == 0 || plan_.goals.empty()) updateStrategy();
    if (state_.frame % 6 == 1) updateMacro();
    if (state_.frame % 12 == 2) updateWorkers();
    if (state_.frame % 24 == 3) updateScouting();
    if (state_.frame % std::max(1, state_.latencyFrames) == 0) updateCombat();
    if (state_.frame % 24 == 5) bridge_.runMaintenance();
    if (state_.frame % (24 * 15) == 0) logDecision();

    bridge_.drawDebug(plan_, opponent_.assessment(), fight_);
}

void AstraModule::onUnitDiscover(const BWAPI::Unit unit) { bridge_.remember(unit); }
void AstraModule::onUnitShow(const BWAPI::Unit unit) { bridge_.remember(unit); }
void AstraModule::onUnitDestroy(const BWAPI::Unit unit) { bridge_.forget(unit); }
void AstraModule::onUnitMorph(const BWAPI::Unit unit) { bridge_.remember(unit); }
void AstraModule::onUnitRenegade(const BWAPI::Unit unit) {
    bridge_.forget(unit);
    bridge_.remember(unit);
}

void AstraModule::updateStrategy() {
    plan_ = strategy_.plan(state_, opponent_.assessment());
}

void AstraModule::updateMacro() {
    ResourceLedger ledger{state_.self.minerals, state_.self.gas};
    const auto actions = macro_.reconcile(state_, plan_, ledger);
    bridge_.executeMacro(actions, plan_);
}

void AstraModule::updateWorkers() {
    const auto assignments = workers_.assign(state_, plan_, influence_);
    bridge_.executeWorkers(assignments);
}

void AstraModule::updateScouting() {
    std::vector<UnitId> available;
    for (const auto& unit : state_.self.units) {
        if (unit.kind == UnitKind::observer || unit.kind == UnitKind::corsair) {
            available.push_back(unit.id);
        }
    }
    if (available.empty() && state_.frame < 6 * 60 * 24) {
        const auto probe = std::ranges::find_if(state_.self.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::probe && !unit.carryingResources;
        });
        if (probe != state_.self.units.end()) available.push_back(probe->id);
    }
    const auto orders = scouts_.assign(state_, available, influence_);
    bridge_.executeScouts(orders);
}

void AstraModule::updateCombat() {
    const auto friendly = combatUnits(true);
    const auto enemy = combatUnits(false);
    fight_ = combat_.evaluate(friendly, enemy, plan_.attackThreshold,
                              opponent_.assessment().uncertainty);
    const auto objective = plan_.attackTarget.valid() ? plan_.attackTarget : plan_.rallyPoint;
    const auto orders = tactics_.control(friendly, enemy, fight_, objective,
                                         retreatPoint(), influence_);
    commands_.beginFrame(state_.frame, state_.latencyFrames);
    for (const auto& order : orders) commands_.submit(order);
    for (const auto& command : commands_.finalize()) {
        if (bridge_.execute(command)) commands_.markIssued(command);
    }
}

std::vector<UnitSnapshot> AstraModule::combatUnits(const bool ours) const {
    const auto& source = ours ? state_.self.units : state_.enemy.units;
    std::vector<UnitSnapshot> result;
    for (const auto& unit : source) {
        if (!isCombatUnit(unit.kind) || !unit.completed) continue;
        if (!ours && !unit.visible && state_.frame - unit.lastSeen > 24 * 45) continue;
        result.push_back(unit);
    }
    return result;
}

Position AstraModule::retreatPoint() const {
    const auto nexus = std::ranges::find(state_.self.units, UnitKind::nexus, &UnitSnapshot::kind);
    return nexus != state_.self.units.end() ? nexus->position : plan_.rallyPoint;
}

void AstraModule::logDecision() {
    if (!log_) return;
    log_ << "STATE," << state_.frame << ',' << plan_.name << ','
         << postureName(plan_.posture) << ','
         << enemyPlanName(opponent_.assessment().mostLikely) << ','
         << opponent_.assessment().uncertainty << ',' << fight_.ratio << ','
         << state_.self.minerals << ',' << state_.self.gas << ','
         << state_.self.supplyUsed << ',' << state_.self.supplyTotal << '\n';
    log_.flush();
}

}  // namespace astra::bwapi
