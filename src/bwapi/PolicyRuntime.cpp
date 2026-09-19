#include "PolicyRuntime.hpp"
#include <BWAPI.h>
#include <algorithm>
#include <filesystem>
#include <iterator>
#include <vector>

namespace protodd::bwapi {
namespace {
std::string read(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
}
void PolicyRuntime::start() {
    const auto mode = read("bwapi-data/read/Policy-mode.txt");
    training_ = mode == "train\n" || mode == "train\r\n" || mode == "train";
    enabled_ = training_ || mode == "frozen\n" || mode == "frozen\r\n" || mode == "frozen";
    nextFrame_ = 0;
    action_ = PolicyAction::balanced;
    learner_ = PolicyLearner{};
    const auto snapshot = read("bwapi-data/read/Policy.q");
    const auto loaded = snapshot.empty() || learner_.parse(snapshot);
    if (!loaded) enabled_ = false; // fail closed, don't silently train a corrupt table
    learner_.freeze(!training_);
    const auto selfRace = BWAPI::Broodwar->self()->getRace().getName();
    const auto enemy = BWAPI::Broodwar->enemy();
    const auto enemyRace = enemy ? enemy->getRace().getName() : "Unknown";
    // Share across opponents/maps within a race matchup; never across own races.
    context_ = selfRace + "_" + enemyRace + "_v1";
    std::error_code error;
    std::filesystem::create_directories("bwapi-data/write", error);
    trace_.open("bwapi-data/write/PolicyTrace.log", std::ios::trunc);
    trace_ << "BEGIN,1," << selfRace << ',' << enemyRace << ',' << context_ << ','
           << BWAPI::Broodwar->getRandomSeed() << ','
           << (enabled_ ? (training_ ? "train" : "frozen") : "off") << '\n';
    trace_.flush();
}
PolicyAction PolicyRuntime::decision() {
    if (!enabled_) return PolicyAction::balanced;
    const auto game = BWAPI::BroodwarPtr;
    const auto self = game->self();
    const auto frame = game->getFrameCount();
    int workers = 0, army = 0, nearby = 0;
    std::vector<BWAPI::Position> bases;
    for (const auto unit : self->getUnits()) {
        if (!unit->exists()) continue;
        const auto type = unit->getType();
        if (type.isResourceDepot()) bases.push_back(unit->getPosition());
        if (!unit->isCompleted()) continue;
        if (type.isWorker()) ++workers;
        else if (!type.isBuilding() && type.canAttack()) ++army;
    }
    if (const auto enemy = game->enemy()) for (const auto unit : enemy->getUnits()) {
        if (!unit->exists() || !unit->isVisible() || !unit->getType().canAttack()) continue;
        if (std::ranges::any_of(bases, [unit](const auto base) { return unit->getDistance(base) < 640; }))
            ++nearby;
    }
    const bool emergency = nearby >= 2 || workers < 5;
    PolicyActionMask mask = policyActionBit(PolicyAction::balanced) | policyActionBit(PolicyAction::defend);
    if (emergency) mask = policyActionBit(PolicyAction::defend);
    else {
        if (army >= 6 && frame >= 4000) mask |= policyActionBit(PolicyAction::pressure);
        if (workers >= 12) mask |= policyActionBit(PolicyAction::economy);
    }
    if (frame < nextFrame_ && (mask & policyActionBit(action_))) return action_;
    const auto phase = std::min(3, frame / 7200);
    const auto economy = workers < 13 ? 0 : workers < 27 ? 1 : 2;
    const auto force = army < 6 ? 0 : army < 16 ? 1 : 2;
    const auto state = ((((phase * 3 + economy) * 3 + force) * 2 + (emergency ? 1 : 0)) * 2)
        + (self->supplyTotal() - self->supplyUsed() <= 4 ? 1 : 0);
    const auto seed = static_cast<std::uint64_t>(game->getRandomSeed()) ^
        (static_cast<std::uint64_t>(frame) * UINT64_C(0x9e3779b97f4a7c15));
    action_ = learner_.choose(context_, state, seed, mask, training_).value_or(PolicyAction::balanced);
    nextFrame_ = frame + 480;
    trace_ << "DECISION," << frame << ',' << state << ',' << static_cast<int>(action_) << ',' << mask << '\n';
    trace_.flush();
    return action_;
}
void PolicyRuntime::end(bool won) {
    trace_ << "END," << BWAPI::Broodwar->getFrameCount() << ',' << (won ? 1 : 0) << '\n';
    trace_.flush();
}
}
