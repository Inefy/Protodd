#include "protodd/CommandBus.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace protodd {

bool FrameCommandClaims::available(const Frame frame, const UnitId actor) {
    beginFrame(frame);
    return actor >= 0 && !acceptedActors_.contains(actor);
}

void FrameCommandClaims::recordAccepted(const Frame frame, const UnitId actor) {
    beginFrame(frame);
    if (actor >= 0) acceptedActors_.insert(actor);
}

void FrameCommandClaims::forgetUnit(const UnitId actor) noexcept {
    if (actor >= 0) acceptedActors_.erase(actor);
}

void FrameCommandClaims::clear() noexcept {
    frame_ = -1;
    acceptedActors_.clear();
}

void FrameCommandClaims::beginFrame(const Frame frame) {
    if (frame_ == frame) return;
    frame_ = frame;
    acceptedActors_.clear();
}

void CommandBus::beginFrame(const Frame frame, const int latencyFrames) {
    frame_ = frame;
    latencyFrames_ = std::max(0, latencyFrames);
    pending_.clear();
    authorityWinners_.clear();
    displacedCommands_.clear();
    budgetDeferredCommands_.clear();
    stats_ = {};
}

Frame CommandBus::budgetDeferralAge(const UnitId actor) const noexcept {
    const auto found = budgetDeferredSince_.find(actor);
    return found == budgetDeferredSince_.end() ? 0 : std::max<Frame>(0, frame_ - found->second);
}

void CommandBus::submit(Command command) {
    if (command.actor < 0 || command.earliestFrame > frame_) {
        return;
    }
    if (command.deadlineFrame >= 0 && frame_ > command.deadlineFrame) {
        ++stats_.expired;
        return;
    }
    if (command.owner != CommandOwner::unspecified && command.leaseGeneration > 0) {
        const auto actor = latestLeaseGenerations_.find(command.actor);
        if (actor != latestLeaseGenerations_.end()) {
            const auto owner = actor->second.find(command.owner);
            if (owner != actor->second.end() && command.leaseGeneration < owner->second) {
                ++stats_.staleLease;
                return;
            }
        }
    }
    pending_.push_back(std::move(command));
    ++stats_.proposed;
}

std::size_t CommandBus::discardPending(const UnitId actor, const CommandOwner owner) {
    if (actor < 0) return 0;
    const auto before = pending_.size();
    std::erase_if(pending_, [actor, owner](const Command& command) {
        return command.actor == actor && command.owner == owner;
    });
    return before - pending_.size();
}

std::vector<Command> CommandBus::finalize(const std::size_t maximumCommands) {
    authorityWinners_.clear();
    displacedCommands_.clear();
    budgetDeferredCommands_.clear();
    std::ranges::stable_sort(pending_, [](const Command& left, const Command& right) {
        if (left.actor != right.actor) {
            return left.actor < right.actor;
        }
        return left.effectiveUrgency() > right.effectiveUrgency();
    });

    std::vector<Command> selected;
    selected.reserve(pending_.size());
    UnitId actor = -1;
    for (const auto& command : pending_) {
        if (command.actor == actor) {
            ++stats_.superseded;
            displacedCommands_.push_back({command, authorityWinners_.back()});
            continue;
        }
        actor = command.actor;
        authorityWinners_.push_back(command);
        // A redundant winning order still owns the unit. Otherwise a lower
        // priority attack can cancel a retreat/recharge on the very next tick.
        if (!redundant(command)) selected.push_back(command);
        else ++stats_.redundant;
    }
    const auto updateDeferralAges = [this](const std::vector<Command>& deferred) {
        std::unordered_set<UnitId> currentActors;
        std::unordered_set<UnitId> deferredActors;
        currentActors.reserve(authorityWinners_.size());
        deferredActors.reserve(deferred.size());
        for (const auto& command : authorityWinners_) currentActors.insert(command.actor);
        for (const auto& command : deferred) {
            deferredActors.insert(command.actor);
            budgetDeferredSince_.try_emplace(command.actor, frame_);
        }
        for (const auto& command : authorityWinners_) {
            if (!deferredActors.contains(command.actor)) budgetDeferredSince_.erase(command.actor);
        }
        std::erase_if(budgetDeferredSince_, [&currentActors](const auto& entry) {
            return !currentActors.contains(entry.first);
        });
    };
    if (selected.size() <= maximumCommands) {
        updateDeferralAges(budgetDeferredCommands_);
        return selected;
    }
    stats_.budgetDeferred = selected.size() - maximumCommands;
    if (maximumCommands == 0) {
        budgetDeferredCommands_ = selected;
        updateDeferralAges(budgetDeferredCommands_);
        return {};
    }

    // Preserve every command above the cutoff priority, then rotate fairly
    // within the tied cutoff group. Large max-supply battles therefore have a
    // hard command budget without permanently starving high unit IDs.
    std::ranges::stable_sort(selected, [](const Command& left, const Command& right) {
        if (left.effectiveUrgency() != right.effectiveUrgency())
            return left.effectiveUrgency() > right.effectiveUrgency();
        return left.actor < right.actor;
    });
    const auto cutoffUrgency = selected[maximumCommands - 1].effectiveUrgency();
    const auto firstTie = std::ranges::find_if(selected, [cutoffUrgency](const Command& command) {
        return command.effectiveUrgency() == cutoffUrgency;
    });
    const auto lastTie = std::ranges::find_if(
        firstTie, selected.end(), [cutoffUrgency](const Command& command) {
            return command.effectiveUrgency() != cutoffUrgency;
        });
    const auto higherCount = static_cast<std::size_t>(firstTie - selected.begin());
    const auto tieCount = static_cast<std::size_t>(lastTie - firstTie);
    const auto tieSlots = maximumCommands - higherCount;
    const auto start = fairnessCursor_ % tieCount;

    std::vector<Command> budgeted(selected.begin(), firstTie);
    budgeted.reserve(maximumCommands);
    for (std::size_t i = 0; i < tieSlots; ++i) {
        budgeted.push_back(*(firstTie + static_cast<std::ptrdiff_t>((start + i) % tieCount)));
    }
    fairnessCursor_ = (start + tieSlots) % tieCount;
    std::unordered_set<UnitId> selectedActors;
    selectedActors.reserve(budgeted.size());
    for (const auto& command : budgeted) selectedActors.insert(command.actor);
    for (const auto& command : selected) {
        if (!selectedActors.contains(command.actor)) budgetDeferredCommands_.push_back(command);
    }
    updateDeferralAges(budgetDeferredCommands_);
    std::ranges::sort(budgeted, {}, &Command::actor);
    return budgeted;
}

