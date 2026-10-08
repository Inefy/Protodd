#include "protodd/LearnedPolicy.hpp"
#include "protodd/MacroPlanner.hpp"
#include "protodd/Strategy.hpp"
#include "protodd/Technology.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>

namespace {
int failures{};
void expect(bool condition, std::string_view message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
using namespace protodd;
std::size_t action(std::string_view name) {
    const auto& all = learnedIntents();
    for (std::size_t i = 0; i < all.size(); ++i) if (all[i].name == name) return i;
    throw std::runtime_error("missing action");
}
std::size_t feature(std::string_view name) {
    const auto& all = modelFeatures();
    for (std::size_t i = 0; i < all.size(); ++i) if (all[i].name == name) return i;
    throw std::runtime_error("missing feature");
}
void integer(std::string& result, std::uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) result.push_back(static_cast<char>((value >> (8 * i)) & 255));
}
std::string modelBytes() {
    const auto n = modelFeatures().size();
    const auto a = learnedIntents().size();
    std::string data = "PTDMLP1\n";
    integer(data, 1, 4); integer(data, macroSchemaFingerprint(), 8);
    integer(data, n, 4); integer(data, 2, 4); integer(data, 2, 4); integer(data, a, 4);
    std::vector<float> values(2 * n + 2 + 4 + 2 + a * 2 + a, 0.0F);
    values[0] = 1; // first hidden neuron reads frame feature
    values[n + 1] = 1; // second reads minerals
    values[2 * n + 2] = 1; values[2 * n + 5] = 1; // identity second layer
    values[2 * n + 8 + action("train_probe") * 2] = 2;
    values[2 * n + 8 + a * 2 + action("build_pylon")] = 0.25F;
    for (const auto value : values) integer(data, std::bit_cast<std::uint32_t>(value), 4);
    return data;
}
bool samePlan(const StrategicPlan& a, const StrategicPlan& b) {
    if (a.name != b.name || a.posture != b.posture ||
        a.desiredBases != b.desiredBases || a.desiredWorkers != b.desiredWorkers ||
        a.desiredGasWorkers != b.desiredGasWorkers || a.goals.size() != b.goals.size() ||
        a.composition.size() != b.composition.size()) return false;
    for (std::size_t i = 0; i < a.goals.size(); ++i) {
        const auto& x = a.goals[i];
        const auto& y = b.goals[i];
        if (x.goal != y.goal || x.target != y.target || x.desiredCount != y.desiredCount ||
            x.priority != y.priority || x.blocking != y.blocking || x.reason != y.reason ||
            x.technology != y.technology || x.constructionSite.id != y.constructionSite.id ||
            x.constructionSite.baseId != y.constructionSite.baseId ||
            x.constructionSite.anchor.x != y.constructionSite.anchor.x ||
            x.constructionSite.anchor.y != y.constructionSite.anchor.y) return false;
    }
    for (std::size_t i = 0; i < a.composition.size(); ++i) {
        if (a.composition[i].kind != b.composition[i].kind ||
            a.composition[i].weight != b.composition[i].weight) return false;
    }
    return true;
}
bool sameActions(const std::vector<MacroAction>& a, const std::vector<MacroAction>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto& x = a[i];
        const auto& y = b[i];
        if (x.action != y.action || x.target != y.target || x.priority != y.priority ||
            x.minerals != y.minerals || x.gas != y.gas || x.reserved != y.reserved ||
            x.reason != y.reason || x.technology != y.technology ||
            x.blocksLowerPriority != y.blocksLowerPriority || x.executable != y.executable)
            return false;
    }
    return true;
}
UnitSnapshot unit(UnitKind kind) {
    UnitSnapshot result;
    result.kind = kind; result.ours = result.completed = result.visible = true;
    return result;
}
}  // namespace

