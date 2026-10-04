#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <span>
#include <string_view>

namespace protodd {

enum class HybridDiagnosticCommand : std::uint8_t {
    attackUnit, rightClickUnit, rightClickPosition, other, count
};
enum class HybridDiagnosticRelation : std::uint8_t { enemy, friendly, neutral, position, missing, unknown, count };
enum class HybridDiagnosticStage : std::uint8_t { intake, proposal };

enum class HybridDiagnosticReason : std::uint8_t {
    candidateQueued,
    commandTypeNotAttackUnit,
    rightClickBridgeDisabled,
    rightClickIneligible,
    missingActor,
    missingTarget,
    hybridControlDisabled,
    controllerNotControlling,
    replacedByNewerProposal,
    futureProposal,
    expiredProposal,
    proposalTypeNotAttackUnit,
    squadWithdrawing,
    emergencyDefense,
    fightNotEngaging,
    advanceBlocked,
    holdScreen,
    actorNotInSquad,
    targetNotInCurrentCandidates,
    unsupportedActorType,
    actorNotOwned,
    actorIncomplete,
    actorLoaded,
    actorDisabled,
    actorHallucination,
    actorInAttackFrame,
    actorInAttackWindup,
    actorUnderStorm,
    actorCoolingDown,
    targetFriendly,
    targetNotVisible,
    targetNotDetected,
    targetInvincible,
    targetLoaded,
    targetAlreadyCovered,
    actorPositionInvalid,
    targetPositionInvalid,
    actorCannotAttack,
    outsideWeaponRange,
    outsideDefenseArea,
    passedSafetyGates,
    safetyGateRejected,
    count
};

struct HybridDiagnosticSample {
    HybridDiagnosticStage stage{HybridDiagnosticStage::intake};
    std::int64_t frame{-1};
    std::int64_t evaluationFrame{-1};
    HybridDiagnosticCommand command{HybridDiagnosticCommand::other};
    int commandTypeId{-1};
    int actorTypeId{-1};
    HybridDiagnosticRelation targetRelation{HybridDiagnosticRelation::unknown};
    bool targetVisible{};
    bool targetDetected{};
    HybridDiagnosticReason reason{HybridDiagnosticReason::commandTypeNotAttackUnit};
};

[[nodiscard]] constexpr HybridDiagnosticCommand classifyHybridDiagnosticCommand(
    const bool attackUnit, const bool rightClickUnit,
    const bool rightClickPosition) noexcept {
    return attackUnit ? HybridDiagnosticCommand::attackUnit :
           rightClickUnit ? HybridDiagnosticCommand::rightClickUnit :
           rightClickPosition ? HybridDiagnosticCommand::rightClickPosition :
                            HybridDiagnosticCommand::other;
}

[[nodiscard]] constexpr HybridDiagnosticRelation classifyHybridDiagnosticTarget(
    const bool hasUnitTarget, const bool hasPositionTarget, const bool enemy,
    const bool friendly, const bool neutral) noexcept {
    if (!hasUnitTarget) return hasPositionTarget ? HybridDiagnosticRelation::position :
                                                  HybridDiagnosticRelation::missing;
    if (enemy) return HybridDiagnosticRelation::enemy;
    if (friendly) return HybridDiagnosticRelation::friendly;
    if (neutral) return HybridDiagnosticRelation::neutral;
    return HybridDiagnosticRelation::unknown;
}

[[nodiscard]] constexpr std::string_view hybridDiagnosticCommandName(
    const HybridDiagnosticCommand value) noexcept {
    switch (value) {
        case HybridDiagnosticCommand::attackUnit: return "attack-unit";
        case HybridDiagnosticCommand::rightClickUnit: return "right-click-unit";
        case HybridDiagnosticCommand::rightClickPosition: return "right-click-position";
        case HybridDiagnosticCommand::other: return "other";
        default: return "invalid";
    }
}

[[nodiscard]] constexpr std::string_view hybridDiagnosticRelationName(
    const HybridDiagnosticRelation value) noexcept {
    switch (value) {
        case HybridDiagnosticRelation::enemy: return "enemy";
        case HybridDiagnosticRelation::friendly: return "friendly";
        case HybridDiagnosticRelation::neutral: return "neutral";
        case HybridDiagnosticRelation::position: return "position";
        case HybridDiagnosticRelation::missing: return "missing";
        case HybridDiagnosticRelation::unknown: return "unknown";
        default: return "invalid";
    }
}

[[nodiscard]] constexpr std::string_view hybridDiagnosticReasonName(
    const HybridDiagnosticReason value) noexcept {
    switch (value) {
        case HybridDiagnosticReason::candidateQueued: return "candidate-queued";
        case HybridDiagnosticReason::commandTypeNotAttackUnit: return "command-type-not-attack-unit";
        case HybridDiagnosticReason::rightClickBridgeDisabled: return "right-click-bridge-disabled";
        case HybridDiagnosticReason::rightClickIneligible: return "right-click-ineligible";
        case HybridDiagnosticReason::missingActor: return "missing-actor";
        case HybridDiagnosticReason::missingTarget: return "missing-target";
        case HybridDiagnosticReason::hybridControlDisabled: return "hybrid-control-disabled";
        case HybridDiagnosticReason::controllerNotControlling: return "controller-not-controlling";
        case HybridDiagnosticReason::replacedByNewerProposal: return "replaced-by-newer-proposal";
        case HybridDiagnosticReason::futureProposal: return "future-proposal";
        case HybridDiagnosticReason::expiredProposal: return "expired-proposal";
        case HybridDiagnosticReason::proposalTypeNotAttackUnit: return "proposal-type-not-attack-unit";
        case HybridDiagnosticReason::squadWithdrawing: return "squad-withdrawing";
        case HybridDiagnosticReason::emergencyDefense: return "emergency-defense";
        case HybridDiagnosticReason::fightNotEngaging: return "fight-not-engaging";
        case HybridDiagnosticReason::advanceBlocked: return "advance-blocked";
        case HybridDiagnosticReason::holdScreen: return "hold-screen";
        case HybridDiagnosticReason::actorNotInSquad: return "actor-not-in-squad";
        case HybridDiagnosticReason::targetNotInCurrentCandidates: return "target-not-in-current-candidates";
        case HybridDiagnosticReason::unsupportedActorType: return "unsupported-actor-type";
        case HybridDiagnosticReason::actorNotOwned: return "actor-not-owned";
        case HybridDiagnosticReason::actorIncomplete: return "actor-incomplete";
        case HybridDiagnosticReason::actorLoaded: return "actor-loaded";
        case HybridDiagnosticReason::actorDisabled: return "actor-disabled";
        case HybridDiagnosticReason::actorHallucination: return "actor-hallucination";
        case HybridDiagnosticReason::actorInAttackFrame: return "actor-in-attack-frame";
        case HybridDiagnosticReason::actorInAttackWindup: return "actor-in-attack-windup";
        case HybridDiagnosticReason::actorUnderStorm: return "actor-under-storm";
        case HybridDiagnosticReason::actorCoolingDown: return "actor-cooling-down";
        case HybridDiagnosticReason::targetFriendly: return "target-friendly";
        case HybridDiagnosticReason::targetNotVisible: return "target-not-visible";
        case HybridDiagnosticReason::targetNotDetected: return "target-not-detected";
        case HybridDiagnosticReason::targetInvincible: return "target-invincible";
        case HybridDiagnosticReason::targetLoaded: return "target-loaded";
        case HybridDiagnosticReason::targetAlreadyCovered: return "target-already-covered";
        case HybridDiagnosticReason::actorPositionInvalid: return "actor-position-invalid";
        case HybridDiagnosticReason::targetPositionInvalid: return "target-position-invalid";
        case HybridDiagnosticReason::actorCannotAttack: return "actor-cannot-attack";
        case HybridDiagnosticReason::outsideWeaponRange: return "outside-weapon-range";
        case HybridDiagnosticReason::outsideDefenseArea: return "outside-defense-area";
        case HybridDiagnosticReason::passedSafetyGates: return "passed-safety-gates";
        case HybridDiagnosticReason::safetyGateRejected: return "safety-gate-rejected";
        default: return "invalid";
    }
}

class HybridDiagnosticCounters {
public:
    static constexpr std::uint32_t counterLimit = 1'000'000;
    static constexpr std::size_t sampleLimit = 8;

