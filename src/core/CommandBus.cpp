#include "astra/CommandBus.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace astra {

void CommandBus::beginFrame(const Frame frame, const int latencyFrames) {
    frame_ = frame;
    latencyFrames_ = std::max(0, latencyFrames);
    pending_.clear();
}

void CommandBus::submit(Command command) {
    if (command.actor < 0 || command.earliestFrame > frame_) {
        return;
    }
    pending_.push_back(std::move(command));
}

std::vector<Command> CommandBus::finalize(const std::size_t maximumCommands) {
    std::ranges::stable_sort(pending_, [](const Command& left, const Command& right) {
        if (left.actor != right.actor) {
            return left.actor < right.actor;
        }
        return left.priority > right.priority;
    });

    std::vector<Command> selected;
    selected.reserve(pending_.size());
    UnitId actor = -1;
    for (const auto& command : pending_) {
        if (command.actor == actor) continue;
        actor = command.actor;
        // A redundant winning order still owns the unit. Otherwise a lower
        // priority attack can cancel a retreat/recharge on the very next tick.
        if (!redundant(command)) selected.push_back(command);
    }
    if (selected.size() <= maximumCommands) return selected;
    if (maximumCommands == 0) return {};

    // Preserve every command above the cutoff priority, then rotate fairly
    // within the tied cutoff group. Large max-supply battles therefore have a
    // hard command budget without permanently starving high unit IDs.
    std::ranges::stable_sort(selected, [](const Command& left, const Command& right) {
        if (left.priority != right.priority) return left.priority > right.priority;
        return left.actor < right.actor;
    });
    const auto cutoffPriority = selected[maximumCommands - 1].priority;
    const auto firstTie = std::ranges::find(selected, cutoffPriority, &Command::priority);
    const auto lastTie = std::ranges::find_if(
        firstTie, selected.end(), [cutoffPriority](const Command& command) {
            return command.priority != cutoffPriority;
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
    std::ranges::sort(budgeted, {}, &Command::actor);
    return budgeted;
}

void CommandBus::markIssued(const Command& command) {
    lastIssued_.insert_or_assign(command.actor, IssuedCommand{command, frame_});
}

void CommandBus::clear() {
    pending_.clear();
    lastIssued_.clear();
    fairnessCursor_ = 0;
}

bool CommandBus::redundant(const Command& command) const {
    const auto found = lastIssued_.find(command.actor);
    if (found == lastIssued_.end()) {
        return false;
    }
    const auto& previous = found->second;
    const auto age = frame_ - previous.frame;
    auto suppressionWindow = std::max(2, latencyFrames_ + 1);
    if (command.type == CommandType::attackUnit) {
        suppressionWindow = std::max(suppressionWindow, 18);
    } else if (command.type == CommandType::move ||
               command.type == CommandType::attackMove) {
        suppressionWindow = std::max(suppressionWindow, 24);
    } else if (command.type == CommandType::recharge) {
        suppressionWindow = std::max(suppressionWindow, 24);
    }
    if (age > suppressionWindow) {
        return false;
    }
    const auto samePosition = !command.targetPosition.valid() ||
                              distanceSquared(command.targetPosition,
                                              previous.command.targetPosition) <= 8 * 8;
    return command.type == previous.command.type &&
           command.targetUnit == previous.command.targetUnit &&
           command.targetKind == previous.command.targetKind && samePosition;
}

}  // namespace astra