void CommandBus::markIssued(const Command& command) {
    const auto tracksEffect = command.type == CommandType::move ||
        command.type == CommandType::attackMove || command.type == CommandType::attackUnit ||
        command.type == CommandType::load || command.type == CommandType::recharge ||
        command.type == CommandType::hold;
    lastIssued_.insert_or_assign(command.actor,
        IssuedCommand{command, frame_, tracksEffect, false, false, -1});
    if (command.owner != CommandOwner::unspecified && command.leaseGeneration > 0) {
        auto& latest = latestLeaseGenerations_[command.actor][command.owner];
        latest = std::max(latest, command.leaseGeneration);
    }
}

std::vector<CommandEffectFeedback> CommandBus::pendingEffectFeedback() const {
    std::vector<CommandEffectFeedback> feedback;
    feedback.reserve(lastIssued_.size());
    for (const auto& [actor, issued] : lastIssued_) {
        if (issued.effectTracked && !issued.effectObserved)
            feedback.push_back({issued.command, issued.frame, issued.effectTimeoutReported});
    }
    std::ranges::sort(feedback, {}, [](const CommandEffectFeedback& entry) {
        return entry.command.actor;
    });
    return feedback;
}

bool CommandBus::markEffectObserved(const Command& command, const Frame observedFrame) {
    const auto found = lastIssued_.find(command.actor);
    if (found == lastIssued_.end() || found->second.command != command ||
        !found->second.effectTracked || found->second.effectObserved ||
        observedFrame < found->second.frame) return false;
    found->second.effectObserved = true;
    found->second.effectFrame = observedFrame;
    return true;
}

bool CommandBus::markEffectTimeoutReported(const Command& command) {
    const auto found = lastIssued_.find(command.actor);
    if (found == lastIssued_.end() || found->second.command != command ||
        !found->second.effectTracked || found->second.effectObserved ||
        found->second.effectTimeoutReported) return false;
    found->second.effectTimeoutReported = true;
    return true;
}

void CommandBus::clear() {
    stats_ = {};
    pending_.clear();
    authorityWinners_.clear();
    displacedCommands_.clear();
    budgetDeferredCommands_.clear();
    budgetDeferredSince_.clear();
    lastIssued_.clear();
    latestLeaseGenerations_.clear();
    fairnessCursor_ = 0;
}

