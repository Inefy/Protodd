#include "WholeGameIntent.hpp"
#include "WholeGameTraining.hpp"

#include <iostream>
#include <optional>
#include <string_view>
#include <vector>

namespace {
int failures{};

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

enum class FakeUnitType { none, gateway, dragoon, carrier, interceptor, reaver, scarab };

void testProductionTrainingTypeResolver() {
    using protodd::bwapi::wholeGameTrainingType;
    const auto reaver = wholeGameTrainingType(
        29, std::optional<FakeUnitType>{}, FakeUnitType::reaver, FakeUnitType::reaver,
        FakeUnitType::scarab, FakeUnitType::carrier, FakeUnitType::interceptor);
    expect(reaver == FakeUnitType::scarab, "Reaver fighter intent resolves to Scarab");

    const auto carrier = wholeGameTrainingType(
        29, std::optional<FakeUnitType>{}, FakeUnitType::carrier, FakeUnitType::reaver,
        FakeUnitType::scarab, FakeUnitType::carrier, FakeUnitType::interceptor);
    expect(carrier == FakeUnitType::interceptor, "Carrier fighter intent resolves to Interceptor");

    const auto unsupported = wholeGameTrainingType(
        29, std::optional<FakeUnitType>{}, FakeUnitType::gateway, FakeUnitType::reaver,
        FakeUnitType::scarab, FakeUnitType::carrier, FakeUnitType::interceptor);
    expect(!unsupported, "unsupported fighter producer has no train command");

    const auto ordinary = wholeGameTrainingType(
        13, std::optional<FakeUnitType>{FakeUnitType::dragoon}, FakeUnitType::gateway, FakeUnitType::reaver,
        FakeUnitType::scarab, FakeUnitType::carrier, FakeUnitType::interceptor);
    expect(ordinary == FakeUnitType::dragoon, "ordinary train preserves its requested type");

    const auto unrelated = wholeGameTrainingType(
        7, std::optional<FakeUnitType>{FakeUnitType::dragoon}, FakeUnitType::gateway, FakeUnitType::reaver,
        FakeUnitType::scarab, FakeUnitType::carrier, FakeUnitType::interceptor);
    expect(!unrelated, "non-train intent cannot enter the train command path");
}

struct FakeCommand { int value{}; };
struct FakeActor {
    bool issueable{};
    bool canIssueCommand(const FakeCommand&) const { return issueable; }
};

void testProductionLegalityGate() {
    using protodd::bwapi::wholeGameCommandIssueable;
    const FakeCommand command{17};
    const FakeActor accepted{true};
    const FakeActor rejected{false};
    expect(wholeGameCommandIssueable(&accepted, command),
           "production gate accepts an issueable command");
    expect(!wholeGameCommandIssueable(&rejected, command),
           "production gate retains canIssueCommand rejection");
    expect(!wholeGameCommandIssueable(static_cast<const FakeActor*>(nullptr), command),
           "production gate rejects a missing actor");
}

using protodd::cpu::EncodedObservation;
using protodd::cpu::Output;
using protodd::whole_observation::Entity;
using protodd::whole_observation::Snapshot;

Output mergePrediction(const std::size_t mergeKind) {
    Output prediction;
    prediction.heads["event"] = {10.0f};
    prediction.heads["kind"] = std::vector<float>(protodd::cpu::kindNames.size(), -30.0f);
    prediction.heads["kind"][mergeKind] = 11.0f;
    prediction.heads["kind"][0] = 10.0f;  // High supported competitor, lower joint score.
    prediction.heads["target_mode"] = {-30.0f, 10.0f, -30.0f};
    prediction.heads["domain"] = {10.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    prediction.heads["queued"] = {10.0f, 0.0f};
    prediction.heads["actor"] = {10.0f, 0.0f};
    prediction.heads["target"] = {0.0f, 10.0f};
    prediction.heads["unit_type"] = std::vector<float>(256, 0.0f);
    prediction.heads["technology"] = std::vector<float>(44, 0.0f);
    prediction.heads["upgrade"] = std::vector<float>(61, 0.0f);
    prediction.heads["queue_slot"] = std::vector<float>(16, 0.0f);
    return prediction;
}

void testWinningMergeReturnsNoIntent() {
    EncodedObservation encoded;
    encoded.entityIds = {10, 20};
    encoded.input.width = 128;
    encoded.input.height = 128;
    encoded.input.relation = {0, 0};

    Snapshot observation;
    Entity own;
    own.id = 10;
    own.relation = 0;
    own.visible = true;
    observation.entities.emplace(10, own);
    Entity visible;
    visible.id = 20;
    visible.relation = 1;
    visible.visible = true;
    observation.entities.emplace(20, visible);

    const auto run = [&](const std::size_t kind) {
        const auto prediction = mergePrediction(kind);
        return protodd::cpu::decodeIntent(prediction, encoded, observation);
    };
    expect(!run(32), "winning merge_archon pair returns no intent");
    expect(!run(33), "winning merge_dark_archon pair returns no intent");

    auto control = mergePrediction(32);
    control.heads["kind"][32] = -30.0f;
    const auto supported = protodd::cpu::decodeIntent(control, encoded, observation);
    expect(supported && supported->kind == 0,
           "supported competitor decodes when it is the actual winner");
}
}  // namespace

int main() {
    testProductionTrainingTypeResolver();
    testProductionLegalityGate();
    testWinningMergeReturnsNoIntent();
    if (failures) {
        std::cerr << failures << " whole-game runtime contract checks failed\n";
        return 1;
    }
    std::cout << "whole-game runtime contract checks passed\n";
    return 0;
}
