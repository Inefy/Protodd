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
    opponent_.reset(state_.enemy.race);
    influence_ = InfluenceMap(64);
    commands_.clear();

    std::error_code error;
    std::filesystem::create_directories("bwapi-data/write", error);
    opponentName_ = BWAPI::Broodwar->enemy() ? BWAPI::Broodwar->enemy()->getName() : "unknown";
    mapName_ = BWAPI::Broodwar->mapName();
    auto historyCsv = readFile("bwapi-data/read/AstraBot.csv");
    if (historyCsv.empty()) historyCsv = readFile("bwapi-data/write/AstraBot.csv");
    history_.parse(historyCsv);
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
    plan_ = strategy_.plan(state_, opponent_.assessment(), openingStyle_);
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
    auto observersSeen = 0;
    for (const auto& unit : state_.self.units) {
        if (unit.kind == UnitKind::observer) {
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
    bridge_.executeScouts(orders);
}

void AstraModule::updateCombat() {
    const auto friendly = combatUnits(true);
    auto enemy = combatUnits(false);
    const auto aggressive = plan_.posture == Posture::pressure ||
                            plan_.posture == Posture::attack ||
                            plan_.posture == Posture::harass;
    if (!aggressive) {
        std::erase_if(enemy, [&friendly](const UnitSnapshot& target) {
            return !target.visible || std::ranges::none_of(
                friendly,
                [&target](const UnitSnapshot& unit) {
                    return distanceSquared(unit.position, target.position) <= 800 * 800;
                });
        });
    }
    const auto requiredRatio = aggressive ? plan_.attackThreshold : 0.88;
    fight_ = combat_.evaluate(friendly, enemy, requiredRatio,
                              opponent_.assessment().uncertainty);
    const auto objective = aggressive && friendly.size() >= 4 && plan_.attackTarget.valid()
                               ? plan_.attackTarget
                               : plan_.rallyPoint;
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