void CommandBus::forgetUnit(const UnitId id) {
    if (id < 0) return;
    std::erase_if(pending_, [id](const Command& command) {
        return command.actor == id || command.targetUnit == id;
    });
    std::erase_if(authorityWinners_, [id](const Command& command) {
        return command.actor == id || command.targetUnit == id;
    });
    std::erase_if(displacedCommands_, [id](const DisplacedCommand& command) {
        return command.proposal.actor == id || command.proposal.targetUnit == id ||
               command.winner.actor == id || command.winner.targetUnit == id;
    });
    budgetDeferredSince_.erase(id);
    std::erase_if(budgetDeferredCommands_, [id](const Command& command) {
        return command.actor == id || command.targetUnit == id;
    });
    std::erase_if(lastIssued_, [id](const auto& entry) {
        return entry.first == id || entry.second.command.targetUnit == id;
    });
    latestLeaseGenerations_.erase(id);
}

bool CommandBus::redundant(const Command& command) const {
    if (command.alreadyActive) return true;
    const auto found = lastIssued_.find(command.actor);
    if (found == lastIssued_.end()) {
        return false;
    }
    const auto& previous = found->second;
    const auto age = frame_ - previous.frame;
    const auto isTacticalMicroMove = [](const Command& candidate) {
        return candidate.type == CommandType::move &&
            candidate.owner == CommandOwner::combat &&
            (candidate.source == "combat-retreat" ||
             candidate.source == "detection-retreat" ||
             candidate.source == "rotate-wounded" ||
             candidate.source == "regroup-frontline" ||
             candidate.source == "melee-support-regroup" ||
             candidate.source == "melee-contact" ||
             candidate.source == "wait-for-mobile-detection");
    };
    // Let an accepted attack reach the engine before ordinary retargeting or
    // formation orders replace it. Higher-priority escapes remain immediate.
    if (previous.command.type == CommandType::attackUnit &&
        age <= latencyFrames_ + 2 &&
        command.effectiveUrgency() <= previous.command.effectiveUrgency() &&
        (command.type == CommandType::attackUnit || command.type == CommandType::attackMove ||
         command.type == CommandType::move || command.type == CommandType::hold)) return true;
    auto suppressionWindow = std::max(2, latencyFrames_ + 1);
    if (command.type == CommandType::attackUnit) {
        suppressionWindow = std::max(suppressionWindow, 18);
    } else if (command.type == CommandType::move ||
               command.type == CommandType::attackMove) {
        suppressionWindow = std::max(suppressionWindow, 24);
    } else if (command.type == CommandType::recharge) {
        suppressionWindow = std::max(suppressionWindow, 24);
    } else if (command.type == CommandType::hold) {
        // Hold is a persistent stance. Reissuing it every latency window
        // repeatedly restarts acquisition and floods otherwise idle squads.
        // Any intervening attack/move changes lastIssued_ and bypasses this.
        suppressionWindow = std::max(suppressionWindow, 120);
    }
    if (isTacticalMicroMove(command) &&
        isTacticalMicroMove(previous.command) &&
        command.effectiveUrgency() < 109 &&
        command.effectiveUrgency() <= previous.command.effectiveUrgency() + 20) {
        suppressionWindow = std::max(suppressionWindow, 48);
    }
    const auto preserveRetreatFromStagingHold =
        isTacticalMicroMove(previous.command) &&
        command.type == CommandType::hold &&
        command.owner == CommandOwner::combat &&
        command.source != "navigation-route-unavailable" &&
        previous.command.effectiveUrgency() < 109 &&
        command.effectiveUrgency() <= previous.command.effectiveUrgency();
    if (preserveRetreatFromStagingHold && age <= std::max(48, latencyFrames_ + 1))
        return true;
    if (age > suppressionWindow) {
        return false;
    }
    const auto tacticalRetreatDeadband = isTacticalMicroMove(command) &&
        isTacticalMicroMove(previous.command) &&
        command.targetPosition.valid() && previous.command.targetPosition.valid() &&
        command.effectiveUrgency() < 109 &&
        command.effectiveUrgency() <= previous.command.effectiveUrgency() + 20 &&
        distanceSquared(command.targetPosition, previous.command.targetPosition) <= 96 * 96;
    if (tacticalRetreatDeadband) {
        // Threat maps, melee contact choices, and detection staging can shift
        // their safe point slightly on each combat tick. Keep the accepted
        // route through that jitter so adjacent branches do not make a unit
        // run between nearby points.
        // Storm and extraction escapes use urgency 109+ and remain immediate.
        return true;
    }
    const auto samePosition = !command.targetPosition.valid() ||
                              distanceSquared(command.targetPosition,
                                              previous.command.targetPosition) <= 8 * 8;
    return command.type == previous.command.type &&
           command.targetUnit == previous.command.targetUnit &&
           command.technology == previous.command.technology &&
           command.targetKind == previous.command.targetKind && samePosition;
}

}  // namespace protodd