int main() {
    using namespace protodd;
    GameState state;
    state.self.race = Race::protoss; state.enemy.race = Race::terran;
    state.frame = 24; state.self.minerals = 150; state.self.gatheredMinerals = 80;
    state.self.units = {unit(UnitKind::probe), unit(UnitKind::nexus)};
    auto enemy = unit(UnitKind::marine);
    enemy.ours = false; enemy.visible = false; enemy.lastSeen = 12;
    state.enemy.units = {enemy};
    ObservationEncoder encoder;
    const auto initial = encoder.encode(state);
    expect(initial.size() == modelFeatures().size(), "complete feature vector");
    expect(std::abs(initial[feature("minerals")] - 0.075F) < 1e-6F, "fixed normalization");
    expect(initial[feature("own_complete/probe")] == 0.02F, "own counts");
    expect(initial[feature("history_ready")] == 0, "no fabricated initial history");
    auto hidden = state;
    hidden.enemy.minerals = 999999; hidden.enemy.gas = 999999;
    hidden.enemy.queuedUnits = {UnitKind::battlecruiser};
    hidden.enemy.technologies = {{TechnologyKind::psionicStorm, 1, true}};
    hidden.enemy.units[0].hitPoints = 999; hidden.enemy.units[0].energy = 250;
    hidden.enemy.units[0].id = 9999; hidden.enemy.units[0].position = {123, 456};
    ObservationEncoder other;
    expect(initial == other.encode(hidden), "private enemy fields and replay IDs never affect features");
    expect(initial == encoder.encode(hidden), "same-frame encoding is idempotent");
    auto privateState = state;
    privateState.enemy.minerals = 999999;
    privateState.enemy.gas = 999999;
    privateState.enemy.supplyUsed = 398;
    privateState.enemy.supplyTotal = 400;
    privateState.enemy.gatheredMinerals = 999999;
    privateState.enemy.gatheredGas = 999999;
    privateState.enemy.queuedUnits = {UnitKind::battlecruiser};
    privateState.enemy.busyProducers = {UnitKind::starport};
    privateState.enemy.producerSlots = {{77, UnitKind::starport, true, 5, 1, 8,
                                         true, true, true, {UnitKind::battlecruiser}}};
    privateState.enemy.technologies = {{TechnologyKind::psionicStorm, 1, true}};
    ObservationEncoder privateEncoder;
    expect(initial == privateEncoder.encode(privateState),
           "enemy bank, production timers, orders and research stay outside policy features");
    const auto publicPlan = StrategyEngine{}.plan(state, {});
    const auto privatePlan = StrategyEngine{}.plan(privateState, {});
    expect(samePlan(publicPlan, privatePlan),
           "enemy private resources, queues and research cannot change the strategic plan");
    OpponentModel publicOpponent;
    OpponentModel privateOpponent;
    publicOpponent.update(state);
    privateOpponent.update(privateState);
    for (int value = static_cast<int>(EnemyPlan::unknown);
         value < static_cast<int>(EnemyPlan::count); ++value) {
        const auto plan = static_cast<EnemyPlan>(value);
        expect(publicOpponent.probability(plan) == privateOpponent.probability(plan),
               "enemy private resources, queues and research cannot change opponent beliefs");
    }
    ResourceLedger publicBank{state.self.minerals, state.self.gas};
    ResourceLedger privateBank{privateState.self.minerals, privateState.self.gas};
    const auto publicActions = MacroPlanner{}.reconcile(state, publicPlan, publicBank);
    const auto privateActions = MacroPlanner{}.reconcile(privateState, privatePlan, privateBank);
    expect(sameActions(publicActions, privateActions),
           "enemy private production state cannot change reserved or executable macro actions");

    constexpr std::array privateUnitKinds{
        UnitKind::marine, UnitKind::battlecruiser, UnitKind::ghost,
        UnitKind::siegeTank, UnitKind::vulture};
    constexpr std::array privateTechKinds{
        TechnologyKind::psionicStorm, TechnologyKind::singularityCharge,
        TechnologyKind::graviticDrive, TechnologyKind::protossGroundWeapons};
    std::uint32_t hiddenRandom = 0xB11D1E55U;
    const auto nextHidden = [&hiddenRandom]() {
        hiddenRandom ^= hiddenRandom << 13U;
        hiddenRandom ^= hiddenRandom >> 17U;
        hiddenRandom ^= hiddenRandom << 5U;
        return hiddenRandom;
    };
    for (int sample = 0; sample < 48; ++sample) {
        auto variant = state;
        variant.enemy.minerals = static_cast<int>(nextHidden() % 400001U);
        variant.enemy.gas = static_cast<int>(nextHidden() % 200001U);
        variant.enemy.supplyUsed = static_cast<int>(nextHidden() % 400U);
        variant.enemy.supplyTotal = static_cast<int>(nextHidden() % 401U);
        variant.enemy.gatheredMinerals = static_cast<int>(nextHidden() % 800001U);
        variant.enemy.gatheredGas = static_cast<int>(nextHidden() % 400001U);
        variant.enemy.queuedUnits.clear();
        variant.enemy.busyProducers.clear();
        variant.enemy.producerSlots.clear();
        variant.enemy.technologies.clear();
        const auto queuedCount = static_cast<std::size_t>(nextHidden() % 5U);
        for (std::size_t index = 0; index < queuedCount; ++index) {
            variant.enemy.queuedUnits.push_back(
                privateUnitKinds[nextHidden() % privateUnitKinds.size()]);
            variant.enemy.busyProducers.push_back(
                privateUnitKinds[nextHidden() % privateUnitKinds.size()]);
        }
        variant.enemy.producerSlots.push_back({
            1000 + sample, UnitKind::starport, (nextHidden() & 1U) != 0,
            static_cast<int>(nextHidden() % 1200U), static_cast<int>(nextHidden() % 6U),
            static_cast<int>(nextHidden() % 100U), (nextHidden() & 1U) != 0,
            (nextHidden() & 1U) != 0, (nextHidden() & 1U) != 0,
            {privateUnitKinds[nextHidden() % privateUnitKinds.size()]}});
        if ((nextHidden() & 1U) != 0) {
            variant.enemy.technologies.push_back({
                privateTechKinds[nextHidden() % privateTechKinds.size()],
                static_cast<int>(nextHidden() % 4U), (nextHidden() & 1U) != 0});
        }
        ObservationEncoder variantEncoder;
        expect(initial == variantEncoder.encode(variant),
               "seeded enemy-only private states leave policy features unchanged");
        const auto variantPlan = StrategyEngine{}.plan(variant, {});
        expect(samePlan(publicPlan, variantPlan),
               "seeded hidden enemy economy and production leave strategy unchanged");
        OpponentModel variantOpponent;
        variantOpponent.update(variant);
        for (int value = static_cast<int>(EnemyPlan::unknown);
             value < static_cast<int>(EnemyPlan::count); ++value) {
            const auto plan = static_cast<EnemyPlan>(value);
            expect(publicOpponent.probability(plan) == variantOpponent.probability(plan),
                   "seeded hidden enemy fields cannot change opponent beliefs");
        }
        ResourceLedger variantBank{variant.self.minerals, variant.self.gas};
        const auto variantActions = MacroPlanner{}.reconcile(variant, publicPlan, variantBank);
        expect(sameActions(publicActions, variantActions),
               "seeded hidden enemy production cannot change macro actions");
    }
    state.frame = 48; state.self.gatheredMinerals = 130;
    const auto next = encoder.encode(state);
    expect(next[feature("history_ready")] == 1, "history becomes available");
    expect(next[feature("mineral_income_per_second")] == 0.5F, "income uses elapsed game frames");
    state.frame = 0;
    expect(encoder.encode(state)[feature("history_ready")] == 0, "backward frame resets history");
    encoder.reset();
    expect(encoder.encode(state)[feature("history_ready")] == 0, "explicit reset clears history");
    const auto allowed = [&state](std::string_view name) {
        return (learnedIntentMask(state) & (LearnedActionMask{1} << action(name))) != 0;
    };
    expect(allowed("wait") && allowed("train_probe") && allowed("build_pylon"), "early macro candidates");
    expect(!allowed("train_dragoon"), "missing technology masked");
    state.self.units.push_back(unit(UnitKind::gateway));
    state.self.units.push_back(unit(UnitKind::cyberneticsCore));
    expect(allowed("train_dragoon"), "valid savings intent can be proposed before affordability");
    state.self.units.push_back(unit(UnitKind::roboticsFacility));
    expect(!allowed("train_reaver"), "Reaver needs support bay");
    state.self.units.push_back(unit(UnitKind::roboticsSupportBay));
    expect(allowed("train_reaver"), "Reaver enabled with support bay");
    state.self.technologies.push_back({TechnologyKind::protossAirWeapons, 1, false});
    expect(!allowed("upgrade_protoss_air_weapons"), "air upgrade level 2 requires beacon");
    state.self.units.push_back(unit(UnitKind::fleetBeacon));
    expect(allowed("upgrade_protoss_air_weapons"), "beacon enables air upgrade level 2");
    state.self.units.push_back(unit(UnitKind::forge));
    state.self.technologies.push_back({TechnologyKind::protossPlasmaShields, 1, false});
    expect(allowed("upgrade_protoss_plasma_shields"), "shield level 2 uses Core, not Archives");

    LearnedPolicy model;
    std::vector<float> values(modelFeatures().size(), 0.0F);
    values[0] = 0.5F;
    expect(!model.predict(values, 1), "unloaded model cannot predict");
    std::string error;
    const auto valid = modelBytes();
    std::istringstream input(valid);
    expect(model.load(input, error), "valid binary loads");
    auto mask = (LearnedActionMask{1} << action("train_probe")) | (LearnedActionMask{1} << action("build_pylon"));
    const auto prediction = model.predict(values, mask);
    expect(prediction && prediction->action == action("train_probe"), "nonzero layers produce expected action");
    expect(prediction && std::abs(prediction->logits[action("train_probe")] - 1) < 1e-6F, "dense math matches analytic result");
    expect(prediction && std::abs(prediction->probability - 0.6791787F) < 1e-6F, "masked softmax probability");
    const auto publicInference = model.predict(initial, learnedIntentMask(state));
    const auto privateInference = model.predict(privateEncoder.encode(privateState),
                                                learnedIntentMask(privateState));
    expect(publicInference && privateInference &&
               publicInference->action == privateInference->action &&
               publicInference->logits == privateInference->logits,
           "enemy private fields cannot affect model inference");
    auto inferenceRandom = std::uint32_t{0x5EEDC0DEU};
    for (int sample = 0; sample < 48; ++sample) {
        inferenceRandom ^= inferenceRandom << 13U;
        inferenceRandom ^= inferenceRandom >> 17U;
        inferenceRandom ^= inferenceRandom << 5U;
        auto variant = privateState;
        variant.enemy.minerals = static_cast<int>(inferenceRandom % 400001U);
        variant.enemy.gas = static_cast<int>((inferenceRandom >> 3U) % 200001U);
        variant.enemy.queuedUnits = {
            privateUnitKinds[inferenceRandom % privateUnitKinds.size()]};
        variant.enemy.busyProducers = {
            privateUnitKinds[(inferenceRandom >> 5U) % privateUnitKinds.size()]};
        variant.enemy.technologies = {{
            privateTechKinds[(inferenceRandom >> 8U) % privateTechKinds.size()],
            static_cast<int>((inferenceRandom >> 12U) % 4U),
            (inferenceRandom & 1U) != 0}};
        ObservationEncoder variantEncoder;
        const auto variantInference = model.predict(
            variantEncoder.encode(variant), learnedIntentMask(variant));
        expect(variantInference && publicInference &&
                   variantInference->action == publicInference->action &&
                   variantInference->logits == publicInference->logits,
               "seeded private enemy states cannot change learned policy inference");
    }
    const auto onlyPylon = LearnedActionMask{1} << action("build_pylon");
    expect(model.predict(values, onlyPylon)->action == action("build_pylon"), "illegal best action is excluded");
    expect(!model.predict(values, 0), "empty mask rejected");
    expect(!model.predict(values, LearnedActionMask{1} << 63), "unknown-only mask rejected");
    values[0] = std::numeric_limits<float>::quiet_NaN();
    expect(!model.predict(values, mask), "NaN feature rejected");
    values[0] = 17;
    expect(!model.predict(values, mask), "out-of-contract feature rejected");
    values[0] = 0.5F;
    for (const auto cut : {std::size_t{0}, std::size_t{12}, valid.size() - 1}) {
        std::istringstream truncated(valid.substr(0, cut));
        expect(!model.load(truncated, error), "truncated model rejected");
        expect(model.predict(values, mask)->action == action("train_probe"), "failed reload preserves current model");
    }
    auto bad = valid;
    bad[12] ^= 1;
    std::istringstream mismatch(bad);
    expect(!model.load(mismatch, error), "schema fingerprint mismatch rejected");
    bad = valid; bad[24] = static_cast<char>(255); bad[25] = static_cast<char>(255);
    std::istringstream excessive(bad);
    expect(!model.load(excessive, error), "excessive width rejected before allocation");
    std::istringstream trailing(valid + "x");
    expect(!model.load(trailing, error), "trailing model bytes rejected");
    bad = valid;
    const auto bits = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::infinity());
    for (unsigned i = 0; i < 4; ++i) bad[36 + i] = static_cast<char>((bits >> (8 * i)) & 255);
    std::istringstream nonfinite(bad);
    expect(!model.load(nonfinite, error), "nonfinite weight rejected");
    model.clear();
    expect(!model.loaded() && !model.predict(values, mask), "clear disables inference");
    std::cout << "Learned model contract tests: " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
