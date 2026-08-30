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

std::vector<Command> CommandBus::finalize() {
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
        if (command.actor == actor || redundant(command)) {
            continue;
        }
        selected.push_back(command);
        actor = command.actor;
    }
    return selected;
}

void CommandBus::markIssued(const Command& command) {
    lastIssued_.insert_or_assign(command.actor, IssuedCommand{command, frame_});
}

void CommandBus::clear() {
    pending_.clear();
    lastIssued_.clear();
}

bool CommandBus::redundant(const Command& command) const {
    const auto found = lastIssued_.find(command.actor);
    if (found == lastIssued_.end()) {
        return false;
    }
    const auto& previous = found->second;
    const auto age = frame_ - previous.frame;
    if (age > std::max(2, latencyFrames_ + 1)) {
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

