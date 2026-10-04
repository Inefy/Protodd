#include "protodd/HybridDiagnostics.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures{};

void check(const bool condition, const std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void testSerializedSummaryKeysAreUnique() {
    using namespace protodd;
    HybridDiagnosticCounters diagnostics;
    auto sample = HybridDiagnosticSample{};
    sample.command = HybridDiagnosticCommand::attackUnit;
    sample.reason = HybridDiagnosticReason::candidateQueued;
    diagnostics.recordIntake(sample);

    sample.command = HybridDiagnosticCommand::rightClickPosition;
    sample.commandTypeId = 30; // BWAPI UnitCommandTypes::Right_Click_Position
    sample.targetRelation = HybridDiagnosticRelation::position;
    sample.reason = HybridDiagnosticReason::commandTypeNotAttackUnit;
    diagnostics.recordIntake(sample);

    sample.command = HybridDiagnosticCommand::rightClickUnit;
    sample.commandTypeId = 31; // BWAPI UnitCommandTypes::Right_Click_Unit
    sample.targetRelation = HybridDiagnosticRelation::enemy;
    diagnostics.recordIntake(sample);

    // Relation totals are collected only for unit-target right-clicks. Keep the
    // position-command sample above, and separately exercise the relation field.
    sample.targetRelation = HybridDiagnosticRelation::position;
    diagnostics.recordIntake(sample);

    std::ostringstream serialized;
    writeHybridDiagnosticTrace(serialized, diagnostics);
    std::istringstream lines(serialized.str());
    std::string summary;
    std::getline(lines, summary);

    std::istringstream fields(summary);
    std::string field;
    std::getline(fields, field, ',');
    std::vector<std::string> keys;
    while (std::getline(fields, field, ',')) {
        const auto separator = field.find('=');
        check(separator != std::string::npos, "summary field has a key/value separator");
        if (separator == std::string::npos) continue;
        const auto key = field.substr(0, separator);
        check(std::ranges::find(keys, key) == keys.end(), "serialized summary key is unique");
        keys.push_back(key);
    }
    check(summary.find("right_click_position_total=1") != std::string::npos,
          "position right-click command count is serialized");
    check(summary.find("right_click_target_position=1") != std::string::npos,
          "unit-target relation position count has a distinct key");
    check(summary.find("right_click_unit_total=2") != std::string::npos,
          "enemy-unit right-click count is distinct from other command types");
    std::string detail;
    std::getline(lines, detail); // reason summary
    std::getline(lines, detail); // Attack_Unit sample
    std::getline(lines, detail); // position right-click sample
    std::getline(lines, detail); // enemy-unit right-click sample
    check(detail.find("command=right-click-unit") != std::string::npos &&
          detail.find("command_type_id=31") != std::string::npos,
          "sample retains the raw command class and BWAPI command type ID");
    check(summary.find("attack_unit_total=1") != std::string::npos,
          "Attack_Unit count is serialized");
}

void testSampleBoundsResetAndSaturation() {
    using namespace protodd;
    HybridDiagnosticCounters diagnostics;
    auto sample = HybridDiagnosticSample{};
    sample.command = HybridDiagnosticCommand::rightClickUnit;
    sample.commandTypeId = 12;
    sample.targetRelation = HybridDiagnosticRelation::enemy;
    sample.reason = HybridDiagnosticReason::commandTypeNotAttackUnit;
    for (std::size_t i = 0; i < HybridDiagnosticCounters::sampleLimit + 3; ++i)
        diagnostics.recordIntake(sample);
    for (std::size_t i = 0; i < HybridDiagnosticCounters::sampleLimit + 3; ++i)
        diagnostics.recordProposal(sample, HybridDiagnosticReason::expiredProposal);
    check(diagnostics.intakeSamples().size() == HybridDiagnosticCounters::sampleLimit,
          "intake samples are capped");
    check(diagnostics.proposalSamples().size() == HybridDiagnosticCounters::sampleLimit,
          "proposal samples are capped");
    check(diagnostics.proposalSamples().front().command == HybridDiagnosticCommand::rightClickUnit &&
          diagnostics.proposalSamples().front().commandTypeId == 12 &&
          diagnostics.proposalSamples().front().stage == HybridDiagnosticStage::proposal,
          "proposal stage preserves original command class and type ID");
    check(diagnostics.commandCount(HybridDiagnosticCommand::rightClickUnit) ==
              HybridDiagnosticCounters::sampleLimit + 3,
          "aggregate counts continue beyond sample cap");
    check(HybridDiagnosticCounters::incrementSaturated(HybridDiagnosticCounters::counterLimit) ==
              HybridDiagnosticCounters::counterLimit,
          "aggregate counters saturate without wrapping");
    diagnostics.reset();
    check(diagnostics.commandCount(HybridDiagnosticCommand::rightClickUnit) == 0 &&
          diagnostics.reasonCount(HybridDiagnosticReason::expiredProposal) == 0 &&
          diagnostics.intakeSamples().empty() && diagnostics.proposalSamples().empty(),
          "diagnostics reset between matches");
}
}

int main() {
    testSerializedSummaryKeysAreUnique();
    testSampleBoundsResetAndSaturation();
    return failures == 0 ? 0 : 1;
}
