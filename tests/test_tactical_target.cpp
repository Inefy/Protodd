#include "protodd/Combat.hpp"
#include "protodd/TacticalTargetModel.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace {
using namespace protodd;
int failures{};
void check(const bool condition, const std::string_view message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

void writeModel(const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    constexpr char magic[8]{'P', 'T', 'T', 'A', 'C', 'T', '1', '\0'};
    constexpr std::uint32_t version = 1;
    constexpr std::uint32_t count = TacticalTargetModel::featureCount;
    std::array<float, TacticalTargetModel::featureCount> weights{};
    std::array<float, TacticalTargetModel::typeCount> typeBias{};
    typeBias[11] = 20.0F;
    output.write(magic, sizeof(magic));
    output.write(reinterpret_cast<const char*>(&version), sizeof(version));
    output.write(reinterpret_cast<const char*>(&count), sizeof(count));
    output.write(reinterpret_cast<const char*>(weights.data()), sizeof(weights));
    output.write(reinterpret_cast<const char*>(typeBias.data()), sizeof(typeBias));
}

UnitSnapshot unit(const int id, const int type, const Position position, const bool ours) {
    UnitSnapshot result;
    result.id = id; result.typeId = type; result.kind = ours ? UnitKind::dragoon : UnitKind::marine;
    result.position = position; result.ours = ours; result.visible = result.detected = result.completed = true;
    result.hitPoints = result.maxHitPoints = 100;
    result.groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
    return result;
}
}

int main(const int argc, char** argv) {
    if (argc == 2) {
        TacticalTargetModel exported;
        if (!exported.load(argv[1])) return 2;
        const auto actor = unit(1, 66, {512, 512}, true);
        const auto first = unit(2, 10, {600, 512}, false);
        const auto second = unit(3, 11, {620, 512}, false);
        const std::array candidates{first, second};
        std::cout << std::setprecision(9)
                  << exported.score(actor, first, candidates) << ' '
                  << exported.score(actor, second, candidates) << '\n';
        return 0;
    }
    const auto path = std::filesystem::temp_directory_path() / "protodd-tactical-target-test.bin";
    writeModel(path);
    TacticalTargetModel model;
    check(model.load(path) && model.valid(), "valid trained target weights load");
    auto attacker = unit(1, 66, {512, 512}, true);
    auto first = unit(2, 10, {600, 512}, false);
    auto second = unit(3, 11, {620, 512}, false);
    std::array candidates{first, second};
    const auto values = model.features(attacker, first, candidates);
    check(std::abs(values[0] - 88.0F / 640.0F) < 0.00001F &&
          std::abs(values[1] - std::log1p(100.0F) / 8.0F) < 0.00001F &&
          values[4] == 0.125F, "features match the training formula");
    CombatEvaluator evaluator;
    check(evaluator.selectTarget(attacker, candidates)->id == first.id,
          "ladder target choice is unchanged without the model");
    check(evaluator.selectTarget(attacker, candidates, {}, &model)->id == second.id,
          "opt-in tactical model ranks legal targets");
    check(model.comparisonCount() == 1 && model.disagreementCount() == 1,
          "trained and heuristic choices are compared on the same legal set");
    candidates[1].kind = UnitKind::probe;
    candidates[1].groundWeapon = {};
    check(evaluator.selectTarget(attacker, candidates, {}, &model)->id == second.id &&
          model.workerOverCombatCount() == 1 && model.threatAbandonedCount() == 1,
          "comparison records a trained worker choice over an attacking combat unit");
    candidates[1] = second;
    candidates[1].invincible = true;
    check(evaluator.selectTarget(attacker, candidates, {}, &model)->id == first.id,
          "model cannot select an invincible target");
    candidates[1] = second;
    candidates[1].position.x = 800;
    check(evaluator.selectTarget(attacker, candidates, {}, &model)->id == first.id,
          "model cannot chase a distant target while a legal shot exists");
    candidates[1] = second;
    const std::array allocation{TargetAllocation{second.id, 100.0}};
    check(evaluator.selectTarget(attacker, candidates, allocation, &model)->id == first.id,
          "model cannot select an already covered target");
    std::filesystem::remove(path);
    check(!model.load(path) && !model.valid() && model.comparisonCount() == 0,
          "missing weights disable model control and reset diagnostics");
    return failures == 0 ? 0 : 1;
}
