#include "AstraModule.hpp"

#include "astra/UnitCatalog.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {

std::uint64_t stableSeed(const std::string_view value) {
    std::uint64_t result = 1469598103934665603ULL;
    for (const auto character : value) {
        result ^= static_cast<unsigned char>(character);
        result *= 1099511628211ULL;
    }
    return result;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return input ? std::string(std::istreambuf_iterator<char>(input),
                               std::istreambuf_iterator<char>())
                 : std::string{};
}

}  // namespace

namespace astra::bwapi {

void AstraModule::onStart() {
    BWAPI::Broodwar->setCommandOptimizationLevel(2);
    BWAPI::Broodwar->setLatCom(true);
    bridge_.onStart();
    state_ = bridge_.observe();
    navigation_ = bridge_.navigationGrid();
    opponent_.reset(state_.enemy.race);
    influence_ = InfluenceMap(64);
    commands_.clear();
    transports_.reset();

    std::error_code error;
    std::filesystem::create_directories("bwapi-data/write", error);
    opponentName_ = BWAPI::Broodwar->enemy() ? BWAPI::Broodwar->enemy()->getName() : "unknown";
    mapName_ = BWAPI::Broodwar->mapName();
    history_.parse(readFile("bwapi-data/read/AstraBot.csv"));
    history_.merge(readFile("bwapi-data/write/AstraBot.csv"));
    openingStyle_ = history_.choose(opponentName_, mapName_,
                                    stableSeed(opponentName_ + "|" + mapName_));
    log_.open("bwapi-data/write/AstraBot.log", std::ios::app);
    if (log_) {
        log_ << "START," << BWAPI::Broodwar->mapName() << ','
             << opponentName_ << ',' << openingStyleName(openingStyle_) << '\n';
    }
}

void AstraModule::onEnd(const bool winner) {
    history_.record(opponentName_, mapName_, openingStyle_, winner);
    std::ofstream historyOutput("bwapi-data/write/AstraBot.csv",
                                std::ios::binary | std::ios::trunc);
    if (historyOutput) historyOutput << history_.serialize();
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
    if (state_.frame % 24 == 5) {
        bridge_.runMaintenance(maintenanceMineralReserve_, maintenanceGasReserve_);
    }
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
    plan_ = strategy_.plan(state_, opponent_.assessment(), openingStyle_);
}

void AstraModule::updateMacro() {
    ResourceLedger ledger{state_.self.minerals, state_.self.gas};
    const auto actions = macro_.reconcile(state_, plan_, ledger);
    maintenanceMineralReserve_ = 0;
    maintenanceGasReserve_ = 0;
    for (const auto& action : actions) {
        if (!action.blocksLowerPriority) continue;
        maintenanceMineralReserve_ = std::max(maintenanceMineralReserve_, action.minerals);
        maintenanceGasReserve_ = std::max(maintenanceGasReserve_, action.gas);
        break;
    }
    bridge_.executeMacro(actions, plan_);
}

void AstraModule::updateWorkers() {
    auto reserved = bridge_.reservedBuilders();
    reserved.insert(reserved.end(), leasedScouts_.begin(), leasedScouts_.end());
    std::ranges::sort(reserved);
    reserved.erase(std::unique(reserved.begin(), reserved.end()), reserved.end());
    const auto assignments = workers_.assign(state_, plan_, influence_, reserved);
    bridge_.executeWorkers(assignments);
}

void AstraModule::updateScouting() {
    leasedScouts_.clear();
    std::vector<UnitId> available;
    auto observersSeen = 0;
    for (const auto& unit : state_.self.units) {
        if (unit.kind == UnitKind::observer) {
            if (std::ranges::find(detectorEscorts_, unit.id) != detectorEscorts_.end()) continue;
            // Keep the first observer attached to the main army. Additional
            // observers perform high-value scouting passes.
            if (++observersSeen == 1) continue;
            available.push_back(unit.id);
        } else if (unit.kind == UnitKind::corsair && available.empty()) {
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
    for (const auto& order : orders) leasedScouts_.push_back(order.scout);
    bridge_.executeScouts(orders);
}

void AstraModule::updateCombat() {
    const auto friendly = combatUnits(true);
    const auto enemy = combatUnits(false);
    const auto aggressive = plan_.posture == Posture::pressure ||
                            plan_.posture == Posture::attack ||
                            plan_.posture == Posture::harass;
    const auto formed = squads_.form(state_, friendly, enemy, plan_, retreatPoint());
    const auto refreshNavigation = navigationRefresh_ < 0 ||
                                   state_.frame - navigationRefresh_ >= 24 ||
                                   advanceWaypoints_.size() != formed.size();
    if (refreshNavigation) {
        navigationRefresh_ = state_.frame;
        advanceWaypoints_.assign(formed.size(), {-1, -1});
        retreatWaypoints_.assign(formed.size(), {-1, -1});
    }
    commands_.beginFrame(state_.frame, state_.latencyFrames);
    fight_ = {};
    auto debugSquadSize = std::size_t{0};
    for (std::size_t squadIndex = 0; squadIndex < formed.size(); ++squadIndex) {
        const auto& squad = formed[squadIndex];
        auto requiredRatio = squad.requiredRatio;
        auto objective = squad.objective;
        if (squad.role == SquadRole::mainArmy && (!aggressive || squad.units.size() < 4)) {
            requiredRatio = 0.88;
            objective = plan_.rallyPoint;
        }
        auto routedRetreat = squad.retreat;
        const auto hasGroundUnit = std::ranges::any_of(
            squad.units, [](const UnitSnapshot& unit) { return !unit.flying; });
        if (refreshNavigation && hasGroundUnit) {
            advanceWaypoints_[squadIndex] =
                navigation_.nextWaypoint(squad.center, objective);
            retreatWaypoints_[squadIndex] =
                navigation_.nextWaypoint(squad.center, squad.retreat);
        }
        if (hasGroundUnit) {
            if (advanceWaypoints_[squadIndex].valid()) {
                objective = advanceWaypoints_[squadIndex];
            }
            if (retreatWaypoints_[squadIndex].valid()) {
                routedRetreat = retreatWaypoints_[squadIndex];
            }
        }
        const auto estimate = combat_.evaluate(
            squad.units, squad.enemies, requiredRatio,
            squad.enemies.empty() ? opponent_.assessment().uncertainty * 0.25
                                  : opponent_.assessment().uncertainty);
        if (squad.role == SquadRole::mainArmy && squad.units.size() >= debugSquadSize) {
            debugSquadSize = squad.units.size();
            fight_ = estimate;
        }
        for (const auto& order : tactics_.control(
                 squad.units, squad.enemies, estimate, objective,
                 routedRetreat, influence_, squad.center)) {
            commands_.submit(order);
        }
    }

    detectorEscorts_.clear();
    for (const auto& order : squads_.detectorEscorts(state_, formed, influence_)) {
        detectorEscorts_.push_back(order.actor);
        commands_.submit(order);
    }
    for (const auto& order : transports_.control(
             state_, plan_.attackTarget, retreatPoint(), influence_)) {
        commands_.submit(order);
    }
    // BWAPI calls are capped per combat tick. Priority-aware rotation keeps
    // retreat and detector orders immediate while bounding large-army spikes.
    for (const auto& command : commands_.finalize(96)) {
        if (bridge_.execute(command)) commands_.markIssued(command);
    }
}

std::vector<UnitSnapshot> AstraModule::combatUnits(const bool ours) const {
    const auto& source = ours ? state_.self.units : state_.enemy.units;
    std::vector<UnitSnapshot> result;
    for (const auto& unit : source) {
        if ((!isCombatUnit(unit.kind) && !isStaticDefense(unit.kind)) || !unit.completed) continue;
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