    void reset() noexcept {
        commandCounts_.fill(0);
        rightClickRelations_.fill(0);
        reasonCounts_.fill(0);
        intakeSamples_.fill({});
        proposalSamples_.fill({});
        intakeSampleCount_ = proposalSampleCount_ = 0;
    }

    void recordIntake(const HybridDiagnosticSample& sample) noexcept {
        increment(commandCounts_[static_cast<std::size_t>(sample.command)]);
        if (sample.command == HybridDiagnosticCommand::rightClickUnit)
            increment(rightClickRelations_[static_cast<std::size_t>(sample.targetRelation)]);
        increment(reasonCounts_[static_cast<std::size_t>(sample.reason)]);
        append(intakeSamples_, intakeSampleCount_, sample);
    }

    void recordProposal(const HybridDiagnosticSample& sample,
                        const HybridDiagnosticReason reason,
                        const std::int64_t evaluationFrame = -1) noexcept {
        auto recorded = sample;
        recorded.stage = HybridDiagnosticStage::proposal;
        recorded.evaluationFrame = evaluationFrame;
        recorded.reason = reason;
        increment(reasonCounts_[static_cast<std::size_t>(reason)]);
        append(proposalSamples_, proposalSampleCount_, recorded);
    }

    [[nodiscard]] std::uint32_t commandCount(const HybridDiagnosticCommand command) const noexcept {
        return commandCounts_[static_cast<std::size_t>(command)];
    }
    [[nodiscard]] std::uint32_t rightClickCount(const HybridDiagnosticRelation relation) const noexcept {
        return rightClickRelations_[static_cast<std::size_t>(relation)];
    }
    [[nodiscard]] std::uint32_t reasonCount(const HybridDiagnosticReason reason) const noexcept {
        return reasonCounts_[static_cast<std::size_t>(reason)];
    }
    [[nodiscard]] std::span<const HybridDiagnosticSample> intakeSamples() const noexcept {
        return {intakeSamples_.data(), intakeSampleCount_};
    }
    [[nodiscard]] std::span<const HybridDiagnosticSample> proposalSamples() const noexcept {
        return {proposalSamples_.data(), proposalSampleCount_};
    }

    [[nodiscard]] static constexpr std::uint32_t incrementSaturated(
        const std::uint32_t value) noexcept {
        return value < counterLimit ? value + 1 : counterLimit;
    }

private:
    static constexpr std::size_t reasonCapacity =
        static_cast<std::size_t>(HybridDiagnosticReason::count);
    static void increment(std::uint32_t& value) noexcept { value = incrementSaturated(value); }

    template <std::size_t N>
    static void append(std::array<HybridDiagnosticSample, N>& samples, std::size_t& count,
                       const HybridDiagnosticSample& sample) noexcept {
        if (count < N) samples[count++] = sample;
    }

    std::array<std::uint32_t, static_cast<std::size_t>(HybridDiagnosticCommand::count)> commandCounts_{};
    std::array<std::uint32_t, static_cast<std::size_t>(HybridDiagnosticRelation::count)> rightClickRelations_{};
    std::array<std::uint32_t, reasonCapacity> reasonCounts_{};
    std::array<HybridDiagnosticSample, sampleLimit> intakeSamples_{};
    std::array<HybridDiagnosticSample, sampleLimit> proposalSamples_{};
    std::size_t intakeSampleCount_{};
    std::size_t proposalSampleCount_{};
};

inline void writeHybridDiagnosticTrace(std::ostream& output,
                                       const HybridDiagnosticCounters& diagnostics) {
    output << "HYBRID_DIAG_SUMMARY,attack_unit_total="
           << diagnostics.commandCount(HybridDiagnosticCommand::attackUnit)
           << ",right_click_unit_total="
           << diagnostics.commandCount(HybridDiagnosticCommand::rightClickUnit)
           << ",right_click_position_total="
           << diagnostics.commandCount(HybridDiagnosticCommand::rightClickPosition)
           << ",other_total=" << diagnostics.commandCount(HybridDiagnosticCommand::other)
           << ",right_click_target_enemy="
           << diagnostics.rightClickCount(HybridDiagnosticRelation::enemy)
           << ",right_click_target_friendly="
           << diagnostics.rightClickCount(HybridDiagnosticRelation::friendly)
           << ",right_click_target_neutral="
           << diagnostics.rightClickCount(HybridDiagnosticRelation::neutral)
           << ",right_click_target_position="
           << diagnostics.rightClickCount(HybridDiagnosticRelation::position)
           << ",right_click_target_missing="
           << diagnostics.rightClickCount(HybridDiagnosticRelation::missing)
           << ",right_click_target_unknown="
           << diagnostics.rightClickCount(HybridDiagnosticRelation::unknown) << '\n';
    output << "HYBRID_DIAG_REASONS";
    for (std::size_t index = 0;
         index < static_cast<std::size_t>(HybridDiagnosticReason::count); ++index) {
        const auto reason = static_cast<HybridDiagnosticReason>(index);
        const auto count = diagnostics.reasonCount(reason);
        if (count > 0) output << ',' << hybridDiagnosticReasonName(reason) << '=' << count;
    }
    output << '\n';
    const auto writeSamples = [&output](const std::span<const HybridDiagnosticSample> samples,
                                        const std::string_view stage) {
        for (const auto& sample : samples) {
            output << "HYBRID_DIAG_SAMPLE,stage=" << stage
                   << ",frame=" << sample.frame
                   << ",evaluation_frame=" << sample.evaluationFrame
                   << ",command=" << hybridDiagnosticCommandName(sample.command)
                   << ",command_type_id=" << sample.commandTypeId
                   << ",actor_type_id=" << sample.actorTypeId
                   << ",target_relation=" << hybridDiagnosticRelationName(sample.targetRelation)
                   << ",target_visible=" << sample.targetVisible
                   << ",target_detected=" << sample.targetDetected
                   << ",reason=" << hybridDiagnosticReasonName(sample.reason) << '\n';
        }
    };
    writeSamples(diagnostics.intakeSamples(), "intake");
    writeSamples(diagnostics.proposalSamples(), "proposal");
}

}  // namespace protodd
